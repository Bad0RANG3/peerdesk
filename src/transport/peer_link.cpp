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
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace pd {
namespace {

constexpr char kRelayMagic[] = {'R', 'L', 'Y', '1'};

QByteArray littleU32(quint32 value)
{
    QByteArray output(4, '\0');
    qToLittleEndian<quint32>(value, output.data());
    return output;
}

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
    , socket_(new QUdpSocket(this))
    , timer_(new QTimer(this))
    , keyPair_(KeyPair::generate())
    , videoReceiver_(
          [this](const QByteArray &frame, bool keyframe) {
              recvFramesWindow_.append(ReliableStream::currentTimeSeconds());
              emit videoFrameReceived(frame, keyframe);
          },
          [this]() { requestKeyframe(); },
          VideoSender::kFecK)
    , controlStream_(
          [this](const QByteArray &message) { deliverControl(message); },
          [this](quint32 sequence, const QByteArray &payload, quint8 flags) {
              sendControlRaw(sequence, payload, flags);
          })
{
    qRegisterMetaType<pd::LinkStats>("pd::LinkStats");

    sessionId_ = randomBytes(4);
    const QByteArray tagBytes = randomBytes(4);
    sessionTag_ = qFromBigEndian<quint32>(tagBytes.constData());

    connect(socket_, &QUdpSocket::readyRead, this, [this]() { tick(); });
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
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        if (error) {
            *error = QStringLiteral("UDP socket is already open");
        }
        return false;
    }

    QHostAddress address(bindAddress);
    if (address.isNull() && bindAddress != QStringLiteral("0.0.0.0")) {
        const QHostInfo info = QHostInfo::fromName(bindAddress);
        if (!info.addresses().isEmpty()) {
            address = info.addresses().constFirst();
        }
    }
    if (address.isNull()) {
        if (error) {
            *error = QStringLiteral("Invalid bind address: %1").arg(bindAddress);
        }
        return false;
    }

    if (!socket_->bind(address, port, QAbstractSocket::DefaultForPlatform)) {
        if (error) {
            *error = socket_->errorString();
        }
        return false;
    }

    localAddress_ = socket_->localAddress();
    localPort_ = socket_->localPort();
    socket_->setSocketOption(
        QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
    socket_->setSocketOption(
        QAbstractSocket::SendBufferSizeSocketOption, 1024 * 1024);
    closing_ = false;
    lastStatsAt_ = ReliableStream::currentTimeSeconds();
    timer_->start();
    emitLog(QStringLiteral("bound udp %1").arg(endpointText(localAddress_, localPort_)));
    return true;
}

void PeerLink::close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    if (crypto_ && socket_->state() == QAbstractSocket::BoundState) {
        (void)sendPacket(PacketType::Bye, 0, 0, {});
    }
    timer_->stop();
    socket_->close();
    crypto_.reset();
    established_ = false;
    setState(QStringLiteral("closed"));
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
        }
    }
}

