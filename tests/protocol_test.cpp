#include "core/protocol.h"
#include "core/util.h"

#include <QJsonObject>
#include <QtTest>

using namespace pd;

class ProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void headerRoundTrip()
    {
        Header header;
        header.type = PacketType::Data;
        header.seq = 4321;
        header.tsMs = 1700000;
        header.stream = static_cast<quint8>(StreamKind::Video);
        header.flags = FlagKeyframe | FlagLastFragment | FlagH264;
        header.plen = 1200;
        header.session = 7;

        const QByteArray raw = header.pack();
        QCOMPARE(raw.size(), kHeaderLen);
        QCOMPARE(raw.left(4), QByteArray("PD\x01\x06", 4));

        const auto parsed = Header::parse(raw);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->type, PacketType::Data);
        QCOMPARE(parsed->seq, 4321U);
        QCOMPARE(parsed->tsMs, 1700000U);
        QCOMPARE(parsed->stream, quint8(0));
        QCOMPARE(
            parsed->flags,
            quint8(FlagKeyframe | FlagLastFragment | FlagH264));
        QCOMPARE(parsed->plen, quint16(1200));
        QCOMPARE(parsed->session, 7U);
    }

    void headerRejectsInvalidInput()
    {
        QVERIFY(!Header::parse(QByteArray("PD")).has_value());
        const QByteArray good = Header{}.pack();

        QByteArray badMagic = good;
        badMagic[0] = 'X';
        QVERIFY(!Header::parse(badMagic).has_value());

        QByteArray badVersion = good;
        badVersion[2] = 99;
        QVERIFY(!Header::parse(badVersion).has_value());
    }

    void jsonIsCompactUtf8()
    {
        QJsonObject object;
        object.insert(QStringLiteral("t"), QStringLiteral("notice"));
        object.insert(QStringLiteral("text"), QStringLiteral("连接已建立 · 直连"));
        const QByteArray payload = jsonPayload(object);
        QCOMPARE(payload, QByteArray("{\"t\":\"notice\",\"text\":\"连接已建立 · 直连\"}"));
        QVERIFY(parseJson(payload).has_value());
        QCOMPARE(parseJson(payload)->value(QStringLiteral("text")).toString(),
                 QStringLiteral("连接已建立 · 直连"));
    }

    void fragmentRoundTrip()
    {
        QByteArray data;
        for (int repeat = 0; repeat < 20; ++repeat) {
            for (int value = 0; value < 256; ++value) {
                data.append(static_cast<char>(value));
            }
        }
        const auto chunks = fragment(data, 1000);
        QCOMPARE(chunks.size(), 6);
        QByteArray rebuilt;
        for (const auto &chunk : chunks) {
            QVERIFY(chunk.size() <= 1000);
            rebuilt.append(chunk);
        }
        QCOMPARE(rebuilt, data);
        QCOMPARE(fragment({}, 100), QList<QByteArray>{QByteArray()});
    }

    void parityRecoversSingleLoss()
    {
        const QList<QByteArray> chunks = {"aaaa", "bbbb", "cccc", "dddd"};
        const QByteArray parity = xorBytes(chunks);
        FragmentAssembler assembler(4);
        QVERIFY(assembler.add(0, chunks[0]));
        QVERIFY(assembler.add(2, chunks[2]));
        QVERIFY(assembler.add(3, chunks[3]));
        QVERIFY(!assembler.complete());
        QVERIFY(assembler.fillParity({0, 1, 2, 3}, parity));
        QVERIFY(assembler.complete());
        QCOMPARE(assembler.assemble(), QByteArray("aaaabbbbccccdddd"));
    }

    void assemblerRejectsDuplicateAndTwoLosses()
    {
        FragmentAssembler assembler(3);
        QVERIFY(assembler.add(0, "x"));
        QVERIFY(!assembler.add(0, "y"));
        QCOMPARE(assembler.missing(), (QList<quint16>{1, 2}));
        QVERIFY(!assembler.fillParity({0, 1, 2}, QByteArray("p")));
    }

    void ackAndNackRoundTrip()
    {
        AckPayload ack;
        ack.cumulative = 0x80000000U;
        ack.bitmap = (quint64(1) << 60) | 5;
        const auto decodedAck = unpackAck(packAck(ack));
        QVERIFY(decodedAck.has_value());
        QCOMPARE(decodedAck->cumulative, ack.cumulative);
        QCOMPARE(decodedAck->bitmap, ack.bitmap);

        const QList<quint16> missing = {1, 3, 5, 9};
        const auto decodedNack = unpackNack(packNack(77, missing));
        QVERIFY(decodedNack.has_value());
        QCOMPARE(decodedNack->first, 77U);
        QCOMPARE(decodedNack->second, missing);
    }

    void videoFragmentRoundTrip()
    {
        VideoFragment fragment;
        fragment.frameId = 9;
        fragment.index = 3;
        fragment.count = 17;
        fragment.data = "abc";
        const auto decoded = unpackVideoFragment(packVideoFragment(fragment));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->frameId, 9U);
        QCOMPARE(decoded->index, quint16(3));
        QCOMPARE(decoded->count, quint16(17));
        QCOMPARE(decoded->data, QByteArray("abc"));
    }

    void audioRoundTrip()
    {
        AudioPacket packet;
        packet.sequence = 12;
        packet.timestampMs = 345;
        packet.samples = 320;
        packet.previous = QByteArray(320, '\x7f');
        packet.current = QByteArray(320, '\xff');
        const auto decoded = unpackAudio(packAudio(packet));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->sequence, 12U);
        QCOMPARE(decoded->timestampMs, 345U);
        QCOMPARE(decoded->samples, quint16(320));
        QCOMPARE(decoded->previous, packet.previous);
        QCOMPARE(decoded->current, packet.current);
    }

    void controlFragmentsRoundTrip()
    {
        QByteArray body = "{\"t\":\"file_data\",\"d\":\"";
        body += QByteArray(4000, 'A');
        body += "\"}";
        ControlFragments sender(512);
        ControlFragments receiver(512);
        const auto parts = sender.split(body);
        QVERIFY(parts.size() > 1);
        for (qsizetype index = 0; index + 1 < parts.size(); ++index) {
            QVERIFY(!receiver.feed(parts[index]).has_value());
        }
        const auto completed = receiver.feed(parts.back());
        QVERIFY(completed.has_value());
        QCOMPARE(*completed, body);
    }

    void codeHelpersMatchReferenceVectors()
    {
        QCOMPARE(normalizeCode(QStringLiteral("123 456-789")), QStringLiteral("123456789"));
        QCOMPARE(formatCode(QStringLiteral("123456789")), QStringLiteral("123 456 789"));
        QVERIFY(codeIsValid(QStringLiteral("123456789")));
        QVERIFY(!codeIsValid(QStringLiteral("023456789")));
        QVERIFY(!codeIsValid(QStringLiteral("12345678")));

        QCOMPARE(pskFromCode(QStringLiteral("123456789")).toHex(),
                 QByteArray("dd8e37a102a3a0103bd02f11eb205743bbd28963a2cdc93e426ce05e7b499de6"));
        QCOMPARE(codeId(QStringLiteral("123456789")), QStringLiteral("76b31e909e45a80b72c0d184283af443"));
    }

    void serverAddressParsing()
    {
        const auto ipv4 = parseServerAddress(QStringLiteral("127.0.0.1:7900"));
        QVERIFY(ipv4.has_value());
        QCOMPARE(ipv4->host, QStringLiteral("127.0.0.1"));
        QCOMPARE(ipv4->port, quint16(7900));

        const auto ipv6 = parseServerAddress(QStringLiteral("[::1]:9000"));
        QVERIFY(ipv6.has_value());
        QCOMPARE(ipv6->host, QStringLiteral("::1"));
        QCOMPARE(ipv6->port, quint16(9000));

        const auto defaults = parseServerAddress(QStringLiteral("example.test"));
        QVERIFY(defaults.has_value());
        QCOMPARE(defaults->host, QStringLiteral("example.test"));
        QCOMPARE(defaults->port, quint16(7900));

        QVERIFY(!parseServerAddress(QStringLiteral("host:0")).has_value());
        QVERIFY(!parseServerAddress(QStringLiteral("host:99999")).has_value());
    }

    void relayHeaderRoundTrip()
    {
        const QByteArray packet = Header{}.pack();
        const auto decoded = unwrapRelay(wrapRelay(0xAABBCCDDU, packet));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->first, 0xAABBCCDDU);
        QCOMPARE(decoded->second, packet);
        QVERIFY(!unwrapRelay("XXXX0000").has_value());
    }
};

QTEST_APPLESS_MAIN(ProtocolTest)

#include "protocol_test.moc"
