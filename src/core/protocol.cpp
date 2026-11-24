#include "core/protocol.h"

#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace pd {
namespace {

constexpr char kMagic[] = {'P', 'D'};
constexpr char kRelayMagic[] = {'R', 'L', 'Y', '1'};
constexpr quint8 kControlFragmentTag = 0xF1;
constexpr quint8 kCursorVisibleFlag = 0x01;

void putU16(char *out, quint16 value)
{
    qToLittleEndian<quint16>(value, out);
}

void putU32(char *out, quint32 value)
{
    qToLittleEndian<quint32>(value, out);
}

quint16 getU16(const char *in)
{
    return qFromLittleEndian<quint16>(in);
}

quint32 getU32(const char *in)
{
    return qFromLittleEndian<quint32>(in);
}

} // namespace

QByteArray Header::pack() const
{
    QByteArray out(kHeaderLen, '\0');
    out[0] = kMagic[0];
    out[1] = kMagic[1];
    out[2] = static_cast<char>(version);
    out[3] = static_cast<char>(type);
    putU32(out.data() + 4, seq);
    putU32(out.data() + 8, tsMs);
    out[12] = static_cast<char>(stream);
    out[13] = static_cast<char>(flags);
    putU16(out.data() + 14, plen);
    putU32(out.data() + 16, session);
    return out;
}

std::optional<Header> Header::parse(const QByteArray &raw)
{
    if (raw.size() < kHeaderLen) {
        return std::nullopt;
    }
    if (raw[0] != kMagic[0] || raw[1] != kMagic[1]) {
        return std::nullopt;
    }
    if (static_cast<quint8>(raw[2]) != kVersion) {
        return std::nullopt;
    }

    Header header;
    header.version = static_cast<quint8>(raw[2]);
    header.type = static_cast<PacketType>(static_cast<quint8>(raw[3]));
    header.seq = getU32(raw.constData() + 4);
    header.tsMs = getU32(raw.constData() + 8);
    header.stream = static_cast<quint8>(raw[12]);
    header.flags = static_cast<quint8>(raw[13]);
    header.plen = getU16(raw.constData() + 14);
    header.session = getU32(raw.constData() + 16);
    return header;
}

