#include "core/crypto.h"

#include <QByteArray>
#include <QtTest>

using namespace pd;

namespace {

QByteArray bytes(int start)
{
    QByteArray result(32, '\0');
    for (int index = 0; index < result.size(); ++index) {
        result[index] = static_cast<char>(start + index);
    }
    return result;
}

KeyPair fixedPair(int start)
{
    const auto pair = KeyPair::fromPrivate(bytes(start));
    Q_ASSERT(pair.has_value());
    return *pair;
}

} // namespace

class CryptoTest : public QObject {
    Q_OBJECT

private slots:
    void keyPairMatchesReference()
    {
        const KeyPair host = fixedPair(0);
        QCOMPARE(host.publicKey.toHex(),
                 QByteArray("8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f"));

        const KeyPair client = fixedPair(32);
        QCOMPARE(client.publicKey.toHex(),
                 QByteArray("358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254"));
        QCOMPARE(KeyPair::fromPrivate(bytes(0))->privateBytes(), bytes(0));
        QVERIFY(!KeyPair::fromPrivate(QByteArray(31, '\0')).has_value());
    }

    void derivedKeysMatchReference()
    {
        const KeyPair host = fixedPair(0);
        const KeyPair client = fixedPair(32);
        const QByteArray helloHost = "{\"role\":\"host\",\"nonce\":\"aaaa\",\"ts\":1}";
        const QByteArray helloClient = "{\"role\":\"client\",\"nonce\":\"bbbb\",\"ts\":2}";
        const QByteArray psk = "123456789";

        const auto hostKeys = SessionKeys::derive(
            host, client.publicKey, helloHost, helloClient, psk);
        const auto clientKeys = SessionKeys::derive(
            client, host.publicKey, helloHost, helloClient, psk);
        QVERIFY(hostKeys.has_value());
        QVERIFY(clientKeys.has_value());
        QCOMPARE(hostKeys->transcript.toHex(),
                 QByteArray("8a0c2c0657f60752ba41b93210ee52d852ab484e3c1f8d485b9f91883b15fe03"));
        QCOMPARE(hostKeys->hostToClient.toHex(),
                 QByteArray("094d226254a19a330fca9abccbf7edfe12d6e02f0822e73f190e79c36ffe3a6c"));
        QCOMPARE(hostKeys->clientToHost.toHex(),
                 QByteArray("f18f12e064727fbefdfe2c0cd8c05fe044bdc6faa0a57393fc931c54f611488e"));
        QCOMPARE(hostKeys->hostToClient, clientKeys->hostToClient);
        QCOMPARE(hostKeys->clientToHost, clientKeys->clientToHost);
        QCOMPARE(hostKeys->transcript, clientKeys->transcript);
    }

    void pskChangesKeysAndTranscriptBindsHellos()
    {
        const KeyPair host = fixedPair(0);
        const KeyPair client = fixedPair(32);
        const auto plain = SessionKeys::derive(
            host, client.publicKey, "host", "client", {});
        const auto protectedKeys = SessionKeys::derive(
            host, client.publicKey, "host", "client", "code-psk");
        QVERIFY(plain.has_value());
        QVERIFY(protectedKeys.has_value());
        QVERIFY(plain->hostToClient != protectedKeys->hostToClient);

        const auto changed = SessionKeys::derive(
            host, client.publicKey, "host", "client-x", {});
        QVERIFY(changed.has_value());
        QVERIFY(plain->hostToClient != changed->hostToClient);
    }

