#include "transport/peer_link.h"

#include "core/stun.h"

#include <QCryptographicHash>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QDateTime>
#include <QSet>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace pd {
namespace {

QString endpointText(const QHostAddress &address, quint16 port)
{
    const QString host = address.toString();
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    }
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

QString candidateKey(const Candidate &candidate)
{
    return QStringLiteral("%1|%2")
        .arg(candidate.host.toCaseFolded())
        .arg(candidate.port);
}

bool isAnyAddress(const QHostAddress &address)
{
    return address.isNull()
        || address == QHostAddress(QHostAddress::AnyIPv4)
        || address == QHostAddress(QHostAddress::AnyIPv6)
        || address == QHostAddress(QHostAddress::Any);
}

} // namespace

PeerLink::PeerLink(bool isHost, const QByteArray &psk, QObject *parent)
    : QObject(parent)
    , isHost_(isHost)
    , psk_(psk)
    , timer_(new QTimer(this))
    , keyPair_(KeyPair::generate())
    , feedback_(&udpIo_, &authenticatedPackets_)
{
    qRegisterMetaType<pd::LinkStats>("pd::LinkStats");

    connect(
        &videoSend_,
        &VideoSendPipeline::packetsReady,
        this,
        &PeerLink::onVideoPacketsReady);
    connect(
        &videoSend_,
        &VideoSendPipeline::packetizationFailed,
        this,
        &PeerLink::emitLog);
    connect(
        &videoReceive_,
        &VideoReceivePipeline::frameReady,
        this,
        &PeerLink::onVideoFrameReady);
    connect(
        &videoReceive_,
        &VideoReceivePipeline::nackReady,
        this,
        [this](quint32 frameId, const QList<quint16> &missing) {
            feedback_.submitNack(
                feedback_.generation(),
                frameId,
                missing);
        });
    connect(
        &videoReceive_,
        &VideoReceivePipeline::keyframeRequested,
        this,
        [this]() {
            feedback_.submitKeyframe(
                feedback_.generation());
        });
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::controlMessageReceived,
        this,
        &PeerLink::controlMessageReceived);
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::videoDataReceived,
        this,
        [this](const QByteArray &payload, bool keyframe) {
            videoReceive_.submitData(payload, keyframe);
        });
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::videoParityReceived,
        this,
        [this](const QByteArray &payload) {
            videoReceive_.submitParity(payload);
        });
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::audioReceived,
        this,
        &PeerLink::audioReceived);
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::videoRetransmitRequested,
        &videoSend_,
        &VideoSendPipeline::retransmit);
    authenticatedPackets_.setWireCommandHandler(
        [this](quint64 authGeneration, const LinkWireCommand &command) {
            feedback_.submitWireCommand(
                feedback_.generation(),
                authGeneration,
                command);
        });
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::rttMeasured,
        this,
        [this](double rttMs) {
            stats_.rttMs = std::max(0.0, rttMs);
        });
    connect(
        &authenticatedPackets_,
        &AuthenticatedPacketPipeline::peerByeReceived,
        this,
        [this]() {
            emitLog(QStringLiteral("peer closed"));
            established_ = false;
            updateFeedbackRoute();
            setState(QStringLiteral("closed"));
        });
    connect(
        &udpIo_,
        &UdpIoPipeline::packetReceived,
        this,
        [this](const UdpReceivePacket &packet) {
            if (closing_) {
                return;
            }
            processDatagram(packet);
        });
    connect(
        &udpIo_,
        &UdpIoPipeline::logMessage,
        this,
        &PeerLink::emitLog);

    sessionId_ = randomBytes(4);
    const QByteArray tagBytes = randomBytes(4);
    sessionTag_ = qFromBigEndian<quint32>(tagBytes.constData());

    connect(timer_, &QTimer::timeout, this, [this]() { tick(); });
    timer_->setInterval(kLinkTickMs);
    timer_->setTimerType(Qt::PreciseTimer);
}

PeerLink::~PeerLink()
{
    if (!closing_) {
        close();
    }
}

