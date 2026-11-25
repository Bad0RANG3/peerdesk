#include "client/signaling_client.h"

#include "core/util.h"

#include <QAbstractSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTcpSocket>

#include <algorithm>
#include <utility>

namespace pd {
namespace {

constexpr int kMaxSignalLine = 1 << 20;

QJsonArray candidatesToJson(const QList<Candidate> &candidates)
{
    QJsonArray result;
    for (const Candidate &candidate : candidates) {
        if (candidate.host.trimmed().isEmpty() || candidate.port == 0) {
            continue;
        }
        result.append(QJsonObject{
            {QStringLiteral("host"), candidate.host.trimmed()},
            {QStringLiteral("port"), candidate.port},
            {QStringLiteral("kind"), candidate.kind},
        });
    }
    return result;
}

QList<Candidate> normalizedCandidates(
    const QList<Candidate> &candidates)
{
    QList<Candidate> result;
    QList<QPair<QString, quint16>> seen;
    for (const Candidate &candidate : candidates) {
        const QString host = candidate.host.trimmed();
        if (host.isEmpty() || candidate.port == 0) {
            continue;
        }
        const QPair<QString, quint16> key{
            host.toCaseFolded(), candidate.port};
        if (seen.contains(key)) {
            continue;
        }
        seen.append(key);
        result.append(Candidate{
            host,
            candidate.port,
            candidate.kind.trimmed().isEmpty()
                ? QStringLiteral("host")
                : candidate.kind.trimmed(),
        });
    }
    return result;
}

QString relayHost(const QJsonArray &relay)
{
    if (relay.size() < 2 || !relay.at(0).isString()) {
        return {};
    }
    return relay.at(0).toString().trimmed();
}

quint16 relayPort(const QJsonArray &relay)
{
    if (relay.size() < 2) {
        return 0;
    }
    const int port = relay.at(1).toInt();
    if (port <= 0 || port > 65535) {
        return 0;
    }
    return static_cast<quint16>(port);
}

} // namespace

SignalingClient::SignalingClient(QObject *parent)
    : QObject(parent)
    , socket_(std::make_unique<QTcpSocket>())
{
    socket_->setParent(this);
    reconnectTimer_.setSingleShot(true);
    pairRetryTimer_.setSingleShot(true);
    timeoutTimer_.setSingleShot(true);
    approvalTimer_.setSingleShot(true);

    qRegisterMetaType<SignalRelay>();
    qRegisterMetaType<RegistrationInfo>();
    qRegisterMetaType<PeerInfo>();
    qRegisterMetaType<Candidate>();
    qRegisterMetaType<QList<Candidate>>();

    connect(
        &reconnectTimer_, &QTimer::timeout, this,
        [this] { connectSocket(); });
    connect(
        &pairRetryTimer_, &QTimer::timeout, this,
        [this] {
            if (role_ != Role::Viewer || !connected()
                || stopRequested_ || terminalError_) {
                return;
            }
            emitLog(QStringLiteral("重新请求与远端设备配对"));
            sendHello();
        });
    connect(
        &timeoutTimer_, &QTimer::timeout, this,
        [this] { onSocketTimeout(); });
    connect(
        &approvalTimer_, &QTimer::timeout, this,
        [this] { onApprovalTimeout(); });
    connect(
        socket_.get(), &QTcpSocket::connected, this,
        [this] { onSocketConnected(); });
    connect(
        socket_.get(), &QTcpSocket::disconnected, this,
        [this] { onSocketDisconnected(); });
    connect(
        socket_.get(), &QTcpSocket::readyRead, this,
        [this] { readMessages(); });
    connect(
        socket_.get(), &QTcpSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError) { onSocketError(); });
}

SignalingClient::~SignalingClient()
{
    stop();
}

void SignalingClient::setServer(const QString &host, quint16 port)
{
    const QString normalized = host.trimmed();
    if (normalized == serverHost_ && port == serverPort_) {
        return;
    }
    stop();
    serverHost_ = normalized;
    serverPort_ = port;
}

void SignalingClient::setToken(const QString &token)
{
    token_ = token.trimmed();
}

