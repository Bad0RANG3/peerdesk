#include "discovery/lan_discovery.h"

#include "core/util.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaType>
#include <QNetworkInterface>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace pd {
namespace {

constexpr auto kProtocolName = "peerdesk-lan-discovery";
constexpr int kProtocolVersion = 1;
constexpr int kMaxNameLength = 64;
constexpr int kMaxInstanceIdLength = 64;
constexpr int kMaxServerLength = 255;

QString appVersion()
{
#ifdef PEERDESK_VERSION
    return QStringLiteral(PEERDESK_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

QString sanitizedName(const QString &text)
{
    QString clean;
    clean.reserve(std::min<qsizetype>(text.size(), kMaxNameLength));
    for (const QChar ch : text) {
        if (ch.category() == QChar::Other_Control
            || ch.category() == QChar::Other_Format) {
            continue;
        }
        clean.append(ch);
    }
    clean = clean.simplified();
    if (clean.size() > kMaxNameLength) {
        clean = clean.left(kMaxNameLength).trimmed();
    }
    return clean;
}

bool validInstanceId(const QString &id)
{
    if (id.isEmpty() || id.size() > kMaxInstanceIdLength) {
        return false;
    }
    return std::all_of(
        id.cbegin(),
        id.cend(),
        [](QChar ch) {
            return ch.isLetterOrNumber()
                || ch == u'-'
                || ch == u'_';
        });
}

bool isLoopbackHostText(const QString &host)
{
    if (host.compare(
            QStringLiteral("localhost"),
            Qt::CaseInsensitive)
        == 0) {
        return true;
    }
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

std::optional<QString> normalizeServer(
    const QString &server,
    bool allowLoopback)
{
    const QString trimmed = server.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxServerLength) {
        return std::nullopt;
    }
    if (std::any_of(
            trimmed.cbegin(),
            trimmed.cend(),
            [](QChar ch) {
                return ch.isSpace()
                    || ch.category() == QChar::Other_Control;
            })) {
        return std::nullopt;
    }
    const std::optional<ServerAddress> address =
        parseServerAddress(trimmed);
    if (!address.has_value()) {
        return std::nullopt;
    }
    if (address->host.size() > kMaxServerLength) {
        return std::nullopt;
    }
    const QHostAddress literal(address->host);
    if (!literal.isNull()
        && (literal == QHostAddress(QHostAddress::AnyIPv4)
            || literal == QHostAddress(QHostAddress::Any))) {
        return std::nullopt;
    }
    if (isLoopbackHostText(address->host) && !allowLoopback) {
        return std::nullopt;
    }
    return formatAddress(address->host.toLower(), address->port);
}

bool isLocalAddress(const QHostAddress &address)
{
    const QList<QHostAddress> local =
        QNetworkInterface::allAddresses();
    return std::any_of(
        local.cbegin(),
        local.cend(),
        [&address](const QHostAddress &candidate) {
            return candidate.protocol() == QHostAddress::IPv4Protocol
                && candidate == address;
        });
}

bool isAllowedSource(
    const QHostAddress &address,
    bool allowLoopback)
{
    if (address.protocol() != QHostAddress::IPv4Protocol) {
        return false;
    }
    const quint32 value = address.toIPv4Address();
    const bool loopback = (value & 0xff000000U) == 0x7f000000U;
    if (loopback) {
        return allowLoopback;
    }
    if (isLocalAddress(address)) {
        return true;
    }
    if ((value & 0xff000000U) == 0x0a000000U) {
        return true;
    }
    if ((value & 0xfff00000U) == 0xac100000U) {
        return true;
    }
    if ((value & 0xffff0000U) == 0xc0a80000U) {
        return true;
    }
    return (value & 0xffff0000U) == 0xa9fe0000U;
}

struct IncomingMessage {
    QString type;
    QString instanceId;
    QString name;
    QString code;
    QString server;
};

std::optional<IncomingMessage> parseMessage(
    const QByteArray &payload,
    bool allowLoopback,
    QString *error)
{
    if (payload.size() > LanDiscovery::kMaxPayloadBytes) {
        if (error) {
            *error = QStringLiteral("发现报文超过大小限制");
        }
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("发现报文不是有效的 JSON 对象");
        }
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("p")).toString()
            != QLatin1String(kProtocolName)
        || object.value(QStringLiteral("v")).toInt(-1)
            != kProtocolVersion) {
        if (error) {
            *error = QStringLiteral("发现协议版本不匹配");
        }
        return std::nullopt;
    }

    IncomingMessage message;
    message.type = object.value(QStringLiteral("t")).toString();
    message.instanceId = object.value(QStringLiteral("id")).toString();
    if (!validInstanceId(message.instanceId)) {
        if (error) {
            *error = QStringLiteral("发现报文实例 ID 无效");
        }
        return std::nullopt;
    }

    if (message.type == QStringLiteral("query")) {
        return message;
    }
    if (message.type != QStringLiteral("announce")
        && message.type != QStringLiteral("bye")) {
        if (error) {
            *error = QStringLiteral("发现报文类型无效");
        }
        return std::nullopt;
    }

    message.name = sanitizedName(
        object.value(QStringLiteral("name")).toString());
    message.code = normalizeCode(
        object.value(QStringLiteral("code")).toString());
    const std::optional<QString> server = normalizeServer(
        object.value(QStringLiteral("server")).toString(),
        allowLoopback);
    if (message.type == QStringLiteral("announce")) {
        if (message.name.isEmpty()
            || !codeIsValid(message.code)
            || !server.has_value()) {
            if (error) {
                *error = QStringLiteral("发现报文设备信息无效");
            }
            return std::nullopt;
        }
    } else if (!codeIsValid(message.code)) {
        if (error) {
            *error = QStringLiteral("下线报文连接码无效");
        }
        return std::nullopt;
    }
    message.server = server.value_or(QString());
    return message;
}

bool sameDeviceContent(
    const QList<NearbyDevice> &left,
    const QList<NearbyDevice> &right)
{
    return left.size() == right.size()
        && std::equal(
            left.cbegin(),
            left.cend(),
            right.cbegin(),
            [](const NearbyDevice &a, const NearbyDevice &b) {
                return a.instanceId == b.instanceId
                    && a.name == b.name
                    && a.code == b.code
                    && a.server == b.server
                    && a.address == b.address;
            });
}

} // namespace