bool PeerLink::start(const QString &bindAddress, quint16 port, QString *error)
{
    if (udpIo_.running()) {
        if (error) {
            *error = QStringLiteral("UDP socket is already open");
        }
        return false;
    }

    QHostAddress localAddress;
    quint16 localPort = 0;
    if (!udpIo_.start(
            bindAddress,
            port,
            &localAddress,
            &localPort,
            error)) {
        return false;
    }

    localAddress_ = localAddress;
    localPort_ = localPort;
    closing_ = false;
    lastStatsAt_ = ReliableStream::currentTimeSeconds();
    authenticatedPackets_.start();
    feedback_.start();
    timer_->start();
    videoSend_.start();
    videoReceive_.start();
    updateFeedbackRoute();
    emitLog(QStringLiteral("bound udp %1").arg(endpointText(localAddress_, localPort_)));
    return true;
}

void PeerLink::close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    authenticatedPackets_.setWireCommandHandler({});
    feedback_.stop();
    videoSend_.stop();
    videoReceive_.stop();
    if (sessionReady_ && udpIo_.running()) {
        (void)sendPacket(PacketType::Bye, 0, 0, {});
    }
    authenticatedPackets_.stop();
    timer_->stop();
    udpIo_.stop();
    sessionReady_ = false;
    established_ = false;
    setState(QStringLiteral("closed"));
}

void PeerLink::resetPeer()
{
    if (closing_) {
        return;
    }
    if (established_ && sessionReady_
        && udpIo_.running()) {
        (void)sendPacket(PacketType::Bye, 0, 0, {});
    }

    const bool packetizerRunning =
        udpIo_.running();
    if (packetizerRunning) {
        videoSend_.stop();
    }

    peerCandidates_.clear();
    peerAddress_.clear();
    peerPort_ = 0;

    relayAddress_.clear();
    relayPort_ = 0;
    relayOutTag_ = 0;
    usingRelay_ = false;

    sessionId_ = randomBytes(4);
    const QByteArray tagBytes = randomBytes(4);
    sessionTag_ = qFromBigEndian<quint32>(tagBytes.constData());
    keyPair_ = KeyPair::generate();
    feedback_.resetSession();
    udpIo_.resetSession();
    authenticatedPackets_.resetSession();
    sessionReady_ = false;
    helloClientBytes_.clear();
    helloHostBytes_.clear();

    handshakeStarted_ = 0.0;
    helloSentAt_ = -1e9;
    lastRxAt_ = 0.0;
    lastPingAt_ = 0.0;
    established_ = false;
    remoteSessionId_.clear();
    remoteDeviceId_.clear();

    videoReceive_.resetSession();
    updateFeedbackRoute();

    sentBytesWindow_.clear();
    sentBytesSnapshot_ = 0;
    recvBytesWindow_.clear();
    sentFramesWindow_.clear();
    recvFramesWindow_.clear();
    const QString reflexive = stats_.reflexive;
    stats_ = LinkStats{};
    stats_.reflexive = reflexive;

    if (packetizerRunning) {
        videoSend_.start();
    }
    setState(QStringLiteral("idle"));
    emitLog(QStringLiteral("peer state reset"));
}

void PeerLink::setPsk(const QByteArray &psk)
{
    psk_ = psk;
}

void PeerLink::setDeviceId(const QString &deviceId)
{
    deviceId_ = deviceId;
}

void PeerLink::setLabel(const QString &label)
{
    label_ = label;
}

void PeerLink::setForceRelay(bool forceRelay)
{
    forceRelay_ = forceRelay;
}

void PeerLink::addCandidates(const QList<Candidate> &candidates)
{
    QSet<QString> known;
    bool changed = false;
    for (const Candidate &candidate : peerCandidates_) {
        known.insert(candidateKey(candidate));
    }
    for (const Candidate &candidate : candidates) {
        if (candidate.port == 0 || candidate.host.trimmed().isEmpty()) {
            continue;
        }
        Candidate normalized = candidate;
        normalized.host = normalized.host.trimmed();
        if (normalized.kind.trimmed().isEmpty()) {
            normalized.kind = QStringLiteral("host");
        }
        const QString key = candidateKey(normalized);
        if (!known.contains(key)) {
            peerCandidates_.append(normalized);
            known.insert(key);
            changed = true;
        }
    }
    if (changed) {
        updateFeedbackRoute();
    }
}