void SignalingClient::setAutoReconnect(bool enabled)
{
    autoReconnect_ = enabled;
    if (!enabled) {
        cancelReconnect();
    }
}

void SignalingClient::setReconnectPolicy(
    int initialDelayMs, int maximumDelayMs)
{
    reconnectInitialMs_ = std::max(10, initialDelayMs);
    reconnectMaximumMs_ =
        std::max(reconnectInitialMs_, maximumDelayMs);
}

void SignalingClient::setPairRetryPolicy(
    int initialDelayMs, int maximumDelayMs)
{
    pairRetryInitialMs_ = std::max(10, initialDelayMs);
    pairRetryMaximumMs_ =
        std::max(pairRetryInitialMs_, maximumDelayMs);
}

bool SignalingClient::startHost(
    const QString &code,
    const QString &name,
    const QList<Candidate> &candidates)
{
    return start(Role::Host, code, name, candidates);
}

bool SignalingClient::startViewer(
    const QString &code,
    const QString &name,
    const QList<Candidate> &candidates)
{
    return start(Role::Viewer, code, name, candidates);
}

bool SignalingClient::start(
    Role role,
    const QString &code,
    const QString &name,
    const QList<Candidate> &candidates)
{
    const QString normalized = normalizeCode(code);
    if (role == Role::None || !codeIsValid(normalized)) {
        setError(
            QStringLiteral("bad_code"),
            QStringLiteral("连接码需要 9 位数字"));
        return false;
    }
    if (serverHost_.isEmpty() || serverPort_ == 0) {
        setError(
            QStringLiteral("bad_server"),
            QStringLiteral("信令服务器地址无效"));
        return false;
    }

    cancelReconnect();
    cancelPairRetry();
    timeoutTimer_.stop();
    approvalTimer_.stop();
    stopRequested_ = false;
    terminalError_ = false;
    lastError_.clear();
    lastErrorText_.clear();
    role_ = role;
    code_ = normalized;
    name_ = name.trimmed();
    candidates_ = normalizedCandidates(candidates);
    peer_ = {};
    registration_ = {};
    relay_ = {};
    pendingRequestId_.clear();
    submittedRequestId_.clear();
    pendingHostName_.clear();
    pendingViewerName_.clear();
    access_.clear();
    requestOutcomeEmitted_ = false;
    pingPending_ = false;
    pairRetryAttempt_ = 0;
    lineBuffer_.clear();

    if (socket_->state() == QAbstractSocket::ConnectedState) {
        setState(State::Negotiating);
        sendHello();
    } else {
        reconnectAttempt_ = 0;
        connectSocket();
    }
    return true;
}

void SignalingClient::stop()
{
    const bool wasConnected = connected();
    const bool cancelling =
        wasConnected && role_ == Role::Viewer
        && state_ == State::WaitingApproval
        && !pendingRequestId_.isEmpty();
    stopRequested_ = true;
    cancelReconnect();
    cancelPairRetry();
    timeoutTimer_.stop();
    approvalTimer_.stop();

    if (cancelling) {
        QJsonObject cancel = baseMessage(QStringLiteral("cancel"));
        cancel.insert(
            QStringLiteral("request_id"), pendingRequestId_);
        sendMessage(cancel);
    }

    role_ = Role::None;
    peer_ = {};
    registration_ = {};
    relay_ = {};
    pendingRequestId_.clear();
    submittedRequestId_.clear();
    pendingHostName_.clear();
    pendingViewerName_.clear();
    access_.clear();
    requestOutcomeEmitted_ = false;
    pingPending_ = false;
    lineBuffer_.clear();

    if (wasConnected) {
        QTcpSocket *socket = socket_.get();
        const QByteArray bye =
            QJsonDocument(QJsonObject{
                              {QStringLiteral("t"),
                               QStringLiteral("bye")}})
                .toJson(QJsonDocument::Compact)
            + '\n';
        socket->write(bye);
        socket->disconnectFromHost();
    } else {
        socket_->abort();
    }
    setState(State::Idle);
    stopRequested_ = false;
}

