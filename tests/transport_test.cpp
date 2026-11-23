#include "core/protocol.h"
#include "core/reliable.h"
#include "core/video.h"

#include <QtTest>

using namespace pd;

namespace {

struct SentPacket {
    quint32 seq = 0;
    QByteArray payload;
    quint8 flags = 0;
};

} // namespace

class TransportTest : public QObject {
    Q_OBJECT

private slots:
    void reliableStreamOrdersAndAcknowledges()
    {
        QList<QByteArray> received;
        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        ReliableStream receiver(
            [&received](const QByteArray &message) { received.append(message); },
            [](quint32, const QByteArray &, quint8) {});

        for (int index = 0; index < 3; ++index) {
            (void)sender.send("msg" + QByteArray::number(index));
        }
        for (const auto &packet : sent) {
            receiver.onData(packet.seq, packet.payload);
        }
        QCOMPARE(received, (QList<QByteArray>{"msg0", "msg1", "msg2"}));

        const AckPayload ack = receiver.ackPayload();
        sender.onAck(ack.cumulative, ack.bitmap);
        QCOMPARE(sender.pending(), 0);
    }

    void reliableStreamRepairsLossAndStopsAfterRetries()
    {
        QList<QByteArray> received;
        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        ReliableStream receiver(
            [&received](const QByteArray &message) { received.append(message); },
            [](quint32, const QByteArray &, quint8) {});

        (void)sender.send("first", FlagReliable, 0.0);
        (void)sender.send("lost", FlagReliable, 0.0);
        (void)sender.send("third", FlagReliable, 0.0);
        receiver.onData(0, sent[0].payload);
        receiver.onData(2, sent[2].payload);
        QCOMPARE(received, (QList<QByteArray>{"first"}));

        const int before = sent.size();
        sender.tick(1e9);
        QVERIFY(sent.size() > before);
        for (int index = before; index < sent.size(); ++index) {
            receiver.onData(sent[index].seq, sent[index].payload);
        }
        QCOMPARE(received, (QList<QByteArray>{"first", "lost", "third"}));
        QCOMPARE(sender.pending(), 3);

        ReliableStream dropping(
            [](const QByteArray &) {},
            [](quint32, const QByteArray &, quint8) {});
        (void)dropping.send("x", FlagReliable, 0.0);
        for (int index = 0; index < ReliableStream::kMaxRetries + 2; ++index) {
            dropping.tick(1e9 + index * 3);
        }
        QCOMPARE(dropping.pending(), 0);
    }

    void reliableStreamIgnoresDuplicatesAndTracksAckNeed()
    {
        QList<QByteArray> received;
        ReliableStream receiver(
            [&received](const QByteArray &message) { received.append(message); },
            [](quint32, const QByteArray &, quint8) {});
        (void)receiver.ackPayload();
        QVERIFY(!receiver.needsAck());
        receiver.onData(0, "a");
        receiver.onData(0, "a");
        receiver.onData(1, "b");
        QCOMPARE(received, (QList<QByteArray>{"a", "b"}));
        QVERIFY(receiver.needsAck());
    }

    void reliableStreamAcksOutOfOrderWithoutAcknowledgingGap()
    {
        QList<QByteArray> received;
        ReliableStream receiver(
            [&received](const QByteArray &message) { received.append(message); },
            [](quint32, const QByteArray &, quint8) {});

        receiver.onData(1, "one");
        QVERIFY(receiver.needsAck());
        QVERIFY(received.isEmpty());
        const AckPayload selective = receiver.ackPayload();
        QCOMPARE(
            selective.cumulative,
            ReliableStream::kNoCumulativeAck);
        QCOMPARE(selective.bitmap, quint64(1) << 1);
        QVERIFY(!receiver.needsAck());

        receiver.onData(0, "zero");
        QCOMPARE(received, (QList<QByteArray>{"zero", "one"}));
        QVERIFY(receiver.needsAck());
        const AckPayload ack = receiver.ackPayload();
        QCOMPARE(ack.cumulative, quint32(1));
        QCOMPARE(ack.bitmap, quint64(0));
    }

    void reliableStreamAcknowledgementBitmapUsesReferenceOffsets()
    {
        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        for (int index = 0; index < 5; ++index) {
            (void)sender.send("m" + QByteArray::number(index), FlagReliable, 0.0);
        }

        // cumulative=2 acknowledges 0..2; bitmap bit 0 acknowledges sequence 3.
        sender.onAck(2, quint64(1));
        QCOMPARE(sender.pending(), 1);
        QCOMPARE(sent.size(), 5);

        sender.tick(1e9);
        QCOMPARE(sent.size(), 6);
        QCOMPARE(sent.constLast().seq, quint32(4));
    }