void PeerLink::beginHandshake()
{
    const double now = ReliableStream::currentTimeSeconds();
    handshakeStarted_ = now;
    if (forceRelay_ && relayPort_ != 0 && !relayAddress_.isNull()) {
        usingRelay_ = true;
        stats_.usingRelay = true;
        updateFeedbackRoute();
        emitLog(QStringLiteral("force relay mode: skipping direct paths"));
    }
    setState(QStringLiteral("punching"));
    sendHello(true);
}

void PeerLink::offerRelay(const QString &host, quint16 port)
{
    QHostAddress address(host);
    if (address.isNull()) {
        const QHostInfo info = QHostInfo::fromName(host);
        if (!info.addresses().isEmpty()) {
            address = info.addresses().constFirst();
        }
    }
    if (address.isNull() || port == 0) {
        return;
    }
    relayAddress_ = address;
    relayPort_ = port;
    updateFeedbackRoute();
    emitLog(QStringLiteral("relay offered %1").arg(endpointText(address, port)));
}

void PeerLink::setRelayOutTag(quint32 tag)
{
    relayOutTag_ = tag;
    updateFeedbackRoute();
}

void PeerLink::discoverReflexive(
    const QString &host,
    quint16 port,
    int timeoutMs)
{
    if (!udpIo_.running() || port == 0) {
        emit reflexiveDiscoveryFinished(false, {}, 0);
        return;
    }

    const auto query = [this, port, timeoutMs](const QHostAddress &address) {
        if (address.isNull()) {
            emit reflexiveDiscoveryFinished(false, {}, 0);
            return;
        }
        stunTransactionId_ = stun::randomTransactionId();
        stunPending_ = true;
        stunDeadline_ = ReliableStream::currentTimeSeconds()
            + std::max(1, timeoutMs) / 1000.0;
        const QByteArray request = stun::buildBindingRequest(stunTransactionId_);
        udpIo_.sendRawDatagram(request, address, port);
    };

    QHostAddress address(host);
    if (!address.isNull()) {
        query(address);
        return;
    }
    QHostInfo::lookupHost(
        host,
        this,
        [this, query](const QHostInfo &info) {
            if (info.error() != QHostInfo::NoError
                || info.addresses().isEmpty()) {
                emit reflexiveDiscoveryFinished(false, {}, 0);
                return;
            }
            query(info.addresses().constFirst());
        });
}

void PeerLink::sendControl(const QJsonObject &message)
{
    if (!sessionReady_) {
        return;
    }
    authenticatedPackets_.sendControl(message);
}

void PeerLink::sendVideo(const QByteArray &jpeg, bool keyframe)
{
    if (!sessionReady_ || !established_) {
        return;
    }
    videoSend_.submit(jpeg, keyframe);
}

void PeerLink::sendAudio(const QByteArray &payload)
{
    if (!sessionReady_ || !established_) {
        return;
    }
    (void)sendPacket(PacketType::Audio, static_cast<quint8>(StreamKind::Audio),
                     0, payload);
}

QString PeerLink::localAddressText() const
{
    return isAnyAddress(localAddress_) ? QString() : localAddress_.toString();
}

quint16 PeerLink::localPort() const
{
    return localPort_;
}

QString PeerLink::videoPacketizerThreadName() const
{
    return videoSend_.packetizerThreadName();
}

QString PeerLink::videoReassemblerThreadName() const
{
    return videoReceive_.reassemblerThreadName();
}

QString PeerLink::udpIoThreadName() const
{
    return udpIo_.ioThreadName();
}

QString PeerLink::authenticatedThreadName() const
{
    return authenticatedPackets_.workerThreadName();
}

QString PeerLink::linkFeedbackThreadName() const
{
    return feedback_.workerThreadName();
}

