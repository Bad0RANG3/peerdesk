#pragma once

#include "core/protocol.h"

#include <QByteArray>
#include <QHash>

#include <functional>

namespace pd {

class ReliableStream {
public:
    static constexpr double kRtoMin = 0.15;
    static constexpr double kRtoMax = 1.2;
    static constexpr int kMaxRetries = 14;

    using MessageHandler = std::function<void(const QByteArray &)>;
    using RawSender = std::function<void(quint32, const QByteArray &, quint8)>;

    ReliableStream(MessageHandler onMessage, RawSender sendRaw);

    [[nodiscard]] quint32 send(
        const QByteArray &payload,
        quint8 flags = FlagReliable,
        double now = currentTimeSeconds());
    void tick(double now);
    void onAck(quint32 cumulative, quint64 bitmap);
    void onData(quint32 seq, const QByteArray &payload);
    void reset();

    [[nodiscard]] AckPayload ackPayload();
    [[nodiscard]] bool needsAck() const;
    [[nodiscard]] int pending() const { return pending_.size(); }
    [[nodiscard]] quint32 nextSendSeq() const { return nextSeq_; }

    [[nodiscard]] static double currentTimeSeconds();

private:
    struct Pending {
        QByteArray payload;
        double deadline = 0;
        int retries = 0;
    };

    MessageHandler onMessage_;
    RawSender sendRaw_;
    quint32 nextSeq_ = 0;
    QHash<quint32, Pending> pending_;
    quint32 recvNext_ = 0;
    QHash<quint32, QByteArray> recvBuffer_;
    qint32 lastAckCum_ = -1;
};

} // namespace pd
