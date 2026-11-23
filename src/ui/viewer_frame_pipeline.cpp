#include "ui/viewer_frame_pipeline.h"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

namespace pd::ui {

namespace {

QImage scaleForViewport(
    const QImage &source,
    const QSize &logicalSize,
    qreal devicePixelRatio)
{
    if (source.isNull() || logicalSize.isEmpty()) {
        return {};
    }
    const qreal ratio =
        std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0
        ? devicePixelRatio
        : 1.0;
    const QSize physicalSize(
        std::max(1, qRound(logicalSize.width() * ratio)),
        std::max(1, qRound(logicalSize.height() * ratio)));
    const QSize target =
        source.size().scaled(physicalSize, Qt::KeepAspectRatio);
    if (target.isEmpty()) {
        return {};
    }
    if (target == source.size()) {
        return source;
    }
    return source.scaled(
        target,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);
}

} // namespace

class ViewerFrameWorker final : public QObject {
    Q_OBJECT

public:
    void start()
    {
        const std::scoped_lock locker(mutex_);
        accepting_ = true;
        pending_.reset();
    }

    void cancel()
    {
        const std::scoped_lock locker(mutex_);
        accepting_ = false;
        pending_.reset();
    }

    void discard()
    {
        const std::scoped_lock locker(mutex_);
        pending_.reset();
    }

    void submit(
        quint64 generation,
        const QImage &source,
        const QSize &logicalSize,
        qreal devicePixelRatio)
    {
        if (source.isNull() || logicalSize.isEmpty()) {
            return;
        }
        bool schedule = false;
        {
            const std::scoped_lock locker(mutex_);
            if (!accepting_) {
                return;
            }
            pending_ = Pending{
                generation,
                source,
                logicalSize,
                devicePixelRatio,
            };
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (schedule) {
            QMetaObject::invokeMethod(
                this,
                &ViewerFrameWorker::presentPending,
                Qt::QueuedConnection);
        }
    }

signals:
    void framePresented(
        quint64 generation,
        const QImage &image,
        const QString &threadName);

private:
    struct Pending {
        quint64 generation = 0;
        QImage source;
        QSize logicalSize;
        qreal devicePixelRatio = 1.0;
    };

    void presentPending()
    {
        const QString threadName = QThread::currentThread()->objectName();
        for (;;) {
            Pending pending;
            {
                const std::scoped_lock locker(mutex_);
                if (!pending_.has_value()) {
                    scheduled_ = false;
                    return;
                }
                pending = std::move(*pending_);
                pending_.reset();
            }
            const QImage presented = scaleForViewport(
                pending.source,
                pending.logicalSize,
                pending.devicePixelRatio);
            if (!presented.isNull()) {
                emit framePresented(
                    pending.generation,
                    presented,
                    threadName);
            }
        }
    }

    std::mutex mutex_;
    std::optional<Pending> pending_;
    bool accepting_ = false;
    bool scheduled_ = false;
};

ViewerFramePipeline::ViewerFramePipeline(QObject *parent)
    : QObject(parent)
{
    worker_ = new ViewerFrameWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskViewerPresent"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &ViewerFrameWorker::framePresented,
        this,
        &ViewerFramePipeline::onWorkerFrame);
    thread_.start();
    worker_->start();
}

ViewerFramePipeline::~ViewerFramePipeline()
{
    if (worker_) {
        worker_->cancel();
    }
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void ViewerFramePipeline::setFrame(const QImage &frame)
{
    if (frame.isNull()) {
        clear();
        return;
    }
    frame_ = frame;
    submit();
}

void ViewerFramePipeline::setViewport(
    const QSize &logicalSize,
    qreal devicePixelRatio)
{
    const qreal ratio =
        std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0
        ? devicePixelRatio
        : 1.0;
    if (viewport_ == logicalSize
        && qFuzzyCompare(devicePixelRatio_, ratio)) {
        return;
    }
    viewport_ = logicalSize;
    devicePixelRatio_ = ratio;
    if (!frame_.isNull()) {
        submit();
    }
}

void ViewerFramePipeline::clear()
{
    ++generation_;
    frame_ = QImage();
    if (worker_) {
        worker_->discard();
    }
}

QString ViewerFramePipeline::presentationThreadName() const
{
    const std::scoped_lock locker(threadNameMutex_);
    return presentationThreadName_;
}

void ViewerFramePipeline::submit()
{
    if (!worker_ || frame_.isNull() || viewport_.isEmpty()) {
        return;
    }
    worker_->submit(
        generation_,
        frame_,
        viewport_,
        devicePixelRatio_);
}

void ViewerFramePipeline::onWorkerFrame(
    quint64 generation,
    const QImage &image,
    const QString &threadName)
{
    if (generation != generation_ || image.isNull()) {
        return;
    }
    {
        const std::scoped_lock locker(threadNameMutex_);
        presentationThreadName_ = threadName;
    }
    emit framePresented(image, threadName);
}

} // namespace pd::ui

#include "viewer_frame_pipeline.moc"
