#include "session/host_session.h"

#include "core/protocol.h"
#include "transport/peer_link.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QPointF>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifndef PEERDESK_VERSION
#define PEERDESK_VERSION "0.0.0"
#endif

namespace pd {

namespace {

constexpr int kClipboardLimit = 256 * 1024;

qint64 jsonInteger(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : 0;
    }
    return 0;
}

QString jsonString(
    const QJsonObject &object,
    const char *key,
    const QString &fallback = {})
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

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

HostSession::HostSession(PeerLink *link, QObject *parent)
    : QObject(parent)
    , link_(link)
    , receiver_(defaultReceiveDirectory())
    , fileSender_(new FileSender(this))
    , captureTimer_(new QTimer(this))
    , audioTimer_(new QTimer(this))
    , statsTimer_(new QTimer(this))
    , clipboardTimer_(new QTimer(this))
{
    captureTimer_->setSingleShot(true);
    captureTimer_->setTimerType(Qt::PreciseTimer);
    connect(
        captureTimer_,
        &QTimer::timeout,
        this,
        &HostSession::captureOnce);

    audioTimer_->setInterval(kAudioFrameMs);
    audioTimer_->setTimerType(Qt::PreciseTimer);
    connect(audioTimer_, &QTimer::timeout, this, &HostSession::pollAudio);

    statsTimer_->setInterval(1000);
    statsTimer_->setTimerType(Qt::CoarseTimer);
    connect(statsTimer_, &QTimer::timeout, this, &HostSession::pushStats);

    clipboardTimer_->setInterval(400);
    clipboardTimer_->setTimerType(Qt::CoarseTimer);
    connect(
        clipboardTimer_,
        &QTimer::timeout,
        this,
        &HostSession::pollClipboard);

    connect(
        fileSender_,
        &FileSender::started,
        this,
        &HostSession::outgoingFileStarted);
    connect(
        fileSender_,
        &FileSender::progress,
        this,
        &HostSession::outgoingFileProgress);
    connect(
        fileSender_,
        &FileSender::completed,
        this,
        &HostSession::outgoingFileFinished);
    connect(
        fileSender_,
        &FileSender::failed,
        this,
        &HostSession::outgoingFileFailed);
    connect(
        fileSender_,
        &FileSender::cancelled,
        this,
        &HostSession::outgoingFileCancelled);
    connect(
        fileSender_,
        &FileSender::interrupted,
        this,
        &HostSession::outgoingFileInterrupted);

    connectLink();
}

HostSession::~HostSession()
{
    blockSignals(true);
    stop();
    receiver_.abortAll();
}

void HostSession::connectLink()
{
    if (!link_) {
        return;
    }
    connect(
        link_,
        &PeerLink::stateChanged,
        this,
        &HostSession::onLinkStateChanged);
    connect(
        link_,
        &PeerLink::controlMessageReceived,
        this,
        &HostSession::dispatch);
}

void HostSession::setScreenSource(std::shared_ptr<IScreenSource> source)
{
    screenSource_ = std::move(source);
    monitors_.clear();
    currentMonitor_ = 0;
    lastFingerprint_.clear();
    lastFingerprintMonitor_ = -1;
}

void HostSession::setAudioSource(std::shared_ptr<IAudioSource> source)
{
    if (audioRunning_) {
        stopAudio();
    }
    audioSource_ = std::move(source);
    audioError_.clear();
}

void HostSession::setInputSink(std::shared_ptr<IInputSink> sink)
{
    inputSink_ = std::move(sink);
}

void HostSession::setClipboard(std::shared_ptr<IClipboard> clipboard)
{
    clipboard_ = std::move(clipboard);
    lastClipboard_.clear();
}

bool HostSession::start()
{
    wantedRunning_ = true;
    if (link_ && link_->established()) {
        startStreaming();
    }
    return true;
}

void HostSession::suspend()
{
    stopStreaming();
}

void HostSession::stop()
{
    wantedRunning_ = false;
    stopStreaming();
    receiver_.abortAll();
    fileSender_->abort();
    fileSender_->discardResume();
}

void HostSession::onLinkStateChanged(const QString &state)
{
    const LinkStats linkStats = link_ ? link_->stats() : LinkStats{};
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("link")},
        {QStringLiteral("state"), state},
        {QStringLiteral("path"), linkStats.path},
        {QStringLiteral("relay"), linkStats.usingRelay},
    });

    if ((state == QStringLiteral("direct")
         || state == QStringLiteral("relay"))
        && wantedRunning_ && !running_) {
        startStreaming();
    } else if (state == QStringLiteral("closed")) {
        stopStreaming();
    }
}

