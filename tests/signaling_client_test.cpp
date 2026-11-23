#include "client/signaling_client.h"
#include "core/util.h"
#include "server/server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

using namespace pd;

namespace {

void pumpEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 10);
        if (timer.elapsed() < milliseconds) {
            QThread::msleep(1);
        }
    } while (timer.elapsed() < milliseconds);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

SignalingServer::Options loopbackOptions()
{
    SignalingServer::Options options;
    options.host = QStringLiteral("127.0.0.1");
    options.port = 0;
    options.udpPort = 0;
    return options;
}

QList<Candidate> sampleCandidates(quint16 port)
{
    return {
        Candidate{
            QStringLiteral("192.0.2.10"), port,
            QStringLiteral("host")},
        Candidate{
            QStringLiteral("198.51.100.10"),
            static_cast<quint16>(port + 1),
            QStringLiteral("srflx")},
    };
}

} // namespace

class SignalingClientTest : public QObject {
    Q_OBJECT

private slots:
    void hostAndViewerFlow()
    {
        SignalingServer server(loopbackOptions());
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        SignalingClient host;
        host.setServer(QStringLiteral("127.0.0.1"), server.signalingPort());
        QSignalSpy hostRegistered(
            &host, &SignalingClient::registered);
        QVERIFY(host.startHost(
            QStringLiteral("604173829"),
            QStringLiteral("agent-pc"),
            sampleCandidates(50000)));

        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Ready, 3000);
        QCOMPARE(hostRegistered.count(), 1);
        const RegistrationInfo registered = host.registration();
        QCOMPARE(registered.code, QStringLiteral("604173829"));
        QVERIFY(registered.relay.valid());
        QCOMPARE(registered.relay.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(
            registered.relay.port, server.relayPort());
        QCOMPARE(registered.relay.tag, 0U);
        QCOMPARE(registered.relayMbps, 0.0);
        QVERIFY(!registered.tokenRequired);

        SignalingClient viewer;
        viewer.setServer(
            QStringLiteral("127.0.0.1"), server.signalingPort());
        QSignalSpy viewerPeer(
            &viewer, &SignalingClient::peerJoined);
        QSignalSpy hostPeer(
            &host, &SignalingClient::peerJoined);
        QVERIFY(viewer.startViewer(
            QStringLiteral("604 173 829"),
            QStringLiteral("viewer-pc"),
            sampleCandidates(50020)));

        QTRY_COMPARE_WITH_TIMEOUT(
            viewer.state(), SignalingClient::State::Paired, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Paired, 3000);
        QCOMPARE(viewerPeer.count(), 1);
        QCOMPARE(hostPeer.count(), 1);

        const PeerInfo agentInfo = viewer.peer();
        QCOMPARE(agentInfo.name, QStringLiteral("agent-pc"));
        QCOMPARE(agentInfo.sid.size(), qsizetype(16));
        QCOMPARE(agentInfo.candidates.size(), qsizetype(2));
        QCOMPARE(agentInfo.candidates.at(1).kind, QStringLiteral("srflx"));
        QVERIFY(agentInfo.relay.valid());
        QVERIFY(agentInfo.relay.tag != 0U);
        QCOMPARE(agentInfo.relay.port, server.relayPort());

        const PeerInfo viewerInfo = host.peer();
        QCOMPARE(viewerInfo.name, QStringLiteral("viewer-pc"));
        QCOMPARE(viewerInfo.sid, agentInfo.sid);
        QVERIFY(viewerInfo.relay.tag != 0U);
        QVERIFY(viewerInfo.relay.tag != agentInfo.relay.tag);

        QSignalSpy candidatesChanged(
            &viewer, &SignalingClient::peerCandidatesChanged);
        host.updateCandidates({
            Candidate{
                QStringLiteral("203.0.113.9"), 51000,
                QStringLiteral("srflx")},
        });
        QTRY_COMPARE_WITH_TIMEOUT(
            candidatesChanged.count(), 1, 3000);
        QCOMPARE(viewer.peer().candidates.size(), qsizetype(1));
        QCOMPARE(
            viewer.peer().candidates.first().host,
            QStringLiteral("203.0.113.9"));

        QSignalSpy latencySpy(
            &viewer, &SignalingClient::latencyMeasured);
        QVERIFY(viewer.sendPing());
        QTRY_COMPARE_WITH_TIMEOUT(latencySpy.count(), 1, 3000);
        QVERIFY(latencySpy.first().first().toLongLong() >= 0);

        QSignalSpy hostLeft(&host, &SignalingClient::peerLeft);
        viewer.stop();
        QTRY_COMPARE_WITH_TIMEOUT(hostLeft.count(), 1, 3000);
        QVERIFY(!host.paired());
        QCOMPARE(host.state(), SignalingClient::State::Ready);
    }

