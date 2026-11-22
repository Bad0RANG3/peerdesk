#include "core/crypto.h"

#include <QCryptographicHash>
#include <QList>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <utility>

extern "C" {
#include "monocypher.h"
}

namespace pd {
namespace {

constexpr char kProtocolLabel[] = "peerdesk/1";

QByteArray sha256(const QList<QByteArray> &parts)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &part : parts) {
        hash.addData(part);
    }
    return hash.result();
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(
        data, key, QCryptographicHash::Sha256);
}

QByteArray hkdfSha256(
    const QByteArray &ikm,
    const QByteArray &salt,
    const QByteArray &info,
    int length = kKeyLen)
{
    const QByteArray prk = hmacSha256(salt, ikm);
    QByteArray output;
    output.reserve(length);
    QByteArray previous;
    for (quint8 block = 1; output.size() < length; ++block) {
        QByteArray input = previous;
        input.append(info);
        input.append(static_cast<char>(block));
        previous = hmacSha256(prk, input);
        output.append(previous);
    }
    output.truncate(length);
    return output;
}

QByteArray makeNonce(int direction, quint64 seq)
{
    QByteArray nonce(kNonceLen, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(direction), nonce.data());
    qToBigEndian<quint64>(seq, nonce.data() + 4);
    return nonce;
}