QList<Candidate> PeerLink::localCandidates() const
{
    QList<Candidate> result;
    QSet<QString> seen;
    const auto append = [&result, &seen](Candidate candidate) {
        const QString key = candidateKey(candidate);
        if (candidate.port != 0 && !seen.contains(key)) {
            result.append(candidate);
            seen.insert(key);
        }
    };

    if (localPort_ != 0 && !isAnyAddress(localAddress_)) {
        append(Candidate{localAddress_.toString(), localPort_, QStringLiteral("host")});
    }
    if (reflexivePort_ != 0 && !reflexiveAddress_.isNull()) {
        append(Candidate{
            reflexiveAddress_.toString(), reflexivePort_, QStringLiteral("srflx")});
    }
    if (localPort_ != 0) {
        const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
        for (const QHostAddress &address : addresses) {
            if (address.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            if (address.isLoopback() || isAnyAddress(address)) {
                continue;
            }
            append(Candidate{address.toString(), localPort_, QStringLiteral("host")});
        }
    }
    return result;
}

QList<Candidate> PeerLink::peerCandidates() const
{
    return peerCandidates_;
}

void PeerLink::tick()
{
    if (closing_) {
        return;
    }

    const double now = ReliableStream::currentTimeSeconds();

    if (stunPending_ && now >= stunDeadline_) {
        stunPending_ = false;
        emit reflexiveDiscoveryFinished(false, {}, 0);
    }

    if (!sessionReady_) {
        if (handshakeStarted_ > 0 && now - handshakeStarted_ < kHelloTimeoutMs / 1000.0) {
            sendHello(false);
            static double lastPunch = 0;
            if (now - lastPunch >= kPunchIntervalMs / 1000.0) {
                lastPunch = now;
                for (const Destination &destination : remoteDestinations()) {
                    (void)sendPacket(
                        PacketType::Punch,
                        0,
                        0,
                        {},
                        destination,
                        true,
                        std::nullopt);
                }
            }
        }
    } else if (!established_) {
        sendHello(false);
        for (const Destination &destination : remoteDestinations()) {
            (void)sendPacket(
                PacketType::Punch, 0, 0, {}, destination, true, std::nullopt);
        }
        if (now - lastPingAt_ >= kHandshakePingIntervalMs / 1000.0) {
            sendPing();
        }
        if (relayPort_ != 0 && !relayAddress_.isNull() && !usingRelay_
            && handshakeStarted_ > 0
            && now - handshakeStarted_ > kRelayEscalateAfterMs / 1000.0) {
            usingRelay_ = true;
            stats_.usingRelay = true;
            updateFeedbackRoute();
            sendHello(true);
            emitLog(QStringLiteral("escalating to relay"));
        }
    } else {
        if (now - lastPingAt_ >= kKeepaliveMs / 1000.0) {
            sendPing();
        }
        if (now - lastRxAt_ > kRxTimeoutMs / 1000.0) {
            emitLog(QStringLiteral("rx timeout, re-punching"));
            established_ = false;
            updateFeedbackRoute();
            setState(QStringLiteral("punching"));
        }
    }

    updateStats(now);
}

void PeerLink::processDatagram(const UdpReceivePacket &packet)
{
    if (packet.generation != udpIo_.receiveGeneration()) {
        return;
    }

    stats_.recvBytes += packet.wireBytes;
    ++stats_.recvPackets;
    recvBytesWindow_.append(
        qMakePair(
            ReliableStream::currentTimeSeconds(),
            static_cast<int>(packet.wireBytes)));

    if (packet.kind == UdpReceiveKind::Stun) {
        handleStun(packet.payload);
        return;
    }

    const Header header = packet.header;
    const QByteArray &body = packet.payload;
    const QHostAddress &sender = packet.sender;
    const quint16 senderPort = packet.senderPort;

    if (header.type == PacketType::Hello) {
        if (isHost_) {
            handleHello(body, sender, senderPort);
        }
        return;
    }
    if (header.type == PacketType::HelloAck) {
        if (!isHost_) {
            handleHelloAck(body, sender, senderPort);
        }
        return;
    }
    if (header.type == PacketType::Punch) {
        if (!hasPeer()) {
            peerAddress_ = sender;
            peerPort_ = senderPort;
        }
        (void)sendPacket(
            PacketType::PunchAck,
            0,
            0,
            {},
            Destination{sender, senderPort},
            true);
        if (isHost_ && !sessionReady_) {
            sendHello(true);
        }
        return;
    }
    if (header.type == PacketType::PunchAck) {
        if (!hasPeer()) {
            peerAddress_ = sender;
            peerPort_ = senderPort;
        }
        return;
    }
    if (header.type == PacketType::Bye && !sessionReady_) {
        established_ = false;
        updateFeedbackRoute();
        setState(QStringLiteral("closed"));
        return;
    }

    if (packet.kind != UdpReceiveKind::Encrypted || !sessionReady_) {
        return;
    }
    handleAuthenticated(header, body, sender, senderPort);
}

void PeerLink::handleStun(const QByteArray &datagram)
{
    const auto message = stun::parseMessage(datagram);
    if (!message || message->type != stun::kBindingSuccess
        || !stunPending_ || message->transactionId != stunTransactionId_) {
        return;
    }
    const auto mapped = stun::parseMappedAddress(
        message->attributes, message->transactionId);
    if (!mapped) {
        return;
    }
    reflexiveAddress_ = mapped->address;
    reflexivePort_ = mapped->port;
    stunPending_ = false;
    stats_.reflexive = endpointText(reflexiveAddress_, reflexivePort_);
    emitLog(QStringLiteral("reflexive address %1").arg(stats_.reflexive));
    emit reflexiveDiscoveryFinished(
        true, reflexiveAddress_.toString(), reflexivePort_);
}

void PeerLink::handleHello(
    const QByteArray &body,
    const QHostAddress &sender,
    quint16 port)
{
    const auto object = parseJson(body);
    if (!object) {
        return;
    }
    const QByteArray publicKey = QByteArray::fromBase64(
        object->value(QStringLiteral("pub")).toString().toLatin1());
    if (publicKey.size() != kKeyLen) {
        return;
    }

    helloClientBytes_ = body;
    const QString sid = object->value(QStringLiteral("sid")).toString();
    const bool newRemoteSession =
        remoteSessionId_ != sid && !sid.isEmpty();
    remoteSessionId_ = sid;
    const QString id = object->value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
        remoteDeviceId_ = id;
    }
    if (!hasPeer() || !established_ || newRemoteSession) {
        peerAddress_ = sender;
        peerPort_ = port;
    }

    const QJsonArray candidates = object->value(QStringLiteral("cands")).toArray();
    QList<Candidate> parsedCandidates;
    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        const QString host = candidate.value(QStringLiteral("host")).toString();
        const quint16 candidatePort = static_cast<quint16>(
            candidate.value(QStringLiteral("port")).toInt());
        if (!host.isEmpty() && candidatePort != 0) {
            parsedCandidates.append(Candidate{
                host,
                candidatePort,
                candidate.value(QStringLiteral("kind")).toString(QStringLiteral("host"))});
        }
    }
    addCandidates(parsedCandidates);
    emit peerHello(remoteDeviceId_, remoteSessionId_);

    if (!sessionReady_) {
        if (helloHostBytes_.isEmpty()) {
            helloHostBytes_ = buildHelloBody();
        }
        const auto keys = SessionKeys::derive(
            keyPair_, publicKey, helloHostBytes_, body, psk_);
        if (!keys) {
            emitLog(QStringLiteral("hello key derivation failed"));
            return;
        }
        udpIo_.setSession(*keys, true);
        authenticatedPackets_.resetSession();
        sessionReady_ = true;
        updateFeedbackRoute();
        emitLog(QStringLiteral("hello from %1; keys derived, sending ack")
                    .arg(endpointText(sender, port)));
        helloSentAt_ = ReliableStream::currentTimeSeconds();
        (void)sendPacket(
            PacketType::HelloAck,
            0,
            0,
            helloHostBytes_,
            Destination{sender, port},
            true);
        sendPing();
    }
    if (!established_) {
        setState(QStringLiteral("punching"));
    }
}

