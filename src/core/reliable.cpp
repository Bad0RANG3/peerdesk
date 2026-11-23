#include "core/reliable.h"

#include <algorithm>
#include <chrono>

namespace pd {

ReliableStream::ReliableStream(MessageHandler onMessage, RawSender sendRaw)
    : onMessage_(std::move(onMessage))
    , sendRaw_(std::move(sendRaw))
{
}

quint32 ReliableStream::send(const QByteArray &payload, quint8 flags, double now)
{
    const quint32 seq = nextSeq_++;
    hasSent_ = true;
    pending_.insert(seq, Pending{payload, now + kRtoMin, 0});
    sendRaw_(seq, payload, flags);
    return seq;
}

void ReliableStream::tick(double now)
{
    const auto keys = pending_.keys();
    for (const quint32 seq : keys) {
        auto it = pending_.find(seq);
        if (it == pending_.end() || it->deadline > now) {
            continue;
        }
        if (it->retries >= kMaxRetries) {
            pending_.erase(it);
            continue;
        }
        it->deadline = now + std::min(
            kRtoMin * static_cast<double>(1LL << (it->retries + 1)), kRtoMax);
        ++it->retries;
        sendRaw_(seq, it->payload, FlagReliable);
    }
}

void ReliableStream::onAck(quint32 cumulative, quint64 bitmap)
{
    const bool hasCumulative = cumulative != kNoCumulativeAck;
    if (hasCumulative) {
        if (!hasSent_
            || static_cast<qint32>(cumulative - nextSeq_) >= 0) {
            return;
        }
        const auto keys = pending_.keys();
        for (const quint32 seq : keys) {
            if (static_cast<qint32>(cumulative - seq) >= 0) {
                pending_.remove(seq);
            }
        }
    }
    const quint32 base = hasCumulative ? cumulative + 1 : 0;
    for (int bit = 0; bit < 64; ++bit) {
        if ((bitmap & (quint64(1) << bit)) == 0) {
            continue;
        }
        pending_.remove(base + static_cast<quint32>(bit));
    }
}

void ReliableStream::onData(quint32 seq, const QByteArray &payload)
{
    const quint32 offset = seq - recvNext_;
    if (offset > kReceiveWindow || recvBuffer_.contains(seq)) {
        ackDirty_ = true;
        return;
    }
    recvBuffer_.insert(seq, payload);
    ackDirty_ = true;
    while (recvBuffer_.contains(recvNext_)) {
        const QByteArray message = recvBuffer_.take(recvNext_);
        ++recvNext_;
        if (onMessage_) {
            onMessage_(message);
        }
    }
}

void ReliableStream::reset()
{
    nextSeq_ = 0;
    pending_.clear();
    recvNext_ = 0;
    recvBuffer_.clear();
    hasSent_ = false;
    ackDirty_ = false;
}

AckPayload ReliableStream::ackPayload()
{
    AckPayload ack;
    const quint32 base = recvNext_ == 0 ? 0 : recvNext_;
    ack.cumulative =
        recvNext_ == 0 ? kNoCumulativeAck : recvNext_ - 1;
    for (int bit = 0; bit < 64; ++bit) {
        if (recvBuffer_.contains(base + static_cast<quint32>(bit))) {
            ack.bitmap |= quint64(1) << bit;
        }
    }
    ackDirty_ = false;
    return ack;
}

bool ReliableStream::needsAck() const
{
    return ackDirty_;
}

double ReliableStream::currentTimeSeconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

} // namespace pd