void HostSession::startStreaming()
{
    if (running_ || !link_ || !link_->established()) {
        return;
    }
    if (!screenSource_) {
        emit logMessage(QStringLiteral("没有可用的抓屏源，无法开始会话"));
        return;
    }

    refreshMonitors();
    if (monitors_.isEmpty()) {
        emit logMessage(QStringLiteral("没有检测到显示器，无法开始会话"));
        return;
    }

    running_ = true;
    forceKeyframe_ = true;
    pacer_.setQuality(settings_.quality);
    sentFrames_ = 0;
    statsLastFrames_ = 0;
    lastStatsFps_ = 0.0;
    statsClock_.restart();
    lastFingerprint_.clear();
    lastFingerprintMonitor_ = -1;

    if (clipboard_) {
        lastClipboard_ = clipboard_->text().left(kClipboardLimit);
        clipboardTimer_->start();
    }

    startAudio();
    sendInfo();
    statsTimer_->start();
    captureTimer_->start(0);
    resumeOutgoingFile();
    emit logMessage(QStringLiteral("会话已开始：%1")
                        .arg(screenSource_->backend()));
}

void HostSession::stopStreaming()
{
    const bool wasRunning = running_;
    running_ = false;
    captureTimer_->stop();
    statsTimer_->stop();
    clipboardTimer_->stop();
    stopAudio();
    if (inputSink_) {
        inputSink_->releaseAll();
    }
    if (fileSender_->active()) {
        fileSender_->interrupt(QStringLiteral("interrupted"));
    }
    audioCarry_.clear();
    if (screenSource_) {
        screenSource_->close();
    }
    if (wasRunning) {
        emit logMessage(QStringLiteral("会话已停止"));
    }
}

void HostSession::resumeOutgoingFile()
{
    const FileSender::ResumeOffer offer =
        fileSender_->interruptedOffer();
    if (!offer.valid() || fileSender_->active()
        || !link_ || !link_->established()) {
        return;
    }
    QString error;
    if (!fileSender_->start(
            offer.path,
            [this](const QJsonObject &message) { sendControl(message); },
            &error,
            offer)) {
        emit logMessage(
            QStringLiteral("无法续传 %1：%2")
                .arg(offer.name)
                .arg(error));
    }
}

void HostSession::setSettings(const VideoSettings &settings)
{
    settings_ = settings;
    settings_.clamp();
    pacer_.setQuality(settings_.quality);
    if (running_) {
        refreshMonitors();
        forceKeyframe_ = true;
        lastFingerprint_.clear();
        lastFingerprintMonitor_ = -1;
    }
    if (running_ && settings_.audio) {
        startAudio();
    } else if (!settings_.audio) {
        stopAudio();
    }
}

void HostSession::setAllowInput(bool allow)
{
    allowInput_ = allow;
}

void HostSession::setReceiveDirectory(const QString &directory)
{
    receiver_.setDirectory(directory);
}

QString HostSession::receiveDirectory() const
{
    return receiver_.directory();
}

void HostSession::setRateLimit(double mbps)
{
    rateLimitMbps_ = std::max(0.0, mbps);
    if (rateLimitMbps_ <= 0.0) {
        pacer_.setBudgetKb(0.0);
        emit eventReceived(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("notice")},
            {QStringLiteral("text"), QStringLiteral("带宽限制已关闭")},
        });
        return;
    }

    const double perFrameBytes =
        rateLimitMbps_ * 125000.0 / std::max(1, settings_.fps);
    pacer_.setBudgetKb(perFrameBytes / 1024.0 * 0.75);
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("notice")},
        {QStringLiteral("text"),
         QStringLiteral("中继上限 %1 Mbps，已按此节流")
             .arg(rateLimitMbps_, 0, 'g', 3)},
    });
}