void PeerLink::handleHelloAck(
    const QByteArray &body,
    const QHostAddress &sender,
    quint16 port)
{
    const auto object = parseJson(body);
    if (!object) {
        return;
    }
    const QByteArray publicKey = QByteArray::fromBase64(
        object->value(QStringLiteral("pub")).toString().toLatin1());
    if (publicKey.size() != kKeyLen) {
        return;
    }

    const QString sid = object->value(QStringLiteral("sid")).toString();
    const bool newRemoteSession =
        remoteSessionId_ != sid && !sid.isEmpty();
    if (!hasPeer() || (!established_ && !sessionReady_) || newRemoteSession) {
        peerAddress_ = sender;
        peerPort_ = port;
    }
    helloHostBytes_ = body;
    remoteSessionId_ = sid;
    const QString id = object->value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
        remoteDeviceId_ = id;
    }

    const QJsonArray candidates = object->value(QStringLiteral("cands")).toArray();
    QList<Candidate> parsedCandidates;
    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        const QString host = candidate.value(QStringLiteral("host")).toString();
        const quint16 candidatePort = static_cast<quint16>(
            candidate.value(QStringLiteral("port")).toInt());
        if (!host.isEmpty() && candidatePort != 0) {
            parsedCandidates.append(Candidate{
                host,
                candidatePort,
                candidate.value(QStringLiteral("kind")).toString(QStringLiteral("host"))});
        }
    }
    addCandidates(parsedCandidates);
    emit peerHello(remoteDeviceId_, remoteSessionId_);

    if (!sessionReady_) {
        const auto keys = SessionKeys::derive(
            keyPair_, publicKey, body, helloClientBytes_, psk_);
        if (!keys) {
            emitLog(QStringLiteral("hello_ack key derivation failed"));
            return;
        }
        udpIo_.setSession(*keys, false);
        authenticatedPackets_.resetSession();
        sessionReady_ = true;
        updateFeedbackRoute();
        emitLog(QStringLiteral("hello_ack from %1; keys derived")
                    .arg(endpointText(sender, port)));
        sendPing();
    }
}

