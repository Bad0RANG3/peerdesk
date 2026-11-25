#include "server/server.h"

#include "core/crypto.h"
#include "core/protocol.h"
#include "core/stun.h"
#include "core/util.h"

#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkInterface>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

namespace pd {
namespace {

constexpr int kAttemptMapSoftLimit = 4096;
constexpr int kAttemptMapHardLimit = 8192;

double monotonicSeconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
               Clock::now().time_since_epoch())
        .count();
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    const qsizetype length = std::max(left.size(), right.size());
    quint8 difference = static_cast<quint8>(
        static_cast<quint64>(left.size()) ^ static_cast<quint64>(right.size()));
    for (qsizetype index = 0; index < length; ++index) {
        const quint8 a = index < left.size()
            ? static_cast<quint8>(left[index])
            : 0;
        const quint8 b = index < right.size()
            ? static_cast<quint8>(right[index])
            : 0;
        difference = static_cast<quint8>(difference | (a ^ b));
    }
    return difference == 0;
}

QJsonArray candidatesOf(const QJsonObject &message)
{
    const QJsonValue value = message.value(QStringLiteral("cands"));
    return value.isArray() ? value.toArray() : QJsonArray{};
}

QString nameOf(const QJsonObject &message)
{
    const QJsonValue value = message.value(QStringLiteral("name"));
    if (!value.isString()) {
        return {};
    }
    const QString name = value.toString();
    if (name.size() <= 64) {
        return name;
    }
    std::u32string codePoints = name.toStdU32String();
    if (codePoints.size() > 64) {
        codePoints.resize(64);
    }
    return QString::fromStdU32String(codePoints);
}

QString socketAddress(const QTcpSocket *socket)
{
    if (!socket) {
        return QStringLiteral("?");
    }
    return QStringLiteral("%1:%2")
        .arg(socket->peerAddress().toString())
        .arg(socket->peerPort());
}

quint32 randomTag()
{
    const QByteArray raw = randomBytes(4);
    return qFromBigEndian<quint32>(raw.constData()) | 1U;
}

} // namespace

TokenBucket::TokenBucket(double mbps, double burstSeconds)
    : mbps_(std::max(0.0, mbps))
    , rate_(mbps_ * 125000.0)
    , capacity_(std::max(rate_ * std::max(0.0, burstSeconds), 8192.0))
    , tokens_(capacity_)
{
}

bool TokenBucket::allow(qint64 size, std::optional<double> now)
{
    const qint64 amount = std::max<qint64>(0, size);
    if (!enabled()) {
        ++passed_;
        bytes_ += static_cast<quint64>(amount);
        return true;
    }

    const double current = now.value_or(monotonicSeconds());
    if (current > last_) {
        tokens_ = std::min(
            capacity_, tokens_ + (current - last_) * rate_);
        last_ = current;
    }
    if (tokens_ < static_cast<double>(amount)) {
        ++dropped_;
        return false;
    }
    tokens_ -= static_cast<double>(amount);
    ++passed_;
    bytes_ += static_cast<quint64>(amount);
    return true;
}

ConnectRateLimiter::ConnectRateLimiter(
    int maxAttempts, double windowSeconds)
    : maxAttempts_(std::max(1, maxAttempts))
    , windowSeconds_(std::max(1.0, windowSeconds))
{
}

bool ConnectRateLimiter::allow(
    const QString &ip, std::optional<double> now)
{
    const double current = now.value_or(monotonicSeconds());
    QList<double> stamps = attempts_.value(ip);
    while (!stamps.isEmpty()
           && current - stamps.first() >= windowSeconds_) {
        stamps.removeFirst();
    }
    if (stamps.size() >= maxAttempts_) {
        attempts_.insert(ip, stamps);
        prune(current);
        return false;
    }
    stamps.append(current);
    attempts_.insert(ip, stamps);
    prune(current);
    return true;
}

void ConnectRateLimiter::clear()
{
    attempts_.clear();
}