void SignalingClient::reconnectNow()
{
    if (role_ == Role::None) {
        return;
    }
    cancelReconnect();
    socket_->abort();
    reconnectAttempt_ = 0;
    connectSocket();
}

void SignalingClient::requestPairRetry(bool force)
{
    if (role_ != Role::Viewer || !connected()
        || stopRequested_ || terminalError_
        || (state_ == State::WaitingApproval && !force)
        || (state_ == State::Paired && !force)) {
        return;
    }
    cancelPairRetry();
    pairRetryAttempt_ = 0;
    if (force) {
        sendHello();
    } else {
        schedulePairRetry();
    }
}

void SignalingClient::updateCandidates(
    const QList<Candidate> &candidates)
{
    candidates_ = normalizedCandidates(candidates);
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QJsonObject message{
        {QStringLiteral("t"), QStringLiteral("cands")},
        {QStringLiteral("cands"), candidatesToJson(candidates_)},
    };
    sendMessage(message);
}

bool SignalingClient::sendMessage(const QJsonObject &message)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    QByteArray wire =
        QJsonDocument(message).toJson(QJsonDocument::Compact);
    wire.append('\n');
    if (socket_->write(wire) != wire.size()) {
        emitLog(QStringLiteral("信令发送缓冲不足"));
    }
    socket_->flush();
    return true;
}

bool SignalingClient::sendPing()
{
    if (!sendMessage(
            QJsonObject{{QStringLiteral("t"),
                         QStringLiteral("ping")}})) {
        return false;
    }
    pingClock_.restart();
    pingPending_ = true;
    return true;
}

bool SignalingClient::connected() const
{
    return socket_
        && socket_->state() == QAbstractSocket::ConnectedState;
}

bool SignalingClient::approveViewer(
    const QString &requestId,
    const QString &access)
{
    if (role_ != Role::Host
        || state_ != State::ViewerRequest
        || requestId.isEmpty()
        || requestId != pendingRequestId_
        || access != QStringLiteral("control")
        || !connected()) {
        return false;
    }
    QJsonObject message = baseMessage(QStringLiteral("approve"));
    message.insert(QStringLiteral("request_id"), requestId);
    message.insert(QStringLiteral("access"), access);
    if (!sendMessage(message)) {
        return false;
    }
    submittedRequestId_ = requestId;
    clearPendingRequest();
    approvalTimer_.stop();
    setState(State::Ready);
    emitViewerRequestClearedOnce(QStringLiteral("approved"));
    return true;
}

bool SignalingClient::rejectViewer(
    const QString &requestId,
    const QString &reason)
{
    if (role_ != Role::Host
        || state_ != State::ViewerRequest
        || requestId.isEmpty()
        || requestId != pendingRequestId_
        || !connected()) {
        return false;
    }
    QJsonObject message = baseMessage(QStringLiteral("reject"));
    message.insert(QStringLiteral("request_id"), requestId);
    if (!reason.trimmed().isEmpty()) {
        message.insert(
            QStringLiteral("reason"), reason.trimmed());
    }
    if (!sendMessage(message)) {
        return false;
    }
    submittedRequestId_ = requestId;
    clearPendingRequest();
    approvalTimer_.stop();
    setState(State::Ready);
    emitViewerRequestClearedOnce(QStringLiteral("rejected"));
    return true;
}

bool SignalingClient::cancelConnection()
{
    if (role_ != Role::Viewer
        || state_ != State::WaitingApproval
        || pendingRequestId_.isEmpty()
        || !connected()) {
        return false;
    }
    QJsonObject message = baseMessage(QStringLiteral("cancel"));
    message.insert(
        QStringLiteral("request_id"), pendingRequestId_);
    if (!sendMessage(message)) {
        return false;
    }
    submittedRequestId_ = pendingRequestId_;
    clearPendingRequest();
    approvalTimer_.stop();
    setState(State::Negotiating);
    emitViewerRequestClearedOnce(QStringLiteral("cancelled"));
    return true;
}