void PeerLink::handleAuthenticated(
    const Header &header,
    const QByteArray &payload,
    const QHostAddress &sender,
    quint16 port)
{
    bool pathChanged = false;
    if (!usingRelay_
        && (!hasPeer() || peerAddress_ != sender || peerPort_ != port)) {
        peerAddress_ = sender;
        peerPort_ = port;
        pathChanged = true;
        emitLog(QStringLiteral("path locked %1").arg(endpointText(sender, port)));
    }
    lastRxAt_ = ReliableStream::currentTimeSeconds();

    if (!established_) {
        established_ = true;
        updateFeedbackRoute();
        setState(usingRelay_ ? QStringLiteral("relay") : QStringLiteral("direct"));
        sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"),
             isHost_ ? QStringLiteral("host") : QStringLiteral("client")},
            {QStringLiteral("v"), 1},
            {QStringLiteral("id"), deviceId_},
        });
    } else if (pathChanged) {
        updateFeedbackRoute();
    }

    authenticatedPackets_.receive(header, payload);
}

void PeerLink::onVideoFrameReady(
    const QByteArray &frame,
    bool keyframe)
{
    if (closing_ || !sessionReady_ || !established_) {
        return;
    }
    recvFramesWindow_.append(ReliableStream::currentTimeSeconds());
    emit videoFrameReceived(frame, keyframe);
}

void PeerLink::onVideoPacketsReady(const VideoPacketBatch &batch)
{
    if (!sessionReady_ || !established_) {
        return;
    }
    for (const WirePacket &packet : batch.packets) {
        (void)sendPacket(
            packet.type,
            static_cast<quint8>(StreamKind::Video),
            packet.flags,
            packet.payload);
    }
    if (!batch.retransmit) {
        sentFramesWindow_.append(ReliableStream::currentTimeSeconds());
    }
}

