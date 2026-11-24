#include "transport/video_send_pipeline.h"

#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

#include <optional>
#include <utility>

namespace pd {

class VideoSendWorker final : public QObject {
    Q_OBJECT

public:
    void start(quint64 generation)
    {
        QMutexLocker locker(&mutex_);
        generation_ = generation;
        sender_.reset();
        pending_.reset();
        accepting_ = true;
    }

    void cancel()
    {
        QMutexLocker locker(&mutex_);
        accepting_ = false;
        pending_.reset();
    }

    void submit(
        quint64 generation,
        const QByteArray &data,
        bool keyframe,
        VideoCodec codec)
    {
        if (data.isEmpty()) {
            return;
        }
        bool schedule = false;
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_ || generation != generation_) {
                return;
            }
            if (pending_.has_value()
                && pending_->keyframe
                && !keyframe) {
                return;
            }
            pending_ = Pending{generation, data, keyframe, codec};
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (schedule) {
            QMetaObject::invokeMethod(
                this,
                &VideoSendWorker::packetizePending,
                Qt::QueuedConnection);
        }
    }

    void retransmit(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing)
    {
        const QString threadName =
            QThread::currentThread()->objectName();
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_ || generation != generation_) {
                return;
            }
        }
        const QList<WirePacket> packets =
            sender_.retransmit(frameId, missing);
        if (packets.isEmpty()) {
            return;
        }
        const VideoCodec codec =
            sender_.cachedCodec(frameId).value_or(VideoCodec::Jpeg);
        emit packetsReady(
            generation,
            VideoPacketBatch{frameId, false, true, codec, packets},
            threadName);
    }

signals:
    void packetsReady(
        quint64 generation,
        const pd::VideoPacketBatch &batch,
        const QString &threadName);
    void failed(quint64 generation, const QString &message);

private:
    struct Pending {
        quint64 generation = 0;
        QByteArray data;
        bool keyframe = false;
        VideoCodec codec = VideoCodec::Jpeg;
    };

    void packetizePending()
    {
        const QString threadName =
            QThread::currentThread()->objectName();
        for (;;) {
            Pending pending;
            {
                QMutexLocker locker(&mutex_);
                if (!pending_.has_value()) {
                    scheduled_ = false;
                    return;
                }
                pending = std::move(*pending_);
                pending_.reset();
            }

            const QList<WirePacket> packets = sender_.build(
                pending.data,
                pending.keyframe,
                pending.codec);
            if (packets.isEmpty()) {
                emit failed(
                    pending.generation,
                    QStringLiteral("视频帧分片失败"));
                continue;
            }
            emit packetsReady(
                pending.generation,
                VideoPacketBatch{
                    sender_.frameId(),
                    pending.keyframe,
                    false,
                    pending.codec,
                    packets,
                },
                threadName);
        }
    }

    mutable QMutex mutex_;
    VideoSender sender_;
    std::optional<Pending> pending_;
    quint64 generation_ = 0;
    bool accepting_ = false;
    bool scheduled_ = false;
};

VideoSendPipeline::VideoSendPipeline(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<pd::VideoPacketBatch>("pd::VideoPacketBatch");
    qRegisterMetaType<pd::VideoCodec>("pd::VideoCodec");

    worker_ = new VideoSendWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskVideoSend"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &VideoSendWorker::packetsReady,
        this,
        &VideoSendPipeline::onWorkerPackets);
    connect(
        worker_,
        &VideoSendWorker::failed,
        this,
        &VideoSendPipeline::onWorkerFailed);
    thread_.start();
}

VideoSendPipeline::~VideoSendPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void VideoSendPipeline::start()
{
    if (running_ || !worker_) {
        return;
    }
    running_ = true;
    const quint64 generation = ++generation_;
    {
        QMutexLocker locker(&threadNameMutex_);
        packetizerThreadName_.clear();
    }
    QMetaObject::invokeMethod(
        worker_,
        [this, generation]() { worker_->start(generation); },
        Qt::BlockingQueuedConnection);
}

void VideoSendPipeline::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    ++generation_;
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [this]() { worker_->cancel(); },
            Qt::BlockingQueuedConnection);
    }
}

void VideoSendPipeline::submit(
    const QByteArray &data,
    bool keyframe,
    VideoCodec codec)
{
    if (!running_ || !worker_) {
        return;
    }
    worker_->submit(generation_, data, keyframe, codec);
}

void VideoSendPipeline::retransmit(
    quint32 frameId,
    const QList<quint16> &missing)
{
    if (!running_ || !worker_ || missing.isEmpty()) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation, frameId, missing]() {
            worker_->retransmit(generation, frameId, missing);
        },
        Qt::QueuedConnection);
}

QString VideoSendPipeline::packetizerThreadName() const
{
    QMutexLocker locker(&threadNameMutex_);
    return packetizerThreadName_;
}

void VideoSendPipeline::onWorkerPackets(
    quint64 generation,
    const pd::VideoPacketBatch &batch,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    {
        QMutexLocker locker(&threadNameMutex_);
        packetizerThreadName_ = threadName;
    }
    emit packetsReady(batch);
}

void VideoSendPipeline::onWorkerFailed(
    quint64 generation,
    const QString &message)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit packetizationFailed(message);
}

} // namespace pd

#include "video_send_pipeline.moc"