LanDiscovery::LanDiscovery(QObject *parent)
    : QObject(parent)
    , announceTimer_(new QTimer(this))
    , sweepTimer_(new QTimer(this))
    , instanceId_(
          QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    qRegisterMetaType<NearbyDevice>("pd::NearbyDevice");
    qRegisterMetaType<QList<NearbyDevice>>(
        "QList<pd::NearbyDevice>");
    announceTimer_->setSingleShot(false);
    sweepTimer_->setSingleShot(false);
    connect(
        announceTimer_,
        &QTimer::timeout,
        this,
        &LanDiscovery::sendAnnouncement);
    connect(
        sweepTimer_,
        &QTimer::timeout,
        this,
        &LanDiscovery::pruneExpired);
}

LanDiscovery::~LanDiscovery()
{
    stop();
}

void LanDiscovery::setOptions(const Options &options)
{
    if (active()) {
        emit logMessage(
            QStringLiteral("局域网发现运行中，忽略配置变更"));
        return;
    }
    options_ = options;
}

bool LanDiscovery::active() const
{
    return socket_
        && socket_->state() == QAbstractSocket::BoundState;
}

bool LanDiscovery::start(QString *error)
{
    if (error) {
        error->clear();
    }
    if (active()) {
        return true;
    }
    if (!options_.bindAddress.isNull()
        && options_.bindAddress.protocol()
            != QHostAddress::IPv4Protocol) {
        const QString message =
            QStringLiteral("局域网发现只支持 IPv4 绑定地址");
        if (error) {
            *error = message;
        }
        emit errorOccurred(message);
        return false;
    }
    if (options_.targetAddress.isNull()
        || options_.targetAddress.protocol()
            != QHostAddress::IPv4Protocol
        || options_.targetPort == 0) {
        const QString message =
            QStringLiteral("局域网发现目标地址无效");
        if (error) {
            *error = message;
        }
        emit errorOccurred(message);
        return false;
    }
    if (options_.allowLoopback
        && (options_.bindAddress.isLoopback()
            || options_.targetAddress.isLoopback())) {
        // Loopback is supported for deterministic local tests.
    } else if (!options_.allowLoopback
        && (options_.bindAddress.isLoopback()
            || options_.targetAddress.isLoopback())) {
        const QString message =
            QStringLiteral("回环发现地址需要显式启用");
        if (error) {
            *error = message;
        }
        emit errorOccurred(message);
        return false;
    }
    const int announceInterval =
        std::max(50, options_.announceIntervalMs);
    const int expireAfter =
        std::max(announceInterval, options_.expireAfterMs);
    const int sweepInterval = std::clamp(
        options_.sweepIntervalMs,
        20,
        expireAfter);
    options_.announceIntervalMs = announceInterval;
    options_.expireAfterMs = expireAfter;
    options_.sweepIntervalMs = sweepInterval;
    options_.maxDevices = std::clamp(options_.maxDevices, 1, 128);

    auto *socket = new QUdpSocket(this);
    const auto flags =
        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    if (!socket->bind(
            options_.bindAddress,
            options_.bindPort,
            flags)) {
        const QString message =
            QStringLiteral("无法监听局域网发现端口：%1")
                .arg(socket->errorString());
        if (error) {
            *error = message;
        }
        emit errorOccurred(message);
        socket->deleteLater();
        return false;
    }
    socket->setSocketOption(
        QAbstractSocket::ReceiveBufferSizeSocketOption,
        64 * 1024);
    connect(
        socket,
        &QUdpSocket::readyRead,
        this,
        &LanDiscovery::onReadyRead);
    socket_ = socket;
    lastSendError_.clear();

    sweepTimer_->start(options_.sweepIntervalMs);
    updateAnnounceTimer();
    emit logMessage(
        QStringLiteral("局域网发现已启动：UDP %1，实例 %2")
            .arg(socket_->localPort())
            .arg(instanceId_));

    if (published_.valid()) {
        sendAnnouncement();
    } else {
        sendQuery();
    }
    return true;
}