void PeerLink::sendHello(bool force)
{
    const double now = ReliableStream::currentTimeSeconds();
    if (!force && now - helloSentAt_ < kHelloIntervalMs / 1000.0) {
        return;
    }

    PacketType type = PacketType::Hello;
    QByteArray body;
    if (isHost_) {
        if (helloHostBytes_.isEmpty()) {
            helloHostBytes_ = buildHelloBody();
        }
        body = helloHostBytes_;
        type = PacketType::HelloAck;
    } else {
        if (helloClientBytes_.isEmpty()) {
            helloClientBytes_ = buildHelloBody();
        }
        body = helloClientBytes_;
        type = PacketType::Hello;
    }

    helloSentAt_ = now;
    for (const Destination &destination : targetDestinations()) {
        (void)sendPacket(type, 0, 0, body, destination, true);
    }
}

QByteArray PeerLink::buildHelloBody()
{
    QJsonArray candidates;
    for (const Candidate &candidate : localCandidates()) {
        candidates.append(QJsonObject{
            {QStringLiteral("host"), candidate.host},
            {QStringLiteral("port"), candidate.port},
            {QStringLiteral("kind"), candidate.kind},
        });
    }

    const QJsonObject object{
        {QStringLiteral("v"), 1},
        {QStringLiteral("sid"), QString::fromLatin1(sessionId_.toHex())},
        {QStringLiteral("pub"), QString::fromLatin1(keyPair_.publicKey.toBase64())},
        {QStringLiteral("psk"), QString::fromLatin1(pskVerifier())},
        {QStringLiteral("role"), isHost_ ? QStringLiteral("host") : QStringLiteral("client")},
        {QStringLiteral("id"), deviceId_},
        {QStringLiteral("cands"), candidates},
        {QStringLiteral("ts"),
         static_cast<qint64>(QDateTime::currentMSecsSinceEpoch())},
    };
    return jsonPayload(object);
}

QByteArray PeerLink::pskVerifier() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("peerdesk-psk"));
    hash.addData(psk_);
    return hash.result().toHex().left(16);
}

QList<PeerLink::Destination> PeerLink::targetDestinations() const
{
    if (usingRelay_ && relayPort_ != 0 && !relayAddress_.isNull()) {
        return {Destination{relayAddress_, relayPort_}};
    }
    return remoteDestinations();
}

QList<PeerLink::Destination> PeerLink::remoteDestinations() const
{
    QList<Destination> result;
    QSet<QString> seen;

    const auto append = [&result, &seen](const Destination &destination) {
        if (destination.address.isNull() || destination.port == 0) {
            return;
        }
        const QString key = QStringLiteral("%1|%2")
            .arg(destination.address.toString())
            .arg(destination.port);
        if (!seen.contains(key)) {
            result.append(destination);
            seen.insert(key);
        }
    };

    if (hasPeer()) {
        append(Destination{peerAddress_, peerPort_});
    }
    for (const Candidate &candidate : peerCandidates_) {
        QHostAddress address(candidate.host);
        if (!address.isNull()) {
            append(Destination{address, candidate.port});
        }
    }
    return result;
}

bool PeerLink::sendPacket(
    PacketType type,
    quint8 stream,
    quint8 flags,
    const QByteArray &payload,
    const std::optional<Destination> &destination,
    bool plain,
    std::optional<quint64> sequence)
{
    if (!udpIo_.running()) {
        return false;
    }

    Destination target;
    bool haveTarget = false;
    if (usingRelay_ && relayPort_ != 0 && !relayAddress_.isNull()) {
        target = Destination{relayAddress_, relayPort_};
        haveTarget = true;
    } else if (destination && !destination->address.isNull()
               && destination->port != 0) {
        target = *destination;
        haveTarget = true;
    } else if (hasPeer()) {
        target = Destination{peerAddress_, peerPort_};
        haveTarget = true;
    } else {
        const QList<Destination> destinations = remoteDestinations();
        if (!destinations.isEmpty()) {
            target = destinations.constFirst();
            haveTarget = true;
        }
    }
    if (!haveTarget) {
        return false;
    }

    if (!plain && !sessionReady_) {
        return false;
    }

    UdpSendJob job;
    job.generation = udpIo_.receiveGeneration();
    job.type = type;
    job.stream = stream;
    job.flags = flags;
    job.payload = payload;
    job.address = target.address;
    job.port = target.port;
    job.plain = plain;
    job.wrapRelay =
        usingRelay_ && relayPort_ != 0 && !relayAddress_.isNull();
    job.sessionTag = sessionTag_;
    job.relayTag = relayOutTag_;
    job.sequence = sequence;
    return udpIo_.send(job);
}