void SignalingClient::connectSocket()
{
    reconnectTimer_.stop();
    if (role_ == Role::None || stopRequested_ || terminalError_) {
        return;
    }

    socket_->abort();
    lineBuffer_.clear();
    setState(State::Connecting);
    timeoutTimer_.start(connectTimeoutMs_);
    emitLog(
        QStringLiteral("正在连接信令服务器 %1:%2")
            .arg(serverHost_)
            .arg(serverPort_));
    socket_->connectToHost(serverHost_, serverPort_);
}

void SignalingClient::scheduleReconnect()
{
    if (!autoReconnect_ || role_ == Role::None || stopRequested_
        || terminalError_ || reconnectTimer_.isActive()) {
        return;
    }

    int delay = reconnectInitialMs_;
    for (int attempt = 0;
         attempt < reconnectAttempt_
         && delay < reconnectMaximumMs_;
         ++attempt) {
        delay = std::min(delay * 2, reconnectMaximumMs_);
    }
    ++reconnectAttempt_;
    setState(State::Reconnecting);
    emitLog(
        QStringLiteral("%1 ms 后重连信令服务器").arg(delay));
    reconnectTimer_.start(delay);
}

void SignalingClient::schedulePairRetry()
{
    if (role_ != Role::Viewer || !autoReconnect_ || !connected()
        || stopRequested_ || terminalError_
        || state_ == State::WaitingApproval
        || state_ == State::Paired
        || pairRetryTimer_.isActive()) {
        return;
    }

    int delay = pairRetryInitialMs_;
    for (int attempt = 0;
         attempt < pairRetryAttempt_
         && delay < pairRetryMaximumMs_;
         ++attempt) {
        delay = std::min(delay * 2, pairRetryMaximumMs_);
    }
    ++pairRetryAttempt_;
    emitLog(
        QStringLiteral("%1 ms 后重新请求配对").arg(delay));
    pairRetryTimer_.start(delay);
}

void SignalingClient::cancelReconnect()
{
    reconnectTimer_.stop();
}

void SignalingClient::cancelPairRetry()
{
    pairRetryTimer_.stop();
}

void SignalingClient::resetSession()
{
    const bool hadRequest = !currentRequestId().isEmpty();
    if (!peer_.sid.isEmpty()) {
        peer_ = {};
        emit peerLeft(QStringLiteral("connection_lost"));
    }
    clearPendingRequest();
    submittedRequestId_.clear();
    approvalTimer_.stop();
    access_.clear();
    registration_ = {};
    relay_ = {};
    pingPending_ = false;
    if (hadRequest) {
        emitViewerRequestClearedOnce(
            QStringLiteral("signaling_lost"));
    }
}

void SignalingClient::setState(State state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}

void SignalingClient::setError(
    const QString &reason, const QString &text)
{
    lastError_ = reason;
    lastErrorText_ = text;
    setState(State::Error);
    emit errorOccurred(reason, text);
}

void SignalingClient::onSocketConnected()
{
    timeoutTimer_.stop();
    pairRetryAttempt_ = 0;
    emit serverConnected();
    setState(State::Negotiating);
    sendHello();
}

void SignalingClient::onSocketDisconnected()
{
    timeoutTimer_.stop();
    approvalTimer_.stop();
    cancelPairRetry();
    lineBuffer_.clear();
    emit serverDisconnected();
    resetSession();
    if (stopRequested_ || role_ == Role::None) {
        return;
    }
    if (terminalError_) {
        setState(State::Error);
        return;
    }
    scheduleReconnect();
}

void SignalingClient::onSocketError()
{
    if (stopRequested_) {
        return;
    }
    lastError_ = QStringLiteral("network");
    lastErrorText_ = socket_->errorString();
    emit errorOccurred(lastError_, lastErrorText_);
    emitLog(lastErrorText_);
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }
}

void SignalingClient::onSocketTimeout()
{
    if (stopRequested_ || role_ == Role::None) {
        return;
    }
    lastError_ = QStringLiteral("timeout");
    lastErrorText_ = QStringLiteral("信令服务器响应超时");
    emit errorOccurred(lastError_, lastErrorText_);
    emitLog(lastErrorText_);
    socket_->abort();
    scheduleReconnect();
}

