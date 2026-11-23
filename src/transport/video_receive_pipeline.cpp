#include "transport/video_receive_pipeline.h"

#include "core/protocol.h"
#include "core/reliable.h"
#include "core/video.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace pd {

class VideoReceiveWorker final : public QObject {
    Q_OBJECT

public:
    explicit VideoReceiveWorker(QObject *parent = nullptr)
        : QObject(parent)
        , receiver_(
              [this](const QByteArray &frame, bool keyframe) {
                  emit frameDecoded(
                      generation_,
                      frame,
                      keyframe,
                      QThread::currentThread()->objectName());
              },
              [this]() {
                  emit keyframeNeeded(
                      generation_,
                      QThread::currentThread()->objectName());
              },
              VideoSender::kFecK)
    {
    }

    void start(quint64 generation)
    {
        generation_ = generation;
        accepting_ = true;
        receiver_.reset();
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setInterval(20);
            timer_->setTimerType(Qt::PreciseTimer);
            connect(timer_, &QTimer::timeout, this, &VideoReceiveWorker::tick);
        }
        timer_->start();
    }

    void stop()
    {
        accepting_ = false;
        if (timer_) {
            timer_->stop();
        }
        receiver_.reset();
    }

    void resetSession(quint64 generation)
    {
        generation_ = generation;
        receiver_.reset();
    }

    void submitData(
        quint64 generation,
        const QByteArray &payload,
        bool keyframe)
    {
        if (!accepting_ || generation != generation_) {
            return;
        }
        const auto fragment = unpackVideoFragment(payload);
        if (!fragment) {
            return;
        }
        receiver_.onFragment(
            fragment->frameId,
            fragment->index,
            fragment->count,
            fragment->data,
            keyframe,
            ReliableStream::currentTimeSeconds());
    }

    void submitParity(quint64 generation, const QByteArray &payload)
    {
        if (!accepting_ || generation != generation_) {
            return;
        }
        const auto parity = unpackParityFragment(payload);
        if (!parity) {
            return;
        }
        receiver_.onParity(
            parity->frameId,
            parity->groupIndex,
            parity->shards,
            parity->data);
    }

signals:
    void frameDecoded(
        quint64 generation,
        const QByteArray &frame,
        bool keyframe,
        const QString &threadName);
    void nackDecided(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing,
        const QString &threadName);
    void keyframeNeeded(quint64 generation, const QString &threadName);

private:
    void tick()
    {
        if (!accepting_) {
            return;
        }
        const QString threadName = QThread::currentThread()->objectName();
        const QList<VideoNack> nacks =
            receiver_.tick(ReliableStream::currentTimeSeconds());
        for (const VideoNack &nack : nacks) {
            emit nackDecided(
                generation_,
                nack.frameId,
                nack.missing,
                threadName);
        }
    }

    VideoReceiver receiver_;
    QTimer *timer_ = nullptr;
    quint64 generation_ = 0;
    bool accepting_ = false;
};

VideoReceivePipeline::VideoReceivePipeline(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QList<quint16>>("QList<quint16>");

    worker_ = new VideoReceiveWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskVideoReceive"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &VideoReceiveWorker::frameDecoded,
        this,
        &VideoReceivePipeline::onWorkerFrame);
    connect(
        worker_,
        &VideoReceiveWorker::nackDecided,
        this,
        &VideoReceivePipeline::onWorkerNack);
    connect(
        worker_,
        &VideoReceiveWorker::keyframeNeeded,
        this,
        &VideoReceivePipeline::onWorkerKeyframe);
    thread_.start();
}

VideoReceivePipeline::~VideoReceivePipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void VideoReceivePipeline::start()
{
    if (running_ || !worker_) {
        return;
    }
    running_ = true;
    const quint64 generation = ++generation_;
    reassemblerThreadName_.clear();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->start(generation);
        },
        Qt::BlockingQueuedConnection);
}

void VideoReceivePipeline::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    ++generation_;
    reassemblerThreadName_.clear();
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() { worker->stop(); },
            Qt::BlockingQueuedConnection);
    }
}

void VideoReceivePipeline::resetSession()
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = ++generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->resetSession(generation);
        },
        Qt::BlockingQueuedConnection);
}

void VideoReceivePipeline::submitData(
    const QByteArray &payload,
    bool keyframe)
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, payload, keyframe]() {
            worker->submitData(generation, payload, keyframe);
        },
        Qt::QueuedConnection);
}

void VideoReceivePipeline::submitParity(const QByteArray &payload)
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, payload]() {
            worker->submitParity(generation, payload);
        },
        Qt::QueuedConnection);
}

QString VideoReceivePipeline::reassemblerThreadName() const
{
    return reassemblerThreadName_;
}

void VideoReceivePipeline::onWorkerFrame(
    quint64 generation,
    const QByteArray &frame,
    bool keyframe,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    reassemblerThreadName_ = threadName;
    emit frameReady(frame, keyframe);
}

void VideoReceivePipeline::onWorkerNack(
    quint64 generation,
    quint32 frameId,
    const QList<quint16> &missing,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    reassemblerThreadName_ = threadName;
    emit nackReady(frameId, missing);
}

void VideoReceivePipeline::onWorkerKeyframe(
    quint64 generation,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    reassemblerThreadName_ = threadName;
    emit keyframeRequested();
}

} // namespace pd

#include "video_receive_pipeline.moc"