    void reliableStreamSelectiveAckBridgesOutOfOrderGap()
    {
        QList<QByteArray> received;
        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        ReliableStream receiver(
            [&received](const QByteArray &message) {
                received.append(message);
            },
            [](quint32, const QByteArray &, quint8) {});

        for (int index = 0; index < 4; ++index) {
            (void)sender.send(
                "m" + QByteArray::number(index),
                FlagReliable,
                0.0);
        }
        receiver.onData(0, sent[0].payload);
        receiver.onData(2, sent[2].payload);
        receiver.onData(3, sent[3].payload);

        const AckPayload ack = receiver.ackPayload();
        QCOMPARE(ack.cumulative, quint32(0));
        QCOMPARE(ack.bitmap, quint64(0b110));
        sender.onAck(ack.cumulative, ack.bitmap);
        QCOMPARE(sender.pending(), 1);

        receiver.onData(1, sent[1].payload);
        QCOMPARE(
            received,
            (QList<QByteArray>{"m0", "m1", "m2", "m3"}));
        const AckPayload complete = receiver.ackPayload();
        QCOMPARE(complete.cumulative, quint32(3));
        QCOMPARE(complete.bitmap, quint64(0));
        sender.onAck(complete.cumulative, complete.bitmap);
        QCOMPARE(sender.pending(), 0);
    }

    void reliableStreamRejectsInvalidAcksAndBoundsReceiveWindow()
    {
        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        for (int index = 0; index < 4; ++index) {
            (void)sender.send(
                "m" + QByteArray::number(index),
                FlagReliable,
                0.0);
        }

        sender.onAck(ReliableStream::kNoCumulativeAck, 0);
        QCOMPARE(sender.pending(), 4);
        sender.onAck(100, 0);
        QCOMPARE(sender.pending(), 4);
        sender.onAck(1, (quint64(1) << 62));
        QCOMPARE(sender.pending(), 2);

        QList<QByteArray> received;
        ReliableStream receiver(
            [&received](const QByteArray &message) {
                received.append(message);
            },
            [](quint32, const QByteArray &, quint8) {});
        receiver.onData(ReliableStream::kReceiveWindow + 1, "too-far");
        QVERIFY(receiver.needsAck());
        QVERIFY(received.isEmpty());
        const AckPayload empty = receiver.ackPayload();
        QCOMPARE(empty.cumulative, ReliableStream::kNoCumulativeAck);
        QCOMPARE(empty.bitmap, quint64(0));

        receiver.onData(0, "zero");
        QCOMPARE(received, (QList<QByteArray>{"zero"}));
    }