void SignalingClient::readMessages()
{
    lineBuffer_.append(socket_->readAll());
    for (;;) {
        const qsizetype newline = lineBuffer_.indexOf('\n');
        if (newline < 0) {
            if (lineBuffer_.size() > kMaxSignalLine) {
                setError(
                    QStringLiteral("line_too_large"),
                    QStringLiteral("信令消息超过大小限制"));
                socket_->abort();
            }
            return;
        }

        const QByteArray line = lineBuffer_.left(newline);
        lineBuffer_.remove(0, newline + 1);
        if (line.size() + 1 > kMaxSignalLine) {
            setError(
                QStringLiteral("line_too_large"),
                QStringLiteral("信令消息超过大小限制"));
            socket_->abort();
            return;
        }

        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError
            || !document.isObject()) {
            continue;
        }
        handleMessage(document.object());
    }
}

void SignalingClient::handleMessage(const QJsonObject &message)
{
    emit messageReceived(message);
    const QString kind =
        message.value(QStringLiteral("t")).toString();
    if (kind == QStringLiteral("registered")) {
        handleRegistered(message);
    } else if (kind == QStringLiteral("viewer")
               || kind == QStringLiteral("peer")) {
        handlePeer(message);
    } else if (kind == QStringLiteral("peer_cands")) {
        handlePeerCandidates(message);
    } else if (kind == QStringLiteral("peer_bye")) {
        handlePeerLeft(QStringLiteral("bye"));
    } else if (kind == QStringLiteral("peer_gone")
               || kind == QStringLiteral("viewer_gone")) {
        handlePeerLeft(QStringLiteral("gone"));
    } else if (kind == QStringLiteral("viewer_request")) {
        handleViewerRequest(message);
    } else if (kind == QStringLiteral("waiting")) {
        handleWaiting(message);
    } else if (kind == QStringLiteral("access")) {
        handleAccess(message);
    } else if (kind == QStringLiteral("viewer_cancelled")
               || kind == QStringLiteral("viewer_request_expired")) {
        const QString requestId =
            message.value(QStringLiteral("request_id")).toString();
        if (requestId.isEmpty()
            || requestId != currentRequestId()) {
            return;
        }
        handleViewerRequestCleared(
            kind == QStringLiteral("viewer_cancelled")
                ? QStringLiteral("cancelled")
                : QStringLiteral("timeout"));
    } else if (kind == QStringLiteral("error")) {
        handleServerError(message);
    } else if (kind == QStringLiteral("pong")) {
        if (pingPending_) {
            pingPending_ = false;
            emit latencyMeasured(pingClock_.elapsed());
        }
    }
}

void SignalingClient::handleRegistered(const QJsonObject &message)
{
    timeoutTimer_.stop();
    reconnectAttempt_ = 0;
    cancelPairRetry();
    pairRetryAttempt_ = 0;
    lastError_.clear();
    lastErrorText_.clear();
    registration_.code = normalizeCode(
        message.value(QStringLiteral("code")).toString());
    registration_.serverTime =
        message.value(QStringLiteral("server_time"))
            .toInteger();
    registration_.tokenRequired =
        message.value(QStringLiteral("token_required")).toBool();
    registration_.relayMbps =
        message.value(QStringLiteral("relay_mbps")).toDouble(0.0);
    registration_.relay = parseRelay(
        message.value(QStringLiteral("relay")),
        registration_.relayMbps);
    if (registration_.relay.valid()) {
        relay_ = registration_.relay;
        emit relayChanged(relay_);
    }
    if (state_ != State::ViewerRequest
        && state_ != State::WaitingApproval) {
        setState(State::Ready);
    }
    emit registered(registration_);
}

