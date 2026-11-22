#include "core/video.h"

#include <algorithm>

namespace pd {

VideoSender::VideoSender(int mtu, int cacheFrames)
    : mtu_(std::max(200, mtu))
    , cacheFrames_(std::max(1, cacheFrames))
{
}

QList<WirePacket> VideoSender::build(const QByteArray &data, bool keyframe)
{
    ++frameId_;
    const quint32 currentFrame = frameId_;
    const QList<QByteArray> fragments = fragment(data, mtu_);
    QList<WirePacket> output;
    output.reserve(fragments.size() + fragments.size() / kFecK);

    for (qsizetype index = 0; index < fragments.size(); ++index) {
        quint8 flags = index == fragments.size() - 1 ? FlagLastFragment : 0;
        if (keyframe) {
            flags = static_cast<quint8>(flags | FlagKeyframe);
        }
        VideoFragment part;
        part.frameId = currentFrame;
        part.index = static_cast<quint16>(index);
        part.count = static_cast<quint16>(fragments.size());
        part.data = fragments[index];
        output.append(
            WirePacket{PacketType::Data, flags, packVideoFragment(part)});
    }

    if (kFecK > 1) {
        for (int start = 0, group = 0; start < fragments.size();
             start += kFecK, ++group) {
            const QList<QByteArray> shards =
                fragments.mid(start, kFecK);
            if (shards.size() < 3) {
                continue;
            }
            int paritySize = 0;
            for (const auto &shard : shards) {
                paritySize = std::max(paritySize, static_cast<int>(shard.size()));
            }
            ParityFragment parity;
            parity.frameId = currentFrame;
            parity.groupIndex = static_cast<quint8>(group);
            parity.shards = static_cast<quint8>(shards.size());
            parity.data = xorBytes(shards, paritySize);
            output.append(
                WirePacket{PacketType::Parity, 0, packParityFragment(parity)});
        }
    }

    cache_.insert(currentFrame, fragments);
    order_.append(currentFrame);
    while (order_.size() > cacheFrames_) {
        cache_.remove(order_.takeFirst());
    }
    return output;
}

QList<WirePacket> VideoSender::retransmit(
    quint32 frameId, const QList<quint16> &missing) const
{
    const auto it = cache_.constFind(frameId);
    if (it == cache_.constEnd()) {
        return {};
    }
    const QList<QByteArray> &fragments = *it;
    QList<WirePacket> output;
    for (const quint16 index : missing) {
        if (index >= fragments.size()) {
            continue;
        }
        VideoFragment part;
        part.frameId = frameId;
        part.index = index;
        part.count = static_cast<quint16>(fragments.size());
        part.data = fragments[index];
        const quint8 flags = index == fragments.size() - 1
            ? FlagLastFragment
            : 0;
        output.append(
            WirePacket{PacketType::Data, flags, packVideoFragment(part)});
    }
    return output;
}

VideoReceiver::VideoReceiver(
    FrameHandler onFrame, KeyframeHandler onNeedKeyframe, int fecK)
    : onFrame_(std::move(onFrame))
    , onNeedKeyframe_(std::move(onNeedKeyframe))
    , fecK_(std::max(1, fecK))
{
}

void VideoReceiver::onFragment(
    quint32 frameId,
    quint16 index,
    quint16 count,
    const QByteArray &data,
    bool keyframe,
    double now)
{
    auto it = frames_.find(frameId);
    if (it == frames_.end()) {
        if (count == 0) {
            return;
        }
        FrameState state(count);
        state.count = count;
        state.keyframe = keyframe;
        state.created = now;
        it = frames_.insert(frameId, state);
    }
    (void)it->assembler.add(index, data);
    if (it->assembler.complete()) {
        deliver(frameId, *it);
    }
}

void VideoReceiver::onParity(
    quint32 frameId,
    quint8 groupIndex,
    quint8 shards,
    const QByteArray &data)
{
    Q_UNUSED(shards);
    auto it = frames_.find(frameId);
    if (it == frames_.end()) {
        return;
    }
    const int start = static_cast<int>(groupIndex) * fecK_;
    const int end = std::min(start + fecK_, static_cast<int>(it->count));
    QList<int> indices;
    for (int index = start; index < end; ++index) {
        indices.append(index);
    }
    if (it->assembler.fillParity(indices, data)
        && it->assembler.complete()) {
        deliver(frameId, *it);
    }
}

QList<VideoNack> VideoReceiver::tick(double now)
{
    QList<VideoNack> requests;
    const auto keys = frames_.keys();
    for (const quint32 frameId : keys) {
        auto it = frames_.find(frameId);
        if (it == frames_.end()) {
            continue;
        }
        if (now - it->created > 0.45) {
            const bool wasKeyframe = it->keyframe;
            frames_.erase(it);
            ++lostFrames_;
            if (wasKeyframe && onNeedKeyframe_) {
                onNeedKeyframe_();
            }
            continue;
        }
        if (now - it->lastNack > 0.05 && frames_.size() <= 4) {
            const QList<quint16> missing = it->assembler.missing();
            if (!missing.isEmpty()) {
                it->lastNack = now;
                requests.append(
                    VideoNack{frameId, missing.mid(0, 32)});
            }
        }
    }
    return requests;
}

void VideoReceiver::deliver(quint32 frameId, FrameState &state)
{
    const QByteArray frame = state.assembler.assemble();
    const bool keyframe = state.keyframe;
    frames_.remove(frameId);
    lastDelivered_ = frameId;
    if (onFrame_) {
        onFrame_(frame, keyframe);
    }
}

} // namespace pd
