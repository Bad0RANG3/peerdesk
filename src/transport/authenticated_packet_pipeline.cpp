#include "transport/authenticated_packet_pipeline.h"

#include "core/reliable.h"

#include <QHash>
#include <QJsonArray>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace pd {
namespace {

inline constexpr int kWorkerTickMs = 20;

QByteArray littleU32(quint32 value)
{
    QByteArray output(4, '\0');
    qToLittleEndian<quint32>(value, output.data());
    return output;
}

QString currentThreadName()
{
    return QThread::currentThread()->objectName();
}

} // namespace

class AuthenticatedPacketWorker final : public QObject {
    Q_OBJECT

public:
    AuthenticatedPacketWorker()
        : controlStream_(
              [this](const QByteArray &message) {
                  deliverControl(message);
              },
              [this](quint32 sequence, const QByteArray &payload, quint8 flags) {
                  dispatchWireCommand(LinkWireCommand{
                          PacketType::Data,
                          static_cast<quint8>(StreamKind::Control),
                          flags,
                          littleU32(sequence) + payload,
                      });
              })
    {
    }

    void start(quint64 generation, QString *threadName)
    {
        *threadName = currentThreadName();
        generation_ = generation;
        accepting_ = true;
        resetState();
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setInterval(kWorkerTickMs);
            timer_->setTimerType(Qt::PreciseTimer);
            connect(
                timer_,
                &QTimer::timeout,
                this,
                &AuthenticatedPacketWorker::tick);
        }
        timer_->start();
    }

    void stop()
    {
        accepting_ = false;
        if (timer_) {
            timer_->stop();
        }
        resetState();
    }

    void resetSession(quint64 generation)
    {
        generation_ = generation;
        resetState();
    }

    void receive(
        quint64 generation,
        const Header &header,
        const QByteArray &payload)
    {
        if (!accepting_ || generation != generation_) {
            return;
        }

        const QString threadName = currentThreadName();
        switch (header.type) {
        case PacketType::Data:
            if (header.stream == static_cast<quint8>(StreamKind::Video)) {
                emit videoDataReady(
                    generation_,
                    payload,
                    (header.flags & FlagKeyframe) != 0,
                    threadName);
            } else if (header.stream == static_cast<quint8>(StreamKind::Control)
                       && payload.size() >= 4) {
                const quint32 messageSequence =
                    qFromLittleEndian<quint32>(payload.constData());
                controlStream_.onData(messageSequence, payload.mid(4));
                maybeAck(ReliableStream::currentTimeSeconds());
            }
            break;
        case PacketType::Parity:
            emit videoParityReady(generation_, payload, threadName);
            break;
        case PacketType::Audio:
            emit audioReady(generation_, payload, threadName);
            break;
        case PacketType::Ack: {
            const auto ack = unpackAck(payload);
            if (ack) {
                controlStream_.onAck(ack->cumulative, ack->bitmap);
            }
            break;
        }
        case PacketType::Nack: {
            const auto nack = unpackNack(payload);
            if (nack) {
                emit videoRetransmitNeeded(
                    generation_,
                    nack->first,
                    nack->second,
                    threadName);
            }
            break;
        }
        case PacketType::Ping:
            dispatchWireCommand(
                LinkWireCommand{PacketType::Pong, 0, 0, payload});
            break;
        case PacketType::Pong:
            handlePong(payload, threadName);
            break;
        case PacketType::Bye:
            emit peerBye(generation_, threadName);
            break;
        default:
            break;
        }
    }

    void sendControl(quint64 generation, const QJsonObject &message)
    {
        if (!accepting_ || generation != generation_) {
            return;
        }
        const QList<QByteArray> parts =
            controlFragments_.split(jsonPayload(message));
        for (const QByteArray &part : parts) {
            (void)controlStream_.send(
                part,
                FlagReliable,
                ReliableStream::currentTimeSeconds());
        }
    }

    void requestKeyframe(quint64 expectedGeneration)
    {
        sendControl(
            expectedGeneration,
            QJsonObject{{QStringLiteral("t"), QStringLiteral("keyframe_req")}});
    }

    void sendPing(quint64 generation)
    {
        if (!accepting_ || generation != generation_) {
            return;
        }
        const double now = ReliableStream::currentTimeSeconds();
        pingSequence_ = static_cast<quint16>((pingSequence_ + 1) & 0xFFFF);
        pingsOut_.insert(pingSequence_, now);
        if (pingsOut_.size() > 32) {
            quint16 oldest = 0xFFFF;
            for (auto it = pingsOut_.constBegin(); it != pingsOut_.constEnd(); ++it) {
                oldest = std::min(oldest, it.key());
            }
            pingsOut_.remove(oldest);
        }
        dispatchWireCommand(LinkWireCommand{
                PacketType::Ping,
                0,
                0,
                jsonPayload(
                    QJsonObject{{QStringLiteral("n"), pingSequence_}}),
            });
    }

    void setWireCommandHandler(AuthenticatedPacketPipeline::WireCommandHandler handler)
    {
        wireCommandHandler_ = std::move(handler);
    }