QJsonObject HostSession::info()
{
    refreshMonitors();
    QJsonArray monitorArray;
    for (const MonitorInfo &monitor : monitors_) {
        monitorArray.append(monitor.toJson());
    }
    return {
        {QStringLiteral("t"), QStringLiteral("info")},
        {QStringLiteral("name"), QSysInfo::machineHostName()},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("monitors"), monitorArray},
        {QStringLiteral("settings"), settings_.toJson()},
        {QStringLiteral("input"), allowInput_},
        {QStringLiteral("audio"), audioState()},
        {QStringLiteral("version"), QStringLiteral(PEERDESK_VERSION)},
    };
}

QJsonObject HostSession::audioState() const
{
    return {
        {QStringLiteral("t"), QStringLiteral("audio")},
        {QStringLiteral("on"), audioRunning_},
        {QStringLiteral("enabled"), settings_.audio},
        {QStringLiteral("codec"), QStringLiteral("pcmu")},
        {QStringLiteral("rate"), kAudioRate},
        {QStringLiteral("frame_ms"), kAudioFrameMs},
        {QStringLiteral("source"),
         audioSource_ ? audioSource_->label() : QString()},
        {QStringLiteral("error"), audioError_},
    };
}

void HostSession::refreshMonitors()
{
    if (!screenSource_) {
        return;
    }
    monitors_ = screenSource_->monitors();
    if (monitors_.isEmpty()) {
        currentMonitor_ = 0;
        return;
    }
    settings_.monitor = std::clamp(
        settings_.monitor,
        0,
        static_cast<int>(monitors_.size()) - 1);
    currentMonitor_ = settings_.monitor;
}

void HostSession::sendInfo()
{
    if (!link_ || !link_->established()) {
        return;
    }
    const QJsonObject message = info();
    sendControl(message);
    emit eventReceived(message);
}

void HostSession::sendControl(const QJsonObject &message)
{
    if (link_ && link_->established()) {
        link_->sendControl(message);
    }
}

void HostSession::startAudio()
{
    if (audioRunning_ || !settings_.audio || !running_) {
        return;
    }
    if (!audioSource_) {
        audioError_ = QStringLiteral("未配置音频源");
        broadcastAudioState();
        return;
    }
    QString error;
    if (!audioSource_->start(&error)) {
        audioError_ = error.isEmpty()
            ? QStringLiteral("音频源启动失败")
            : error;
        broadcastAudioState();
        return;
    }
    audioError_.clear();
    audioCarry_.clear();
    audioEncoder_.reset();
    audioRunning_ = true;
    audioTimer_->start();
    broadcastAudioState();
}

void HostSession::stopAudio()
{
    const bool wasRunning = audioRunning_;
    audioRunning_ = false;
    audioTimer_->stop();
    audioCarry_.clear();
    if (audioSource_) {
        audioSource_->stop();
    }
    if (wasRunning) {
        broadcastAudioState();
    }
}

void HostSession::broadcastAudioState()
{
    const QJsonObject state = audioState();
    sendControl(state);
    emit audioStateChanged(state);
}

void HostSession::pollAudio()
{
    if (!audioRunning_ || !audioSource_ || !link_
        || !link_->established()) {
        return;
    }
    QString error;
    const QByteArray pcm =
        audioSource_->readFrame(kAudioSamples, 0, &error);
    if (!error.isEmpty()) {
        audioError_ = error;
        audioRunning_ = false;
        audioTimer_->stop();
        audioSource_->stop();
        broadcastAudioState();
        emit logMessage(QStringLiteral("音频采集停止：%1").arg(error));
        return;
    }
    if (pcm.isEmpty()) {
        return;
    }
    if (pcm.size() % 2 != 0) {
        audioError_ = QStringLiteral("音频源返回了非 PCM16 数据");
        return;
    }

    audioCarry_.append(pcm);
    while (audioCarry_.size() >= kAudioPcmBytes) {
        const QByteArray frame = audioCarry_.left(kAudioPcmBytes);
        audioCarry_.remove(0, kAudioPcmBytes);
        const auto packet = audioEncoder_.next(frame);
        if (packet.has_value()) {
            link_->sendAudio(packAudio(*packet));
        }
    }
}

