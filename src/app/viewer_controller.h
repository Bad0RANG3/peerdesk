#pragma once

#include "app/preferences.h"
#include "client/signaling_client.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

class QTimer;

namespace pd {

class IAudioPlayback;
class PeerLink;
class ViewerSession;

class ViewerController final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Connecting,
        Streaming,
        Reconnecting,
        Error,
    };
    Q_ENUM(State)

    explicit ViewerController(QObject *parent = nullptr);
    ~ViewerController() override;

    void setPreferences(std::shared_ptr<Preferences> preferences);
    [[nodiscard]] std::shared_ptr<Preferences> preferences() const
    {
        return preferences_;
    }
    void setAudioPlayback(std::shared_ptr<IAudioPlayback> playback);
    void setBindAddress(const QString &address);
    void setReconnectTimeoutMs(int milliseconds);
    [[nodiscard]] QString bindAddress() const { return bindAddress_; }

    bool connectTo(
        const QString &code,
        const QString &server,
        const QString &token = {},
        const QString &name = {},
        QString *error = nullptr);
    void hangUp();

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] QString code() const { return code_; }
    [[nodiscard]] QString server() const { return server_; }
    [[nodiscard]] QString token() const { return token_; }
    void setServer(const QString &server);
    void setToken(const QString &token);
    [[nodiscard]] QString peerName() const { return peerName_; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool streaming() const;
    [[nodiscard]] QJsonObject info() const;
    [[nodiscard]] QJsonObject statistics() const { return statistics_; }
    [[nodiscard]] QJsonObject audioState() const;

    [[nodiscard]] bool audioEnabled() const { return audioEnabled_; }
    void setAudioEnabled(bool enabled);

    [[nodiscard]] QString qualityId() const { return qualityId_; }
    [[nodiscard]] int maxWidth() const { return maxWidth_; }
    [[nodiscard]] int fps() const { return fps_; }
    [[nodiscard]] int quality() const { return quality_; }
    void setQualityPreset(const QString &id);
    void setMaxWidth(int maxWidth);
    void setFps(int fps);
    void setQuality(int quality);
    void selectMonitor(int index);
    [[nodiscard]] int monitor() const { return monitor_; }

    void requestKeyframe();
    void sendMouseMove(double nx, double ny);
    void sendMouseButton(
        const QString &action,
        double nx,
        double ny,
        const QString &button = QStringLiteral("left"));
    void sendMouseWheel(int delta, bool horizontal = false);
    void sendMouseRelative(int dx, int dy);
    void sendMouseRelativeButton(
        const QString &action,
        const QString &button = QStringLiteral("left"));
    [[nodiscard]] bool sendKey(const QString &code, bool down);
    void sendText(const QString &text);
    [[nodiscard]] bool sendClipboard(const QString &text);
    [[nodiscard]] bool sendClipboardImage(const QImage &image);
    void sendCtrlAltDelete();

    [[nodiscard]] bool beginUpload(
        const QString &name,
        qint64 size,
        QString *error = nullptr);
    [[nodiscard]] bool uploadChunk(
        const QByteArray &data,
        QString *error = nullptr);
    [[nodiscard]] bool endUpload(QString *error = nullptr);
    [[nodiscard]] bool sendFile(
        const QString &path,
        QString *error = nullptr);
    [[nodiscard]] bool cancelFile();
    void setReceiveDirectory(const QString &directory);

signals:
    void stateChanged(pd::ViewerController::State state);
    void inputReleaseRequested();
    void errorOccurred(const QString &message);
    void peerChanged(const QString &name, bool connected);
    void frameDecoded(const QImage &image, bool keyframe);
    void audioEnabledChanged(bool enabled);
    void audioStateChanged(const QJsonObject &state);
    void infoChanged(const QJsonObject &info);
    void statisticsChanged(const QJsonObject &stats);
    void linkStateChanged(const QString &state);
    void eventReceived(const QJsonObject &message);
    void clipboardImageReceived(const QImage &image);
    void fileReceived(const QString &path, qint64 size);
    void outgoingFileStarted(
        const QString &id,
        const QString &name,
        qint64 size,
        const QString &sourcePath);
    void outgoingFileProgress(
        const QString &id,
        qint64 sent,
        qint64 total);
    void outgoingFileFinished(
        const QString &id,
        const QString &name,
        qint64 size);
    void outgoingFileFailed(const QString &id, const QString &reason);
    void outgoingFileCancelled(const QString &id, const QString &reason);
    void outgoingFileInterrupted(const QString &id, const QString &reason);
    void logMessage(const QString &message);
    void sessionEnded(const QString &reason);

private:
    void connectComponents();
    void applyPreferences();
    void applyVideoSettings();
    void persist();
    void setState(State state);
    void setError(const QString &message);
    void failActive(const QString &message);
    void stopSession(bool notify, const QString &reason = {});
    void beginRecovery();
    void ensureInitialDeadline();

    void onRegistered(const RegistrationInfo &info);
    void onPeerJoined(const PeerInfo &info);
    void onPeerCandidatesChanged(const QList<Candidate> &candidates);
    void onPeerLeft(const QString &reason);
    void onSignalingError(const QString &reason, const QString &text);
    void onLinkState(const QString &state);
    void onAudio(const QByteArray &pcm, quint32 sequence, quint32 timestampMs);

    std::shared_ptr<Preferences> preferences_;
    std::shared_ptr<IAudioPlayback> playback_;
    PeerLink *link_ = nullptr;
    SignalingClient *signaling_ = nullptr;
    ViewerSession *session_ = nullptr;
    QTimer *deadlineTimer_ = nullptr;

    QString bindAddress_ = QStringLiteral("0.0.0.0");
    QString code_;
    QString server_ = QStringLiteral("127.0.0.1:7900");
    QString token_;
    QString name_;
    QString peerName_;
    QString peerSid_;
    QString lastError_;
    QString qualityId_ = QStringLiteral("balanced");
    QString receiveDirectory_;
    int maxWidth_ = 1600;
    int fps_ = 30;
    int quality_ = 62;
    int monitor_ = 0;
    bool audioEnabled_ = true;
    bool forceRelay_ = false;
    bool active_ = false;
    bool linkStarted_ = false;
    bool stopping_ = false;
    bool hadLiveSession_ = false;
    bool recovering_ = false;
    bool recoveryHandshakeArmed_ = false;
    int reconnectTimeoutMs_ = 30000;
    State state_ = State::Idle;
    QJsonObject info_;
    QJsonObject statistics_;
    QJsonObject audioState_;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::ViewerController::State)
