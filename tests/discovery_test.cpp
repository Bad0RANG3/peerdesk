#include "discovery/lan_discovery.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUdpSocket>

#include <optional>

using namespace pd;

namespace {

quint16 unusedLoopbackPort()
{
    QUdpSocket probe;
    if (!probe.bind(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return probe.localPort();
}

LanDiscovery::Options loopbackOptions(
    quint16 bindPort,
    quint16 targetPort)
{
    LanDiscovery::Options options;
    options.bindAddress = QHostAddress::LocalHost;
    options.bindPort = bindPort;
    options.targetAddress = QHostAddress::LocalHost;
    options.targetPort = targetPort;
    options.allowLoopback = true;
    options.announceIntervalMs = 60;
    options.expireAfterMs = 1000;
    options.sweepIntervalMs = 20;
    return options;
}

std::optional<QByteArray> receiveDatagram(
    QUdpSocket &socket,
    int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (socket.hasPendingDatagrams()) {
            QByteArray payload;
            payload.resize(
                static_cast<qsizetype>(
                    socket.pendingDatagramSize()));
            const qint64 read = socket.readDatagram(
                payload.data(),
                payload.size());
            if (read < 0) {
                return std::nullopt;
            }
            payload.resize(read);
            return payload;
        }
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return std::nullopt;
}

bool sendJson(
    QUdpSocket &socket,
    const QJsonObject &object,
    quint16 port)
{
    const QByteArray payload =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    return socket.writeDatagram(
               payload,
               QHostAddress::LocalHost,
               port)
        == payload.size();
}

QJsonObject message(
    const QString &type,
    const QString &instanceId)
{
    return QJsonObject{
        {QStringLiteral("p"),
         QStringLiteral("peerdesk-lan-discovery")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("t"), type},
        {QStringLiteral("id"), instanceId},
    };
}

} // namespace

class DiscoveryTest final : public QObject {
    Q_OBJECT

private slots:
    void discoversPeersAndRemovesThemOnWithdrawal();
    void announcementIsCompactAndSecretFree();
    void rejectsMalformedAnnouncementsAndTruncatesNames();
    void expiresSilentPeers();
    void filtersOwnAnnouncements();
    void rejectsInvalidConfigurationWithoutBreakingOtherInstances();
};

void DiscoveryTest::discoversPeersAndRemovesThemOnWithdrawal()
{
    const quint16 portA = unusedLoopbackPort();
    const quint16 portB = unusedLoopbackPort();
    QVERIFY(portA != 0);
    QVERIFY(portB != 0);
    QVERIFY(portA != portB);

    LanDiscovery host;
    host.setOptions(loopbackOptions(portA, portB));
    QString error;
    QVERIFY2(host.start(&error), qPrintable(error));

    LanDiscovery viewer;
    viewer.setOptions(loopbackOptions(portB, portA));
    QSignalSpy viewerChanges(
        &viewer,
        &LanDiscovery::devicesChanged);
    QSignalSpy hostChanges(
        &host,
        &LanDiscovery::devicesChanged);
    QVERIFY2(viewer.start(&error), qPrintable(error));

    host.publish(
        QStringLiteral("STUDIO-PC"),
        QStringLiteral("246 813 579"),
        QStringLiteral("192.168.10.20:7900"));
    QTRY_COMPARE_WITH_TIMEOUT(viewer.devices().size(), 1, 2000);
    QCOMPARE(viewer.devices().constFirst().name, QStringLiteral("STUDIO-PC"));
    QCOMPARE(
        viewer.devices().constFirst().code,
        QStringLiteral("246813579"));
    QCOMPARE(
        viewer.devices().constFirst().server,
        QStringLiteral("192.168.10.20:7900"));
    QCOMPARE(viewerChanges.count(), 1);

    viewer.publish(
        QStringLiteral("VIEWER-PC"),
        QStringLiteral("135792468"),
        QStringLiteral("192.168.10.21:7900"));
    QTRY_COMPARE_WITH_TIMEOUT(host.devices().size(), 1, 2000);
    QCOMPARE(host.devices().constFirst().name, QStringLiteral("VIEWER-PC"));
    QCOMPARE(hostChanges.count(), 1);

    QTest::qWait(260);
    QCOMPARE(viewerChanges.count(), 1);
    QCOMPARE(hostChanges.count(), 1);

    viewer.withdraw();
    QTRY_VERIFY_WITH_TIMEOUT(host.devices().isEmpty(), 2000);
    QCOMPARE(hostChanges.count(), 2);

    host.withdraw();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.devices().isEmpty(), 2000);
    QCOMPARE(viewerChanges.count(), 2);
}

void DiscoveryTest::announcementIsCompactAndSecretFree()
{
    QUdpSocket sink;
    QVERIFY(sink.bind(QHostAddress::LocalHost, 0));

    const quint16 bindPort = unusedLoopbackPort();
    QVERIFY(bindPort != 0);

    LanDiscovery publisher;
    publisher.setOptions(loopbackOptions(bindPort, sink.localPort()));
    publisher.publish(
        QStringLiteral("OFFICE-PC"),
        QStringLiteral("123456789"),
        QStringLiteral("192.168.50.2:7900"));
    QString error;
    QVERIFY2(publisher.start(&error), qPrintable(error));

    const std::optional<QByteArray> payload = receiveDatagram(sink);
    QVERIFY(payload.has_value());
    QVERIFY(payload->size() <= LanDiscovery::kMaxPayloadBytes);

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(*payload, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject object = document.object();
    QCOMPARE(
        object.value(QStringLiteral("p")).toString(),
        QStringLiteral("peerdesk-lan-discovery"));
    QCOMPARE(object.value(QStringLiteral("v")).toInt(), 1);
    QCOMPARE(
        object.value(QStringLiteral("t")).toString(),
        QStringLiteral("announce"));
    QCOMPARE(
        object.value(QStringLiteral("id")).toString(),
        publisher.instanceId());
    QCOMPARE(
        object.value(QStringLiteral("name")).toString(),
        QStringLiteral("OFFICE-PC"));
    QCOMPARE(
        object.value(QStringLiteral("code")).toString(),
        QStringLiteral("123456789"));
    QCOMPARE(
        object.value(QStringLiteral("server")).toString(),
        QStringLiteral("192.168.50.2:7900"));
    QVERIFY(!object.value(QStringLiteral("app")).toString().isEmpty());
    QVERIFY(!object.contains(QStringLiteral("token")));
    QVERIFY(!object.contains(QStringLiteral("secret")));
    QVERIFY(!payload->contains("server-token"));
}

void DiscoveryTest::rejectsMalformedAnnouncementsAndTruncatesNames()
{
    const quint16 bindPort = unusedLoopbackPort();
    const quint16 targetPort = unusedLoopbackPort();
    QVERIFY(bindPort != 0);
    QVERIFY(targetPort != 0);
    QVERIFY(bindPort != targetPort);

    LanDiscovery discovery;
    LanDiscovery::Options options =
        loopbackOptions(bindPort, targetPort);
    options.expireAfterMs = 400;
    discovery.setOptions(options);
    QString error;
    QVERIFY2(discovery.start(&error), qPrintable(error));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
    QSignalSpy changes(
        &discovery,
        &LanDiscovery::devicesChanged);

    const QByteArray invalidJson = "not-json";
    QCOMPARE(
        sender.writeDatagram(
            invalidJson,
            QHostAddress::LocalHost,
            discovery.boundPort()),
        invalidJson.size());

    QJsonObject wrongVersion =
        message(QStringLiteral("announce"), QStringLiteral("raw-peer-1"));
    wrongVersion.insert(QStringLiteral("v"), 2);
    wrongVersion.insert(QStringLiteral("name"), QStringLiteral("BAD"));
    wrongVersion.insert(QStringLiteral("code"), QStringLiteral("123456789"));
    wrongVersion.insert(
        QStringLiteral("server"),
        QStringLiteral("10.0.0.2:7900"));
    QVERIFY(sendJson(sender, wrongVersion, discovery.boundPort()));

    QJsonObject invalidId =
        message(QStringLiteral("announce"), QStringLiteral("bad id"));
    invalidId.insert(QStringLiteral("name"), QStringLiteral("BAD"));
    invalidId.insert(QStringLiteral("code"), QStringLiteral("123456789"));
    invalidId.insert(
        QStringLiteral("server"),
        QStringLiteral("10.0.0.2:7900"));
    QVERIFY(sendJson(sender, invalidId, discovery.boundPort()));

    QJsonObject unknownType =
        message(QStringLiteral("probe"), QStringLiteral("raw-peer-2"));
    QVERIFY(sendJson(sender, unknownType, discovery.boundPort()));

    QJsonObject invalidCode =
        message(QStringLiteral("announce"), QStringLiteral("raw-peer-3"));
    invalidCode.insert(QStringLiteral("name"), QStringLiteral("BAD"));
    invalidCode.insert(QStringLiteral("code"), QStringLiteral("12345"));
    invalidCode.insert(
        QStringLiteral("server"),
        QStringLiteral("10.0.0.2:7900"));
    QVERIFY(sendJson(sender, invalidCode, discovery.boundPort()));

    QJsonObject invalidServer =
        message(QStringLiteral("announce"), QStringLiteral("raw-peer-4"));
    invalidServer.insert(QStringLiteral("name"), QStringLiteral("BAD"));
    invalidServer.insert(QStringLiteral("code"), QStringLiteral("123456789"));
    invalidServer.insert(
        QStringLiteral("server"),
        QStringLiteral("not a server"));
    QVERIFY(sendJson(sender, invalidServer, discovery.boundPort()));

    const QByteArray oversized(
        LanDiscovery::kMaxPayloadBytes + 8,
        'x');
    QCOMPARE(
        sender.writeDatagram(
            oversized,
            QHostAddress::LocalHost,
            discovery.boundPort()),
        oversized.size());

    QTest::qWait(100);
    QVERIFY(discovery.devices().isEmpty());
    QCOMPARE(changes.count(), 0);

    QJsonObject valid =
        message(QStringLiteral("announce"), QStringLiteral("raw-peer-5"));
    valid.insert(
        QStringLiteral("name"),
        QString(100, QChar(u'N')));
    valid.insert(QStringLiteral("code"), QStringLiteral("975 310 864"));
    valid.insert(
        QStringLiteral("server"),
        QStringLiteral("192.168.1.10:7900"));
    QVERIFY(sendJson(sender, valid, discovery.boundPort()));

    QTRY_COMPARE_WITH_TIMEOUT(discovery.devices().size(), 1, 2000);
    QCOMPARE(discovery.devices().constFirst().name.size(), 64);
    QCOMPARE(changes.count(), 1);

    QVERIFY(sendJson(sender, valid, discovery.boundPort()));
    QTest::qWait(100);
    QCOMPARE(changes.count(), 1);

    QJsonObject bye =
        message(QStringLiteral("bye"), QStringLiteral("raw-peer-5"));
    bye.insert(QStringLiteral("code"), QStringLiteral("975310864"));
    bye.insert(
        QStringLiteral("server"),
        QStringLiteral("192.168.1.10:7900"));
    QVERIFY(sendJson(sender, bye, discovery.boundPort()));
    QTRY_VERIFY_WITH_TIMEOUT(discovery.devices().isEmpty(), 1000);
    QCOMPARE(changes.count(), 2);
}

void DiscoveryTest::expiresSilentPeers()
{
    const quint16 bindPort = unusedLoopbackPort();
    const quint16 targetPort = unusedLoopbackPort();
    QVERIFY(bindPort != 0);
    QVERIFY(targetPort != 0);
    QVERIFY(bindPort != targetPort);

    LanDiscovery discovery;
    LanDiscovery::Options options =
        loopbackOptions(bindPort, targetPort);
    options.expireAfterMs = 220;
    options.sweepIntervalMs = 20;
    discovery.setOptions(options);
    QString error;
    QVERIFY2(discovery.start(&error), qPrintable(error));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
    QJsonObject announce =
        message(QStringLiteral("announce"), QStringLiteral("ephemeral-peer"));
    announce.insert(QStringLiteral("name"), QStringLiteral("TEMP-PC"));
    announce.insert(QStringLiteral("code"), QStringLiteral("864209753"));
    announce.insert(
        QStringLiteral("server"),
        QStringLiteral("172.16.0.8:7900"));
    QVERIFY(sendJson(sender, announce, discovery.boundPort()));

    QTRY_COMPARE_WITH_TIMEOUT(discovery.devices().size(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(discovery.devices().isEmpty(), 1500);
}

void DiscoveryTest::filtersOwnAnnouncements()
{
    const quint16 port = unusedLoopbackPort();
    QVERIFY(port != 0);

    LanDiscovery discovery;
    discovery.setOptions(loopbackOptions(port, port));
    QSignalSpy changes(
        &discovery,
        &LanDiscovery::devicesChanged);
    QString error;
    QVERIFY2(discovery.start(&error), qPrintable(error));
    discovery.publish(
        QStringLiteral("SELF-PC"),
        QStringLiteral("112233445"),
        QStringLiteral("10.10.10.10:7900"));

    QTest::qWait(180);
    QVERIFY(discovery.devices().isEmpty());
    QCOMPARE(changes.count(), 0);
}

void DiscoveryTest::rejectsInvalidConfigurationWithoutBreakingOtherInstances()
{
    LanDiscovery invalid;
    LanDiscovery::Options invalidOptions = loopbackOptions(
        unusedLoopbackPort(),
        unusedLoopbackPort());
    invalidOptions.bindAddress = QHostAddress(QStringLiteral("::1"));
    invalid.setOptions(invalidOptions);
    QString error;
    QVERIFY(!invalid.start(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!invalid.active());

    const quint16 bindPort = unusedLoopbackPort();
    const quint16 targetPort = unusedLoopbackPort();
    QVERIFY(bindPort != 0);
    QVERIFY(targetPort != 0);
    QVERIFY(bindPort != targetPort);

    LanDiscovery healthy;
    healthy.setOptions(loopbackOptions(bindPort, targetPort));
    QVERIFY2(healthy.start(&error), qPrintable(error));
    QVERIFY(healthy.active());
    QCOMPARE(healthy.boundPort(), bindPort);
    healthy.publish(
        QStringLiteral("HEALTHY-PC"),
        QStringLiteral("556677889"),
        QStringLiteral("192.168.20.4:7900"));
}

QTEST_GUILESS_MAIN(DiscoveryTest)

#include "discovery_test.moc"
