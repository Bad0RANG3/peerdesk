#include "session/video_pipeline.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImageWriter>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace pd {

namespace {

QByteArray fingerprint(const QImage &image, int monitor)
{
    const QImage probe = image
                             .scaled(
                                 48,
                                 27,
                                 Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    QByteArray result("PD1", 3);
    result.append(static_cast<char>((monitor >> 24) & 0xFF));
    result.append(static_cast<char>((monitor >> 16) & 0xFF));
    result.append(static_cast<char>((monitor >> 8) & 0xFF));
    result.append(static_cast<char>(monitor & 0xFF));
    result.append(
        reinterpret_cast<const char *>(probe.constBits()),
        probe.sizeInBytes());
    return result;
}

bool encodeJpeg(
    const QImage &image,
    int quality,
    QByteArray *encoded,
    QString *error)
{
    if (image.isNull() || encoded == nullptr) {
        if (error) {
            *error = QStringLiteral("empty image");
        }
        return false;
    }
    QByteArray output;
    QBuffer buffer(&output);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("无法打开编码缓冲区");
        }
        return false;
    }
    QImageWriter writer(&buffer, QByteArrayLiteral("jpeg"));
    writer.setQuality(std::clamp(quality, 1, 100));
    writer.setOptimizedWrite(false);
    writer.setProgressiveScanWrite(false);
    if (!writer.write(image)) {
        if (error) {
            *error = writer.errorString();
        }
        return false;
    }
    buffer.close();
    *encoded = output;
    if (error) {
        error->clear();
    }
    return true;
}

} // namespace

class VideoCaptureWorker final : public QObject {
    Q_OBJECT

public:
    void setSource(std::shared_ptr<IScreenSource> source)
    {
        stop();
        source_ = std::move(source);
        lastFingerprint_.clear();
        lastFingerprintMonitor_ = -1;
    }

    void start(
        quint64 generation,
        const VideoSettings &settings,
        int quality,
        double rateLimitMbps)
    {
        stop();
        generation_ = generation;
        settings_ = settings;
        settings_.clamp();
        pacer_ = FramePacer(quality);
        pacer_.setQualityLimit(adaptiveQualityLimit_);
        rateLimitMbps_ = std::max(0.0, rateLimitMbps);
        updateBudget();
        currentQuality_ = pacer_.quality();
        forceKeyframe_ = true;
        lastError_.clear();

        if (!source_) {
            emit failed(generation_, QStringLiteral("没有可用的抓屏源"));
            return;
        }
        if (source_->monitors().isEmpty()) {
            emit failed(generation_, QStringLiteral("没有检测到显示器"));
            source_.reset();
            return;
        }

        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setSingleShot(true);
            timer_->setTimerType(Qt::PreciseTimer);
            connect(
                timer_,
                &QTimer::timeout,
                this,
                &VideoCaptureWorker::tick);
        }
        running_ = true;
        emit started(generation_, source_->backend());
        timer_->start(0);
    }

    void stop()
    {
        running_ = false;
        if (timer_) {
            timer_->stop();
        }
        if (source_) {
            source_->close();
        }
        lastFingerprint_.clear();
        lastFingerprintMonitor_ = -1;
    }

    void configure(
        const VideoSettings &settings,
        int quality,
        bool forceKeyframe)
    {
        settings_ = settings;
        settings_.clamp();
        pacer_.setQuality(quality);
        currentQuality_ = pacer_.quality();
        updateBudget();
        if (forceKeyframe) {
            forceKeyframe_ = true;
            lastFingerprint_.clear();
            lastFingerprintMonitor_ = -1;
        }
    }

    void setRateLimit(double mbps)
    {
        rateLimitMbps_ = std::max(0.0, mbps);
        updateBudget();
    }

    void setAdaptiveLimits(int qualityLimit, int fpsLimit)
    {
        adaptiveQualityLimit_ = std::clamp(qualityLimit, 1, 100);
        adaptiveFpsLimit_ = std::clamp(fpsLimit, 1, 120);
        pacer_.setQualityLimit(adaptiveQualityLimit_);
        currentQuality_ = pacer_.quality();
    }

    void requestKeyframe()
    {
        forceKeyframe_ = true;
    }

    [[nodiscard]] QList<MonitorInfo> monitors() const
    {
        return source_ ? source_->monitors() : QList<MonitorInfo>{};
    }

    [[nodiscard]] QString backend() const
    {
        return source_ ? source_->backend() : QString();
    }

signals:
    void frameReady(
        quint64 generation,
        const pd::EncodedVideoFrame &frame);
    void failed(quint64 generation, const QString &message);
    void started(quint64 generation, const QString &backend);

