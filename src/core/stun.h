#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QMap>

#include <optional>

namespace pd::stun {

inline constexpr quint16 kBindingRequest = 0x0001;
inline constexpr quint16 kBindingSuccess = 0x0101;
inline constexpr quint16 kAttributeMappedAddress = 0x0001;
inline constexpr quint16 kAttributeXorMappedAddress = 0x0020;
inline constexpr quint32 kMagicCookie = 0x2112A442U;
inline constexpr int kTransactionIdLen = 12;

struct Message {
    quint16 type = 0;
    QByteArray transactionId;
    QMap<quint16, QByteArray> attributes;
};

struct Endpoint {
    QHostAddress address;
    quint16 port = 0;
};

[[nodiscard]] QByteArray randomTransactionId();
[[nodiscard]] QByteArray buildBindingRequest(QByteArray transactionId = {});
[[nodiscard]] QByteArray buildBindingResponse(
    const QByteArray &transactionId,
    const QHostAddress &address,
    quint16 port);

[[nodiscard]] std::optional<Message> parseMessage(const QByteArray &datagram);
[[nodiscard]] std::optional<Endpoint> parseMappedAddress(
    const QMap<quint16, QByteArray> &attributes,
    const QByteArray &transactionId);

} // namespace pd::stun