signals:
    void controlMessageReady(
        quint64 generation,
        const QJsonObject &message,
        const QString &threadName);
    void videoDataReady(
        quint64 generation,
        const QByteArray &payload,
        bool keyframe,
        const QString &threadName);
    void videoParityReady(
        quint64 generation,
        const QByteArray &payload,
        const QString &threadName);
    void audioReady(
        quint64 generation,
        const QByteArray &payload,
        const QString &threadName);
    void videoRetransmitNeeded(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing,
        const QString &threadName);
    void wireCommandReady(
        quint64 generation,
        const pd::LinkWireCommand &command,
        const QString &threadName);
    void rttMeasured(
        quint64 generation,
        double rttMs,
        const QString &threadName);
    void peerBye(quint64 generation, const QString &threadName);

private:
    void resetState()
    {
        controlFragments_.reset();
        controlStream_.reset();
        pingSequence_ = 0;
        pingsOut_.clear();
        lastAckAt_ = 0.0;
    }

    void tick()
    {
        if (!accepting_) {
            return;
        }
        const double now = ReliableStream::currentTimeSeconds();
        controlStream_.tick(now);
        maybeAck(now);
    }

    void maybeAck(double now)
    {
        if (!controlStream_.needsAck() || now - lastAckAt_ <= 0.004) {
            return;
        }
        lastAckAt_ = now;
        dispatchWireCommand(LinkWireCommand{
                PacketType::Ack,
                0,
                0,
                packAck(controlStream_.ackPayload()),
            });
    }

    void dispatchWireCommand(LinkWireCommand command)
    {
        if (wireCommandHandler_) {
            wireCommandHandler_(generation_, command);
            return;
        }
        emit wireCommandReady(
            generation_,
            command,
            currentThreadName());
    }

    void deliverControl(const QByteArray &payload)
    {
        const auto body = controlFragments_.feed(payload);
        if (!body) {
            return;
        }
        const auto object = parseJson(*body);
        if (object) {
            emit controlMessageReady(
                generation_,
                *object,
                currentThreadName());
        }
    }

    void handlePong(const QByteArray &payload, const QString &threadName)
    {
        const auto object = parseJson(payload);
        if (!object) {
            return;
        }
        const uint raw =
            object->value(QStringLiteral("n")).toVariant().toUInt();
        const quint16 sequence = static_cast<quint16>(raw & 0xFFFF);
        const auto sent = pingsOut_.find(sequence);
        if (sent == pingsOut_.end()) {
            return;
        }
        const double now = ReliableStream::currentTimeSeconds();
        const double rttMs = std::max(0.0, (now - *sent) * 1000.0);
        pingsOut_.erase(sent);
        emit rttMeasured(generation_, rttMs, threadName);
    }

    ControlFragments controlFragments_;
    ReliableStream controlStream_;
    QTimer *timer_ = nullptr;
    QHash<quint16, double> pingsOut_;
    quint16 pingSequence_ = 0;
    double lastAckAt_ = 0.0;
    quint64 generation_ = 0;
    bool accepting_ = false;
    AuthenticatedPacketPipeline::WireCommandHandler wireCommandHandler_;
};