int HostSession::effectiveFps() const
{
    double fps = std::clamp(settings_.fps, 1, 120);
    if (rateLimitMbps_ > 0.0 && pacer_.lastSize() > 0) {
        const double needed =
            static_cast<double>(pacer_.lastSize()) * 8.0 * fps / 1'000'000.0;
        if (needed > rateLimitMbps_) {
            fps = std::max(
                1.0,
                std::floor(fps * rateLimitMbps_ / needed));
        }
    }
    return std::clamp(static_cast<int>(std::lround(fps)), 1, 120);
}

void HostSession::scheduleNextFrame(qint64 elapsedMs)
{
    if (!running_ || !link_ || !link_->established()) {
        return;
    }
    const qint64 targetMs = std::max<qint64>(1, 1000 / effectiveFps());
    const qint64 delay = std::max<qint64>(0, targetMs - elapsedMs);
    captureTimer_->start(static_cast<int>(std::min<qint64>(delay, 1000)));
}

void HostSession::captureOnce()
{
    if (!running_ || !link_ || !link_->established()) {
        return;
    }
    if (!screenSource_) {
        return;
    }

    QElapsedTimer clock;
    clock.start();
    CaptureRequest request;
    request.monitor = settings_.monitor;
    request.maxWidth = settings_.maxWidth;
    request.pixelFormat = settings_.pixelFormat;

    CaptureResult captured = screenSource_->capture(request);
    if (!captured.ok()) {
        const QString error = captured.error.isEmpty()
            ? QStringLiteral("抓屏失败")
            : captured.error;
        if (error != lastCaptureError_) {
            lastCaptureError_ = error;
            emit logMessage(error);
        }
        scheduleNextFrame(clock.elapsed());
        return;
    }
    lastCaptureError_.clear();

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

    const int monitor = std::clamp(
        settings_.monitor,
        0,
        std::max(0, static_cast<int>(monitors_.size()) - 1));
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

    QByteArray jpeg;
    QString encodeError;
    if (!encodeJpeg(image, pacer_.quality(), &jpeg, &encodeError)) {
        if (encodeError != lastCaptureError_) {
            lastCaptureError_ = encodeError;
            emit logMessage(
                QStringLiteral("JPEG 编码失败：%1").arg(encodeError));
        }
        scheduleNextFrame(clock.elapsed());
        return;
    }

    const bool keyframe = forceKeyframe_;
    pacer_.observe(
        jpeg.size(),
        static_cast<double>(clock.elapsed()));
    link_->sendVideo(jpeg, keyframe);
    forceKeyframe_ = false;
    ++sentFrames_;
    scheduleNextFrame(clock.elapsed());
}

void HostSession::pushStats()
{
    if (!running_) {
        return;
    }
    const qint64 elapsedMs = std::max<qint64>(1, statsClock_.restart());
    const qint64 delta = sentFrames_ - statsLastFrames_;
    statsLastFrames_ = sentFrames_;
    lastStatsFps_ =
        static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsedMs);

    const LinkStats linkStats = link_ ? link_->stats() : LinkStats{};
    const double kb =
        static_cast<double>(pacer_.lastSize()) / 1024.0;
    QJsonObject stats{
        {QStringLiteral("t"), QStringLiteral("stats")},
        {QStringLiteral("fps"),
         std::round(lastStatsFps_ * 10.0) / 10.0},
        {QStringLiteral("mbps"),
         std::round(linkStats.mbps * 100.0) / 100.0},
        {QStringLiteral("rtt"),
         std::round(linkStats.rttMs * 10.0) / 10.0},
        {QStringLiteral("mode"), linkStats.mode},
        {QStringLiteral("relay"), linkStats.usingRelay},
        {QStringLiteral("quality"), pacer_.quality()},
        {QStringLiteral("kb"), std::round(kb * 10.0) / 10.0},
        {QStringLiteral("enc_ms"),
         std::round(pacer_.lastMs() * 10.0) / 10.0},
        {QStringLiteral("cap"),
         std::round(rateLimitMbps_ * 10.0) / 10.0},
        {QStringLiteral("sfps"), effectiveFps()},
    };
    sendControl(stats);
    emit statsChanged(stats);
}