void SignalingClient::handlePeer(const QJsonObject &message)
{
    timeoutTimer_.stop();
    reconnectAttempt_ = 0;
    cancelPairRetry();
    pairRetryAttempt_ = 0;
    lastError_.clear();
    lastErrorText_.clear();
    const double mbps =
        message.value(QStringLiteral("relay_mbps")).toDouble(0.0);
    const quint32 tag = static_cast<quint32>(
        static_cast<quint64>(
            message.value(QStringLiteral("relay_tag")).toDouble(0.0)));

    PeerInfo info;
    info.sid = message.value(QStringLiteral("sid")).toString();
    info.name = message.value(QStringLiteral("name")).toString();
    info.candidates = parseCandidates(
        message.value(QStringLiteral("cands")));
    info.relay = parseRelay(
        message.value(QStringLiteral("relay")), mbps, tag);
    const bool hadRequest = !currentRequestId().isEmpty();
    if (!peer_.sid.isEmpty() && peer_.sid == info.sid) {
        approvalTimer_.stop();
        clearPendingRequest();
        submittedRequestId_.clear();
        peer_.name = info.name;
        peer_.candidates = info.candidates;
        peer_.relay = info.relay;
        if (info.relay.valid()) {
            relay_ = info.relay;
            emit relayChanged(relay_);
        }
        emit peerCandidatesChanged(info.candidates);
        if (state_ != State::Paired) {
            setState(State::Paired);
        }
        if (hadRequest) {
            emitViewerRequestClearedOnce(QStringLiteral("paired"));
        }
        return;
    }
    peer_ = info;
    approvalTimer_.stop();
    clearPendingRequest();
    submittedRequestId_.clear();
    if (info.relay.valid()) {
        relay_ = info.relay;
        emit relayChanged(relay_);
    }
    setState(State::Paired);
    emit peerJoined(info);
    if (hadRequest) {
        emitViewerRequestClearedOnce(QStringLiteral("paired"));
    }
}

void SignalingClient::handlePeerCandidates(
    const QJsonObject &message)
{
    const QList<Candidate> candidates = parseCandidates(
        message.value(QStringLiteral("cands")));
    peer_.candidates = candidates;
    emit peerCandidatesChanged(candidates);
}

void SignalingClient::handlePeerLeft(const QString &reason)
{
    if (peer_.sid.isEmpty()) {
        return;
    }
    peer_ = {};
    access_.clear();
    clearPendingRequest();
    submittedRequestId_.clear();
    approvalTimer_.stop();
    setState(State::Ready);
    emit peerLeft(reason);
}

void SignalingClient::handleViewerRequest(
    const QJsonObject &message)
{
    if (role_ != Role::Host) {
        return;
    }
    const QString requestId =
        message.value(QStringLiteral("request_id")).toString();
    if (requestId.isEmpty()) {
        return;
    }
    const QString viewerName =
        message.value(QStringLiteral("name")).toString();
    const int expiresMs = std::clamp(
        message.value(QStringLiteral("expires_ms")).toInt(30000),
        1, 300000);
    timeoutTimer_.stop();
    cancelPairRetry();

    if (state_ == State::ViewerRequest
        && pendingRequestId_ == requestId) {
        pendingViewerName_ = viewerName;
        emit viewerRequested(
            requestId, viewerName, approvalRemainingMs());
        return;
    }

    peer_ = {};
    access_.clear();
    clearPendingRequest();
    requestOutcomeEmitted_ = false;
    pendingRequestId_ = requestId;
    pendingViewerName_ = viewerName;
    approvalTimer_.start(expiresMs);
    setState(State::ViewerRequest);
    emit viewerRequested(
        requestId, viewerName, approvalRemainingMs());
    emitLog(
        QStringLiteral("%1 请求远程控制")
            .arg(viewerName.isEmpty()
                     ? QStringLiteral("观看端")
                     : viewerName));
}