    void aeadMatchesPythonVector()
    {
        const KeyPair host = fixedPair(0);
        const KeyPair client = fixedPair(32);
        const QByteArray helloHost = "{\"role\":\"host\",\"nonce\":\"aaaa\",\"ts\":1}";
        const QByteArray helloClient = "{\"role\":\"client\",\"nonce\":\"bbbb\",\"ts\":2}";
        auto hostKeys = SessionKeys::derive(
            host, client.publicKey, helloHost, helloClient, "123456789");
        auto clientKeys = SessionKeys::derive(
            client, host.publicKey, helloHost, helloClient, "123456789");
        QVERIFY(hostKeys.has_value());
        QVERIFY(clientKeys.has_value());

        CryptoContext sender(std::move(*hostKeys), true);
        CryptoContext receiver(std::move(*clientKeys), false);
        const QByteArray header(20, 'H');
        const QByteArray body = sender.seal(header, "payload-h2c", 1);
        QCOMPARE(body.toHex(),
                 QByteArray("2a734960b96f94ba3dd1de634bfc4671021fbeeae47a547a212c13"));
        const auto opened = receiver.open(header, body, 1);
        QVERIFY(opened.has_value());
        QCOMPARE(*opened, QByteArray("payload-h2c"));
    }

    void directionsRoundTripAndRejectTampering()
    {
        const KeyPair host = fixedPair(0);
        const KeyPair client = fixedPair(32);
        auto hostKeys = SessionKeys::derive(
            host, client.publicKey, "host", "client");
        auto clientKeys = SessionKeys::derive(
            client, host.publicKey, "host", "client");
        QVERIFY(hostKeys.has_value());
        QVERIFY(clientKeys.has_value());
        CryptoContext hostContext(std::move(*hostKeys), true);
        CryptoContext clientContext(std::move(*clientKeys), false);

        const QByteArray header(20, 'H');
        QByteArray body = hostContext.seal(header, "secret", hostContext.nextSeq());
        QVERIFY(!body.contains("secret"));
        const auto opened = clientContext.open(header, body, 1);
        QVERIFY(opened.has_value());
        QCOMPARE(*opened, QByteArray("secret"));
        QVERIFY(!clientContext.open(header, body, 1).has_value());

        QByteArray tampered = body;
        tampered[0] = static_cast<char>(tampered[0] ^ 0x55);
        auto freshClientKeys = SessionKeys::derive(
            client, host.publicKey, "host", "client");
        CryptoContext freshClient(std::move(*freshClientKeys), false);
        QVERIFY(!freshClient.open(header, tampered, 1).has_value());

        auto freshClientKeys2 = SessionKeys::derive(
            client, host.publicKey, "host", "client");
        CryptoContext freshClient2(std::move(*freshClientKeys2), false);
        QVERIFY(!freshClient2.open(QByteArray(20, 'X'), body, 1).has_value());
    }

    void replayWindowFiltersOldAndDuplicatePackets()
    {
        ReplayWindow window(8);
        for (quint64 sequence = 1; sequence < 20; ++sequence) {
            QVERIFY(window.checkAndUpdate(sequence));
        }
        QVERIFY(!window.checkAndUpdate(19));
        QVERIFY(!window.checkAndUpdate(2));

        ReplayWindow sparse(64);
        QVERIFY(sparse.checkAndUpdate(10));
        QVERIFY(sparse.checkAndUpdate(7));
        QVERIFY(!sparse.checkAndUpdate(7));
        QVERIFY(sparse.checkAndUpdate(11));
    }

    void failedAuthenticationDoesNotAdvanceReplayWindow()
    {
        const KeyPair host = fixedPair(0);
        const KeyPair client = fixedPair(32);
        auto hostKeys = SessionKeys::derive(
            host, client.publicKey, "host", "client");
        auto clientKeys = SessionKeys::derive(
            client, host.publicKey, "host", "client");
        QVERIFY(hostKeys.has_value());
        QVERIFY(clientKeys.has_value());
        CryptoContext hostContext(std::move(*hostKeys), true);
        CryptoContext clientContext(std::move(*clientKeys), false);

        const QByteArray header(20, 'H');
        const QByteArray body = hostContext.seal(header, "payload", 1);
        QByteArray forged = body;
        forged[forged.size() - 1] = static_cast<char>(
            forged[forged.size() - 1] ^ 0x01);

        QVERIFY(!clientContext.open(header, forged, 100000).has_value());
        const auto opened = clientContext.open(header, body, 1);
        QVERIFY(opened.has_value());
        QCOMPARE(*opened, QByteArray("payload"));
    }
};

QTEST_APPLESS_MAIN(CryptoTest)

#include "crypto_test.moc"
