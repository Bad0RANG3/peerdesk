#include "session/viewer_video_pipeline.h"

#ifdef PD_HAS_H264
#include "codec/media_foundation_h264.h"
#endif

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>

#include <optional>
#include <utility>

namespace pd {

namespace {

QImage decodeJpeg(const QByteArray &jpeg)
{
    if (jpeg.isEmpty()) {
        return {};
    }
    QImage image = QImage::fromData(jpeg, "jpeg");
    if (image.isNull()) {
        image = QImage::fromData(jpeg, "jpg");
    }
    return image;
}

} // namespace

class ViewerVideoWorker final : public QObject {
    Q_OBJECT

public:
    void start()
    {
        QMutexLocker locker(&mutex_);
        accepting_ = true;
        pending_.reset();
    }

    void cancel()
    {
        QMutexLocker locker(&mutex_);
        accepting_ = false;
        pending_.reset();
    }

    void resetDecoder()
    {
#ifdef PD_HAS_H264
        h264Decoder_.close();
#endif
        activeCodec_ = VideoCodec::Jpeg;
        haveCodec_ = false;
        waitingForKeyframe_ = true;
        h264Broken_ = false;
        lastH264Error_.clear();
    }

    void shutdown()
    {
#ifdef PD_HAS_H264
        h264Decoder_.close();
#endif
    }

    void submit(
        quint64 generation,
        quint32 sequence,
        QByteArray data,
        bool keyframe,
        VideoCodec codec)
    {
        if (data.isEmpty()) {
            return;
        }
        bool schedule = false;
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_) {
                return;
            }
            if (pending_.has_value()
                && pending_->keyframe
                && !keyframe) {
                return;
            }
            if (pending_.has_value()
                && pending_->codec == VideoCodec::H264
                && codec == VideoCodec::H264
                && !pending_->keyframe
                && !keyframe) {
                return;
            }
            pending_ = Pending{
                generation,
                sequence,
                std::move(data),
                keyframe,
                codec,
            };
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (schedule) {
            QMetaObject::invokeMethod(
                this,
                &ViewerVideoWorker::decodePending,
                Qt::QueuedConnection);
        }
    }

signals:
    void frameDecoded(
        quint64 generation,
        quint32 sequence,
        const QImage &image,
        bool keyframe,
        const QString &threadName);
    void decodeFailed(
        quint64 generation,
        quint32 sequence,
        const QString &message);

private:
    struct Pending {
        quint64 generation = 0;
        quint32 sequence = 0;
        QByteArray data;
        bool keyframe = false;
        VideoCodec codec = VideoCodec::Jpeg;
    };

    void selectCodec(VideoCodec codec)
    {
        if (haveCodec_ && activeCodec_ == codec) {
            return;
        }
#ifdef PD_HAS_H264
        h264Decoder_.close();
#endif
        activeCodec_ = codec;
        haveCodec_ = true;
        waitingForKeyframe_ = true;
        if (codec == VideoCodec::H264) {
            h264Broken_ = false;
            lastH264Error_.clear();
        }
    }

#ifdef PD_HAS_H264
    void decodeH264(
        const Pending &pending,
        const QString &threadName)
    {
        const bool keyframe =
            pending.keyframe || isH264Keyframe(pending.data);
        if (h264Broken_ || (waitingForKeyframe_ && !keyframe)) {
            return;
        }
        if (!h264Decoder_.isOpen()) {
            QString error;
            if (!h264Decoder_.open(&error)) {
                h264Broken_ = true;
                waitingForKeyframe_ = true;
                if (error != lastH264Error_) {
                    lastH264Error_ = error;
                    emit decodeFailed(
                        pending.generation,
                        pending.sequence,
                        error.isEmpty()
                            ? QStringLiteral("无法启动 H.264 解码器")
                            : error);
                }
                return;
            }
        }

        QImage image;
        bool decodedKeyframe = false;
        QString error;
        if (!h264Decoder_.decode(
                pending.data,
                pending.sequence,
                30,
                &image,
                &decodedKeyframe,
                &error)) {
            h264Decoder_.reset();
            waitingForKeyframe_ = true;
            const QString message = error.isEmpty()
                ? QStringLiteral("H.264 视频帧解码失败")
                : error;
            if (message != lastH264Error_) {
                lastH264Error_ = message;
                emit decodeFailed(
                    pending.generation,
                    pending.sequence,
                    message);
            }
            return;
        }
        if (keyframe) {
            waitingForKeyframe_ = false;
        }
        lastH264Error_.clear();
        if (image.isNull()) {
            return;
        }
        emit frameDecoded(
            pending.generation,
            pending.sequence,
            image,
            pending.keyframe || decodedKeyframe,
            threadName);
    }
#endif