private:
    void updateBudget()
    {
        if (rateLimitMbps_ <= 0.0) {
            pacer_.setBudgetKb(0.0);
            return;
        }
        const double perFrameBytes =
            rateLimitMbps_ * 125000.0
            / static_cast<double>(std::max(1, settings_.fps));
        pacer_.setBudgetKb(perFrameBytes / 1024.0 * 0.75);
    }

    [[nodiscard]] int effectiveFps() const
    {
        double fps = std::clamp(
            std::min(settings_.fps, adaptiveFpsLimit_),
            1,
            120);
        if (rateLimitMbps_ > 0.0 && pacer_.lastSize() > 0) {
            const double needed =
                static_cast<double>(pacer_.lastSize()) * 8.0 * fps
                / 1'000'000.0;
            if (needed > rateLimitMbps_) {
                fps = std::max(
                    1.0,
                    std::floor(fps * rateLimitMbps_ / needed));
            }
        }
        return std::clamp(
            static_cast<int>(std::lround(fps)),
            1,
            120);
    }

    void scheduleNextFrame(qint64 elapsedMs)
    {
        if (!running_ || !timer_) {
            return;
        }
        const qint64 targetMs =
            std::max<qint64>(1, 1000 / effectiveFps());
        const qint64 delay =
            std::max<qint64>(0, targetMs - elapsedMs);
        timer_->start(
            static_cast<int>(std::min<qint64>(delay, 1000)));
    }

    void reportError(const QString &error)
    {
        if (error == lastError_) {
            return;
        }
        lastError_ = error;
        emit failed(generation_, error);
    }

    void tick()
    {
        if (!running_ || !source_) {
            return;
        }

        QElapsedTimer clock;
        clock.start();
        CaptureRequest request;
        request.monitor = settings_.monitor;
        request.maxWidth = settings_.maxWidth;
        request.pixelFormat = settings_.pixelFormat;

        CaptureResult captured = source_->capture(request);
        if (!captured.ok()) {
            reportError(
                captured.error.isEmpty()
                    ? QStringLiteral("抓屏失败")
                    : captured.error);
            scheduleNextFrame(clock.elapsed());
            return;
        }

        QImage image = captured.image;
        if (settings_.maxWidth > 0 && image.width() > settings_.maxWidth) {
            image = image.scaled(
                settings_.maxWidth,
                std::max(
                    2,
                    static_cast<int>(std::lround(
                        static_cast<double>(image.height())
                        * settings_.maxWidth / image.width()))),
                Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation);
        }

        const int monitor =
            std::clamp(settings_.monitor, 0, 65535);
        const QByteArray currentFingerprint =
            fingerprint(image, monitor);
        const bool unchanged = settings_.skipStatic
            && !forceKeyframe_
            && currentFingerprint == lastFingerprint_
            && monitor == lastFingerprintMonitor_;
        lastFingerprint_ = currentFingerprint;
        lastFingerprintMonitor_ = monitor;
        if (unchanged) {
            scheduleNextFrame(clock.elapsed());
            return;
        }

        const qint64 captureMs = clock.elapsed();
        QElapsedTimer encodeClock;
        encodeClock.start();
        QByteArray jpeg;
        QString encodeError;
        if (!encodeJpeg(
                image,
                currentQuality_,
                &jpeg,
                &encodeError)) {
            reportError(
                QStringLiteral("JPEG 编码失败：%1")
                    .arg(encodeError));
            scheduleNextFrame(clock.elapsed());
            return;
        }
        const qint64 encodeMs = encodeClock.elapsed();
        lastError_.clear();

        const bool keyframe = forceKeyframe_;
        const int encodedQuality = currentQuality_;
        pacer_.observe(
            jpeg.size(),
            static_cast<double>(clock.elapsed()));
        currentQuality_ = pacer_.quality();
        forceKeyframe_ = false;

        emit frameReady(
            generation_,
            EncodedVideoFrame{
                jpeg,
                keyframe,
                encodedQuality,
                effectiveFps(),
                captureMs,
                encodeMs,
            });
        scheduleNextFrame(clock.elapsed());
    }

    quint64 generation_ = 0;
    std::shared_ptr<IScreenSource> source_;
    VideoSettings settings_;
    FramePacer pacer_;
    QTimer *timer_ = nullptr;
    bool running_ = false;
    bool forceKeyframe_ = true;
    int currentQuality_ = 65;
    double rateLimitMbps_ = 0.0;
    int adaptiveQualityLimit_ = 92;
    int adaptiveFpsLimit_ = 120;
    QByteArray lastFingerprint_;
    int lastFingerprintMonitor_ = -1;
    QString lastError_;
};