void HostSession::pollClipboard()
{
    if (!running_ || !settings_.clipboard || !clipboard_) {
        return;
    }
    const QString text = clipboard_->text().left(kClipboardLimit);
    if (text == lastClipboard_) {
        return;
    }
    lastClipboard_ = text;
    sendControl(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("clip")},
        {QStringLiteral("s"), text},
    });
}

void HostSession::applySettings(const QJsonObject &message)
{
    const QJsonObject values =
        message.value(QStringLiteral("settings")).isObject()
        ? message.value(QStringLiteral("settings")).toObject()
        : message;
    settings_.update(values);
    refreshMonitors();
    pacer_.setQuality(settings_.quality);
    forceKeyframe_ = true;
    lastFingerprint_.clear();
    lastFingerprintMonitor_ = -1;

    if (running_ && settings_.audio) {
        startAudio();
    } else {
        stopAudio();
    }

    const QJsonObject ack{
        {QStringLiteral("t"), QStringLiteral("settings_ack")},
        {QStringLiteral("settings"), settings_.toJson()},
    };
    sendControl(ack);
    emit eventReceived(ack);
}

void HostSession::dispatch(const QJsonObject &message)
{
    const QString kind =
        message.value(QStringLiteral("t")).toString();
    if (kind == QStringLiteral("hello")) {
        sendInfo();
        return;
    }
    if (kind == QStringLiteral("settings")) {
        applySettings(message);
        return;
    }
    if (kind == QStringLiteral("quality")) {
        const QJsonValue value = message.value(QStringLiteral("v"));
        if (value.isDouble()) {
            applySettings(QJsonObject{
                {QStringLiteral("quality"),
                 static_cast<int>(value.toDouble())},
            });
        }
        return;
    }
    if (kind == QStringLiteral("keyframe_req")) {
        forceKeyframe_ = true;
        return;
    }
    if (kind == QStringLiteral("audio")) {
        settings_.audio =
            message.value(QStringLiteral("on")).toBool(true);
        if (settings_.audio) {
            startAudio();
        } else {
            stopAudio();
        }
        broadcastAudioState();
        return;
    }
    if (kind == QStringLiteral("mouse")) {
        if (allowInput_) {
            handleMouse(message);
        }
        return;
    }
    if (kind == QStringLiteral("key")) {
        if (allowInput_ && inputSink_) {
            inputSink_->key(
                jsonString(message, "code"),
                message.value(QStringLiteral("down")).toBool());
        }
        return;
    }
    if (kind == QStringLiteral("text")) {
        if (allowInput_ && inputSink_) {
            inputSink_->typeText(jsonString(message, "s"));
        }
        return;
    }
    if (kind == QStringLiteral("clip")) {
        if (!settings_.clipboard || !clipboard_) {
            return;
        }
        const QString text =
            jsonString(message, "s").left(kClipboardLimit);
        if (text == lastClipboard_) {
            return;
        }
        if (clipboard_->setText(text)) {
            lastClipboard_ = text;
        } else {
            emit logMessage(QStringLiteral("写入本机剪贴板失败"));
        }
        return;
    }
    if (kind == QStringLiteral("cad")) {
        if (allowInput_ && inputSink_) {
            inputSink_->ctrlAltDelete();
        }
        return;
    }
    if (kind == QStringLiteral("file_begin")) {
        handleFileBegin(message);
        return;
    }
    if (kind == QStringLiteral("file_resume")) {
        handleFileResume(message);
        return;
    }
    if (kind == QStringLiteral("file_data")) {
        handleFileData(message);
        return;
    }
    if (kind == QStringLiteral("file_end")) {
        handleFileEnd(message);
        return;
    }
    if (kind == QStringLiteral("file_error")) {
        const QString id = jsonString(message, "id");
        if (fileSender_->active() && id == fileSender_->id()) {
            fileSender_->failFromPeer(jsonString(message, "reason"));
        }
        return;
    }
    if (kind == QStringLiteral("file_cancel")) {
        const QString id = jsonString(message, "id");
        if (!id.isEmpty()) {
            receiver_.abort(id);
            const QString reason = jsonString(message, "reason");
            sendControl(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("file_cancelled")},
                {QStringLiteral("id"), id},
                {QStringLiteral("reason"),
                 reason.isEmpty() ? QStringLiteral("cancelled") : reason},
            });
            emit eventReceived(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("file_in")},
                {QStringLiteral("state"), QStringLiteral("cancelled")},
                {QStringLiteral("id"), id},
                {QStringLiteral("reason"),
                 reason.isEmpty() ? QStringLiteral("cancelled") : reason},
            });
            emit logMessage(QStringLiteral("接收文件已取消"));
        }
        return;
    }
    if (kind == QStringLiteral("file_cancelled")) {
        emit eventReceived(message);
        return;
    }
    if (kind == QStringLiteral("file_done")) {
        emit eventReceived(message);
        return;
    }
    if (kind == QStringLiteral("reboot")) {
        const QJsonObject notice{
            {QStringLiteral("t"), QStringLiteral("notice")},
            {QStringLiteral("text"),
             QStringLiteral("收到重启指令（未执行）")},
        };
        emit eventReceived(notice);
        return;
    }
}