    void decodePending()
    {
        const QString threadName = QThread::currentThread()->objectName();
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

            selectCodec(pending.codec);
            if (pending.codec == VideoCodec::H264) {
#ifdef PD_HAS_H264
                decodeH264(pending, threadName);
#else
                emit decodeFailed(
                    pending.generation,
                    pending.sequence,
                    QStringLiteral("当前构建不支持 H.264 解码"));
#endif
                continue;
            }

            const QImage image = decodeJpeg(pending.data);
            if (image.isNull()) {
                emit decodeFailed(
                    pending.generation,
                    pending.sequence,
                    QStringLiteral("收到无法解码的视频帧 #%1")
                        .arg(pending.sequence));
                continue;
            }
            emit frameDecoded(
                pending.generation,
                pending.sequence,
                image,
                pending.keyframe,
                threadName);
        }
    }

    QMutex mutex_;
    std::optional<Pending> pending_;
    bool accepting_ = false;
    bool scheduled_ = false;
    VideoCodec activeCodec_ = VideoCodec::Jpeg;
    bool haveCodec_ = false;
    bool waitingForKeyframe_ = true;
    bool h264Broken_ = false;
    QString lastH264Error_;
#ifdef PD_HAS_H264
    H264Decoder h264Decoder_;
#endif
};

ViewerVideoPipeline::ViewerVideoPipeline(QObject *parent)
    : QObject(parent)
{
    worker_ = new ViewerVideoWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskViewerVideo"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &ViewerVideoWorker::frameDecoded,
        this,
        &ViewerVideoPipeline::onWorkerFrame);
    connect(
        worker_,
        &ViewerVideoWorker::decodeFailed,
        this,
        &ViewerVideoPipeline::onWorkerFailed);
    thread_.start();
}

ViewerVideoPipeline::~ViewerVideoPipeline()
{
    stop();
    if (worker_ && thread_.isRunning()) {
        ViewerVideoWorker *worker = worker_;
        QMetaObject::invokeMethod(
            worker,
            [worker]() { worker->shutdown(); },
            Qt::BlockingQueuedConnection);
    }
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void ViewerVideoPipeline::start()
{
    running_ = true;
    ++generation_;
    {
        QMutexLocker locker(&threadNameMutex_);
        decoderThreadName_.clear();
    }
    if (worker_) {
        worker_->start();
        ViewerVideoWorker *worker = worker_;
        QMetaObject::invokeMethod(
            worker,
            [worker]() { worker->resetDecoder(); },
            Qt::QueuedConnection);
    }
}

void ViewerVideoPipeline::stop()
{
    running_ = false;
    ++generation_;
    if (worker_) {
        worker_->cancel();
        ViewerVideoWorker *worker = worker_;
        QMetaObject::invokeMethod(
            worker,
            [worker]() { worker->shutdown(); },
            Qt::QueuedConnection);
    }
}

void ViewerVideoPipeline::submit(
    const QByteArray &data,
    quint32 sequence,
    bool keyframe,
    VideoCodec codec)
{
    if (!running_ || !worker_ || data.isEmpty()) {
        return;
    }
    worker_->submit(generation_, sequence, data, keyframe, codec);
}

QString ViewerVideoPipeline::decoderThreadName() const
{
    QMutexLocker locker(&threadNameMutex_);
    return decoderThreadName_;
}

void ViewerVideoPipeline::onWorkerFrame(
    quint64 generation,
    quint32 sequence,
    const QImage &image,
    bool keyframe,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    {
        QMutexLocker locker(&threadNameMutex_);
        decoderThreadName_ = threadName;
    }
    emit frameDecoded(image, sequence, keyframe);
}

void ViewerVideoPipeline::onWorkerFailed(
    quint64 generation,
    quint32,
    const QString &message)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit decodeFailed(message);
}

} // namespace pd

#include "viewer_video_pipeline.moc"
