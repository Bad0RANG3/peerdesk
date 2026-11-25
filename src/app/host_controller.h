#pragma once

#include "app/preferences.h"
#include "client/signaling_client.h"
#include "session/session_types.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

namespace pd {

class HostSession;
class IAudioSource;
class IClipboard;
class IInputSink;
class IScreenSource;
class LanDiscovery;
class PeerLink;

class HostController final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Connecting,
        Waiting,
        Streaming,
        Error,
    };
    Q_ENUM(State)

    explicit HostController(QObject *parent = nullptr);
    ~HostController() override;

    void setPreferences(std::shared_ptr<Preferences> preferences);
    [[nodiscard]] std::shared_ptr<Preferences> preferences() const
    {
        return preferences_;
    }

    void setBindAddress(const QString &address);
    [[nodiscard]] QString bindAddress() const { return bindAddress_; }

    void setDeviceId(const QString &deviceId);
    [[nodiscard]] QString deviceId() const { return deviceId_; }
    [[nodiscard]] QString deviceName() const;

    void setLanDiscovery(LanDiscovery *discovery);

    void setScreenSource(std::shared_ptr<IScreenSource> source);
    void setAudioSource(std::shared_ptr<IAudioSource> source);
    void setInputSink(std::shared_ptr<IInputSink> sink);
    void setClipboard(std::shared_ptr<IClipboard> clipboard);

    bool start(QString *error = nullptr);
    void stop();
    [[nodiscard]] bool active() const { return active_; }

    bool regenerateCode(QString *error = nullptr);
    [[nodiscard]] QString code() const { return code_; }
    [[nodiscard]] QString formattedCode() const;

    void setServer(const QString &server);
    [[nodiscard]] QString server() const { return server_; }
    void setToken(const QString &token);
    [[nodiscard]] QString token() const { return token_; }

    void setVideoSettings(const VideoSettings &settings);
    [[nodiscard]] VideoSettings videoSettings() const { return video_; }
    void setQualityPreset(const QString &id);

    void setAllowInput(bool allow);
    [[nodiscard]] bool allowInput() const { return allowInput_; }
    void setClipboardEnabled(bool enabled);
    void setAudioEnabled(bool enabled);
    [[nodiscard]] bool audioEnabled() const { return video_.audio; }
    void setForceRelay(bool forceRelay);
    [[nodiscard]] bool forceRelay() const { return forceRelay_; }
    void setReceiveDirectory(const QString &directory);

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] QString peerName() const { return peerName_; }
    [[nodiscard]] bool streaming() const;
    [[nodiscard]] QJsonObject statistics() const { return statistics_; }
    [[nodiscard]] QJsonObject viewerInfo() const;
    [[nodiscard]] QList<MonitorInfo> monitors() const;
    [[nodiscard]] bool approvalPending() const
    {
        return !pendingRequestId_.isEmpty();
    }
    [[nodiscard]] QString pendingViewerName() const
    {
        return pendingViewerName_;
    }
    [[nodiscard]] QString pendingRequestId() const
    {
        return pendingRequestId_;
    }

    bool approveViewer();
    bool rejectViewer(const QString &reason = {});

    [[nodiscard]] bool sendFile(
        const QString &path,
        QString *error = nullptr);
    [[nodiscard]] bool cancelFile();

signals:
    void stateChanged(pd::HostController::State state);
    void codeChanged(const QString &code, const QString &formattedCode);
    void peerChanged(const QString &name, bool connected);
    void errorOccurred(const QString &message);
    void videoSettingsChanged(const pd::VideoSettings &settings);
    void monitorsChanged(const QList<pd::MonitorInfo> &monitors);
    void statisticsChanged(const QJsonObject &statistics);
    void linkStateChanged(const QString &state);
    void eventReceived(const QJsonObject &message);
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
    void approvalChanged();

private:
    void connectComponents();
    void applyPreferences();
    void applySessionSettings();
    void persist();
    void setState(State state);
    void setError(const QString &message);
    void failActive(const QString &message);
    void resetPeerState();
    void clearApproval();
    void syncAnnouncement();
    bool startSignaling(QString *error);
    void updateCandidates();

    void onRegistered(const RegistrationInfo &info);
    void onPeerJoined(const PeerInfo &info);
    void onPeerCandidatesChanged(const QList<Candidate> &candidates);
    void onPeerLeft(const QString &reason);
    void onSignalingError(const QString &reason, const QString &text);
    void onLinkState(const QString &state);
    void onSessionEvent(const QJsonObject &message);
    void onViewerRequested(
        const QString &requestId,
        const QString &name,
        int expiresMs);
    void onViewerRequestCleared(const QString &reason);

    std::shared_ptr<Preferences> preferences_;
    PeerLink *link_ = nullptr;
    SignalingClient *signaling_ = nullptr;
    HostSession *session_ = nullptr;
    LanDiscovery *lanDiscovery_ = nullptr;

    std::shared_ptr<IScreenSource> screenSource_;
    std::shared_ptr<IAudioSource> audioSource_;
    std::shared_ptr<IInputSink> inputSink_;
    std::shared_ptr<IClipboard> clipboard_;

    QString bindAddress_ = QStringLiteral("0.0.0.0");
    QString deviceId_;
    QString code_;
    QString server_ = QStringLiteral("127.0.0.1:7900");
    QString token_;
    QString peerName_;
    QString peerSid_;
    QString pendingRequestId_;
    QString pendingViewerName_;
    QString lastError_;
    VideoSettings video_;
    bool allowInput_ = true;
    bool forceRelay_ = false;
    QString receiveDirectory_;
    bool active_ = false;
    bool linkStarted_ = false;
    bool stopping_ = false;
    State state_ = State::Idle;
    QJsonObject statistics_;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::HostController::State)
