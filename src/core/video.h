#pragma once

#include "core/protocol.h"
#include "core/reliable.h"

#include <QByteArray>
#include <QHash>
#include <QList>

#include <functional>

namespace pd {

struct WirePacket {
    PacketType type = PacketType::Data;
    quint8 flags = 0;
    QByteArray payload;
};

class VideoSender {
public:
    static constexpr int kFecK = 8;

    explicit VideoSender(int mtu = kVideoPayloadMtu, int cacheFrames = 10);

    [[nodiscard]] QList<WirePacket> build(const QByteArray &data, bool keyframe);
    [[nodiscard]] QList<WirePacket> retransmit(
        quint32 frameId, const QList<quint16> &missing) const;

    [[nodiscard]] quint32 frameId() const { return frameId_; }

private:
    int mtu_;
    int cacheFrames_;
    quint32 frameId_ = 0;
    QHash<quint32, QList<QByteArray>> cache_;
    QList<quint32> order_;
};

struct VideoNack {
    quint32 frameId = 0;
    QList<quint16> missing;
};

class VideoReceiver {
public:
    using FrameHandler = std::function<void(const QByteArray &, bool)>;
    using KeyframeHandler = std::function<void()>;

    VideoReceiver(
        FrameHandler onFrame,
        KeyframeHandler onNeedKeyframe,
        int fecK = VideoSender::kFecK);

    void onFragment(
        quint32 frameId,
        quint16 index,
        quint16 count,
        const QByteArray &data,
        bool keyframe,
        double now = ReliableStream::currentTimeSeconds());
    void onParity(
        quint32 frameId,
        quint8 groupIndex,
        quint8 shards,
        const QByteArray &data);
    [[nodiscard]] QList<VideoNack> tick(double now);

    [[nodiscard]] int lostFrames() const { return lostFrames_; }
    [[nodiscard]] quint32 lastDelivered() const { return lastDelivered_; }
    [[nodiscard]] int frameStateCount() const { return frames_.size(); }

private:
    struct FrameState {
        quint16 count = 0;
        FragmentAssembler assembler;
        bool keyframe = false;
        double created = 0;
        double lastNack = 0;

        explicit FrameState(quint16 fragments = 0)
            : assembler(fragments)
        {
        }
    };

    void deliver(quint32 frameId, FrameState &state);

    FrameHandler onFrame_;
    KeyframeHandler onNeedKeyframe_;
    int fecK_;
    QHash<quint32, FrameState> frames_;
    quint32 lastDelivered_ = 0;
    int lostFrames_ = 0;
};

} // namespace pd