void SignalingClient::handleWaiting(
    const QJsonObject &message)
{
    if (role_ != Role::Viewer) {
        return;
    }
    const QString requestId =
        message.value(QStringLiteral("request_id")).toString();
    if (requestId.isEmpty()) {
        return;
    }
    const QString hostName =
        message.value(QStringLiteral("name")).toString();
    const int expiresMs = std::clamp(
        message.value(QStringLiteral("expires_ms")).toInt(30000),
        1, 300000);

    timeoutTimer_.stop();
    cancelPairRetry();
    if (state_ == State::WaitingApproval
        && pendingRequestId_ == requestId) {
        pendingHostName_ = hostName;
        emit waitingApproval(
            requestId, hostName, approvalRemainingMs());
        return;
    }

    peer_ = {};
    access_.clear();
    clearPendingRequest();
    requestOutcomeEmitted_ = false;
    pendingRequestId_ = requestId;
    pendingHostName_ = hostName;
    approvalTimer_.start(expiresMs);
    setState(State::WaitingApproval);
    emit waitingApproval(
        requestId, hostName, approvalRemainingMs());
    emitLog(
        hostName.isEmpty()
            ? QStringLiteral("等待对方确认连接")
            : QStringLiteral("等待 %1 确认连接").arg(hostName));
}

void SignalingClient::handleAccess(
    const QJsonObject &message)
{
    const QString access =
        message.value(QStringLiteral("access")).toString().trimmed();
    if (access != QStringLiteral("control")) {
        emitLog(QStringLiteral("信令服务器忽略了无效的访问权限"));
        return;
    }
    access_ = access;
    emit accessChanged(access_);
}

void SignalingClient::handleViewerRequestCleared(
    const QString &reason)
{
    if (role_ == Role::Host && state_ == State::ViewerRequest) {
        const bool hadRequest = !currentRequestId().isEmpty();
        clearPendingRequest();
        submittedRequestId_.clear();
        approvalTimer_.stop();
        setState(State::Ready);
        if (hadRequest) {
            emitViewerRequestClearedOnce(reason);
        }
        return;
    }
    if (role_ == Role::Viewer
        && state_ == State::WaitingApproval) {
        const bool hadRequest = !currentRequestId().isEmpty();
        clearPendingRequest();
        submittedRequestId_.clear();
        approvalTimer_.stop();
        setState(State::Negotiating);
        if (hadRequest) {
            emitViewerRequestClearedOnce(reason);
        }
        return;
    }
}

void SignalingClient::clearPendingRequest()
{
    pendingRequestId_.clear();
    pendingHostName_.clear();
    pendingViewerName_.clear();
}

void SignalingClient::emitViewerRequestClearedOnce(
    const QString &reason)
{
    if (requestOutcomeEmitted_) {
        return;
    }
    requestOutcomeEmitted_ = true;
    emit viewerRequestCleared(reason);
}

QString SignalingClient::currentRequestId() const
{
    return pendingRequestId_.isEmpty()
        ? submittedRequestId_
        : pendingRequestId_;
}

int SignalingClient::approvalRemainingMs() const
{
    const int remaining = approvalTimer_.remainingTime();
    return remaining > 0 ? remaining : 0;
}

void SignalingClient::onApprovalTimeout()
{
    if (pendingRequestId_.isEmpty()) {
        return;
    }
    if (role_ == Role::Host
        && state_ == State::ViewerRequest) {
        clearPendingRequest();
        submittedRequestId_.clear();
        approvalTimer_.stop();
        setState(State::Ready);
        emitViewerRequestClearedOnce(QStringLiteral("timeout"));
        emitLog(QStringLiteral("连接请求已超时"));
        return;
    }
    if (role_ == Role::Viewer
        && state_ == State::WaitingApproval) {
        clearPendingRequest();
        submittedRequestId_.clear();
        approvalTimer_.stop();
        cancelPairRetry();
        emitViewerRequestClearedOnce(QStringLiteral("timeout"));
        setError(
            QStringLiteral("approval_timeout"),
            QStringLiteral("对方未及时确认连接请求"));
    }
}

