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
        if (!tcp_ && !udp_ && peers_.isEmpty() && pairs_.isEmpty()) {
            return;
        }

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
        } else if (kind == QStringLiteral("ping")) {
            sendObject(
                peer,
                QJsonObject{{QStringLiteral("t"),
                             QStringLiteral("pong")}});
        } else if (kind == QStringLiteral("cands")
                   || kind == QStringLiteral("relay_hello")
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
        const QString ip = remoteIp(peer);
        if (!limiter_.allow(ip)) {
            log(QStringLiteral("connect rate limited ip=%1").arg(ip));
            sendError(
                peer, QStringLiteral("rate_limited"),
                QStringLiteral("连接尝试过于频繁，稍后再试"));
            return;
        }

        const QString code =
            normalizeCode(message.value(QStringLiteral("code")).toString());
        Peer *agent = registry_.value(codeId(code), nullptr);
        if (!agent || agent->cleanedUp || !agent->socket
            || agent->socket->state()
                != QAbstractSocket::ConnectedState) {
            sendError(peer, QStringLiteral("offline"));
            return;
        }

        if (!agent->sid.isEmpty()) {
            teardownPair(agent->sid);
        }
        if (!peer->sid.isEmpty()) {
            teardownPair(peer->sid);
        }
        if (peer->role == QStringLiteral("agent")
            && !peer->codeId.isEmpty()
            && registry_.value(peer->codeId) == peer) {
            registry_.remove(peer->codeId);
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

        peer->role = QStringLiteral("viewer");
        peer->name = nameOf(message);
        peer->cands = candidatesOf(message);
        peer->sid = sid;
        agent->sid = sid;
        peer->partner = agent;
        agent->partner = peer;
        peer->relayTag = viewerTag;
        agent->relayTag = agentTag;

        auto *pair = new Pair{
            sid,
            agent,
            peer,
            agentTag,
            viewerTag,
            TokenBucket(options_.relayMbps),
        };
        pairs_.insert(sid, pair);
        tagMap_.insert(agentTag, pair);
        tagMap_.insert(viewerTag, pair);

        log(QStringLiteral("pair %1 agent=%2 viewer=%3")
                .arg(sid, socketAddress(agent->socket),
                     socketAddress(peer->socket)));

        const QJsonArray relay{
            publicHost_, static_cast<int>(relayPort())};
        sendObject(
            agent,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("viewer")},
                {QStringLiteral("sid"), sid},
                {QStringLiteral("cands"), peer->cands},
                {QStringLiteral("name"), peer->name},
                {QStringLiteral("relay"), relay},
                {QStringLiteral("relay_tag"),
                 static_cast<qint64>(agentTag)},
                {QStringLiteral("relay_mbps"), options_.relayMbps},
            });
        sendObject(
            peer,
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("peer")},
                {QStringLiteral("sid"), sid},
                {QStringLiteral("cands"), agent->cands},
                {QStringLiteral("name"), agent->name},
                {QStringLiteral("relay"), relay},
                {QStringLiteral("relay_tag"),
                 static_cast<qint64>(viewerTag)},
                {QStringLiteral("relay_mbps"), options_.relayMbps},
            });
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
        const QString &text = {})
    {
        QJsonObject error{
            {QStringLiteral("t"), QStringLiteral("error")},
            {QStringLiteral("reason"), reason},
        };
        if (!text.isEmpty()) {
            error.insert(QStringLiteral("text"), text);
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