VideoPipeline::VideoPipeline(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<pd::EncodedVideoFrame>("pd::EncodedVideoFrame");

    worker_ = new VideoCaptureWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskVideo"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &VideoCaptureWorker::frameReady,
        this,
        &VideoPipeline::onWorkerFrame);
    connect(
        worker_,
        &VideoCaptureWorker::failed,
        this,
        &VideoPipeline::onWorkerFailed);
    connect(
        worker_,
        &VideoCaptureWorker::started,
        this,
        &VideoPipeline::onWorkerStarted);
    thread_.start();
}

VideoPipeline::~VideoPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void VideoPipeline::setSource(std::shared_ptr<IScreenSource> source)
{
    if (running_) {
        stop();
    }
    hasSource_ = static_cast<bool>(source);
    QList<MonitorInfo> found;
    QString backend;
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [this, source, &found, &backend]() {
                worker_->setSource(source);
                found = worker_->monitors();
                backend = worker_->backend();
            },
            Qt::BlockingQueuedConnection);
    }
    monitorsCache_ = found;
    backendCache_ = backend;
    hasSource_ = hasSource_ && static_cast<bool>(source);
}

bool VideoPipeline::start(
    const VideoSettings &settings,
    int quality,
    QString *error)
{
    if (!worker_ || !hasSource_) {
        if (error) {
            *error = QStringLiteral("没有可用的抓屏源");
        }
        return false;
    }
    if (monitorsCache_.isEmpty()) {
        if (error) {
            *error = QStringLiteral("没有检测到显示器");
        }
        return false;
    }
    stop();

    running_ = true;
    const quint64 generation = ++generation_;
    const double rateLimitMbps = rateLimitMbps_;
    QMetaObject::invokeMethod(
        worker_,
        [this,
         generation,
         settings,
         quality,
         rateLimitMbps,
         adaptiveQualityLimit = adaptiveQualityLimit_,
         adaptiveFpsLimit = adaptiveFpsLimit_]() {
            worker_->setAdaptiveLimits(
                adaptiveQualityLimit,
                adaptiveFpsLimit);
            worker_->start(
                generation,
                settings,
                quality,
                rateLimitMbps);
        },
        Qt::QueuedConnection);
    if (error) {
        error->clear();
    }
    return true;
}

void VideoPipeline::stop()
{
    const bool wasRunning = running_;
    running_ = false;
    ++generation_;
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [this]() { worker_->stop(); },
            Qt::BlockingQueuedConnection);
    }
    Q_UNUSED(wasRunning)
}

void VideoPipeline::update(
    const VideoSettings &settings,
    int quality)
{
    if (!running_ || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [this, settings, quality]() {
            worker_->configure(settings, quality, true);
        },
        Qt::QueuedConnection);
}

void VideoPipeline::setRateLimit(double mbps)
{
    rateLimitMbps_ = std::max(0.0, mbps);
    if (!worker_) {
        return;
    }
    const double limit = rateLimitMbps_;
    QMetaObject::invokeMethod(
        worker_,
        [this, limit]() { worker_->setRateLimit(limit); },
        Qt::QueuedConnection);
}

void VideoPipeline::setAdaptiveLimits(int qualityLimit, int fpsLimit)
{
    adaptiveQualityLimit_ = std::clamp(qualityLimit, 1, 100);
    adaptiveFpsLimit_ = std::clamp(fpsLimit, 1, 120);
    if (!running_ || !worker_) {
        return;
    }
    const int quality = adaptiveQualityLimit_;
    const int fps = adaptiveFpsLimit_;
    QMetaObject::invokeMethod(
        worker_,
        [this, quality, fps]() {
            worker_->setAdaptiveLimits(quality, fps);
        },
        Qt::QueuedConnection);
}

void VideoPipeline::requestKeyframe()
{
    if (!running_ || !worker_) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [this]() { worker_->requestKeyframe(); },
        Qt::QueuedConnection);
}

QList<MonitorInfo> VideoPipeline::monitors() const
{
    if (!worker_ || !thread_.isRunning()) {
        return monitorsCache_;
    }
    QList<MonitorInfo> found;
    QString backend;
    const bool invoked = QMetaObject::invokeMethod(
        worker_,
        [this, &found, &backend]() {
            found = worker_->monitors();
            backend = worker_->backend();
        },
        Qt::BlockingQueuedConnection);
    if (invoked) {
        monitorsCache_ = found;
        backendCache_ = backend;
    }
    return monitorsCache_;
}

QString VideoPipeline::backend() const
{
    return backendCache_;
}

void VideoPipeline::onWorkerFrame(
    quint64 generation,
    const pd::EncodedVideoFrame &frame)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit frameReady(frame);
}

void VideoPipeline::onWorkerFailed(
    quint64 generation,
    const QString &message)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit failed(message);
}

void VideoPipeline::onWorkerStarted(
    quint64 generation,
    const QString &backend)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit started(backend);
}

} // namespace pd

#include "video_pipeline.moc"
