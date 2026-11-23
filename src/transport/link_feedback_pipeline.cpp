#include "transport/link_feedback_pipeline.h"

#include "core/reliable.h"
#include "transport/udp_io_pipeline.h"

#include <QMetaObject>
#include <QThread>

namespace pd {
namespace {

inline constexpr double kKeyframeRequestIntervalSeconds = 0.2;

} // namespace

class LinkFeedbackWorker final : public QObject {
    Q_OBJECT

public:
    LinkFeedbackWorker(
        UdpIoPipeline *udpIo,
        AuthenticatedPacketPipeline *authenticatedPackets)
        : udpIo_(udpIo)
        , authenticatedPackets_(authenticatedPackets)
    {
    }

    void start(quint64 generation, QString *threadName)
    {
        *threadName = QThread::currentThread()->objectName();
        generation_ = generation;
        accepting_ = true;
        route_ = LinkFeedbackRoute{};
        lastKeyframeRequestAt_ = -1e9;
    }

    void stop()
    {
        accepting_ = false;
        route_ = LinkFeedbackRoute{};
    }

    void resetSession(quint64 generation)
    {
        generation_ = generation;
        route_ = LinkFeedbackRoute{};
        lastKeyframeRequestAt_ = -1e9;
    }

    void setRoute(const LinkFeedbackRoute &route)
    {
        if (!accepting_ || route.generation != generation_) {
            return;
        }
        route_ = route;
    }

    void sendNack(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing)
    {
        if (!canSend(generation, true) || missing.isEmpty()) {
            return;
        }
        UdpSendJob job = feedbackJob(
            PacketType::Nack,
            static_cast<quint8>(StreamKind::Video),
            0,
            packNack(frameId, missing));
        (void)udpIo_->send(job);
    }

    void requestKeyframe(quint64 generation)
    {
        if (!canSend(generation, true)) {
            return;
        }
        const double now = ReliableStream::currentTimeSeconds();
        if (now - lastKeyframeRequestAt_ < kKeyframeRequestIntervalSeconds) {
            return;
        }
        lastKeyframeRequestAt_ = now;
        authenticatedPackets_->requestKeyframe(route_.authGeneration);
    }

    void sendWireCommand(
        quint64 generation,
        quint64 authGeneration,
        const LinkWireCommand &command)
    {
        if (!canSend(generation, false)
            || authGeneration != route_.authGeneration) {
            return;
        }
        UdpSendJob job = feedbackJob(
            command.type,
            command.stream,
            command.flags,
            command.payload);
        (void)udpIo_->send(job);
    }

private:
    [[nodiscard]] bool canSend(quint64 generation, bool requireEstablished) const
    {
        return accepting_
            && generation == generation_
            && route_.generation == generation_
            && route_.udpGeneration != 0
            && route_.udpGeneration == udpIo_->receiveGeneration()
            && route_.sessionReady
            && (!requireEstablished || route_.established)
            && !route_.address.isNull()
            && route_.port != 0;
    }

    [[nodiscard]] UdpSendJob feedbackJob(
        PacketType type,
        quint8 stream,
        quint8 flags,
        const QByteArray &payload) const
    {
        UdpSendJob job;
        job.generation = route_.udpGeneration;
        job.type = type;
        job.stream = stream;
        job.flags = flags;
        job.payload = payload;
        job.address = route_.address;
        job.port = route_.port;
        job.wrapRelay = route_.wrapRelay;
        job.sessionTag = route_.sessionTag;
        job.relayTag = route_.relayTag;
        return job;
    }

    UdpIoPipeline *udpIo_ = nullptr;
    AuthenticatedPacketPipeline *authenticatedPackets_ = nullptr;
    LinkFeedbackRoute route_;
    quint64 generation_ = 0;
    double lastKeyframeRequestAt_ = -1e9;
    bool accepting_ = false;
};

LinkFeedbackPipeline::LinkFeedbackPipeline(
    UdpIoPipeline *udpIo,
    AuthenticatedPacketPipeline *authenticatedPackets,
    QObject *parent)
    : QObject(parent)
{
    worker_ = new LinkFeedbackWorker(udpIo, authenticatedPackets);
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskLinkFeedback"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    thread_.start();
}

LinkFeedbackPipeline::~LinkFeedbackPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void LinkFeedbackPipeline::start()
{
    if (running_.load() || !worker_) {
        return;
    }
    running_.store(true);
    const quint64 generation = generation_.fetch_add(1) + 1;
    QString threadName;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, &threadName]() {
            worker->start(generation, &threadName);
        },
        Qt::BlockingQueuedConnection);
    workerThreadName_ = threadName;
}

void LinkFeedbackPipeline::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    generation_.fetch_add(1);
    workerThreadName_.clear();
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() { worker->stop(); },
            Qt::BlockingQueuedConnection);
    }
}

void LinkFeedbackPipeline::resetSession()
{
    if (!running_.load() || !worker_) {
        return;
    }
    const quint64 generation = generation_.fetch_add(1) + 1;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->resetSession(generation);
        },
        Qt::BlockingQueuedConnection);
}

void LinkFeedbackPipeline::setRoute(const LinkFeedbackRoute &route)
{
    if (!running_.load() || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, route]() {
            worker->setRoute(route);
        },
        Qt::BlockingQueuedConnection);
}

void LinkFeedbackPipeline::submitNack(
    quint64 generation,
    quint32 frameId,
    const QList<quint16> &missing)
{
    if (!running_.load() || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, frameId, missing]() {
            worker->sendNack(generation, frameId, missing);
        },
        Qt::QueuedConnection);
}

void LinkFeedbackPipeline::submitKeyframe(quint64 generation)
{
    if (!running_.load() || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->requestKeyframe(generation);
        },
        Qt::QueuedConnection);
}

void LinkFeedbackPipeline::submitWireCommand(
    quint64 generation,
    quint64 authGeneration,
    const LinkWireCommand &command)
{
    if (!running_.load() || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, authGeneration, command]() {
            worker->sendWireCommand(generation, authGeneration, command);
        },
        Qt::QueuedConnection);
}

} // namespace pd

#include "link_feedback_pipeline.moc"