void ConnectRateLimiter::prune(double now)
{
    if (attempts_.size() > kAttemptMapSoftLimit) {
        for (auto it = attempts_.begin(); it != attempts_.end();) {
            const QList<double> &stamps = it.value();
            if (stamps.isEmpty()
                || now - stamps.last() >= windowSeconds_) {
                it = attempts_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (attempts_.size() > kAttemptMapHardLimit) {
        int excess = attempts_.size() - kAttemptMapSoftLimit;
        auto it = attempts_.begin();
        while (excess > 0 && it != attempts_.end()) {
            it = attempts_.erase(it);
            --excess;
        }
    }
}

class SignalingServer::Impl {
public:
    struct Endpoint {
        QHostAddress address;
        quint16 port = 0;
        bool valid = false;
    };

    struct Peer : QObject {
        explicit Peer(QObject *parent)
            : QObject(parent)
        {
        }

        QTcpSocket *socket = nullptr;
        QString role = QStringLiteral("unknown");
        QString codeId;
        QString name;
        QJsonArray cands;
        QString sid;
        Peer *partner = nullptr;
        quint32 relayTag = 0;
        Endpoint udp;
        QByteArray lineBuffer;
        bool cleanedUp = false;
        bool closing = false;
    };

    struct Pair {
        QString sid;
        Peer *agent = nullptr;
        Peer *viewer = nullptr;
        quint32 agentTag = 0;
        quint32 viewerTag = 0;
        TokenBucket bucket;
    };

    struct Request {
        QString id;
        Peer *agent = nullptr;
        Peer *viewer = nullptr;
        double expiresAt = 0.0;
    };

    Impl(SignalingServer *owner, Options options)
        : owner_(owner)
        , options_(std::move(options))
        , limiter_(
              std::max(1, options_.connectMax),
              std::max(1.0, options_.connectWindow))
    {
        options_.relayMbps = std::max(0.0, options_.relayMbps);
        options_.connectMax = limiter_.maxAttempts();
        options_.connectWindow = limiter_.windowSeconds();
        options_.approvalTimeoutMs =
            std::max(200, options_.approvalTimeoutMs);
        approvalTimer_.setInterval(200);
        approvalTimer_.setSingleShot(false);
        QObject::connect(
            &approvalTimer_, &QTimer::timeout, owner_,
            [this] { expireRequests(); });
    }

    ~Impl()
    {
        stop();
    }

    bool start(QString *error)
    {
        stop();

        const QHostAddress bindAddress = resolveBindAddress();
        if (bindAddress.isNull()) {
            if (error) {
                *error = QStringLiteral("无法解析监听地址：%1")
                             .arg(options_.host);
            }
            return false;
        }

        auto udp = std::make_unique<QUdpSocket>();
        if (!udp->bind(bindAddress, options_.udpPort)) {
            if (error) {
                *error = QStringLiteral("UDP %1:%2 绑定失败：%3")
                             .arg(bindAddress.toString())
                             .arg(options_.udpPort)
                             .arg(udp->errorString());
            }
            return false;
        }
        udp->setSocketOption(
            QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        udp->setSocketOption(
            QAbstractSocket::SendBufferSizeSocketOption, 4 * 1024 * 1024);

        auto tcp = std::make_unique<QTcpServer>();
        tcp->setMaxPendingConnections(512);
        QObject::connect(
            tcp.get(), &QTcpServer::newConnection, owner_,
            [this] { acceptConnections(); });
        if (!tcp->listen(bindAddress, options_.port)) {
            if (error) {
                *error = QStringLiteral("TCP %1:%2 监听失败：%3")
                             .arg(bindAddress.toString())
                             .arg(options_.port)
                             .arg(tcp->errorString());
            }
            udp->close();
            return false;
        }

        udp_ = std::move(udp);
        tcp_ = std::move(tcp);
        QObject::connect(
            udp_.get(), &QUdpSocket::readyRead, owner_,
            [this] { readDatagrams(); });
        approvalTimer_.start();

        publicHost_ = resolvePublicHost();
        log(QStringLiteral(
                "signaling on %1:%2, udp/relay+stun on %3:%4")
                .arg(options_.host)
                .arg(signalingPort())
                .arg(options_.host)
                .arg(relayPort()));
        log(QStringLiteral("relay address advertised to peers: %1:%2")
                .arg(publicHost_)
                .arg(relayPort()));
        if (!options_.token.isEmpty()) {
            log(QStringLiteral("handshake token required (%1 chars)")
                    .arg(options_.token.size()));
        }
        if (options_.relayMbps > 0.0) {
            log(QStringLiteral("relay limited to %1 Mbps per session")
                    .arg(options_.relayMbps, 0, 'f', 1));
        }
        return true;
    }

    void stop()
    {
        if (!tcp_ && !udp_ && peers_.isEmpty() && pairs_.isEmpty()
            && requests_.isEmpty()) {
            return;
        }

        approvalTimer_.stop();
        clearAllRequests();
        if (tcp_) {
            tcp_->close();
            tcp_.reset();
        }
        if (udp_) {
            udp_->close();
            udp_.reset();
        }

        for (Pair *pair : std::as_const(pairs_)) {
            delete pair;
        }
        pairs_.clear();
        tagMap_.clear();
        registry_.clear();

        const QList<Peer *> peers = peers_.values();
        peers_.clear();
        for (Peer *peer : peers) {
            peer->cleanedUp = true;
            if (peer->socket) {
                QObject::disconnect(peer->socket, nullptr, peer, nullptr);
                peer->socket->abort();
            }
            peer->deleteLater();
        }
        publicHost_.clear();
    }

    [[nodiscard]] bool isListening() const
    {
        return tcp_ && udp_ && tcp_->isListening()
            && udp_->state() == QAbstractSocket::BoundState;
    }

    [[nodiscard]] quint16 signalingPort() const
    {
        return tcp_ ? tcp_->serverPort() : 0;
    }

    [[nodiscard]] quint16 relayPort() const
    {
        return udp_ ? udp_->localPort() : 0;
    }

    [[nodiscard]] QString publicHost() const { return publicHost_; }

    [[nodiscard]] Options options() const { return options_; }

private:
    QHostAddress resolveBindAddress() const
    {
        const QString host = options_.host.trimmed();
        if (host.isEmpty() || host == QStringLiteral("0.0.0.0")) {
            return QHostAddress(QHostAddress::AnyIPv4);
        }
        if (host == QStringLiteral("::")) {
            return QHostAddress(QHostAddress::AnyIPv6);
        }

        QHostAddress address;
        if (address.setAddress(host)) {
            return address;
        }
        if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive)
            == 0) {
            return QHostAddress(QHostAddress::LocalHost);
        }
        const QHostInfo info = QHostInfo::fromName(host);
        for (const QHostAddress &candidate : info.addresses()) {
            if (!candidate.isNull()) {
                return candidate;
            }
        }
        return {};
    }

    [[nodiscard]] QString resolvePublicHost() const
    {
        const QString host = options_.host.trimmed();
        if (!host.isEmpty() && host != QStringLiteral("0.0.0.0")
            && host != QStringLiteral("::")) {
            return host;
        }

        QUdpSocket probe;
        probe.connectToHost(
            QHostAddress(QStringLiteral("223.5.5.5")), 53);
        if (probe.waitForConnected(200)) {
            const QHostAddress local = probe.localAddress();
            if (!local.isNull()
                && local != QHostAddress(QHostAddress::AnyIPv4)
                && local != QHostAddress(QHostAddress::AnyIPv6)) {
                return local.toString();
            }
        }

        const QList<QNetworkInterface> interfaces =
            QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &interface : interfaces) {
            const auto flags = interface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp)
                || !flags.testFlag(QNetworkInterface::IsRunning)
                || flags.testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const QNetworkAddressEntry &entry :
                 interface.addressEntries()) {
                const QHostAddress address = entry.ip();
                if (address.protocol()
                        == QAbstractSocket::IPv4Protocol
                    && !address.isLoopback()) {
                    return address.toString();
                }
            }
        }
        return QStringLiteral("127.0.0.1");
    }

    void acceptConnections()
    {
        if (!tcp_) {
            return;
        }
        while (tcp_->hasPendingConnections()) {
            QTcpSocket *socket = tcp_->nextPendingConnection();
            if (!socket) {
                break;
            }

            auto *peer = new Peer(owner_);
            peer->socket = socket;
            socket->setParent(peer);
            peers_.insert(peer);
            QObject::connect(
                socket, &QTcpSocket::readyRead, peer,
                [this, peer] { readClient(peer); });
            QObject::connect(
                socket, &QTcpSocket::disconnected, peer,
                [this, peer] { clientDisconnected(peer); });
            log(QStringLiteral("tcp connect %1")
                    .arg(socketAddress(socket)));
        }
    }

    void readClient(Peer *peer)
    {
        if (!peer || peer->cleanedUp || !peer->socket) {
            return;
        }
        peer->lineBuffer.append(peer->socket->readAll());

        for (;;) {
            const qsizetype newline =
                peer->lineBuffer.indexOf('\n');
            if (newline < 0) {
                if (peer->lineBuffer.size() > kMaxSignalLine) {
                    closePeer(peer);
                }
                return;
            }

            const QByteArray line =
                peer->lineBuffer.left(newline);
            peer->lineBuffer.remove(0, newline + 1);
            if (line.size() + 1 > kMaxSignalLine) {
                closePeer(peer);
                return;
            }

            QJsonParseError parseError;
            const QJsonDocument document =
                QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError
                || !document.isObject()) {
                continue;
            }
            dispatch(peer, document.object());
            if (peer->cleanedUp || peer->closing) {
                return;
            }
        }
    }

    void clientDisconnected(Peer *peer)
    {
        if (!peer || peer->cleanedUp) {
            return;
        }
        dropPeer(peer);
        peers_.remove(peer);
        peer->deleteLater();
    }

    void closePeer(Peer *peer)
    {
        if (!peer || peer->closing) {
            return;
        }
        peer->closing = true;
        if (peer->socket) {
            peer->socket->abort();
        }
    }

    bool authenticate(Peer *peer, const QJsonObject &message)
    {
        if (options_.token.isEmpty()) {
            return true;
        }
        const QJsonValue given = message.value(QStringLiteral("token"));
        if (given.isString()
            && constantTimeEqual(
                given.toString().toUtf8(), options_.token.toUtf8())) {
            return true;
        }

        log(QStringLiteral("bad token from %1 (%2)")
                .arg(socketAddress(peer->socket))
                .arg(remoteIp(peer)));
        sendError(
            peer, QStringLiteral("bad_token"),
            QStringLiteral("服务器令牌不匹配"));
        return false;
    }

    [[nodiscard]] QString remoteIp(const Peer *peer) const
    {
        if (!peer || !peer->socket) {
            return QStringLiteral("?");
        }
        return peer->socket->peerAddress().toString();
    }

    void dispatch(Peer *peer, const QJsonObject &message)
    {
        const QString kind =
            message.value(QStringLiteral("t")).toString();
        if (kind == QStringLiteral("register")) {
            handleRegister(peer, message);
        } else if (kind == QStringLiteral("connect")) {
            handleConnect(peer, message);
        } else if (kind == QStringLiteral("reconnect")) {
            handleReconnect(peer);
        } else if (kind == QStringLiteral("approve")) {
            handleApprove(peer, message);
        } else if (kind == QStringLiteral("reject")) {
            handleReject(peer, message);
        } else if (kind == QStringLiteral("cancel")) {
            handleCancel(peer, message);
        } else if (kind == QStringLiteral("ping")) {
            sendObject(
                peer,
                QJsonObject{{QStringLiteral("t"),
                             QStringLiteral("pong")}});
        } else if (kind == QStringLiteral("cands")) {
            peer->cands = candidatesOf(message);
            if (peer->partner) {
                QJsonObject forwarded = message;
                forwarded.insert(
                    QStringLiteral("t"),
                    QStringLiteral("peer_cands"));
                sendObject(peer->partner, forwarded);
            }
        } else if (kind == QStringLiteral("relay_hello")
                   || kind == QStringLiteral("bye")) {
            if (peer->partner) {
                QJsonObject forwarded = message;
                forwarded.insert(
                    QStringLiteral("t"),
                    QStringLiteral("peer_") + kind);
                sendObject(peer->partner, forwarded);
            }
        } else {
            sendError(peer, QStringLiteral("unknown_type"));
        }
    }

    void handleRegister(Peer *peer, const QJsonObject &message)
    {
        if (!authenticate(peer, message)) {
            return;
        }

        const QString code =
            normalizeCode(message.value(QStringLiteral("code")).toString());
        if (!codeIsValid(code)) {
            sendError(peer, QStringLiteral("bad_code"));
            return;
        }

        cancelPendingForViewer(peer, true);
        cancelPendingForAgent(peer, true);
        if (peer->role == QStringLiteral("viewer") && peer->partner) {
            teardownPair(peer->sid);
        }
        if (peer->role == QStringLiteral("agent")
            && !peer->codeId.isEmpty()
            && registry_.value(peer->codeId) == peer) {
            registry_.remove(peer->codeId);
        }

        peer->role = QStringLiteral("agent");
        peer->codeId = codeId(code);
        peer->name = nameOf(message);
        peer->cands = candidatesOf(message);
        registry_.insert(peer->codeId, peer);
        log(QStringLiteral("agent registered %1 name=%2 cands=%3")
                .arg(peer->codeId.left(8), peer->name)
                .arg(peer->cands.size()));

        sendObject(
            peer,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("registered")},
                {QStringLiteral("code"), formatCode(code)},
                {QStringLiteral("relay"),
                 QJsonArray{publicHost_,
                            static_cast<int>(relayPort())}},
                {QStringLiteral("server_time"),
                 QDateTime::currentSecsSinceEpoch()},
                {QStringLiteral("token_required"),
                 !options_.token.isEmpty()},
                {QStringLiteral("relay_mbps"), options_.relayMbps},
            });
    }

    void handleConnect(Peer *peer, const QJsonObject &message)
    {
        if (!authenticate(peer, message)) {
            return;
        }
        const QString code =
            normalizeCode(message.value(QStringLiteral("code")).toString());
        if (!codeIsValid(code)) {
            sendError(peer, QStringLiteral("bad_code"));
            return;
        }
        Peer *agent = registry_.value(codeId(code), nullptr);
        const bool agentOnline = peerOnline(agent);

        Request *pending = requestByViewer_.value(peer, nullptr);
        if (agentOnline && pending && pending->agent == agent) {
            peer->role = QStringLiteral("viewer");
            peer->name = nameOf(message);
            peer->cands = candidatesOf(message);
            sendPendingRequest(pending);
            return;
        }

        const QString ip = remoteIp(peer);
        if (!limiter_.allow(ip)) {
            log(QStringLiteral("connect rate limited ip=%1").arg(ip));
            sendError(
                peer, QStringLiteral("rate_limited"),
                QStringLiteral("连接尝试过于频繁，稍后再试"));
            return;
        }

        if (!agentOnline) {
            Request *existing = requestByViewer_.value(peer, nullptr);
            if (existing) {
                const QString requestId = existing->id;
                if (peerOnline(existing->agent)) {
                    sendObject(
                        existing->agent,
                        QJsonObject{
                            {QStringLiteral("t"),
                             QStringLiteral("viewer_cancelled")},
                            {QStringLiteral("request_id"), requestId},
                        });
                }
                sendError(
                    peer, QStringLiteral("offline"),
                    QStringLiteral("对方设备暂时离线"),
                    requestId);
                clearRequest(existing);
                return;
            }
            sendError(peer, QStringLiteral("offline"));
            return;
        }
        if (peer == agent) {
            sendError(
                peer, QStringLiteral("bad_request"),
                QStringLiteral("不能连接到本机连接码"));
            return;
        }

        if (peer->partner) {
            sendObject(
                peer->partner,
                QJsonObject{{QStringLiteral("t"),
                             QStringLiteral("peer_gone")}});
        }
        if (!peer->sid.isEmpty()) {
            teardownPair(peer->sid);
        }
        if (pending) {
            cancelPendingForViewer(peer, true);
        }

        if (agent->partner || requestByAgent_.contains(agent)) {
            sendError(
                peer, QStringLiteral("host_busy"),
                QStringLiteral("对方正在处理其他连接请求"));
            return;
        }

        if (peer->role == QStringLiteral("agent")
            && !peer->codeId.isEmpty()
            && registry_.value(peer->codeId) == peer) {
            registry_.remove(peer->codeId);
        }

        auto *request = new Request;
        do {
            request->id =
                QString::fromLatin1(randomBytes(8).toHex());
        } while (requests_.contains(request->id));
        request->agent = agent;
        request->viewer = peer;
        request->expiresAt =
            monotonicSeconds()
            + options_.approvalTimeoutMs / 1000.0;
        requests_.insert(request->id, request);
        requestByViewer_.insert(peer, request);
        requestByAgent_.insert(agent, request);

        peer->role = QStringLiteral("viewer");
        peer->name = nameOf(message);
        peer->cands = candidatesOf(message);
        sendPendingRequest(request);
        log(QStringLiteral("approval requested %1 agent=%2 viewer=%3")
                .arg(request->id.left(8),
                     socketAddress(agent->socket),
                     socketAddress(peer->socket)));
    }

    void sendPendingRequest(Request *request)
    {
        if (!request || request->agent == nullptr
            || request->viewer == nullptr) {
            return;
        }
        const int remainingMs = std::max(
            0,
            static_cast<int>(std::ceil(
                (request->expiresAt - monotonicSeconds()) * 1000.0)));
        sendObject(
            request->agent,
            QJsonObject{
                {QStringLiteral("t"),
                 QStringLiteral("viewer_request")},
                {QStringLiteral("request_id"), request->id},
                {QStringLiteral("name"), request->viewer->name},
                {QStringLiteral("expires_ms"), remainingMs},
            });
        sendObject(
            request->viewer,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("waiting")},
                {QStringLiteral("request_id"), request->id},
                {QStringLiteral("name"), request->agent->name},
                {QStringLiteral("expires_ms"), remainingMs},
            });
    }

    void handleApprove(Peer *peer, const QJsonObject &message)
    {
        if (!authenticate(peer, message)) {
            return;
        }
        const QString requestId =
            message.value(QStringLiteral("request_id")).toString();
        Request *request = requests_.value(requestId, nullptr);
        if (!request || request->agent != peer) {
            sendError(
                peer, QStringLiteral("bad_request"),
                QStringLiteral("连接请求已失效"), requestId);
            return;
        }
        const QString access =
            message.value(QStringLiteral("access")).toString();
        if (access != QStringLiteral("control")) {
            sendError(
                peer, QStringLiteral("bad_access"),
                QStringLiteral("仅支持控制访问"), request->id);
            sendError(
                request->viewer, QStringLiteral("bad_access"),
                QStringLiteral("对方无法授予控制权限"),
                request->id);
            clearRequest(request);
            return;
        }

        Peer *agent = request->agent;
        Peer *viewer = request->viewer;
        if (!peerOnline(viewer)) {
            sendObject(
                peer,
                QJsonObject{
                    {QStringLiteral("t"),
                     QStringLiteral("viewer_cancelled")},
                    {QStringLiteral("request_id"), request->id},
                });
            clearRequest(request);
            return;
        }
        if (!peerOnline(agent) || agent->partner
            || viewer->partner
            || requestByAgent_.value(agent, nullptr) != request
            || requestByViewer_.value(viewer, nullptr) != request) {
            sendError(
                peer, QStringLiteral("host_busy"),
                QStringLiteral("连接请求已失效"), request->id);
            sendError(
                viewer, QStringLiteral("host_busy"),
                QStringLiteral("对方正在处理其他连接请求"),
                request->id);
            clearRequest(request);
            return;
        }

        QString sid;
        do {
            sid = QString::fromLatin1(randomBytes(8).toHex());
        } while (pairs_.contains(sid));

        quint32 agentTag = 0;
        quint32 viewerTag = 0;
        do {
            agentTag = randomTag();
        } while (tagMap_.contains(agentTag));
        do {
            viewerTag = randomTag();
        } while (viewerTag == agentTag || tagMap_.contains(viewerTag));

        viewer->role = QStringLiteral("viewer");
        agent->sid = sid;
        viewer->sid = sid;
        agent->partner = viewer;
        viewer->partner = agent;
        agent->relayTag = agentTag;
        viewer->relayTag = viewerTag;

        auto *pair = new Pair{
            sid,
            agent,
            viewer,
            agentTag,
            viewerTag,
            TokenBucket(options_.relayMbps),
        };
        pairs_.insert(sid, pair);
        tagMap_.insert(agentTag, pair);
        tagMap_.insert(viewerTag, pair);

        log(QStringLiteral("pair %1 agent=%2 viewer=%3")
                .arg(sid, socketAddress(agent->socket),
                     socketAddress(viewer->socket)));
        sendPairMessages(pair);
        clearRequest(request);
    }

    void handleReject(Peer *peer, const QJsonObject &message)
    {
        if (!authenticate(peer, message)) {
            return;
        }
        const QString requestId =
            message.value(QStringLiteral("request_id")).toString();
        Request *request = requests_.value(requestId, nullptr);
        if (!request || request->agent != peer) {
            sendError(
                peer, QStringLiteral("bad_request"),
                QStringLiteral("连接请求已失效"), requestId);
            return;
        }
        QString reason =
            message.value(QStringLiteral("reason")).toString().trimmed();
        if (reason.isEmpty()) {
            reason = QStringLiteral("对方拒绝了连接请求");
        }
        sendError(
            request->viewer, QStringLiteral("rejected"), reason,
            request->id);
        log(QStringLiteral("approval rejected %1")
                .arg(request->id.left(8)));
        clearRequest(request);
    }

    void handleCancel(Peer *peer, const QJsonObject &message)
    {
        if (!authenticate(peer, message)) {
            return;
        }
        Request *request = requestByViewer_.value(peer, nullptr);
        if (!request) {
            sendError(
                peer, QStringLiteral("bad_request"),
                QStringLiteral("连接请求已失效"),
                message.value(
                    QStringLiteral("request_id")).toString());
            return;
        }
        const QString requestId =
            message.value(QStringLiteral("request_id")).toString();
        if (requestId.isEmpty() || requestId != request->id) {
            sendError(
                peer, QStringLiteral("bad_request"),
                QStringLiteral("连接请求编号不匹配"), requestId);
            return;
        }
        sendObject(
            request->agent,
            QJsonObject{
                {QStringLiteral("t"),
                 QStringLiteral("viewer_cancelled")},
                {QStringLiteral("request_id"), request->id},
            });
        log(QStringLiteral("approval cancelled %1")
                .arg(request->id.left(8)));
        clearRequest(request);
    }

    void sendPairMessages(Pair *pair)
    {
        if (!pair || !pair->agent || !pair->viewer) {
            return;
        }
        const QJsonArray relay{
            publicHost_, static_cast<int>(relayPort())};
        sendObject(
            pair->agent,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("viewer")},
                {QStringLiteral("sid"), pair->sid},
                {QStringLiteral("cands"), pair->viewer->cands},
                {QStringLiteral("name"), pair->viewer->name},
                {QStringLiteral("relay"), relay},
                {QStringLiteral("relay_tag"),
                 static_cast<qint64>(pair->agentTag)},
                {QStringLiteral("relay_mbps"), options_.relayMbps},
            });
        sendObject(
            pair->viewer,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("peer")},
                {QStringLiteral("sid"), pair->sid},
                {QStringLiteral("cands"), pair->agent->cands},
                {QStringLiteral("name"), pair->agent->name},
                {QStringLiteral("relay"), relay},
                {QStringLiteral("relay_tag"),
                 static_cast<qint64>(pair->viewerTag)},
                {QStringLiteral("relay_mbps"), options_.relayMbps},
            });
        const QJsonObject access{
            {QStringLiteral("t"), QStringLiteral("access")},
            {QStringLiteral("access"), QStringLiteral("control")},
        };
        sendObject(pair->agent, access);
        sendObject(pair->viewer, access);
    }

    [[nodiscard]] bool peerOnline(const Peer *peer) const
    {
        return peer && !peer->cleanedUp && !peer->closing
            && peer->socket
            && peer->socket->state()
                == QAbstractSocket::ConnectedState;
    }

    void cancelPendingForViewer(
        Peer *viewer, bool notifyAgent)
    {
        Request *request = requestByViewer_.value(viewer, nullptr);
        if (!request) {
            return;
        }
        if (notifyAgent && request->agent) {
            sendObject(
                request->agent,
                QJsonObject{
                    {QStringLiteral("t"),
                     QStringLiteral("viewer_cancelled")},
                    {QStringLiteral("request_id"), request->id},
                });
        }
        clearRequest(request);
    }

    void cancelPendingForAgent(
        Peer *agent, bool notifyViewer)
    {
        Request *request = requestByAgent_.value(agent, nullptr);
        if (!request) {
            return;
        }
        if (notifyViewer && request->viewer) {
            sendError(
                request->viewer, QStringLiteral("offline"),
                QStringLiteral("对方设备暂时离线"),
                request->id);
        }
        clearRequest(request);
    }

    void clearRequest(Request *request)
    {
        if (!request) {
            return;
        }
        requests_.remove(request->id);
        if (requestByViewer_.value(request->viewer, nullptr) == request) {
            requestByViewer_.remove(request->viewer);
        }
        if (requestByAgent_.value(request->agent, nullptr) == request) {
            requestByAgent_.remove(request->agent);
        }
        delete request;
    }

    void clearAllRequests()
    {
        const QList<Request *> requests = requests_.values();
        requests_.clear();
        requestByViewer_.clear();
        requestByAgent_.clear();
        for (Request *request : requests) {
            delete request;
        }
    }

    void expireRequests()
    {
        const double now = monotonicSeconds();
        QList<Request *> expired;
        for (Request *request : std::as_const(requests_)) {
            if (request->expiresAt <= now) {
                expired.append(request);
            }
        }
        for (Request *request : expired) {
            if (peerOnline(request->agent)) {
                sendObject(
                    request->agent,
                    QJsonObject{
                        {QStringLiteral("t"),
                         QStringLiteral("viewer_request_expired")},
                        {QStringLiteral("request_id"), request->id},
                    });
            }
            if (peerOnline(request->viewer)) {
                sendError(
                    request->viewer,
                    QStringLiteral("approval_timeout"),
                    QStringLiteral("对方未及时确认连接请求"),
                    request->id);
            }
            log(QStringLiteral("approval expired %1")
                    .arg(request->id.left(8)));
            clearRequest(request);
        }
    }

    void handleReconnect(Peer *peer)
    {
        if (peer->role == QStringLiteral("viewer")
            && peer->partner) {
            sendObject(
                peer->partner,
                QJsonObject{{QStringLiteral("t"),
                             QStringLiteral("viewer_gone")}});
        }
    }

    void sendObject(Peer *peer, const QJsonObject &object)
    {
        if (!peer || peer->cleanedUp || !peer->socket
            || peer->socket->state()
                != QAbstractSocket::ConnectedState) {
            return;
        }
        QByteArray wire =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        wire.append('\n');
        peer->socket->write(wire);
    }

    void sendError(
        Peer *peer, const QString &reason,
        const QString &text = {},
        const QString &requestId = {})
    {
        QJsonObject error{
            {QStringLiteral("t"), QStringLiteral("error")},
            {QStringLiteral("reason"), reason},
        };
        if (!text.isEmpty()) {
            error.insert(QStringLiteral("text"), text);
        }
        if (!requestId.isEmpty()) {
            error.insert(
                QStringLiteral("request_id"), requestId);
        }
        sendObject(peer, error);
    }

    void dropPeer(Peer *peer)
    {
        if (peer->cleanedUp) {
            return;
        }
        peer->cleanedUp = true;
        log(QStringLiteral("tcp disconnect %1 role=%2")
                .arg(socketAddress(peer->socket), peer->role));

        cancelPendingForViewer(peer, true);
        cancelPendingForAgent(peer, true);
        if (peer->role == QStringLiteral("agent")
            && registry_.value(peer->codeId) == peer) {
            registry_.remove(peer->codeId);
        }
        if (peer->partner) {
            sendObject(
                peer->partner,
                QJsonObject{{QStringLiteral("t"),
                             QStringLiteral("peer_gone")}});
            peer->partner->partner = nullptr;
            peer->partner = nullptr;
        }
        if (!peer->sid.isEmpty()) {
            teardownPair(peer->sid);
        }
        peer->sid.clear();
        peer->relayTag = 0;
        peer->udp = {};
    }

    void teardownPair(const QString &sid)
    {
        Pair *pair = pairs_.take(sid);
        if (!pair) {
            return;
        }
        tagMap_.remove(pair->agentTag);
        tagMap_.remove(pair->viewerTag);
        if (pair->bucket.dropped() > 0 || pair->bucket.enabled()) {
            log(QStringLiteral(
                    "pair %1 relay: %2 packets / %3 MB, rate-limit drops %4")
                    .arg(sid.left(8))
                    .arg(pair->bucket.passed())
                    .arg(static_cast<double>(pair->bucket.bytes())
                             / (1024.0 * 1024.0),
                         0, 'f', 1)
                    .arg(pair->bucket.dropped()));
        }
        for (Peer *peer : {pair->agent, pair->viewer}) {
            if (!peer) {
                continue;
            }
            peer->partner = nullptr;
            peer->sid.clear();
            peer->relayTag = 0;
        }
        delete pair;
    }

    void readDatagrams()
    {
        if (!udp_) {
            return;
        }
        while (udp_->hasPendingDatagrams()) {
            const qint64 pending = udp_->pendingDatagramSize();
            if (pending < 0) {
                break;
            }
            QByteArray datagram(
                static_cast<qsizetype>(pending), '\0');
            QHostAddress sender;
            quint16 senderPort = 0;
            const qint64 received = udp_->readDatagram(
                datagram.data(), datagram.size(), &sender,
                &senderPort);
            if (received < 0) {
                continue;
            }
            datagram.truncate(static_cast<qsizetype>(received));

            if (datagram.size() >= 20
                && datagram[0] == '\0' && datagram[1] == '\1') {
                handleStun(datagram, sender, senderPort);
                continue;
            }
            const auto relay = unwrapRelay(datagram);
            if (!relay.has_value()) {
                continue;
            }
            handleRelay(
                relay->first, relay->second, sender, senderPort);
        }
    }

    void handleStun(
        const QByteArray &datagram, const QHostAddress &sender,
        quint16 senderPort)
    {
        const auto message = stun::parseMessage(datagram);
        if (!message.has_value()
            || message->type != stun::kBindingRequest) {
            return;
        }
        const QByteArray response = stun::buildBindingResponse(
            message->transactionId, sender, senderPort);
        if (!response.isEmpty() && udp_) {
            udp_->writeDatagram(response, sender, senderPort);
        }
    }

    void handleRelay(
        quint32 tag, const QByteArray &packet,
        const QHostAddress &sender, quint16 senderPort)
    {
        Pair *pair = tagMap_.value(tag, nullptr);
        if (!pair) {
            return;
        }

        Endpoint *source = nullptr;
        Endpoint *destination = nullptr;
        if (tag == pair->agentTag) {
            source = &pair->agent->udp;
            destination = &pair->viewer->udp;
        } else {
            source = &pair->viewer->udp;
            destination = &pair->agent->udp;
        }
        source->address = sender;
        source->port = senderPort;
        source->valid = senderPort != 0;
        if (!destination->valid || !udp_) {
            return;
        }
        if (!pair->bucket.allow(packet.size())) {
            return;
        }
        udp_->writeDatagram(
            packet, destination->address, destination->port);
    }

    void log(const QString &message)
    {
        emit owner_->logMessage(message);
    }

    SignalingServer *owner_;
    Options options_;
    ConnectRateLimiter limiter_;
    std::unique_ptr<QTcpServer> tcp_;
    std::unique_ptr<QUdpSocket> udp_;
    QSet<Peer *> peers_;
    QHash<QString, Peer *> registry_;
    QHash<QString, Pair *> pairs_;
    QHash<quint32, Pair *> tagMap_;
    QHash<QString, Request *> requests_;
    QHash<Peer *, Request *> requestByViewer_;
    QHash<Peer *, Request *> requestByAgent_;
    QTimer approvalTimer_;
    QString publicHost_;
};

SignalingServer::SignalingServer(QObject *parent)
    : SignalingServer(Options{}, parent)
{
}

SignalingServer::SignalingServer(Options options, QObject *parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this, std::move(options)))
{
}

SignalingServer::~SignalingServer() = default;

bool SignalingServer::start(QString *error)
{
    return impl_->start(error);
}

void SignalingServer::stop()
{
    impl_->stop();
}

bool SignalingServer::isListening() const
{
    return impl_->isListening();
}

quint16 SignalingServer::signalingPort() const
{
    return impl_->signalingPort();
}

quint16 SignalingServer::relayPort() const
{
    return impl_->relayPort();
}

QString SignalingServer::publicHost() const
{
    return impl_->publicHost();
}

SignalingServer::Options SignalingServer::options() const
{
    return impl_->options();
}

} // namespace pd