void HostSession::handleMouse(const QJsonObject &message)
{
    if (!inputSink_) {
        return;
    }
    const QString action =
        jsonString(message, "act", QStringLiteral("move"));
    if (action == QStringLiteral("move")) {
        const QPointF point = mapToVirtualDesktop(
            message.value(QStringLiteral("x")).toDouble(),
            message.value(QStringLiteral("y")).toDouble());
        inputSink_->moveAbsolute(point.x(), point.y());
        return;
    }
    if (action == QStringLiteral("down")
        || action == QStringLiteral("up")) {
        if (action == QStringLiteral("down")) {
            const QPointF point = mapToVirtualDesktop(
                message.value(QStringLiteral("x")).toDouble(),
                message.value(QStringLiteral("y")).toDouble());
            inputSink_->moveAbsolute(point.x(), point.y());
        }
        inputSink_->mouseButton(
            jsonString(message, "btn", QStringLiteral("left")),
            action == QStringLiteral("down"));
        return;
    }
    if (action == QStringLiteral("click")) {
        const QPointF point = mapToVirtualDesktop(
            message.value(QStringLiteral("x")).toDouble(),
            message.value(QStringLiteral("y")).toDouble());
        inputSink_->moveAbsolute(point.x(), point.y());
        inputSink_->click(
            jsonString(message, "btn", QStringLiteral("left")));
        return;
    }
    if (action == QStringLiteral("wheel")) {
        inputSink_->wheel(
            static_cast<int>(
                message.value(QStringLiteral("dy")).toDouble()),
            message.value(QStringLiteral("h")).toBool());
        return;
    }
    if (action == QStringLiteral("rel")) {
        inputSink_->moveRelative(
            static_cast<int>(
                message.value(QStringLiteral("dx")).toDouble()),
            static_cast<int>(
                message.value(QStringLiteral("dy")).toDouble()));
        return;
    }
    if (action == QStringLiteral("reldown")
        || action == QStringLiteral("relup")) {
        inputSink_->mouseButton(
            jsonString(message, "btn", QStringLiteral("left")),
            action == QStringLiteral("reldown"));
    }
}

QPointF HostSession::mapToVirtualDesktop(double nx, double ny) const
{
    const double clampedX = std::clamp(nx, 0.0, 1.0);
    const double clampedY = std::clamp(ny, 0.0, 1.0);
    if (!inputSink_ || monitors_.isEmpty()) {
        return {clampedX, clampedY};
    }
    const int index = std::clamp(
        currentMonitor_,
        0,
        static_cast<int>(monitors_.size()) - 1);
    const MonitorInfo &monitor = monitors_.at(index);
    const double x = monitor.left + clampedX * monitor.width;
    const double y = monitor.top + clampedY * monitor.height;
    const QRect desktop = inputSink_->virtualDesktop();
    return {
        std::clamp(
            (x - desktop.left())
                / std::max(1, desktop.width() - 1),
            0.0,
            1.0),
        std::clamp(
            (y - desktop.top())
                / std::max(1, desktop.height() - 1),
            0.0,
            1.0),
    };
}