void SignalingClient::handleServerError(const QJsonObject &message)
{
    const QString reason =
        message.value(QStringLiteral("reason")).toString(
            QStringLiteral("unknown"));
    const QString detail =
        message.value(QStringLiteral("text")).toString();
    const QString requestId =
        message.value(QStringLiteral("request_id")).toString();
    if (!requestId.isEmpty()) {
        if (requestId != currentRequestId()) {
            emitLog(
                QStringLiteral("忽略了非当前连接请求的错误：%1")
                    .arg(reason));
            return;
        }
        const bool hadRequest = !currentRequestId().isEmpty();
        clearPendingRequest();
        submittedRequestId_.clear();
        approvalTimer_.stop();
        if (role_ == Role::Host) {
            setState(State::Ready);
            if (hadRequest) {
                emitViewerRequestClearedOnce(reason);
            }
            lastError_ = reason;
            lastErrorText_ = detail.isEmpty()
                ? QStringLiteral("连接请求处理失败")
                : detail;
            emitLog(lastErrorText_);
            emit errorOccurred(lastError_, lastErrorText_);
            return;
        }
        if (role_ == Role::Viewer) {
            if (hadRequest) {
                emitViewerRequestClearedOnce(reason);
            }
            if (reason == QStringLiteral("offline")
                || reason == QStringLiteral("not_found")
                || reason == QStringLiteral("host_busy")) {
                lastError_ = reason;
                lastErrorText_ = detail.isEmpty()
                    ? QStringLiteral("远端设备暂时无法连接")
                    : detail;
                setState(State::Negotiating);
                emitLog(lastErrorText_);
                emit errorOccurred(lastError_, lastErrorText_);
                schedulePairRetry();
                return;
            }
            setError(
                reason,
                detail.isEmpty()
                    ? QStringLiteral("连接请求未获批准")
                    : detail);
            return;
        }
        return;
    }
    if (reason == QStringLiteral("offline")
        || reason == QStringLiteral("not_found")
        || reason == QStringLiteral("host_busy")) {
        timeoutTimer_.stop();
        lastError_ = reason;
        lastErrorText_ = detail.isEmpty()
            ? QStringLiteral("远端设备暂时离线")
            : detail;
        emitLog(lastErrorText_);
        emit errorOccurred(lastError_, lastErrorText_);
        if (state_ != State::Negotiating) {
            setState(State::Negotiating);
        }
        schedulePairRetry();
        return;
    }
    terminalError_ = true;
    cancelPairRetry();
    timeoutTimer_.stop();
    setError(reason, detail);
}

void SignalingClient::sendHello()
{
    if (!connected() || role_ == Role::None) {
        return;
    }
    const QString type = role_ == Role::Host
        ? QStringLiteral("register")
        : QStringLiteral("connect");
    QJsonObject message = baseMessage(type);
    message.insert(
        QStringLiteral("t"), type);
    message.insert(
        QStringLiteral("code"), code_);
    message.insert(
        QStringLiteral("name"), name_);
    message.insert(
        QStringLiteral("cands"), candidatesToJson(candidates_));
    if (role_ == Role::Host) {
        message.insert(
            QStringLiteral("role"), QStringLiteral("host"));
    }
    sendMessage(message);
    timeoutTimer_.start(connectTimeoutMs_);
}

QJsonObject SignalingClient::baseMessage(const QString &type) const
{
    QJsonObject message{{QStringLiteral("t"), type}};
    if (!token_.isEmpty()) {
        message.insert(QStringLiteral("token"), token_);
    }
    return message;
}

SignalRelay SignalingClient::parseRelay(
    const QJsonValue &value, double mbps, quint32 tag)
{
    SignalRelay relay;
    const QJsonArray array = value.toArray();
    relay.host = relayHost(array);
    relay.port = relayPort(array);
    relay.tag = tag;
    relay.mbps = std::max(0.0, mbps);
    return relay;
}

QList<Candidate> SignalingClient::parseCandidates(
    const QJsonValue &value)
{
    QList<Candidate> candidates;
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject object = entry.toObject();
        const QString host =
            object.value(QStringLiteral("host")).toString().trimmed();
        const int port =
            object.value(QStringLiteral("port")).toInt();
        if (host.isEmpty() || port <= 0 || port > 65535) {
            continue;
        }
        QString kind =
            object.value(QStringLiteral("kind")).toString().trimmed();
        if (kind.isEmpty()) {
            kind = QStringLiteral("host");
        }
        candidates.append(Candidate{
            host, static_cast<quint16>(port), kind});
    }
    return candidates;
}

void SignalingClient::emitLog(const QString &message)
{
    emit logMessage(message);
}

} // namespace pd
