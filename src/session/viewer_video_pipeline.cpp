#include "session/viewer_video_pipeline.h"

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

    void submit(
        quint64 generation,
        quint32 sequence,
        QByteArray jpeg,
        bool keyframe)
    {
        if (jpeg.isEmpty()) {
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
            pending_ = Pending{
                generation,
                sequence,
                std::move(jpeg),
                keyframe,
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
        QByteArray jpeg;
        bool keyframe = false;
    };

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

            const QImage image = decodeJpeg(pending.jpeg);
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
    }
}

void ViewerVideoPipeline::stop()
{
    running_ = false;
    ++generation_;
    if (worker_) {
        worker_->cancel();
    }
}

void ViewerVideoPipeline::submit(
    const QByteArray &jpeg,
    quint32 sequence,
    bool keyframe)
{
    if (!running_ || !worker_ || jpeg.isEmpty()) {
        return;
    }
    worker_->submit(generation_, sequence, jpeg, keyframe);
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
