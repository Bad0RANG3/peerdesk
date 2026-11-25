#pragma once

#include "transport/peer_link.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

class QTcpSocket;

namespace pd {

struct SignalRelay {
    QString host;
    quint16 port = 0;
    quint32 tag = 0;
    double mbps = 0.0;

    [[nodiscard]] bool valid() const
    {
        return !host.trimmed().isEmpty() && port != 0;
    }
};

struct RegistrationInfo {
    QString code;
    SignalRelay relay;
    qint64 serverTime = 0;
    bool tokenRequired = false;
    double relayMbps = 0.0;
};

struct PeerInfo {
    QString sid;
    QString name;
    QList<Candidate> candidates;
    SignalRelay relay;
};

class SignalingClient final : public QObject {
    Q_OBJECT

public:
    enum class Role {
        None,
        Host,
        Viewer,
    };
    Q_ENUM(Role)

    enum class State {
        Idle,
        Connecting,
        Negotiating,
        Ready,
        Paired,
        Reconnecting,
        WaitingApproval,
        ViewerRequest,
        Error,
    };
    Q_ENUM(State)

    explicit SignalingClient(QObject *parent = nullptr);
    ~SignalingClient() override;

    void setServer(const QString &host, quint16 port);
    [[nodiscard]] QString serverHost() const { return serverHost_; }
    [[nodiscard]] quint16 serverPort() const { return serverPort_; }

    void setToken(const QString &token);
    [[nodiscard]] QString token() const { return token_; }

    void setAutoReconnect(bool enabled);
    [[nodiscard]] bool autoReconnect() const { return autoReconnect_; }
    void setReconnectPolicy(int initialDelayMs, int maximumDelayMs);
    void setPairRetryPolicy(int initialDelayMs, int maximumDelayMs);

    bool startHost(
        const QString &code,
        const QString &name,
        const QList<Candidate> &candidates = {});
    bool startViewer(
        const QString &code,
        const QString &name,
        const QList<Candidate> &candidates = {});
    void stop();
    void reconnectNow();
    void requestPairRetry(bool force = false);

    void updateCandidates(const QList<Candidate> &candidates);
    bool sendMessage(const QJsonObject &message);
    bool sendPing();
    bool approveViewer(
        const QString &requestId,
        const QString &access = QStringLiteral("control"));
    bool rejectViewer(
        const QString &requestId,
        const QString &reason = {});
    bool cancelConnection();

    [[nodiscard]] Role role() const { return role_; }
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] QString code() const { return code_; }
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] bool connected() const;
    [[nodiscard]] bool paired() const { return !peer_.sid.isEmpty(); }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] QString lastErrorText() const { return lastErrorText_; }
    [[nodiscard]] QString pendingRequestId() const
    {
        return pendingRequestId_;
    }
    [[nodiscard]] QString pendingHostName() const
    {
        return pendingHostName_;
    }
    [[nodiscard]] QString pendingViewerName() const
    {
        return pendingViewerName_;
    }
    [[nodiscard]] QString access() const { return access_; }
    [[nodiscard]] RegistrationInfo registration() const
    {
        return registration_;
    }
    [[nodiscard]] PeerInfo peer() const { return peer_; }
    [[nodiscard]] SignalRelay relay() const { return relay_; }
    [[nodiscard]] QList<Candidate> candidates() const
    {
        return candidates_;
    }

signals:
    void stateChanged(pd::SignalingClient::State state);
    void serverConnected();
    void serverDisconnected();
    void registered(const pd::RegistrationInfo &info);
    void peerJoined(const pd::PeerInfo &info);
    void peerCandidatesChanged(const QList<pd::Candidate> &candidates);
    void peerLeft(const QString &reason);
    void relayChanged(const pd::SignalRelay &relay);
    void viewerRequested(
        const QString &requestId,
        const QString &name,
        int expiresMs);
    void waitingApproval(
        const QString &requestId,
        const QString &hostName,
        int expiresMs);
    void viewerRequestCleared(const QString &reason);
    void accessChanged(const QString &access);
    void errorOccurred(
        const QString &reason,
        const QString &text);
    void messageReceived(const QJsonObject &message);
    void latencyMeasured(qint64 milliseconds);
    void logMessage(const QString &message);

private:
    void connectSocket();
    void scheduleReconnect();
    void cancelReconnect();
    void schedulePairRetry();
    void cancelPairRetry();
    void resetSession();
    void setState(State state);
    void setError(const QString &reason, const QString &text);

    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError();
    void onSocketTimeout();
    void readMessages();
    void handleMessage(const QJsonObject &message);
    void handleRegistered(const QJsonObject &message);
    void handlePeer(const QJsonObject &message);
    void handlePeerCandidates(const QJsonObject &message);
    void handlePeerLeft(const QString &reason);
    void handleViewerRequest(const QJsonObject &message);
    void handleWaiting(const QJsonObject &message);
    void handleAccess(const QJsonObject &message);
    void handleViewerRequestCleared(const QString &reason);
    void handleServerError(const QJsonObject &message);
    void clearPendingRequest();
    void emitViewerRequestClearedOnce(const QString &reason);
    [[nodiscard]] QString currentRequestId() const;
    void onApprovalTimeout();
    [[nodiscard]] int approvalRemainingMs() const;

    bool start(
        Role role,
        const QString &code,
        const QString &name,
        const QList<Candidate> &candidates);
    void sendHello();
    [[nodiscard]] QJsonObject baseMessage(const QString &type) const;
    static SignalRelay parseRelay(
        const QJsonValue &value,
        double mbps = 0.0,
        quint32 tag = 0);
    static QList<Candidate> parseCandidates(const QJsonValue &value);
    void emitLog(const QString &message);

    std::unique_ptr<QTcpSocket> socket_;
    QTimer reconnectTimer_;
    QTimer pairRetryTimer_;
    QTimer timeoutTimer_;
    QTimer approvalTimer_;
    QElapsedTimer pingClock_;
    QByteArray lineBuffer_;

    QString serverHost_;
    quint16 serverPort_ = 7900;
    QString token_;
    Role role_ = Role::None;
    State state_ = State::Idle;
    QString code_;
    QString name_;
    QList<Candidate> candidates_;

    RegistrationInfo registration_;
    PeerInfo peer_;
    SignalRelay relay_;
    QString lastError_;
    QString lastErrorText_;
    QString pendingRequestId_;
    QString submittedRequestId_;
    QString pendingHostName_;
    QString pendingViewerName_;
    QString access_;
    bool requestOutcomeEmitted_ = false;

    bool autoReconnect_ = true;
    bool stopRequested_ = false;
    bool terminalError_ = false;
    bool pingPending_ = false;
    int reconnectAttempt_ = 0;
    int reconnectInitialMs_ = 1000;
    int reconnectMaximumMs_ = 15000;
    int pairRetryAttempt_ = 0;
    int pairRetryInitialMs_ = 250;
    int pairRetryMaximumMs_ = 8000;
    int connectTimeoutMs_ = 8000;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::SignalRelay)
Q_DECLARE_METATYPE(pd::RegistrationInfo)
Q_DECLARE_METATYPE(pd::PeerInfo)
Q_DECLARE_METATYPE(pd::Candidate)