bool isAllZero(const QByteArray &data)
{
    for (const char byte : data) {
        if (byte != '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

KeyPair KeyPair::generate()
{
    for (;;) {
        const QByteArray secret = randomBytes(kKeyLen);
        if (const auto pair = fromPrivate(secret)) {
            return *pair;
        }
    }
}

std::optional<KeyPair> KeyPair::fromPrivate(const QByteArray &raw)
{
    if (raw.size() != kKeyLen) {
        return std::nullopt;
    }
    KeyPair pair;
    pair.privateKey = raw;
    pair.publicKey.resize(kKeyLen);
    crypto_x25519_public_key(
        reinterpret_cast<quint8 *>(pair.publicKey.data()),
        reinterpret_cast<const quint8 *>(pair.privateKey.constData()));
    return pair;
}

QByteArray KeyPair::privateBytes() const
{
    return privateKey;
}

std::optional<SessionKeys> SessionKeys::derive(
    const KeyPair &local,
    const QByteArray &peerPublic,
    const QByteArray &helloHost,
    const QByteArray &helloClient,
    const QByteArray &psk)
{
    if (local.privateKey.size() != kKeyLen || local.publicKey.size() != kKeyLen
        || peerPublic.size() != kKeyLen) {
        return std::nullopt;
    }

    QByteArray shared(kKeyLen, '\0');
    crypto_x25519(
        reinterpret_cast<quint8 *>(shared.data()),
        reinterpret_cast<const quint8 *>(local.privateKey.constData()),
        reinterpret_cast<const quint8 *>(peerPublic.constData()));
    if (isAllZero(shared)) {
        return std::nullopt;
    }

    QByteArray label(kProtocolLabel);
    QByteArray transcriptInput = label;
    transcriptInput.append('\0');
    transcriptInput.append(helloHost);
    transcriptInput.append('\0');
    transcriptInput.append(helloClient);
    const QByteArray transcript = sha256({transcriptInput});

    QByteArray pskInput = label;
    pskInput.append("\0psk\0", 5);
    pskInput.append(psk);
    const QByteArray pskDigest = sha256({pskInput});

    QByteArray saltInput = transcript;
    saltInput.append(pskDigest);
    const QByteArray salt = sha256({saltInput});

    SessionKeys keys;
    keys.transcript = transcript;
    keys.hostToClient = hkdfSha256(shared, salt, label + " host->client");
    keys.clientToHost = hkdfSha256(shared, salt, label + " client->host");
    crypto_wipe(shared.data(), static_cast<size_t>(shared.size()));
    return keys;
}

ReplayWindow::ReplayWindow(int width)
    : width_(std::max(1, std::min(width, 2048)))
    , bitmap_(width_)
{
}

bool ReplayWindow::checkAndUpdate(quint64 seq)
{
    if (!hasHighest_) {
        hasHighest_ = true;
        highest_ = seq;
        bitmap_.fill(false);
        bitmap_.setBit(0);
        return true;
    }

    if (seq > highest_) {
        const quint64 shift = seq - highest_;
        const QBitArray previous = bitmap_;
        bitmap_.fill(false);
        if (shift >= static_cast<quint64>(width_)) {
            highest_ = seq;
            bitmap_.setBit(0);
            return true;
        }
        for (int destination = width_ - 1;
             destination >= static_cast<int>(shift);
             --destination) {
            if (previous.testBit(destination - static_cast<int>(shift))) {
                bitmap_.setBit(destination);
            }
        }
        highest_ = seq;
        bitmap_.setBit(0);
        return true;
    }

    const quint64 offset = highest_ - seq;
    if (offset >= static_cast<quint64>(width_)) {
        return false;
    }
    if (bitmap_.testBit(static_cast<int>(offset))) {
        return false;
    }
    bitmap_.setBit(static_cast<int>(offset));
    return true;
}

CryptoContext::CryptoContext(SessionKeys keys, bool isHost)
    : keys_(std::move(keys))
    , isHost_(isHost)
{
}

quint64 CryptoContext::nextSeq()
{
    return ++sendSeq_;
}

QByteArray CryptoContext::seal(
    const QByteArray &header, const QByteArray &payload, quint64 seq) const
{
    const QByteArray &key = isHost_ ? keys_.hostToClient : keys_.clientToHost;
    const QByteArray nonce = makeNonce(outgoingDirection(), seq);
    QByteArray cipherText(payload.size(), '\0');
    std::array<quint8, 16> tag{};
    crypto_aead_ctx context;
    crypto_aead_init_ietf(
        &context,
        reinterpret_cast<const quint8 *>(key.constData()),
        reinterpret_cast<const quint8 *>(nonce.constData()));
    crypto_aead_write(
        &context,
        reinterpret_cast<quint8 *>(cipherText.data()),
        tag.data(),
        reinterpret_cast<const quint8 *>(header.constData()),
        static_cast<size_t>(header.size()),
        reinterpret_cast<const quint8 *>(payload.constData()),
        static_cast<size_t>(payload.size()));
    crypto_wipe(&context, sizeof(context));
    cipherText.append(reinterpret_cast<const char *>(tag.data()), tag.size());
    return cipherText;
}

std::optional<QByteArray> CryptoContext::open(
    const QByteArray &header, const QByteArray &body, quint64 seq)
{
    if (body.size() < kAuthTagLen) {
        return std::nullopt;
    }

    const QByteArray &key = isHost_ ? keys_.clientToHost : keys_.hostToClient;
    const QByteArray nonce = makeNonce(incomingDirection(), seq);
    const qsizetype cipherSize = body.size() - kAuthTagLen;
    QByteArray plain(cipherSize, '\0');
    crypto_aead_ctx context;
    crypto_aead_init_ietf(
        &context,
        reinterpret_cast<const quint8 *>(key.constData()),
        reinterpret_cast<const quint8 *>(nonce.constData()));
    const int mismatch = crypto_aead_read(
        &context,
        reinterpret_cast<quint8 *>(plain.data()),
        reinterpret_cast<const quint8 *>(body.constData() + cipherSize),
        reinterpret_cast<const quint8 *>(header.constData()),
        static_cast<size_t>(header.size()),
        reinterpret_cast<const quint8 *>(body.constData()),
        static_cast<size_t>(cipherSize));
    crypto_wipe(&context, sizeof(context));
    if (mismatch != 0) {
        return std::nullopt;
    }
    // Only committed authenticated packets may advance the replay window.
    // Otherwise an attacker could inject a forged high sequence number and
    // lock out legitimate traffic.
    if (!recvWindow_.checkAndUpdate(seq)) {
        return std::nullopt;
    }
    return plain;
}

QByteArray randomBytes(int size)
{
    QByteArray output(std::max(0, size), '\0');
    auto *random = QRandomGenerator::system();
    for (qsizetype offset = 0; offset < output.size();) {
        const quint32 value = random->generate();
        for (int byte = 0; byte < 4 && offset < output.size(); ++byte, ++offset) {
            output[offset] = static_cast<char>((value >> (byte * 8)) & 0xFFU);
        }
    }
    return output;
}

QString fingerprint(const QList<QByteArray> &parts)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &part : parts) {
        const quint32 size = qToBigEndian<quint32>(static_cast<quint32>(part.size()));
        hash.addData(QByteArrayView(
            reinterpret_cast<const char *>(&size), sizeof(size)));
        hash.addData(part);
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace pd