    void viewerWaitsForOfflineHost()
    {
        SignalingServer::Options options = loopbackOptions();
        options.connectMax = 1000;
        options.connectWindow = 60.0;
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        SignalingClient viewer;
        viewer.setServer(
            QStringLiteral("127.0.0.1"), server.signalingPort());
        viewer.setReconnectPolicy(40, 160);
        viewer.setPairRetryPolicy(40, 160);
        QSignalSpy viewerErrors(
            &viewer, &SignalingClient::errorOccurred);
        QSignalSpy viewerPeer(
            &viewer, &SignalingClient::peerJoined);
        QVERIFY(viewer.startViewer(
            QStringLiteral("918273645"),
            QStringLiteral("waiting-viewer"),
            sampleCandidates(50040)));

        QTRY_VERIFY_WITH_TIMEOUT(viewerErrors.count() >= 1, 3000);
        QCOMPARE(
            viewer.lastError(), QStringLiteral("offline"));
        QCOMPARE(viewer.role(), SignalingClient::Role::Viewer);
        QVERIFY(viewer.state() != SignalingClient::State::Error);
        pumpEvents(250);
        QCOMPARE(viewer.role(), SignalingClient::Role::Viewer);
        QVERIFY(viewer.state() != SignalingClient::State::Error);

        SignalingClient host;
        host.setServer(
            QStringLiteral("127.0.0.1"), server.signalingPort());
        host.setReconnectPolicy(40, 160);
        QSignalSpy hostPeer(
            &host, &SignalingClient::peerJoined);
        QVERIFY(host.startHost(
            QStringLiteral("918273645"),
            QStringLiteral("late-host"),
            sampleCandidates(50060)));

        QTRY_COMPARE_WITH_TIMEOUT(
            viewer.state(), SignalingClient::State::Paired, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Paired, 5000);
        QCOMPARE(viewerPeer.count(), 1);
        QCOMPARE(hostPeer.count(), 1);
        QCOMPARE(
            viewer.peer().name, QStringLiteral("late-host"));
        QCOMPARE(
            host.peer().name, QStringLiteral("waiting-viewer"));

        viewer.stop();
        host.stop();
    }

    void tokenAndInvalidCode()
    {
        SignalingServer::Options options = loopbackOptions();
        options.token = QStringLiteral("s3cret");
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        SignalingClient host;
        host.setServer(QStringLiteral("127.0.0.1"), server.signalingPort());
        host.setAutoReconnect(false);
        QSignalSpy errors(&host, &SignalingClient::errorOccurred);
        QVERIFY(host.startHost(
            QStringLiteral("123456789"),
            QStringLiteral("agent")));
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Error, 3000);
        QCOMPARE(host.lastError(), QStringLiteral("bad_token"));
        QCOMPARE(errors.count(), 1);

        host.setToken(QStringLiteral("s3cret"));
        QVERIFY(host.startHost(
            QStringLiteral("123456789"),
            QStringLiteral("agent")));
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Ready, 3000);
        QVERIFY(host.registration().tokenRequired);

        SignalingClient invalid;
        QSignalSpy invalidErrors(
            &invalid, &SignalingClient::errorOccurred);
        QVERIFY(!invalid.startViewer(
            QStringLiteral("123"),
            QStringLiteral("viewer")));
        QCOMPARE(invalid.lastError(), QStringLiteral("bad_code"));
        QCOMPARE(invalidErrors.count(), 1);

        SignalingClient missingToken;
        missingToken.setServer(
            QStringLiteral("127.0.0.1"), server.signalingPort());
        QVERIFY(missingToken.startViewer(
            QStringLiteral("123456789"),
            QStringLiteral("viewer")));
        QTRY_COMPARE_WITH_TIMEOUT(
            missingToken.state(),
            SignalingClient::State::Error, 3000);
        QCOMPARE(
            missingToken.lastError(), QStringLiteral("bad_token"));
    }

    void tokenFieldOmittedWhenEmpty()
    {
        QTcpServer fakeServer;
        QVERIFY(fakeServer.listen(QHostAddress::LocalHost, 0));

        SignalingClient client;
        client.setServer(
            QStringLiteral("127.0.0.1"), fakeServer.serverPort());
        client.setAutoReconnect(false);
        QVERIFY(client.startHost(
            QStringLiteral("123456789"),
            QStringLiteral("agent")));

        QTRY_VERIFY_WITH_TIMEOUT(
            fakeServer.hasPendingConnections(), 3000);
        QTcpSocket *peer = fakeServer.nextPendingConnection();
        QVERIFY(peer != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            peer->canReadLine() || peer->bytesAvailable() > 0, 3000);
        QByteArray wire;
        QElapsedTimer timer;
        timer.start();
        while (!wire.contains('\n') && timer.elapsed() < 3000) {
            pumpEvents(5);
            wire.append(peer->readAll());
        }
        QVERIFY(wire.contains('\n'));
        const QJsonObject message =
            QJsonDocument::fromJson(wire.left(wire.indexOf('\n')))
                .object();
        QCOMPARE(
            message.value(QStringLiteral("t")).toString(),
            QStringLiteral("register"));
        QVERIFY(!message.contains(QStringLiteral("token")));
        peer->disconnectFromHost();
        peer->deleteLater();
    }

    void reconnectsAfterServerRestart()
    {
        SignalingServer::Options options = loopbackOptions();
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));
        const quint16 port = server.signalingPort();

        SignalingClient host;
        host.setReconnectPolicy(40, 160);
        host.setServer(QStringLiteral("127.0.0.1"), port);
        QSignalSpy registrations(
            &host, &SignalingClient::registered);
        QVERIFY(host.startHost(
            QStringLiteral("123456789"),
            QStringLiteral("agent")));
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Ready, 3000);
        QCOMPARE(registrations.count(), 1);

        server.stop();
        QTRY_VERIFY_WITH_TIMEOUT(
            host.state() == SignalingClient::State::Reconnecting
                || host.state() == SignalingClient::State::Connecting,
            3000);

        SignalingServer::Options restartOptions = loopbackOptions();
        restartOptions.port = port;
        SignalingServer restarted(restartOptions);
        QString restartError;
        QVERIFY2(
            restarted.start(&restartError), qPrintable(restartError));
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), SignalingClient::State::Ready, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(registrations.count() >= 2, 3000);
    }
};

QTEST_GUILESS_MAIN(SignalingClientTest)

#include "signaling_client_test.moc"
