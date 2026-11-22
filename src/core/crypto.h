#pragma once

#include <QByteArray>
#include <QBitArray>
#include <QList>
#include <QString>

#include <array>
#include <optional>

namespace pd {

inline constexpr int kKeyLen = 32;
inline constexpr int kNonceLen = 12;
inline constexpr int kAuthTagLen = 16;

struct KeyPair {
    QByteArray privateKey;
    QByteArray publicKey;

    [[nodiscard]] static KeyPair generate();
    [[nodiscard]] static std::optional<KeyPair> fromPrivate(const QByteArray &raw);
    [[nodiscard]] QByteArray privateBytes() const;
};

struct SessionKeys {
    QByteArray hostToClient;
    QByteArray clientToHost;
    QByteArray transcript;

    [[nodiscard]] static std::optional<SessionKeys> derive(
        const KeyPair &local,
        const QByteArray &peerPublic,
        const QByteArray &helloHost,
        const QByteArray &helloClient,
        const QByteArray &psk = {});
};

class ReplayWindow {
public:
    explicit ReplayWindow(int width = 2048);

    [[nodiscard]] bool checkAndUpdate(quint64 seq);
    [[nodiscard]] quint64 highest() const { return highest_; }

private:
    int width_;
    bool hasHighest_ = false;
    quint64 highest_ = 0;
    QBitArray bitmap_;
};

class CryptoContext {
public:
    CryptoContext(SessionKeys keys, bool isHost);

    [[nodiscard]] quint64 nextSeq();
    [[nodiscard]] QByteArray seal(
        const QByteArray &header, const QByteArray &payload, quint64 seq) const;
    [[nodiscard]] std::optional<QByteArray> open(
        const QByteArray &header, const QByteArray &body, quint64 seq);

    [[nodiscard]] const SessionKeys &keys() const { return keys_; }
    [[nodiscard]] bool isHost() const { return isHost_; }

private:
    [[nodiscard]] int outgoingDirection() const { return isHost_ ? 0 : 1; }
    [[nodiscard]] int incomingDirection() const { return isHost_ ? 1 : 0; }

    SessionKeys keys_;
    bool isHost_;
    quint64 sendSeq_ = 0;
    ReplayWindow recvWindow_;
};

[[nodiscard]] QByteArray randomBytes(int size);
[[nodiscard]] QString fingerprint(const QList<QByteArray> &parts);

} // namespace pd
