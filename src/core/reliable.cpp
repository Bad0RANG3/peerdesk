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
    const auto keys = pending_.keys();
    for (const quint32 seq : keys) {
        if (seq <= cumulative) {
            pending_.remove(seq);
        }
    }
    for (int bit = 0; bit < 64; ++bit) {
        if ((bitmap & (quint64(1) << bit)) == 0) {
            continue;
        }
        // Bit i acknowledges cumulative-1-i. Bit 0 is deliberately not
        // cumulative itself, so an initial "nothing received" ACK cannot
        // discard message zero.
        if (static_cast<quint64>(cumulative)
            >= static_cast<quint64>(bit) + 1) {
            const quint64 acknowledged = static_cast<quint64>(cumulative)
                - static_cast<quint64>(bit) - 1;
            pending_.remove(static_cast<quint32>(acknowledged));
        }
    }
}

void ReliableStream::onData(quint32 seq, const QByteArray &payload)
{
    if (seq < recvNext_ || recvBuffer_.contains(seq)) {
        return;
    }
    recvBuffer_.insert(seq, payload);
    while (recvBuffer_.contains(recvNext_)) {
        const QByteArray message = recvBuffer_.take(recvNext_);
        ++recvNext_;
        if (onMessage_) {
            onMessage_(message);
        }
    }
}

AckPayload ReliableStream::ackPayload()
{
    AckPayload ack;
    const qint32 cumulative = static_cast<qint32>(recvNext_) - 1;
    ack.cumulative = static_cast<quint32>(std::max(cumulative, 0));
    for (int bit = 0; bit < 64; ++bit) {
        const qint64 sequence = static_cast<qint64>(ack.cumulative) - bit;
        if (sequence >= 0
            && recvBuffer_.contains(static_cast<quint32>(sequence))) {
            ack.bitmap |= quint64(1) << bit;
        }
    }
    lastAckCum_ = cumulative;
    return ack;
}

bool ReliableStream::needsAck() const
{
    return static_cast<qint32>(recvNext_) - 1 != lastAckCum_;
}

double ReliableStream::currentTimeSeconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

} // namespace pd
