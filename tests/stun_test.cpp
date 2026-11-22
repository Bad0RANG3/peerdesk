#include "core/stun.h"

#include <QHostAddress>
#include <QtTest>

using namespace pd;

class StunTest : public QObject {
    Q_OBJECT

private slots:
    void requestMatchesReferenceVector()
    {
        QByteArray txid;
        for (int value = 0; value < 12; ++value) {
            txid.append(static_cast<char>(value));
        }
        const QByteArray packet = stun::buildBindingRequest(txid);
        QCOMPARE(packet.toHex(), QByteArray(
            "000100002112a442000102030405060708090a0b"));

        const auto parsed = stun::parseMessage(packet);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->type, stun::kBindingRequest);
        QCOMPARE(parsed->transactionId, txid);
        QVERIFY(parsed->attributes.isEmpty());
    }

    void randomTransactionIdsDiffer()
    {
        QCOMPARE(stun::randomTransactionId().size(), stun::kTransactionIdLen);
        QVERIFY(stun::randomTransactionId()
                != stun::randomTransactionId());
        QCOMPARE(stun::buildBindingRequest().size(), 20);
    }

    void ipv4ResponseMatchesReferenceVector()
    {
        QByteArray txid;
        for (int value = 0; value < 12; ++value) {
            txid.append(static_cast<char>(value));
        }
        const QByteArray packet = stun::buildBindingResponse(
            txid, QHostAddress(QStringLiteral("203.0.113.9")), 45678);
        QCOMPARE(packet.toHex(), QByteArray(
            "0101000c2112a442000102030405060708090a0b"
            "002000080001937cea12d54b"));
        QVERIFY(!packet.contains(QByteArray::fromHex("cb007109")));

        const auto parsed = stun::parseMessage(packet);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->type, stun::kBindingSuccess);
        const auto endpoint = stun::parseMappedAddress(
            parsed->attributes, parsed->transactionId);
        QVERIFY(endpoint.has_value());
        QCOMPARE(endpoint->address.toString(), QStringLiteral("203.0.113.9"));
        QCOMPARE(endpoint->port, quint16(45678));
    }

    void ipv6ResponseMatchesReferenceVector()
    {
        QByteArray txid;
        for (int value = 0; value < 12; ++value) {
            txid.append(static_cast<char>(value));
        }
        const QByteArray packet = stun::buildBindingResponse(
            txid, QHostAddress(QStringLiteral("2001:db8::1")), 45678);
        QCOMPARE(packet.toHex(), QByteArray(
            "010100182112a442000102030405060708090a0b"
            "002000140002937c0113a9fa00010203040506070809"
            "0a0a"));

        const auto parsed = stun::parseMessage(packet);
        QVERIFY(parsed.has_value());
        const auto endpoint = stun::parseMappedAddress(
            parsed->attributes, parsed->transactionId);
        QVERIFY(endpoint.has_value());
        QCOMPARE(endpoint->address.toString(),
                 QStringLiteral("2001:db8::1"));
        QCOMPARE(endpoint->port, quint16(45678));
    }

    void malformedPacketsAreRejected()
    {
        QVERIFY(!stun::parseMessage({}).has_value());
        QVERIFY(!stun::parseMessage(QByteArray(19, '\0')).has_value());
        QVERIFY(!stun::parseMessage(QByteArray(20, '\0')).has_value());

        QByteArray bad = stun::buildBindingRequest(
            QByteArray(12, static_cast<char>(0x42)));
        bad[4] = '\0';
        QVERIFY(!stun::parseMessage(bad).has_value());

        QMap<quint16, QByteArray> attributes;
        attributes.insert(
            stun::kAttributeXorMappedAddress,
            QByteArray::fromHex("00010003"));
        QVERIFY(!stun::parseMappedAddress(
            attributes, QByteArray(12, '\0')).has_value());

        attributes.insert(
            stun::kAttributeXorMappedAddress,
            QByteArray::fromHex("0003000101020304"));
        QVERIFY(!stun::parseMappedAddress(
            attributes, QByteArray(12, '\0')).has_value());

        attributes.insert(
            stun::kAttributeXorMappedAddress,
            QByteArray::fromHex("0002000101020304"));
        QVERIFY(!stun::parseMappedAddress(
            attributes, QByteArray(12, '\0')).has_value());
    }

    void plainMappedAddressIgnoresTrailingBytes()
    {
        QMap<quint16, QByteArray> attributes;
        attributes.insert(
            stun::kAttributeMappedAddress,
            QByteArray::fromHex("0001000101020304ffff"));
        const auto endpoint = stun::parseMappedAddress(
            attributes, QByteArray(12, '\0'));
        QVERIFY(endpoint.has_value());
        QCOMPARE(endpoint->address.toString(), QStringLiteral("1.2.3.4"));
    }
};

QTEST_APPLESS_MAIN(StunTest)

#include "stun_test.moc"