QByteArray jsonPayload(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<QJsonObject> parseJson(const QByteArray &payload)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

QList<QByteArray> fragment(const QByteArray &data, int mtuPayload)
{
    if (mtuPayload <= 0) {
        return {};
    }
    if (data.isEmpty()) {
        return {QByteArray()};
    }

    QList<QByteArray> result;
    result.reserve((data.size() + mtuPayload - 1) / mtuPayload);
    for (qsizetype offset = 0; offset < data.size(); offset += mtuPayload) {
        result.append(data.mid(offset, mtuPayload));
    }
    return result;
}

QByteArray xorBytes(const QList<QByteArray> &chunks, int size)
{
    if (size < 0) {
        size = 0;
        for (const auto &chunk : chunks) {
            size = std::max(size, static_cast<int>(chunk.size()));
        }
    }

    QByteArray result(size, '\0');
    for (const auto &chunk : chunks) {
        const int limit = std::min(size, static_cast<int>(chunk.size()));
        for (int index = 0; index < limit; ++index) {
            result[index] = static_cast<char>(
                static_cast<quint8>(result[index]) ^ static_cast<quint8>(chunk[index]));
        }
    }
    return result;
}

QByteArray packVideoFragment(const VideoFragment &fragment)
{
    QByteArray payload(kVideoFragHeaderLen + fragment.data.size(), '\0');
    putU32(payload.data(), fragment.frameId);
    putU16(payload.data() + 4, fragment.index);
    putU16(payload.data() + 6, fragment.count);
    std::memcpy(payload.data() + kVideoFragHeaderLen, fragment.data.constData(), fragment.data.size());
    return payload;
}

std::optional<VideoFragment> unpackVideoFragment(const QByteArray &payload)
{
    if (payload.size() < kVideoFragHeaderLen) {
        return std::nullopt;
    }
    VideoFragment fragment;
    fragment.frameId = getU32(payload.constData());
    fragment.index = getU16(payload.constData() + 4);
    fragment.count = getU16(payload.constData() + 6);
    fragment.data = payload.mid(kVideoFragHeaderLen);
    return fragment;
}

QByteArray packParityFragment(const ParityFragment &fragment)
{
    QByteArray payload(kParityHeaderLen + fragment.data.size(), '\0');
    putU32(payload.data(), fragment.frameId);
    payload[4] = static_cast<char>(fragment.groupIndex);
    payload[5] = static_cast<char>(fragment.shards);
    payload[6] = 0;
    payload[7] = 0;
    std::memcpy(payload.data() + kParityHeaderLen,
                fragment.data.constData(),
                fragment.data.size());
    return payload;
}

std::optional<ParityFragment> unpackParityFragment(const QByteArray &payload)
{
    if (payload.size() < kParityHeaderLen) {
        return std::nullopt;
    }
    ParityFragment fragment;
    fragment.frameId = getU32(payload.constData());
    fragment.groupIndex = static_cast<quint8>(payload[4]);
    fragment.shards = static_cast<quint8>(payload[5]);
    fragment.data = payload.mid(kParityHeaderLen);
    return fragment;
}

QByteArray packAck(const AckPayload &ack)
{
    QByteArray payload(12, '\0');
    putU32(payload.data(), ack.cumulative);
    putU32(payload.data() + 4, static_cast<quint32>(ack.bitmap & 0xFFFFFFFFU));
    putU32(payload.data() + 8, static_cast<quint32>(ack.bitmap >> 32));
    return payload;
}

std::optional<AckPayload> unpackAck(const QByteArray &payload)
{
    if (payload.size() < 12) {
        return std::nullopt;
    }
    AckPayload ack;
    ack.cumulative = getU32(payload.constData());
    ack.bitmap = static_cast<quint64>(getU32(payload.constData() + 4))
        | (static_cast<quint64>(getU32(payload.constData() + 8)) << 32);
    return ack;
}

QByteArray packNack(quint32 frameId, const QList<quint16> &missing)
{
    QByteArray payload(6 + missing.size() * 2, '\0');
    putU32(payload.data(), frameId);
    putU16(payload.data() + 4, static_cast<quint16>(missing.size()));
    for (qsizetype index = 0; index < missing.size(); ++index) {
        putU16(payload.data() + 6 + index * 2, missing[index]);
    }
    return payload;
}

std::optional<QPair<quint32, QList<quint16>>> unpackNack(const QByteArray &payload)
{
    if (payload.size() < 6) {
        return std::nullopt;
    }
    const quint32 frameId = getU32(payload.constData());
    const quint16 count = getU16(payload.constData() + 4);
    if (payload.size() < 6 + static_cast<int>(count) * 2) {
        return std::nullopt;
    }
    QList<quint16> missing;
    missing.reserve(count);
    for (quint16 index = 0; index < count; ++index) {
        missing.append(getU16(payload.constData() + 6 + static_cast<int>(index) * 2));
    }
    return qMakePair(frameId, missing);
}

QByteArray packAudio(const AudioPacket &packet)
{
    QByteArray payload(12 + packet.previous.size() + packet.current.size(), '\0');
    putU32(payload.data(), packet.sequence);
    putU32(payload.data() + 4, packet.timestampMs);
    putU16(payload.data() + 8, packet.samples);
    putU16(payload.data() + 10, static_cast<quint16>(packet.previous.size()));
    std::memcpy(payload.data() + 12, packet.previous.constData(), packet.previous.size());
    std::memcpy(payload.data() + 12 + packet.previous.size(),
                packet.current.constData(), packet.current.size());
    return payload;
}

std::optional<AudioPacket> unpackAudio(const QByteArray &payload)
{
    if (payload.size() < 12) {
        return std::nullopt;
    }
    AudioPacket packet;
    packet.sequence = getU32(payload.constData());
    packet.timestampMs = getU32(payload.constData() + 4);
    packet.samples = getU16(payload.constData() + 8);
    const quint16 previousLength = getU16(payload.constData() + 10);
    if (payload.size() < 12 + static_cast<int>(previousLength)) {
        return std::nullopt;
    }
    packet.previous = payload.mid(12, previousLength);
    packet.current = payload.mid(12 + previousLength);
    return packet;
}

QByteArray packCursor(const CursorPacket &packet)
{
    QByteArray payload(kCursorPayloadLen, '\0');
    putU32(payload.data(), packet.sequence);
    putU16(payload.data() + 4, packet.x);
    putU16(payload.data() + 6, packet.y);
    putU16(payload.data() + 8, packet.hotspotX);
    putU16(payload.data() + 10, packet.hotspotY);
    payload[12] = static_cast<char>(packet.visible ? kCursorVisibleFlag : 0);
    payload[13] = static_cast<char>(packet.shape);
    putU32(payload.data() + 14, packet.imageId);
    return payload;
}

std::optional<CursorPacket> unpackCursor(const QByteArray &payload)
{
    if (payload.size() != kLegacyCursorPayloadLen
        && payload.size() != kCursorPayloadLen) {
        return std::nullopt;
    }
    const quint8 flags = static_cast<quint8>(payload[12]);
    if ((flags & ~kCursorVisibleFlag) != 0) {
        return std::nullopt;
    }
    const quint8 shape = static_cast<quint8>(payload[13]);
    if (shape > static_cast<quint8>(CursorShape::Bitmap)) {
        return std::nullopt;
    }

    CursorPacket packet;
    packet.sequence = getU32(payload.constData());
    packet.x = getU16(payload.constData() + 4);
    packet.y = getU16(payload.constData() + 6);
    packet.hotspotX = getU16(payload.constData() + 8);
    packet.hotspotY = getU16(payload.constData() + 10);
    packet.visible = (flags & kCursorVisibleFlag) != 0;
    packet.shape = static_cast<CursorShape>(shape);
    if (payload.size() == kCursorPayloadLen) {
        packet.imageId = getU32(payload.constData() + 14);
    }
    return packet;
}

FragmentAssembler::FragmentAssembler(int fragmentCount, int maxBytes)
    : maxBytes_(maxBytes)
    , parts_(std::max(0, fragmentCount))
    , present_(std::max(0, fragmentCount), false)
{
}

bool FragmentAssembler::add(quint16 index, const QByteArray &data)
{
    if (index >= parts_.size() || present_[index]) {
        return false;
    }
    parts_[index] = data;
    present_[index] = true;
    ++received_;
    return true;
}

bool FragmentAssembler::fillParity(const QList<int> &groupIndices, const QByteArray &parity)
{
    int missingIndex = -1;
    QList<QByteArray> known;
    known.reserve(groupIndices.size());

    for (const int index : groupIndices) {
        if (index < 0 || index >= parts_.size()) {
            return false;
        }
        if (!present_[index]) {
            if (missingIndex >= 0) {
                return false;
            }
            missingIndex = index;
        } else {
            known.append(parts_[index]);
        }
    }

    if (missingIndex < 0 || known.size() != groupIndices.size() - 1) {
        return false;
    }
    known.append(parity);
    parts_[missingIndex] = xorBytes(known, parity.size());
    present_[missingIndex] = true;
    ++received_;
    return true;
}

bool FragmentAssembler::complete() const
{
    return received_ == parts_.size();
}

QByteArray FragmentAssembler::assemble() const
{
    QByteArray result;
    for (const auto &part : parts_) {
        if (result.size() + part.size() > maxBytes_) {
            return {};
        }
        result.append(part);
    }
    return result;
}

QList<quint16> FragmentAssembler::missing() const
{
    QList<quint16> result;
    for (qsizetype index = 0; index < present_.size(); ++index) {
        if (!present_[index]) {
            result.append(static_cast<quint16>(index));
        }
    }
    return result;
}

ControlFragments::ControlFragments(int chunk, int maxPending)
    : chunk_(std::max(64, chunk))
    , maxPending_(std::max(1, maxPending))
{
}

QList<QByteArray> ControlFragments::split(const QByteArray &body)
{
    if (body.size() <= chunk_) {
        return {body};
    }

    const quint32 messageId = nextMessageId_++;
    const quint16 count = static_cast<quint16>((body.size() + chunk_ - 1) / chunk_);
    QList<QByteArray> result;
    result.reserve(count);
    for (quint16 index = 0; index < count; ++index) {
        QByteArray part(9, '\0');
        part[0] = static_cast<char>(kControlFragmentTag);
        putU32(part.data() + 1, messageId);
        putU16(part.data() + 5, index);
        putU16(part.data() + 7, count);
        part.append(body.mid(static_cast<qsizetype>(index) * chunk_, chunk_));
        result.append(part);
    }
    return result;
}

std::optional<QByteArray> ControlFragments::feed(const QByteArray &payload)
{
    if (payload.isEmpty() || static_cast<quint8>(payload[0]) != kControlFragmentTag) {
        return payload;
    }
    if (payload.size() < 9) {
        return std::nullopt;
    }

    const quint32 messageId = getU32(payload.constData() + 1);
    const quint16 index = getU16(payload.constData() + 5);
    const quint16 count = getU16(payload.constData() + 7);
    if (count == 0 || index >= count) {
        return std::nullopt;
    }

    auto it = pending_.find(messageId);
    if (it == pending_.end()) {
        while (static_cast<int>(pending_.size()) >= maxPending_ && !pendingOrder_.isEmpty()) {
            pending_.remove(pendingOrder_.takeFirst());
        }
        it = pending_.insert(messageId, Pending{});
        it->count = count;
        pendingOrder_.append(messageId);
    }
    if (it->count != count || it->parts.contains(index)) {
        return std::nullopt;
    }
    it->parts.insert(index, payload.mid(9));
    if (it->parts.size() < it->count) {
        return std::nullopt;
    }

    QByteArray result;
    for (quint16 partIndex = 0; partIndex < it->count; ++partIndex) {
        result.append(it->parts.value(partIndex));
    }
    pending_.remove(messageId);
    pendingOrder_.removeAll(messageId);
    return result;
}

void ControlFragments::reset()
{
    nextMessageId_ = 0;
    pending_.clear();
    pendingOrder_.clear();
}

QByteArray wrapRelay(quint32 tag, const QByteArray &packet)
{
    QByteArray datagram(kRelayHeaderLen + packet.size(), '\0');
    std::memcpy(datagram.data(), kRelayMagic, 4);
    qToBigEndian<quint32>(tag, datagram.data() + 4);
    std::memcpy(datagram.data() + kRelayHeaderLen, packet.constData(), packet.size());
    return datagram;
}

std::optional<QPair<quint32, QByteArray>> unwrapRelay(const QByteArray &datagram)
{
    if (datagram.size() < kRelayHeaderLen
        || std::memcmp(datagram.constData(), kRelayMagic, 4) != 0) {
        return std::nullopt;
    }
    const quint32 tag = qFromBigEndian<quint32>(datagram.constData() + 4);
    return qMakePair(tag, datagram.mid(kRelayHeaderLen));
}

} // namespace pd
