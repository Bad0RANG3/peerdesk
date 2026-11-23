#pragma once

#include "session/audio_codec.h"
#include "session/file_transfer.h"
#include "session/host_input_pipeline.h"
#include "session/media_sources.h"
#include "session/session_types.h"
#include "session/video_pipeline.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QString>

#include <memory>

class QTimer;

namespace pd {

class PeerLink;

class HostSession final : public QObject {
    Q_OBJECT

public:
    explicit HostSession(PeerLink *link, QObject *parent = nullptr);
    ~HostSession() override;

    void setScreenSource(std::shared_ptr<IScreenSource> source);
    void setAudioSource(std::shared_ptr<IAudioSource> source);
    void setInputSink(std::shared_ptr<IInputSink> sink);
    void setClipboard(std::shared_ptr<IClipboard> clipboard);

    [[nodiscard]] bool start();
    void suspend();
    void stop();
    [[nodiscard]] bool running() const { return running_; }

    void setSettings(const VideoSettings &settings);
    [[nodiscard]] const VideoSettings &settings() const { return settings_; }

    void setAllowInput(bool allow);
    [[nodiscard]] bool allowInput() const { return allowInput_; }

    void setReceiveDirectory(const QString &directory);
    [[nodiscard]] QString receiveDirectory() const;

    void setRateLimit(double mbps);
    [[nodiscard]] double rateLimit() const { return rateLimitMbps_; }

    [[nodiscard]] QJsonObject info();
    [[nodiscard]] QJsonObject audioState() const;
    [[nodiscard]] QList<MonitorInfo> monitors() const { return monitors_; }

    [[nodiscard]] bool sendFile(
        const QString &path,
        QString *error = nullptr);
    [[nodiscard]] bool cancelFile();

signals:
    void eventReceived(const QJsonObject &message);
    void statsChanged(const QJsonObject &stats);
    void audioStateChanged(const QJsonObject &state);
    void fileReceived(const QString &path, qint64 size);
    void outgoingFileStarted(
        const QString &id,
        const QString &name,
        qint64 size,
        const QString &sourcePath);
    void outgoingFileProgress(const QString &id, qint64 sent, qint64 total);
    void outgoingFileFinished(
        const QString &id,
        const QString &name,
        qint64 size);
    void outgoingFileFailed(const QString &id, const QString &reason);
    void outgoingFileCancelled(const QString &id, const QString &reason);
    void outgoingFileInterrupted(const QString &id, const QString &reason);
    void logMessage(const QString &message);

private:
    void connectLink();
    void onLinkStateChanged(const QString &state);
    void onVideoFrame(const EncodedVideoFrame &frame);
    void onVideoFailed(const QString &message);
    void startStreaming();
    void stopStreaming();
    void resumeOutgoingFile();
    void refreshMonitors();
    void sendInfo();
    void sendControl(const QJsonObject &message);

    void startAudio();
    void stopAudio();
    void broadcastAudioState();
    void pollAudio();

    void pushStats();
    void pollClipboard();
    void applySettings(const QJsonObject &message);

    void dispatch(const QJsonObject &message);
    void handleMouse(const QJsonObject &message);
    void handleFileBegin(const QJsonObject &message);
    void handleFileResume(const QJsonObject &message);
    void handleFileData(const QJsonObject &message);
    void handleFileEnd(const QJsonObject &message);
    void rejectFile(const QString &id, const QString &reason);

    void submitInput(const HostInputCommand &command);
    [[nodiscard]] QRect currentMonitorRect() const;

    PeerLink *link_ = nullptr;
    VideoPipeline videoPipeline_;
    std::shared_ptr<IAudioSource> audioSource_;
    std::shared_ptr<IClipboard> clipboard_;
    HostInputPipeline inputPipeline_;

    VideoSettings settings_;
    QList<MonitorInfo> monitors_;
    int currentMonitor_ = 0;
    bool wantedRunning_ = false;
    bool running_ = false;
    bool allowInput_ = true;
    double rateLimitMbps_ = 0.0;

    AudioEncoder audioEncoder_;
    QByteArray audioCarry_;
    bool audioRunning_ = false;
    QString audioError_;

    FileReceiver receiver_;
    FileSender *fileSender_ = nullptr;

    QTimer *audioTimer_ = nullptr;
    QTimer *statsTimer_ = nullptr;
    QTimer *clipboardTimer_ = nullptr;
    QElapsedTimer statsClock_;
    qint64 sentFrames_ = 0;
    qint64 statsLastFrames_ = 0;
    double lastStatsFps_ = 0.0;
    qint64 lastFrameBytes_ = 0;
    int lastFrameQuality_ = 65;
    int lastEffectiveFps_ = 0;
    qint64 lastCaptureMs_ = 0;
    qint64 lastEncodeMs_ = 0;
    QString lastClipboard_;
};

} // namespace pd