void PeerLink::sendPing()
{
    if (!sessionReady_ || !authenticatedPackets_.running()) {
        return;
    }
    lastPingAt_ = ReliableStream::currentTimeSeconds();
    authenticatedPackets_.sendPing();
}

void PeerLink::updateFeedbackRoute()
{
    LinkFeedbackRoute route;
    route.generation = feedback_.generation();
    route.udpGeneration = udpIo_.receiveGeneration();
    route.authGeneration = authenticatedPackets_.sessionGeneration();
    route.sessionReady = sessionReady_;
    route.established = established_;
    route.sessionTag = sessionTag_;
    route.relayTag = relayOutTag_;
    route.wrapRelay =
        usingRelay_ && relayPort_ != 0 && !relayAddress_.isNull();

    const QList<Destination> destinations = targetDestinations();
    if (!destinations.isEmpty()) {
        route.address = destinations.constFirst().address;
        route.port = destinations.constFirst().port;
    }
    feedback_.setRoute(route);
}

void PeerLink::setState(const QString &state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    stats_.mode = state;
    emitLog(QStringLiteral("state -> %1").arg(state));
    emit stateChanged(state);
    updateStats(ReliableStream::currentTimeSeconds());
}

void PeerLink::updateStats(double now)
{
    pruneWindows(now);
    const quint64 totalSentBytes = udpIo_.sentBytes();
    if (totalSentBytes >= sentBytesSnapshot_) {
        const quint64 delta = totalSentBytes - sentBytesSnapshot_;
        if (delta > 0) {
            sentBytesWindow_.append(qMakePair(
                now,
                static_cast<int>(std::min<quint64>(
                    delta,
                    static_cast<quint64>(std::numeric_limits<int>::max())))));
        }
    }
    sentBytesSnapshot_ = totalSentBytes;
    stats_.sentBytes = totalSentBytes;
    stats_.sentPackets = udpIo_.sentPackets();

    qint64 sentBytes = 0;
    for (const auto &entry : sentBytesWindow_) {
        sentBytes += entry.second;
    }
    qint64 recvBytes = 0;
    for (const auto &entry : recvBytesWindow_) {
        recvBytes += entry.second;
    }
    stats_.mbps = static_cast<double>(sentBytes + recvBytes) * 8.0 / 1e6;
    stats_.fps = static_cast<double>(
        sentFramesWindow_.size() + recvFramesWindow_.size());
    stats_.mode = state_;
    stats_.usingRelay = usingRelay_;
    stats_.path = hasPeer()
        ? endpointText(peerAddress_, peerPort_)
        : QString();

    if (now - lastStatsAt_ >= 1.0) {
        lastStatsAt_ = now;
        emit statsChanged(stats_);
    }
}

void PeerLink::pruneWindows(double now)
{
    const double cutoff = now - 1.0;
    while (!sentBytesWindow_.isEmpty()
           && sentBytesWindow_.constFirst().first < cutoff) {
        sentBytesWindow_.removeFirst();
    }
    while (!recvBytesWindow_.isEmpty()
           && recvBytesWindow_.constFirst().first < cutoff) {
        recvBytesWindow_.removeFirst();
    }
    while (!sentFramesWindow_.isEmpty()
           && sentFramesWindow_.constFirst() < cutoff) {
        sentFramesWindow_.removeFirst();
    }
    while (!recvFramesWindow_.isEmpty()
           && recvFramesWindow_.constFirst() < cutoff) {
        recvFramesWindow_.removeFirst();
    }
}

void PeerLink::emitLog(const QString &message)
{
    emit logMessage(
        QStringLiteral("[%1] %2")
            .arg(label_.isEmpty() ? QStringLiteral("link") : label_, message));
}

} // namespace pd
