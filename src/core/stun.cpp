#include "core/stun.h"

#include "core/crypto.h"

#include <QtEndian>

#include <array>
#include <cstring>

namespace pd::stun {
namespace {

constexpr int kHeaderLen = 20;
constexpr int kAttributeHeaderLen = 4;

void putU16(char *out, quint16 value)
{
    qToBigEndian<quint16>(value, out);
}

void putU32(char *out, quint32 value)
{
    qToBigEndian<quint32>(value, out);
}

quint16 getU16(const char *in)
{
    return qFromBigEndian<quint16>(in);
}

quint32 getU32(const char *in)
{
    return qFromBigEndian<quint32>(in);
}

QByteArray xorAddress(
    const QByteArray &transactionId,
    const QHostAddress &address,
    quint16 port)
{
    std::array<quint8, 16> raw{};
    int addressLen = 0;
    quint8 family = 0;
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ipv4 = qToBigEndian<quint32>(address.toIPv4Address());
        std::memcpy(raw.data(), &ipv4, sizeof(ipv4));
        addressLen = 4;
        family = 0x01;
    } else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const auto ipv6 = address.toIPv6Address();
        std::memcpy(raw.data(), ipv6.c, 16);
        addressLen = 16;
        family = 0x02;
    } else {
        return {};
    }

    const quint16 xoredPort = static_cast<quint16>(port ^ (kMagicCookie >> 16));
    QByteArray output(4 + addressLen, '\0');
    output[0] = '\0';
    output[1] = static_cast<char>(family);
    putU16(output.data() + 2, xoredPort);

    std::array<quint8, 16> mask{};
    putU32(reinterpret_cast<char *>(mask.data()), kMagicCookie);
    std::memcpy(mask.data() + 4, transactionId.constData(),
                static_cast<size_t>(kTransactionIdLen));
    for (int index = 0; index < addressLen; ++index) {
        output[4 + index] = static_cast<char>(raw[index] ^ mask[index]);
    }
    return output;
}

QByteArray buildMessage(
    quint16 type,
    const QByteArray &transactionId,
    const QMap<quint16, QByteArray> &attributes)
{
    if (transactionId.size() != kTransactionIdLen) {
        return {};
    }

    QByteArray body;
    for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it) {
        const int padding = (4 - it.value().size() % 4) % 4;
        const qsizetype offset = body.size();
        body.resize(offset + kAttributeHeaderLen + it.value().size() + padding);
        putU16(body.data() + offset, it.key());
        putU16(body.data() + offset + 2, static_cast<quint16>(it.value().size()));
        std::memcpy(body.data() + offset + kAttributeHeaderLen,
                    it.value().constData(),
                    it.value().size());
    }

    QByteArray packet(kHeaderLen + body.size(), '\0');
    putU16(packet.data(), type);
    putU16(packet.data() + 2, static_cast<quint16>(body.size()));
    putU32(packet.data() + 4, kMagicCookie);
    std::memcpy(packet.data() + 8, transactionId.constData(),
                static_cast<size_t>(kTransactionIdLen));
    std::memcpy(packet.data() + kHeaderLen, body.constData(),
                static_cast<size_t>(body.size()));
    return packet;
}

} // namespace

QByteArray randomTransactionId()
{
    return randomBytes(kTransactionIdLen);
}

QByteArray buildBindingRequest(QByteArray transactionId)
{
    if (transactionId.isEmpty()) {
        transactionId = randomTransactionId();
    }
    return buildMessage(kBindingRequest, transactionId, {});
}

QByteArray buildBindingResponse(
    const QByteArray &transactionId,
    const QHostAddress &address,
    quint16 port)
{
    if (port == 0 || transactionId.size() != kTransactionIdLen) {
        return {};
    }
    const QByteArray encoded = xorAddress(transactionId, address, port);
    if (encoded.isEmpty()) {
        return {};
    }
    return buildMessage(
        kBindingSuccess,
        transactionId,
        {{kAttributeXorMappedAddress, encoded}});
}

std::optional<Message> parseMessage(const QByteArray &datagram)
{
    if (datagram.size() < kHeaderLen) {
        return std::nullopt;
    }
    const quint16 declaredLength = getU16(datagram.constData() + 2);
    if (declaredLength % 4 != 0
        || datagram.size() < kHeaderLen + declaredLength) {
        return std::nullopt;
    }
    if (getU32(datagram.constData() + 4) != kMagicCookie) {
        return std::nullopt;
    }

    Message message;
    message.type = getU16(datagram.constData());
    message.transactionId = datagram.mid(8, kTransactionIdLen);

    int offset = kHeaderLen;
    const int end = kHeaderLen + declaredLength;
    while (offset + kAttributeHeaderLen <= end) {
        const quint16 type = getU16(datagram.constData() + offset);
        const quint16 length = getU16(datagram.constData() + offset + 2);
        offset += kAttributeHeaderLen;
        if (offset + static_cast<int>(length) > end) {
            return std::nullopt;
        }
        message.attributes.insert(
            type, datagram.mid(offset, static_cast<int>(length)));
        offset += length + ((4 - length % 4) % 4);
        if (offset > end) {
            return std::nullopt;
        }
    }
    return message;
}

std::optional<Endpoint> parseMappedAddress(
    const QMap<quint16, QByteArray> &attributes,
    const QByteArray &transactionId)
{
    if (transactionId.size() != kTransactionIdLen) {
        return std::nullopt;
    }

    bool xored = true;
    QByteArray raw = attributes.value(kAttributeXorMappedAddress);
    if (raw.isEmpty()) {
        raw = attributes.value(kAttributeMappedAddress);
        xored = false;
    }
    if (raw.size() < 4) {
        return std::nullopt;
    }

    const quint8 family = static_cast<quint8>(raw[1]);
    int addressLen = 0;
    if (family == 0x01) {
        addressLen = 4;
    } else if (family == 0x02) {
        addressLen = 16;
    } else {
        return std::nullopt;
    }
    if (raw.size() < 4 + addressLen) {
        return std::nullopt;
    }

    quint16 port = getU16(raw.constData() + 2);
    std::array<quint8, 16> addressBytes{};
    std::memcpy(addressBytes.data(), raw.constData() + 4,
                static_cast<size_t>(addressLen));
    if (xored) {
        std::array<quint8, 16> mask{};
        putU32(reinterpret_cast<char *>(mask.data()), kMagicCookie);
        std::memcpy(mask.data() + 4, transactionId.constData(),
                    static_cast<size_t>(kTransactionIdLen));
        port = static_cast<quint16>(port ^ (kMagicCookie >> 16));
        for (int index = 0; index < addressLen; ++index) {
            addressBytes[index] ^= mask[index];
        }
    }

    Endpoint endpoint;
    endpoint.port = port;
    if (family == 0x01) {
        const quint32 ipv4 = qFromBigEndian<quint32>(
            reinterpret_cast<const char *>(addressBytes.data()));
        endpoint.address = QHostAddress(ipv4);
    } else {
        endpoint.address = QHostAddress(addressBytes.data());
    }
    return endpoint;
}

} // namespace pd::stun