void HostSession::handleFileBegin(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    const QString name = jsonString(message, "name");
    const qint64 size = jsonInteger(message, "size");
    const QString resumeOf = jsonString(message, "resume_of");
    const bool resume =
        message.value(QStringLiteral("resume")).toBool(false);
    if ((resume && resumeOf.isEmpty())
        || (!resume && !resumeOf.isEmpty())) {
        rejectFile(id, QStringLiteral("resume"));
        return;
    }

    qint64 offset = 0;
    bool resumed = false;
    QString error;
    if (!receiver_.begin(
            id,
            name,
            size,
            &error,
            resumeOf,
            &offset,
            &resumed)) {
        rejectFile(id, error);
        return;
    }
    if (resume) {
        sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_resume")},
            {QStringLiteral("id"), id},
            {QStringLiteral("offset"), offset},
        });
    }
    const QJsonObject event{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("begin")},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), size},
        {QStringLiteral("resumed"), resumed},
        {QStringLiteral("offset"), offset},
    };
    emit eventReceived(event);
    emit logMessage(
        resumed
            ? QStringLiteral("续传文件 %1，从 %2 字节继续")
                  .arg(name)
                  .arg(offset)
            : QStringLiteral("接收文件 %1").arg(name));
}

void HostSession::handleFileResume(const QJsonObject &message)
{
    if (!fileSender_->active()
        || !fileSender_->waitingForResume()) {
        return;
    }
    const QString id = jsonString(message, "id");
    if (id.isEmpty() || id != fileSender_->id()) {
        return;
    }
    QString error;
    if (!fileSender_->acceptResume(message, &error)
        && (error == QStringLiteral("resume_offset")
            || error == QStringLiteral("io"))) {
        fileSender_->fail(error);
    }
}

void HostSession::handleFileData(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    QByteArray data;
    QString error;
    if (!decodeFileChunk(
            jsonString(message, "d"),
            &data,
            &error)) {
        receiver_.abort(id);
        rejectFile(id, error.isEmpty() ? QStringLiteral("data") : error);
        return;
    }
    if (!receiver_.append(id, data, &error)) {
        rejectFile(id, error);
        return;
    }
    const QJsonObject event{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("progress")},
        {QStringLiteral("id"), id},
        {QStringLiteral("got"), receiver_.received(id)},
        {QStringLiteral("size"), receiver_.expected(id)},
    };
    emit eventReceived(event);
}

void HostSession::handleFileEnd(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    QString path;
    qint64 size = 0;
    QString error;
    if (!receiver_.finish(id, &path, &size, &error)) {
        rejectFile(id, error);
        return;
    }
    sendControl(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_done")},
        {QStringLiteral("id"), id},
        {QStringLiteral("path"), path},
        {QStringLiteral("size"), size},
    });
    const QJsonObject event{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("done")},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("path"), path},
        {QStringLiteral("size"), size},
    };
    emit eventReceived(event);
    emit fileReceived(path, size);
    emit logMessage(
        QStringLiteral("文件已保存 %1").arg(QFileInfo(path).fileName()));
}

void HostSession::rejectFile(
    const QString &id,
    const QString &reason)
{
    if (id.isEmpty()) {
        return;
    }
    sendControl(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_error")},
        {QStringLiteral("id"), id},
        {QStringLiteral("reason"),
         reason.isEmpty() ? QStringLiteral("io") : reason},
    });
    emit logMessage(
        QStringLiteral("文件传输失败：%1")
            .arg(reason.isEmpty() ? QStringLiteral("io") : reason));
}

bool HostSession::sendFile(const QString &path, QString *error)
{
    if (!link_ || !link_->established()) {
        if (error) {
            *error = QStringLiteral("offline");
        }
        return false;
    }
    FileSender::ResumeOffer offer;
    const FileSender::ResumeOffer saved =
        fileSender_->interruptedOffer();
    if (saved.valid()) {
        if (isSameFilePath(saved.path, path)) {
            offer = saved;
        } else {
            fileSender_->discardResume();
        }
    }
    return fileSender_->start(
        path,
        [this](const QJsonObject &message) { sendControl(message); },
        error,
        offer);
}

bool HostSession::cancelFile()
{
    if (!fileSender_->active()) {
        return false;
    }
    fileSender_->cancel();
    return true;
}

} // namespace pd