AuthenticatedPacketPipeline::AuthenticatedPacketPipeline(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<pd::LinkWireCommand>("pd::LinkWireCommand");
    qRegisterMetaType<QList<quint16>>("QList<quint16>");

    worker_ = new AuthenticatedPacketWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskPeerLinkRx"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &AuthenticatedPacketWorker::controlMessageReady,
        this,
        [this](
            quint64 generation,
            const QJsonObject &message,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit controlMessageReceived(message);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::videoDataReady,
        this,
        [this](
            quint64 generation,
            const QByteArray &payload,
            bool keyframe,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit videoDataReceived(payload, keyframe);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::videoParityReady,
        this,
        [this](
            quint64 generation,
            const QByteArray &payload,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit videoParityReceived(payload);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::audioReady,
        this,
        [this](
            quint64 generation,
            const QByteArray &payload,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit audioReceived(payload);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::videoRetransmitNeeded,
        this,
        [this](
            quint64 generation,
            quint32 frameId,
            const QList<quint16> &missing,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit videoRetransmitRequested(frameId, missing);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::wireCommandReady,
        this,
        [this](
            quint64 generation,
            const LinkWireCommand &command,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit wireCommandReady(command);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::rttMeasured,
        this,
        [this](
            quint64 generation,
            double rttMs,
            const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit rttMeasured(rttMs);
        });
    connect(
        worker_,
        &AuthenticatedPacketWorker::peerBye,
        this,
        [this](quint64 generation, const QString &threadName) {
            if (!running_ || generation != generation_) {
                return;
            }
            observeWorker(generation, threadName);
            emit peerByeReceived();
        });
    thread_.start();
}

AuthenticatedPacketPipeline::~AuthenticatedPacketPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void AuthenticatedPacketPipeline::start()
{
    if (running_ || !worker_) {
        return;
    }
    running_ = true;
    const quint64 generation = ++generation_;
    QString threadName;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation, &threadName]() {
            worker_->start(generation, &threadName);
        },
        Qt::BlockingQueuedConnection);
    workerThreadName_ = threadName;
}

void AuthenticatedPacketPipeline::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    ++generation_;
    workerThreadName_.clear();
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [this]() { worker_->stop(); },
            Qt::BlockingQueuedConnection);
    }
}

void AuthenticatedPacketPipeline::resetSession()
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = ++generation_;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation]() {
            worker_->resetSession(generation);
        },
        Qt::BlockingQueuedConnection);
}

void AuthenticatedPacketPipeline::receive(
    const Header &header,
    const QByteArray &payload)
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation, header, payload]() {
            worker_->receive(generation, header, payload);
        },
        Qt::QueuedConnection);
}

void AuthenticatedPacketPipeline::sendControl(const QJsonObject &message)
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation, message]() {
            worker_->sendControl(generation, message);
        },
        Qt::QueuedConnection);
}

void AuthenticatedPacketPipeline::sendPing()
{
    if (!running_ || !worker_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation]() {
            worker_->sendPing(generation);
        },
        Qt::QueuedConnection);
}

void AuthenticatedPacketPipeline::requestKeyframe(quint64 expectedGeneration)
{
    if (!worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, expectedGeneration]() {
            worker->requestKeyframe(expectedGeneration);
        },
        Qt::QueuedConnection);
}

void AuthenticatedPacketPipeline::setWireCommandHandler(
    WireCommandHandler handler)
{
    wireCommandHandler_ = std::move(handler);
    if (!worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, handler = wireCommandHandler_]() mutable {
            worker->setWireCommandHandler(std::move(handler));
        },
        Qt::BlockingQueuedConnection);
}

void AuthenticatedPacketPipeline::observeWorker(
    quint64 generation,
    const QString &threadName)
{
    if (generation == generation_) {
        workerThreadName_ = threadName;
    }
}

} // namespace pd

#include "authenticated_packet_pipeline.moc"
