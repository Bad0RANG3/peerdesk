#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace pd {

inline constexpr int kHeaderLen = 20;
inline constexpr int kTagLen = 16;
inline constexpr int kMaxDatagram = 1400;
inline constexpr int kPayloadMtu = kMaxDatagram - kHeaderLen - kTagLen;
inline constexpr int kVideoFragHeaderLen = 8;
inline constexpr int kVideoPayloadMtu = kPayloadMtu - kVideoFragHeaderLen;
inline constexpr int kFecK = 8;
inline constexpr int kControlChunk = 1200;
inline constexpr int kRelayHeaderLen = 8;
inline constexpr quint8 kVersion = 1;

enum class PacketType : quint8 {
    Hello = 1,
    HelloAck = 2,
    HelloFin = 3,
    Punch = 4,
    PunchAck = 5,
    Data = 6,
    Parity = 7,
    Ack = 8,
    Nack = 9,
    Ping = 10,
    Pong = 11,
    Bye = 12,
    Audio = 13,
};

enum class StreamKind : quint8 {
    Video = 0,
    Control = 1,
    File = 2,
    Stats = 3,
    Audio = 4,
};

enum PacketFlags : quint8 {
    FlagKeyframe = 0x01,
    FlagLastFragment = 0x02,
    FlagReliable = 0x04,
};

struct Header {
    quint8 version = kVersion;
    PacketType type = PacketType::Data;
    quint32 seq = 0;
    quint32 tsMs = 0;
    quint8 stream = static_cast<quint8>(StreamKind::Video);
    quint8 flags = 0;
    quint16 plen = 0;
    quint32 session = 0;

    [[nodiscard]] QByteArray pack() const;
    [[nodiscard]] static std::optional<Header> parse(const QByteArray &raw);
};

[[nodiscard]] QByteArray jsonPayload(const QJsonObject &object);
[[nodiscard]] std::optional<QJsonObject> parseJson(const QByteArray &payload);

[[nodiscard]] QList<QByteArray> fragment(const QByteArray &data, int mtuPayload);
[[nodiscard]] QByteArray xorBytes(const QList<QByteArray> &chunks, int size = -1);

struct VideoFragment {
    quint32 frameId = 0;
    quint16 index = 0;
    quint16 count = 0;
    QByteArray data;
};

[[nodiscard]] QByteArray packVideoFragment(const VideoFragment &fragment);
[[nodiscard]] std::optional<VideoFragment> unpackVideoFragment(const QByteArray &payload);

struct ParityFragment {
    quint32 frameId = 0;
    quint8 groupIndex = 0;
    quint8 shards = 0;
    QByteArray data;
};

inline constexpr int kParityHeaderLen = 8;

[[nodiscard]] QByteArray packParityFragment(const ParityFragment &fragment);
[[nodiscard]] std::optional<ParityFragment> unpackParityFragment(const QByteArray &payload);

struct AckPayload {
    quint32 cumulative = 0;
    quint64 bitmap = 0;
};

[[nodiscard]] QByteArray packAck(const AckPayload &ack);
[[nodiscard]] std::optional<AckPayload> unpackAck(const QByteArray &payload);
[[nodiscard]] QByteArray packNack(quint32 frameId, const QList<quint16> &missing);
[[nodiscard]] std::optional<QPair<quint32, QList<quint16>>> unpackNack(const QByteArray &payload);

struct AudioPacket {
    quint32 sequence = 0;
    quint32 timestampMs = 0;
    quint16 samples = 0;
    QByteArray previous;
    QByteArray current;
};

[[nodiscard]] QByteArray packAudio(const AudioPacket &packet);
[[nodiscard]] std::optional<AudioPacket> unpackAudio(const QByteArray &payload);

class FragmentAssembler {
public:
    explicit FragmentAssembler(int fragmentCount, int maxBytes = 32 * 1024 * 1024);

    [[nodiscard]] bool add(quint16 index, const QByteArray &data);
    [[nodiscard]] bool fillParity(const QList<int> &groupIndices, const QByteArray &parity);
    [[nodiscard]] bool complete() const;
    [[nodiscard]] QByteArray assemble() const;
    [[nodiscard]] QList<quint16> missing() const;
    [[nodiscard]] int fragmentCount() const { return parts_.size(); }
    [[nodiscard]] int receivedCount() const { return received_; }

private:
    int maxBytes_;
    int received_ = 0;
    QList<QByteArray> parts_;
    QList<bool> present_;
};

class ControlFragments {
public:
    explicit ControlFragments(int chunk = kControlChunk, int maxPending = 16);

    [[nodiscard]] QList<QByteArray> split(const QByteArray &body);
    [[nodiscard]] std::optional<QByteArray> feed(const QByteArray &payload);

private:
    struct Pending {
        quint16 count = 0;
        QHash<quint16, QByteArray> parts;
    };

    int chunk_;
    int maxPending_;
    quint32 nextMessageId_ = 0;
    QHash<quint32, Pending> pending_;
    QList<quint32> pendingOrder_;
};

[[nodiscard]] QByteArray wrapRelay(quint32 tag, const QByteArray &packet);
[[nodiscard]] std::optional<QPair<quint32, QByteArray>> unwrapRelay(const QByteArray &datagram);

} // namespace pd