    void reliableStreamReacksDuplicatesAndResetsCleanly()
    {
        QList<QByteArray> received;
        ReliableStream receiver(
            [&received](const QByteArray &message) {
                received.append(message);
            },
            [](quint32, const QByteArray &, quint8) {});
        receiver.onData(0, "zero");
        QVERIFY(receiver.needsAck());
        (void)receiver.ackPayload();
        QVERIFY(!receiver.needsAck());

        receiver.onData(0, "zero");
        QVERIFY(receiver.needsAck());
        QCOMPARE(received, (QList<QByteArray>{"zero"}));

        receiver.reset();
        QVERIFY(!receiver.needsAck());
        receiver.onData(0, "after-reset");
        QCOMPARE(
            received,
            (QList<QByteArray>{"zero", "after-reset"}));

        QList<SentPacket> sent;
        ReliableStream sender(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        (void)sender.send("before-reset");
        QCOMPARE(sender.nextSendSeq(), quint32(1));
        sender.reset();
        QCOMPARE(sender.pending(), 0);
        QCOMPARE(sender.nextSendSeq(), quint32(0));
        (void)sender.send("after-reset", FlagReliable, 0.0);
        QCOMPARE(sent.constLast().seq, quint32(0));
    }

    void reliableStreamStopsAtConfiguredRetryLimit()
    {
        QList<SentPacket> sent;
        ReliableStream stream(
            [](const QByteArray &) {},
            [&sent](quint32 seq, const QByteArray &payload, quint8 flags) {
                sent.append(SentPacket{seq, payload, flags});
            });
        (void)stream.send("retry", FlagReliable, 0.0);
        for (int index = 0; index < ReliableStream::kMaxRetries + 4; ++index) {
            stream.tick(1e9 + index * 3.0);
        }
        QCOMPARE(stream.pending(), 0);
        QCOMPARE(sent.size(), ReliableStream::kMaxRetries + 1);
    }

    void videoSurvivesOneFragmentLossWithParity()
    {
        QByteArray payload;
        for (int repeat = 0; repeat < 40; ++repeat) {
            for (int value = 0; value < 256; ++value) {
                payload.append(static_cast<char>(value));
            }
        }
        VideoSender sender(1000);
        const auto packets = sender.build(payload, true);
        QList<QPair<QByteArray, bool>> frames;
        QList<int> keyframeRequests;
        VideoReceiver receiver(
            [&frames](const QByteArray &data, bool keyframe) {
                frames.append(qMakePair(data, keyframe));
            },
            [&keyframeRequests]() { keyframeRequests.append(1); });

        bool dropped = false;
        for (const auto &packet : packets) {
            if (packet.type == PacketType::Data) {
                const auto part = unpackVideoFragment(packet.payload);
                QVERIFY(part.has_value());
                if (part->index == 3 && !dropped) {
                    dropped = true;
                    continue;
                }
                receiver.onFragment(
                    part->frameId,
                    part->index,
                    part->count,
                    part->data,
                    (packet.flags & FlagKeyframe) != 0,
                    0.0);
            } else {
                const auto parity = unpackParityFragment(packet.payload);
                QVERIFY(parity.has_value());
                receiver.onParity(
                    parity->frameId,
                    parity->groupIndex,
                    parity->shards,
                    parity->data);
            }
        }
        QVERIFY(dropped);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.front().first, payload);
        QVERIFY(frames.front().second);
        QVERIFY(keyframeRequests.isEmpty());
    }

    void videoRequestsNackAndRetransmits()
    {
        const QByteArray payload(5000, 'Z');
        VideoSender sender(1000);
        const auto packets = sender.build(payload, false);
        QList<QPair<QByteArray, bool>> frames;
        VideoReceiver receiver(
            [&frames](const QByteArray &data, bool keyframe) {
                frames.append(qMakePair(data, keyframe));
            },
            []() {});
        QList<VideoFragment> held;

        for (const auto &packet : packets) {
            if (packet.type == PacketType::Data) {
                const auto part = unpackVideoFragment(packet.payload);
                QVERIFY(part.has_value());
                if (part->index == 1 || part->index == 2) {
                    held.append(*part);
                    continue;
                }
                receiver.onFragment(
                    part->frameId,
                    part->index,
                    part->count,
                    part->data,
                    false,
                    0.0);
            } else {
                const auto parity = unpackParityFragment(packet.payload);
                QVERIFY(parity.has_value());
                receiver.onParity(
                    parity->frameId,
                    parity->groupIndex,
                    parity->shards,
                    parity->data);
            }
        }
        QVERIFY(frames.isEmpty());
        const auto requests = receiver.tick(0.1);
        QVERIFY(!requests.isEmpty());
        QCOMPARE(requests.front().missing, (QList<quint16>{1, 2}));
        const auto repaired = sender.retransmit(
            requests.front().frameId, requests.front().missing);
        for (const auto &packet : repaired) {
            const auto part = unpackVideoFragment(packet.payload);
            QVERIFY(part.has_value());
            receiver.onFragment(
                part->frameId,
                part->index,
                part->count,
                part->data,
                false,
                0.1);
        }
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.front().first, payload);
    }

    void videoKeyframeTimeoutRequestsNewFrame()
    {
        VideoSender sender(1000);
        const auto packets = sender.build(QByteArray(4000, 'K'), true);
        int requested = 0;
        VideoReceiver receiver(
            [](const QByteArray &, bool) {},
            [&requested]() { ++requested; });
        const auto part = unpackVideoFragment(packets.front().payload);
        QVERIFY(part.has_value());
        receiver.onFragment(
            part->frameId, part->index, part->count, part->data, true, 0.0);
        (void)receiver.tick(1e9);
        QCOMPARE(requested, 1);
        QCOMPARE(receiver.lostFrames(), 1);
    }
};

QTEST_APPLESS_MAIN(TransportTest)

#include "transport_test.moc"