void LanDiscovery::stop()
{
    if (published_.valid() && active()) {
        sendBye(published_);
    }
    published_ = {};
    announceTimer_->stop();
    sweepTimer_->stop();
    if (socket_) {
        QUdpSocket *socket = socket_;
        socket_ = nullptr;
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    lastSendError_.clear();
    if (!devices_.isEmpty()) {
        devices_.clear();
        emit devicesChanged({});
    }
}

void LanDiscovery::publish(
    const QString &name,
    const QString &code,
    const QString &server)
{
    const QString normalizedCode = normalizeCode(code);
    const std::optional<QString> normalizedServer =
        normalizeServer(server, options_.allowLoopback);
    if (!codeIsValid(normalizedCode)
        || !normalizedServer.has_value()) {
        emit logMessage(
            QStringLiteral("未发布局域网设备：连接码或信令地址无效"));
        withdraw();
        return;
    }

    Announcement next;
    next.name = sanitizedName(name);
    if (next.name.isEmpty()) {
        next.name = QStringLiteral("PeerDesk Host");
    }
    next.code = normalizedCode;
    next.server = normalizedServer.value();
    if (next.code == published_.code
        && next.name == published_.name
        && next.server == published_.server) {
        return;
    }

    if (published_.valid() && active()) {
        sendBye(published_);
    }
    published_ = next;
    updateAnnounceTimer();
    if (active()) {
        sendAnnouncement();
    }
}

void LanDiscovery::withdraw()
{
    if (published_.valid() && active()) {
        sendBye(published_);
    }
    published_ = {};
    updateAnnounceTimer();
}

QList<NearbyDevice> LanDiscovery::devices() const
{
    QList<NearbyDevice> result = devices_;
    std::sort(
        result.begin(),
        result.end(),
        [](const NearbyDevice &left, const NearbyDevice &right) {
            const int byName = QString::compare(
                left.name,
                right.name,
                Qt::CaseInsensitive);
            if (byName != 0) {
                return byName < 0;
            }
            const int byCode = left.code.compare(right.code);
            if (byCode != 0) {
                return byCode < 0;
            }
            return left.server.compare(
                       right.server,
                       Qt::CaseInsensitive)
                < 0;
        });
    return result;
}

quint16 LanDiscovery::boundPort() const
{
    return socket_ ? socket_->localPort() : 0;
}

void LanDiscovery::onReadyRead()
{
    if (!socket_) {
        return;
    }
    while (socket_->hasPendingDatagrams()) {
        QByteArray payload;
        payload.resize(
            static_cast<qsizetype>(
                socket_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = socket_->readDatagram(
            payload.data(),
            payload.size(),
            &sender,
            &senderPort);
        if (read < 0) {
            emit logMessage(
                QStringLiteral("读取局域网发现报文失败：%1")
                    .arg(socket_->errorString()));
            continue;
        }
        payload.resize(read);
        if (!isAllowedSource(sender, options_.allowLoopback)) {
            continue;
        }

        QString parseError;
        const std::optional<IncomingMessage> message =
            parseMessage(payload, options_.allowLoopback, &parseError);
        if (!message.has_value()) {
            if (!parseError.isEmpty()) {
                emit logMessage(
                    QStringLiteral("忽略局域网发现报文：%1")
                        .arg(parseError));
            }
            continue;
        }
        if (message->instanceId.compare(
                instanceId_,
                Qt::CaseInsensitive)
            == 0) {
            continue;
        }

        if (message->type == QStringLiteral("query")) {
            if (published_.valid()) {
                sendAnnouncementTo(sender, senderPort);
            }
            continue;
        }

        if (message->type == QStringLiteral("bye")) {
            QList<NearbyDevice> next;
            next.reserve(devices_.size());
            for (const NearbyDevice &device : devices_) {
                const bool sameSource = device.address == sender;
                const bool sameInstance =
                    device.instanceId.compare(
                        message->instanceId,
                        Qt::CaseInsensitive)
                    == 0;
                const bool sameEndpoint = device.code == message->code
                    && (message->server.isEmpty()
                        || device.server.compare(
                               message->server,
                               Qt::CaseInsensitive)
                            == 0);
                if (sameSource && sameInstance && sameEndpoint) {
                    continue;
                }
                next.append(device);
            }
            applyDevices(next);
            continue;
        }

        const QDateTime now = QDateTime::currentDateTimeUtc();
        QList<NearbyDevice> next = devices_;
        for (qsizetype index = next.size() - 1; index >= 0; --index) {
            const NearbyDevice &device = next.at(index);
            const bool sameEndpoint = device.code == message->code
                && device.server.compare(
                       message->server,
                       Qt::CaseInsensitive)
                    == 0;
            const bool sameInstance =
                device.instanceId.compare(
                    message->instanceId,
                    Qt::CaseInsensitive)
                == 0;
            if (sameEndpoint || sameInstance) {
                next.removeAt(index);
            }
        }

        NearbyDevice device;
        device.instanceId = message->instanceId;
        device.name = message->name;
        device.code = message->code;
        device.server = message->server;
        device.seenAt = now;
        device.address = sender;
        next.append(device);
        while (next.size() > options_.maxDevices) {
            next.removeFirst();
        }
        applyDevices(next);
    }
}

void LanDiscovery::sendAnnouncement()
{
    if (!published_.valid() || !active()) {
        return;
    }
    sendPayloadEverywhere(announcementPayload(published_));
}

void LanDiscovery::sendQuery()
{
    if (!active()) {
        return;
    }
    const QJsonObject object{
        {QStringLiteral("p"), QLatin1String(kProtocolName)},
        {QStringLiteral("v"), kProtocolVersion},
        {QStringLiteral("t"), QStringLiteral("query")},
        {QStringLiteral("id"), instanceId_},
        {QStringLiteral("app"), appVersion()},
    };
    sendPayloadEverywhere(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void LanDiscovery::sendAnnouncementTo(
    const QHostAddress &address,
    quint16 port)
{
    if (!published_.valid() || !active() || port == 0) {
        return;
    }
    sendPayload(
        announcementPayload(published_),
        address,
        port);
}

void LanDiscovery::sendBye(const Announcement &announcement)
{
    if (!active() || !announcement.valid()) {
        return;
    }
    const QJsonObject object{
        {QStringLiteral("p"), QLatin1String(kProtocolName)},
        {QStringLiteral("v"), kProtocolVersion},
        {QStringLiteral("t"), QStringLiteral("bye")},
        {QStringLiteral("id"), instanceId_},
        {QStringLiteral("code"), announcement.code},
        {QStringLiteral("server"), announcement.server},
        {QStringLiteral("app"), appVersion()},
    };
    sendPayloadEverywhere(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QByteArray LanDiscovery::announcementPayload(
    const Announcement &announcement) const
{
    const QJsonObject object{
        {QStringLiteral("p"), QLatin1String(kProtocolName)},
        {QStringLiteral("v"), kProtocolVersion},
        {QStringLiteral("t"), QStringLiteral("announce")},
        {QStringLiteral("id"), instanceId_},
        {QStringLiteral("name"), announcement.name},
        {QStringLiteral("code"), announcement.code},
        {QStringLiteral("server"), announcement.server},
        {QStringLiteral("app"), appVersion()},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void LanDiscovery::sendPayloadEverywhere(const QByteArray &payload)
{
    if (!active()) {
        return;
    }
    if (options_.targetAddress != QHostAddress::Broadcast) {
        sendPayload(
            payload,
            options_.targetAddress,
            options_.targetPort);
        return;
    }

    QSet<quint32> sent;
    bool wrote = false;
    for (const QNetworkInterface &interface :
         QNetworkInterface::allInterfaces()) {
        const auto interfaceFlags = interface.flags();
        if (!interfaceFlags.testFlag(QNetworkInterface::IsUp)
            || !interfaceFlags.testFlag(QNetworkInterface::IsRunning)
            || interfaceFlags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry :
             interface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            const QHostAddress mask = entry.netmask();
            if (ip.protocol() != QHostAddress::IPv4Protocol
                || mask.protocol() != QHostAddress::IPv4Protocol) {
                continue;
            }
            const quint32 broadcast =
                ip.toIPv4Address()
                | ~mask.toIPv4Address();
            if (!sent.contains(broadcast)) {
                sent.insert(broadcast);
                if (socket_->writeDatagram(
                        payload,
                        QHostAddress(broadcast),
                        options_.targetPort)
                    == payload.size()) {
                    wrote = true;
                }
            }
        }
    }

    constexpr quint32 broadcastAddress = 0xffffffffU;
    if (!sent.contains(broadcastAddress)) {
        wrote = socket_->writeDatagram(
                    payload,
                    QHostAddress::Broadcast,
                    options_.targetPort)
            == payload.size()
            || wrote;
    }
    if (!wrote) {
        const QString message =
            QStringLiteral("局域网发现广播失败：%1")
                .arg(socket_->errorString());
        if (message != lastSendError_) {
            lastSendError_ = message;
            emit logMessage(message);
        }
    } else {
        lastSendError_.clear();
    }
}

void LanDiscovery::sendPayload(
    const QByteArray &payload,
    const QHostAddress &address,
    quint16 port)
{
    if (!active() || port == 0) {
        return;
    }
    if (socket_->writeDatagram(payload, address, port)
        == payload.size()) {
        lastSendError_.clear();
        return;
    }
    const QString message =
        QStringLiteral("局域网发现发送失败：%1")
            .arg(socket_->errorString());
    if (message != lastSendError_) {
        lastSendError_ = message;
        emit logMessage(message);
    }
}

void LanDiscovery::pruneExpired()
{
    if (devices_.isEmpty()) {
        return;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addMSecs(
        -options_.expireAfterMs);
    QList<NearbyDevice> next;
    next.reserve(devices_.size());
    for (const NearbyDevice &device : devices_) {
        if (device.seenAt >= cutoff) {
            next.append(device);
        }
    }
    applyDevices(next);
}

void LanDiscovery::applyDevices(
    const QList<NearbyDevice> &devices)
{
    if (sameDeviceContent(devices_, devices)) {
        devices_ = devices;
        return;
    }
    devices_ = devices;
    emit devicesChanged(this->devices());
}

void LanDiscovery::updateAnnounceTimer()
{
    if (!announceTimer_) {
        return;
    }
    if (active() && published_.valid()) {
        announceTimer_->start(options_.announceIntervalMs);
    } else {
        announceTimer_->stop();
    }
}

} // namespace pd