void PeerLink::beginHandshake()
{
    const double now = ReliableStream::currentTimeSeconds();
    handshakeStarted_ = now;
    if (forceRelay_ && relayPort_ != 0 && !relayAddress_.isNull()) {
        usingRelay_ = true;
        stats_.usingRelay = true;
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
    emitLog(QStringLiteral("relay offered %1").arg(endpointText(address, port)));
}

void PeerLink::setRelayOutTag(quint32 tag)
{
    relayOutTag_ = tag;
}

void PeerLink::discoverReflexive(
    const QString &host,
    quint16 port,
    int timeoutMs)
{
    if (socket_->state() != QAbstractSocket::BoundState || port == 0) {
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
        socket_->writeDatagram(request, address, port);
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
    if (!crypto_) {
        return;
    }
    const QList<QByteArray> parts =
        controlFragments_.split(jsonPayload(message));
    for (const QByteArray &part : parts) {
        (void)controlStream_.send(part, FlagReliable, ReliableStream::currentTimeSeconds());
    }
}

void PeerLink::sendVideo(const QByteArray &jpeg, bool keyframe)
{
    if (!crypto_ || !established_) {
        return;
    }
    const QList<WirePacket> packets = videoSender_.build(jpeg, keyframe);
    for (const WirePacket &packet : packets) {
        (void)sendPacket(packet.type, static_cast<quint8>(StreamKind::Video),
                         packet.flags, packet.payload);
    }
    sentFramesWindow_.append(ReliableStream::currentTimeSeconds());
}

void PeerLink::sendAudio(const QByteArray &payload)
{
    if (!crypto_ || !established_) {
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

    while (socket_->hasPendingDatagrams()) {
        QByteArray datagram(
            static_cast<qsizetype>(std::max<qint64>(0, socket_->pendingDatagramSize())),
            '\0');
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 received = socket_->readDatagram(
            datagram.data(), datagram.size(), &sender, &senderPort);
        if (received < 0) {
            continue;
        }
        datagram.truncate(static_cast<qsizetype>(received));
        processDatagram(datagram, sender, senderPort);
    }

    const double now = ReliableStream::currentTimeSeconds();
    controlStream_.tick(now);
    const QList<VideoNack> nacks = videoReceiver_.tick(now);
    for (const VideoNack &nack : nacks) {
        (void)sendPacket(
            PacketType::Nack,
            static_cast<quint8>(StreamKind::Video),
            0,
            packNack(nack.frameId, nack.missing));
    }

    if (stunPending_ && now >= stunDeadline_) {
        stunPending_ = false;
        emit reflexiveDiscoveryFinished(false, {}, 0);
    }

    if (!crypto_) {
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
            setState(QStringLiteral("punching"));
        }
    }

    updateStats(now);
}

void PeerLink::processDatagram(
    const QByteArray &datagram,
    const QHostAddress &sender,
    quint16 senderPort)
{
    stats_.recvBytes += static_cast<quint64>(datagram.size());
    ++stats_.recvPackets;
    recvBytesWindow_.append(
        qMakePair(ReliableStream::currentTimeSeconds(), datagram.size()));

    if (datagram.size() >= 20
        && ((static_cast<quint8>(datagram[0]) == 0
             && static_cast<quint8>(datagram[1]) == 0x01)
            || (static_cast<quint8>(datagram[0]) == 0x01
                && static_cast<quint8>(datagram[1]) == 0x01))) {
        handleStun(datagram);
        return;
    }

    Header header;
    QByteArray headerBytes = datagram;
    QByteArray body;
    if (datagram.size() >= kRelayHeaderLen
        && std::memcmp(datagram.constData(), kRelayMagic, 4) == 0) {
        const auto relayed = unwrapRelay(datagram);
        if (!relayed) {
            return;
        }
        headerBytes = relayed->second;
        const auto parsed = Header::parse(headerBytes);
        if (!parsed) {
            return;
        }
        header = *parsed;
        body = headerBytes.mid(kHeaderLen);
    } else {
        const auto parsed = Header::parse(datagram);
        if (!parsed) {
            return;
        }
        header = *parsed;
        headerBytes = datagram.left(kHeaderLen);
        body = datagram.mid(kHeaderLen);
    }

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
        if (isHost_ && !crypto_) {
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
    if (header.type == PacketType::Bye && !crypto_) {
        established_ = false;
        setState(QStringLiteral("closed"));
        return;
    }

    if (!crypto_) {
        return;
    }
    handleEncrypted(headerBytes, header, body, sender, senderPort);
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
    remoteSessionId_ = object->value(QStringLiteral("sid")).toString();
    const QString id = object->value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
        remoteDeviceId_ = id;
    }
    if (!hasPeer()) {
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

    if (!crypto_) {
        if (helloHostBytes_.isEmpty()) {
            helloHostBytes_ = buildHelloBody();
        }
        const auto keys = SessionKeys::derive(
            keyPair_, publicKey, helloHostBytes_, body, psk_);
        if (!keys) {
            emitLog(QStringLiteral("hello key derivation failed"));
            return;
        }
        crypto_ = std::make_unique<CryptoContext>(*keys, true);
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

    if (!hasPeer()) {
        peerAddress_ = sender;
        peerPort_ = port;
    }
    helloHostBytes_ = body;
    remoteSessionId_ = object->value(QStringLiteral("sid")).toString();
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

    if (!crypto_) {
        const auto keys = SessionKeys::derive(
            keyPair_, publicKey, body, helloClientBytes_, psk_);
        if (!keys) {
            emitLog(QStringLiteral("hello_ack key derivation failed"));
            return;
        }
        crypto_ = std::make_unique<CryptoContext>(*keys, false);
        emitLog(QStringLiteral("hello_ack from %1; keys derived")
                    .arg(endpointText(sender, port)));
        sendPing();
    }
}

void PeerLink::handleEncrypted(
    const QByteArray &headerBytes,
    const Header &header,
    const QByteArray &body,
    const QHostAddress &sender,
    quint16 port)
{
    const auto plain = crypto_->open(headerBytes, body, header.seq);
    if (!plain) {
        return;
    }
    if (!hasPeer() && !usingRelay_) {
        peerAddress_ = sender;
        peerPort_ = port;
        emitLog(QStringLiteral("path locked %1").arg(endpointText(sender, port)));
    }
    lastRxAt_ = ReliableStream::currentTimeSeconds();

    if (!established_) {
        established_ = true;
        setState(usingRelay_ ? QStringLiteral("relay") : QStringLiteral("direct"));
        sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"),
             isHost_ ? QStringLiteral("host") : QStringLiteral("client")},
            {QStringLiteral("v"), 1},
            {QStringLiteral("id"), deviceId_},
        });
    }

    const QByteArray &payload = *plain;
    if (header.type == PacketType::Data) {
        if (header.stream == static_cast<quint8>(StreamKind::Video)) {
            const auto fragment = unpackVideoFragment(payload);
            if (fragment) {
                videoReceiver_.onFragment(
                    fragment->frameId,
                    fragment->index,
                    fragment->count,
                    fragment->data,
                    (header.flags & FlagKeyframe) != 0,
                    ReliableStream::currentTimeSeconds());
            }
        } else if (header.stream == static_cast<quint8>(StreamKind::Control)
                   && payload.size() >= 4) {
            const quint32 messageSequence =
                qFromLittleEndian<quint32>(payload.constData());
            controlStream_.onData(messageSequence, payload.mid(4));
            maybeAck();
        }
    } else if (header.type == PacketType::Parity) {
        const auto parity = unpackParityFragment(payload);
        if (parity) {
            videoReceiver_.onParity(
                parity->frameId,
                parity->groupIndex,
                parity->shards,
                parity->data);
        }
    } else if (header.type == PacketType::Audio) {
        emit audioReceived(payload);
    } else if (header.type == PacketType::Ack) {
        const auto ack = unpackAck(payload);
        if (ack) {
            controlStream_.onAck(ack->cumulative, ack->bitmap);
        }
    } else if (header.type == PacketType::Nack) {
        const auto nack = unpackNack(payload);
        if (nack) {
            const QList<WirePacket> packets =
                videoSender_.retransmit(nack->first, nack->second);
            for (const WirePacket &packet : packets) {
                (void)sendPacket(
                    packet.type,
                    static_cast<quint8>(StreamKind::Video),
                    packet.flags,
                    packet.payload);
            }
        }
    } else if (header.type == PacketType::Ping) {
        (void)sendPacket(PacketType::Pong, 0, 0, payload);
    } else if (header.type == PacketType::Pong) {
        handlePong(payload);
    } else if (header.type == PacketType::Bye) {
        emitLog(QStringLiteral("peer closed"));
        established_ = false;
        setState(QStringLiteral("closed"));
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
    if (socket_->state() != QAbstractSocket::BoundState) {
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

    quint64 sequenceValue = 0;
    QByteArray body;
    quint16 payloadLength = 0;
    if (plain) {
        body = payload;
        payloadLength = static_cast<quint16>(payload.size());
    } else {
        if (!crypto_) {
            return false;
        }
        sequenceValue = sequence.value_or(crypto_->nextSeq());
        body.clear();
        payloadLength = static_cast<quint16>(payload.size() + kAuthTagLen);
    }

    Header header;
    header.type = type;
    header.seq = static_cast<quint32>(sequenceValue);
    header.tsMs = static_cast<quint32>(
        static_cast<quint64>(ReliableStream::currentTimeSeconds() * 1000.0));
    header.stream = stream;
    header.flags = flags;
    header.plen = payloadLength;
    header.session = sessionTag_;
    const QByteArray headerBytes = header.pack();

    if (!plain) {
        body = crypto_->seal(headerBytes, payload, sequenceValue);
    }
    QByteArray datagram = headerBytes + body;
    if (usingRelay_ && relayPort_ != 0 && !relayAddress_.isNull()) {
        datagram = wrapRelay(relayOutTag_ != 0 ? relayOutTag_ : sessionTag_, datagram);
    }

    const qint64 written = socket_->writeDatagram(
        datagram, target.address, target.port);
    if (written < 0) {
        return false;
    }
    stats_.sentBytes += static_cast<quint64>(datagram.size());
    ++stats_.sentPackets;
    sentBytesWindow_.append(
        qMakePair(ReliableStream::currentTimeSeconds(), datagram.size()));
    return true;
}

void PeerLink::sendControlRaw(
    quint32 sequence,
    const QByteArray &payload,
    quint8 flags)
{
    (void)sendPacket(
        PacketType::Data,
        static_cast<quint8>(StreamKind::Control),
        flags,
        littleU32(sequence) + payload);
}

void PeerLink::maybeAck()
{
    const double now = ReliableStream::currentTimeSeconds();
    if (controlStream_.needsAck() && now - lastAckAt_ > 0.004) {
        lastAckAt_ = now;
        (void)sendPacket(
            PacketType::Ack, 0, 0, packAck(controlStream_.ackPayload()));
    }
}

void PeerLink::deliverControl(const QByteArray &payload)
{
    const auto body = controlFragments_.feed(payload);
    if (!body) {
        return;
    }
    const auto object = parseJson(*body);
    if (object) {
        emit controlMessageReceived(*object);
    }
}

void PeerLink::sendPing()
{
    pingSequence_ = static_cast<quint16>((pingSequence_ + 1) & 0xFFFF);
    const double now = ReliableStream::currentTimeSeconds();
    pingsOut_.insert(pingSequence_, now);
    if (pingsOut_.size() > 32) {
        quint16 oldest = 0xFFFF;
        for (auto it = pingsOut_.constBegin(); it != pingsOut_.constEnd(); ++it) {
            oldest = std::min(oldest, it.key());
        }
        pingsOut_.remove(oldest);
    }
    lastPingAt_ = now;
    (void)sendPacket(
        PacketType::Ping,
        0,
        0,
        jsonPayload(QJsonObject{{QStringLiteral("n"), pingSequence_}}));
}

void PeerLink::handlePong(const QByteArray &payload)
{
    const auto object = parseJson(payload);
    if (!object) {
        return;
    }
    const uint raw = object->value(QStringLiteral("n")).toVariant().toUInt();
    const quint16 sequence = static_cast<quint16>(raw & 0xFFFF);
    const auto sent = pingsOut_.find(sequence);
    if (sent == pingsOut_.end()) {
        return;
    }
    const double now = ReliableStream::currentTimeSeconds();
    stats_.rttMs = std::max(0.0, (now - *sent) * 1000.0);
    pingsOut_.erase(sent);
}

void PeerLink::requestKeyframe()
{
    ++keyframeRequests_;
    if (established_) {
        sendControl(QJsonObject{{QStringLiteral("t"), QStringLiteral("keyframe_req")}});
    }
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
