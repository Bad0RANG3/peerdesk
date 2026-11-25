#include "core/protocol.h"
#include "core/stun.h"
#include "server/server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>
#include <QtTest>

#include <optional>

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

class TestClient {
public:
    bool connectTo(quint16 port)
    {
        socket.abort();
        buffer.clear();
        inbox.clear();
        socket.connectToHost(QHostAddress::LocalHost, port);

        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QAbstractSocket::ConnectedState
               && timer.elapsed() < 3000) {
            pumpEvents(2);
        }
        poll();
        return socket.state() == QAbstractSocket::ConnectedState;
    }

    void send(const QJsonObject &object)
    {
        QByteArray wire =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        wire.append('\n');
        socket.write(wire);
        socket.flush();
    }

    void poll()
    {
        buffer.append(socket.readAll());
        for (;;) {
            const qsizetype newline = buffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            const QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            QJsonParseError error;
            const QJsonDocument document =
                QJsonDocument::fromJson(line, &error);
            if (error.error == QJsonParseError::NoError
                && document.isObject()) {
                inbox.append(document.object());
            }
        }
    }

    [[nodiscard]] std::optional<QJsonObject> take(
        const QString &type, int timeoutMs = 2500)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            poll();
            for (qsizetype index = 0; index < inbox.size(); ++index) {
                if (inbox.at(index)
                        .value(QStringLiteral("t"))
                        .toString()
                    == type) {
                    return inbox.takeAt(index);
                }
            }
            if (timer.elapsed() >= timeoutMs) {
                return std::nullopt;
            }
            pumpEvents(2);
        }
    }

    [[nodiscard]] QString summary() const
    {
        QStringList types;
        for (const QJsonObject &message : inbox) {
            types.append(
                message.value(QStringLiteral("t")).toString());
        }
        return types.join(QStringLiteral(", "));
    }

    QTcpSocket socket;
    QByteArray buffer;
    QList<QJsonObject> inbox;
};

SignalingServer::Options loopbackOptions()
{
    SignalingServer::Options options;
    options.host = QStringLiteral("127.0.0.1");
    options.port = 0;
    options.udpPort = 0;
    return options;
}

bool waitForDatagram(
    QUdpSocket &socket, QByteArray *payload, int timeoutMs = 2500)
{
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        if (socket.hasPendingDatagrams()) {
            const qint64 pending = socket.pendingDatagramSize();
            if (pending >= 0) {
                payload->resize(static_cast<qsizetype>(pending));
                const qint64 received = socket.readDatagram(
                    payload->data(), payload->size());
                if (received >= 0) {
                    payload->truncate(
                        static_cast<qsizetype>(received));
                    return true;
                }
            }
        }
        if (timer.elapsed() >= timeoutMs) {
            return false;
        }
        pumpEvents(2);
    }
}

quint32 relayTag(const QJsonObject &message)
{
    return static_cast<quint32>(
        static_cast<quint64>(
            message.value(QStringLiteral("relay_tag")).toDouble()));
}

struct PairInfo {
    QString sid;
    quint32 agentTag = 0;
    quint32 viewerTag = 0;
};

std::optional<PairInfo> establishPair(
    SignalingServer &server, TestClient &agent, TestClient &viewer,
    const QString &code = QStringLiteral("604173829"))
{
    if (!agent.connectTo(server.signalingPort())
        || !viewer.connectTo(server.signalingPort())) {
        return std::nullopt;
    }

    agent.send(
        QJsonObject{
            {QStringLiteral("t"), QStringLiteral("register")},
            {QStringLiteral("code"), code},
            {QStringLiteral("name"), QStringLiteral("agent-pc")},
            {QStringLiteral("cands"),
             QJsonArray{
                 QJsonObject{
                     {QStringLiteral("host"),
                      QStringLiteral("192.0.2.10")},
                     {QStringLiteral("port"), 50000},
                     {QStringLiteral("kind"),
                      QStringLiteral("host")},
                 },
             }},
        });
    if (!agent.take(QStringLiteral("registered")).has_value()) {
        return std::nullopt;
    }

    viewer.send(
        QJsonObject{
            {QStringLiteral("t"), QStringLiteral("connect")},
            {QStringLiteral("code"), code},
            {QStringLiteral("name"), QStringLiteral("viewer-pc")},
            {QStringLiteral("cands"),
             QJsonArray{
                 QJsonObject{
                     {QStringLiteral("host"),
                      QStringLiteral("192.0.2.20")},
                     {QStringLiteral("port"), 50020},
                     {QStringLiteral("kind"),
                      QStringLiteral("host")},
                 },
             }},
        });
    const auto agentRequest =
        agent.take(QStringLiteral("viewer_request"));
    const auto viewerWaiting =
        viewer.take(QStringLiteral("waiting"));
    if (!agentRequest.has_value() || !viewerWaiting.has_value()) {
        return std::nullopt;
    }
    const QString requestId =
        agentRequest->value(QStringLiteral("request_id")).toString();
    if (requestId.isEmpty()
        || viewerWaiting->value(QStringLiteral("request_id")).toString()
            != requestId) {
        return std::nullopt;
    }
    agent.send(
        QJsonObject{
            {QStringLiteral("t"), QStringLiteral("approve")},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("access"), QStringLiteral("control")},
        });
    const auto agentMessage = agent.take(QStringLiteral("viewer"));
    const auto viewerMessage = viewer.take(QStringLiteral("peer"));
    const auto agentAccess = agent.take(QStringLiteral("access"));
    const auto viewerAccess = viewer.take(QStringLiteral("access"));
    if (!agentMessage.has_value() || !viewerMessage.has_value()
        || !agentAccess.has_value() || !viewerAccess.has_value()
        || agentAccess->value(QStringLiteral("access")).toString()
            != QStringLiteral("control")
        || viewerAccess->value(QStringLiteral("access")).toString()
            != QStringLiteral("control")) {
        return std::nullopt;
    }
    return PairInfo{
        agentMessage->value(QStringLiteral("sid")).toString(),
        relayTag(*agentMessage),
        relayTag(*viewerMessage),
    };
}

bool registerAgent(
    TestClient &agent,
    quint16 port,
    const QString &code,
    const QString &name = QStringLiteral("agent-pc"))
{
    if (!agent.connectTo(port)) {
        return false;
    }
    agent.send(
        QJsonObject{
            {QStringLiteral("t"), QStringLiteral("register")},
            {QStringLiteral("code"), code},
            {QStringLiteral("name"), name},
            {QStringLiteral("cands"), QJsonArray{}},
        });
    return agent.take(QStringLiteral("registered")).has_value();
}

bool requestViewer(
    TestClient &viewer,
    quint16 port,
    const QString &code,
    const QString &name,
    QString *requestId)
{
    if (!viewer.connectTo(port)) {
        return false;
    }
    viewer.send(
        QJsonObject{
            {QStringLiteral("t"), QStringLiteral("connect")},
            {QStringLiteral("code"), code},
            {QStringLiteral("name"), name},
            {QStringLiteral("cands"), QJsonArray{}},
        });
    const auto waiting = viewer.take(QStringLiteral("waiting"));
    if (!waiting.has_value()) {
        return false;
    }
    *requestId =
        waiting->value(QStringLiteral("request_id")).toString();
    return !requestId->isEmpty();
}

} // namespace

class ServerTest : public QObject {
    Q_OBJECT

private slots:
    void tokenBucketPolicy()
    {
        TokenBucket unlimited(0.0);
        QVERIFY(!unlimited.enabled());
        for (int index = 0; index < 100; ++index) {
            QVERIFY(unlimited.allow(1400));
        }
        QCOMPARE(unlimited.passed(), quint64(100));
        QCOMPARE(unlimited.dropped(), quint64(0));
        QCOMPARE(unlimited.bytes(), quint64(140000));

        TokenBucket limited(8.0, 1.0);
        QVERIFY(limited.enabled());
        QCOMPARE(limited.rate(), 1000000.0);
        QCOMPARE(limited.capacity(), 1000000.0);
        QVERIFY(limited.allow(1000000, 0.0));
        QVERIFY(!limited.allow(1400, 0.0));
        QCOMPARE(limited.dropped(), quint64(1));
        QVERIFY(limited.allow(1000000, 1.0));
        QCOMPARE(limited.passed(), quint64(2));

        TokenBucket partial(8.0, 1.0);
        QVERIFY(partial.allow(
            static_cast<qint64>(partial.capacity()), 0.0));
        QVERIFY(!partial.allow(200000, 0.0));
        QVERIFY(partial.allow(200000, 0.3));
        QVERIFY(!partial.allow(200000, 0.35));

        TokenBucket capped(8.0, 1.0);
        QVERIFY(capped.allow(
            static_cast<qint64>(capped.capacity()), 0.0));
        QVERIFY(capped.allow(1, 1000.0));
        QVERIFY(capped.tokens() <= capped.capacity());
    }

    void connectLimiterPolicy()
    {
        ConnectRateLimiter defaults;
        QCOMPARE(defaults.maxAttempts(), kDefaultConnectMax);
        QCOMPARE(
            defaults.windowSeconds(),
            kDefaultConnectWindowSeconds);
        for (int index = 0; index < kDefaultConnectMax; ++index) {
            QVERIFY(defaults.allow(
                QStringLiteral("198.51.100.7"), 1000.0));
        }
        QVERIFY(!defaults.allow(
            QStringLiteral("198.51.100.7"), 1000.5));

        ConnectRateLimiter sliding(2, 10.0);
        QVERIFY(sliding.allow(QStringLiteral("a"), 0.0));
        QVERIFY(sliding.allow(QStringLiteral("a"), 5.0));
        QVERIFY(!sliding.allow(QStringLiteral("a"), 6.0));
        QVERIFY(sliding.allow(QStringLiteral("a"), 11.0));
        QVERIFY(!sliding.allow(QStringLiteral("a"), 11.1));

        ConnectRateLimiter independent(1, 60.0);
        QVERIFY(independent.allow(QStringLiteral("a"), 0.0));
        QVERIFY(!independent.allow(QStringLiteral("a"), 0.1));
        QVERIFY(independent.allow(QStringLiteral("b"), 0.2));

        ConnectRateLimiter bounded(50, 1.0);
        for (int index = 0; index < 5000; ++index) {
            const QString ip =
                QStringLiteral("10.0.%1.%2")
                    .arg(index / 256)
                    .arg(index % 256);
            QVERIFY(bounded.allow(ip, 100.0));
        }
        QVERIFY(bounded.trackedAddresses() <= 8192);
        QVERIFY(bounded.allow(QStringLiteral("fresh"), 200.0));
        QVERIFY(bounded.trackedAddresses() < 100);
    }

    void signalingPairing()
    {
        SignalingServer server(loopbackOptions());
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));
        QVERIFY(server.isListening());
        QVERIFY(server.signalingPort() != 0);
        QVERIFY(server.relayPort() != 0);

        TestClient agent;
        QVERIFY(agent.connectTo(server.signalingPort()));
        const QJsonArray agentCands{
            QJsonObject{
                {QStringLiteral("host"),
                 QStringLiteral("192.0.2.10")},
                {QStringLiteral("port"), 50000},
                {QStringLiteral("kind"), QStringLiteral("host")},
            },
        };
        agent.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("register")},
                {QStringLiteral("code"), QStringLiteral("604173829")},
                {QStringLiteral("name"), QStringLiteral("agent-pc")},
                {QStringLiteral("cands"), agentCands},
            });

        const auto registered =
            agent.take(QStringLiteral("registered"));
        QVERIFY2(
            registered.has_value(),
            qPrintable(
                QStringLiteral("registered missing: %1")
                    .arg(agent.summary())));
        QCOMPARE(
            registered->value(QStringLiteral("code")).toString(),
            QStringLiteral("604 173 829"));
        QVERIFY(
            !registered->value(QStringLiteral("token_required"))
                 .toBool());
        QCOMPARE(
            registered->value(QStringLiteral("relay_mbps")).toDouble(),
            0.0);
        const QJsonArray registeredRelay =
            registered->value(QStringLiteral("relay")).toArray();
        QCOMPARE(registeredRelay.size(), qsizetype(2));
        QCOMPARE(
            registeredRelay.at(0).toString(),
            QStringLiteral("127.0.0.1"));
        QCOMPARE(
            registeredRelay.at(1).toInt(),
            static_cast<int>(server.relayPort()));
        QVERIFY(
            registered->value(QStringLiteral("server_time"))
                .toInteger()
            > 0);

        TestClient viewer;
        QVERIFY(viewer.connectTo(server.signalingPort()));
        const QJsonArray viewerCands{
            QJsonObject{
                {QStringLiteral("host"),
                 QStringLiteral("192.0.2.20")},
                {QStringLiteral("port"), 50020},
                {QStringLiteral("kind"), QStringLiteral("host")},
            },
        };
        viewer.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("connect")},
                {QStringLiteral("code"), QStringLiteral("604173829")},
                {QStringLiteral("name"), QStringLiteral("viewer-pc")},
                {QStringLiteral("cands"), viewerCands},
            });

        const auto agentRequest =
            agent.take(QStringLiteral("viewer_request"));
        const auto viewerWaiting =
            viewer.take(QStringLiteral("waiting"));
        QVERIFY2(
            agentRequest.has_value(),
            qPrintable(
                QStringLiteral("viewer_request missing: %1")
                    .arg(agent.summary())));
        QVERIFY2(
            viewerWaiting.has_value(),
            qPrintable(
                QStringLiteral("waiting missing: %1")
                    .arg(viewer.summary())));
        const QString requestId =
            agentRequest->value(QStringLiteral("request_id")).toString();
        QCOMPARE(
            viewerWaiting->value(QStringLiteral("request_id")).toString(),
            requestId);
        QCOMPARE(
            agentRequest->value(QStringLiteral("name")).toString(),
            QStringLiteral("viewer-pc"));
        QCOMPARE(
            viewerWaiting->value(QStringLiteral("name")).toString(),
            QStringLiteral("agent-pc"));
        QVERIFY(
            agentRequest->value(QStringLiteral("expires_ms"))
                .toInt()
            > 0);
        QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());
        QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());

        agent.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("approve")},
                {QStringLiteral("request_id"), requestId},
                {QStringLiteral("access"), QStringLiteral("control")},
            });
        const auto agentViewer =
            agent.take(QStringLiteral("viewer"));
        const auto viewerPeer =
            viewer.take(QStringLiteral("peer"));
        const auto agentAccess =
            agent.take(QStringLiteral("access"));
        const auto viewerAccess =
            viewer.take(QStringLiteral("access"));
        QVERIFY2(agentViewer.has_value(), "agent did not receive viewer");
        QVERIFY2(viewerPeer.has_value(), "viewer did not receive peer");
        QVERIFY2(agentAccess.has_value(), "agent did not receive access");
        QVERIFY2(viewerAccess.has_value(), "viewer did not receive access");
        QCOMPARE(
            agentAccess->value(QStringLiteral("access")).toString(),
            QStringLiteral("control"));
        QCOMPARE(
            viewerAccess->value(QStringLiteral("access")).toString(),
            QStringLiteral("control"));

        const QString sid =
            viewerPeer->value(QStringLiteral("sid")).toString();
        QCOMPARE(
            agentViewer->value(QStringLiteral("sid")).toString(), sid);
        QCOMPARE(sid.size(), qsizetype(16));
        QCOMPARE(
            agentViewer->value(QStringLiteral("name")).toString(),
            QStringLiteral("viewer-pc"));
        QCOMPARE(
            viewerPeer->value(QStringLiteral("name")).toString(),
            QStringLiteral("agent-pc"));
        QVERIFY(
            agentViewer->value(QStringLiteral("cands")).toArray()
            == viewerCands);
        QVERIFY(
            viewerPeer->value(QStringLiteral("cands")).toArray()
            == agentCands);

        const quint32 agentTag = relayTag(*agentViewer);
        const quint32 viewerTag = relayTag(*viewerPeer);
        QVERIFY((agentTag & 1U) != 0);
        QVERIFY((viewerTag & 1U) != 0);
        QVERIFY(agentTag != viewerTag);

        const QJsonArray updateCands{
            QJsonObject{
                {QStringLiteral("host"),
                 QStringLiteral("198.51.100.44")},
                {QStringLiteral("port"), 50044},
                {QStringLiteral("kind"), QStringLiteral("srflx")},
            },
        };
        viewer.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("cands")},
                {QStringLiteral("cands"), updateCands},
            });
        const auto forwarded =
            agent.take(QStringLiteral("peer_cands"));
        QVERIFY(forwarded.has_value());
        QVERIFY(
            forwarded->value(QStringLiteral("cands")).toArray()
            == updateCands);

        agent.send(
            QJsonObject{{QStringLiteral("t"), QStringLiteral("ping")}});
        QVERIFY(agent.take(QStringLiteral("pong")).has_value());

        viewer.send(
            QJsonObject{{QStringLiteral("t"), QStringLiteral("bye")}});
        const auto bye = agent.take(QStringLiteral("peer_bye"));
        QVERIFY(bye.has_value());

        agent.socket.abort();
        const auto gone = viewer.take(QStringLiteral("peer_gone"));
        QVERIFY2(gone.has_value(), "peer_gone missing after agent drop");

        viewer.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("connect")},
                {QStringLiteral("code"), QStringLiteral("604173829")},
                {QStringLiteral("cands"), QJsonArray{}},
            });
        const auto offline = viewer.take(QStringLiteral("error"));
        QVERIFY(offline.has_value());
        QCOMPARE(
            offline->value(QStringLiteral("reason")).toString(),
            QStringLiteral("offline"));

        viewer.send(
            QJsonObject{{QStringLiteral("t"), QStringLiteral("wat")}});
        const auto unknown = viewer.take(QStringLiteral("error"));
        QVERIFY(unknown.has_value());
        QCOMPARE(
            unknown->value(QStringLiteral("reason")).toString(),
            QStringLiteral("unknown_type"));
    }

    void signalingAuthAndCodes()
    {
        {
            SignalingServer server(loopbackOptions());
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient client;
            QVERIFY(client.connectTo(server.signalingPort()));
            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("register")},
                    {QStringLiteral("code"),
                     QStringLiteral("012345678")},
                });
            const auto badCode = client.take(QStringLiteral("error"));
            QVERIFY(badCode.has_value());
            QCOMPARE(
                badCode->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_code"));

            client.socket.write("{not-json]\n");
            client.socket.flush();
            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("register")},
                    {QStringLiteral("code"),
                     QStringLiteral("123 456 789")},
                    {QStringLiteral("name"),
                     QStringLiteral("after-bad-json")},
                });
            QVERIFY(
                client.take(QStringLiteral("registered")).has_value());
        }

        {
            SignalingServer::Options options = loopbackOptions();
            options.token = QStringLiteral("s3cret");
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient client;
            QVERIFY(client.connectTo(server.signalingPort()));
            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("register")},
                    {QStringLiteral("code"),
                     QStringLiteral("123456789")},
                });
            const auto missing = client.take(QStringLiteral("error"));
            QVERIFY(missing.has_value());
            QCOMPARE(
                missing->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_token"));

            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("register")},
                    {QStringLiteral("code"),
                     QStringLiteral("123456789")},
                    {QStringLiteral("token"), 12345},
                });
            const auto nonString =
                client.take(QStringLiteral("error"));
            QVERIFY(nonString.has_value());
            QCOMPARE(
                nonString->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_token"));

            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("register")},
                    {QStringLiteral("code"),
                     QStringLiteral("123456789")},
                    {QStringLiteral("token"),
                     QStringLiteral("s3cret")},
                });
            const auto registered =
                client.take(QStringLiteral("registered"));
            QVERIFY(registered.has_value());
            QVERIFY(
                registered->value(QStringLiteral("token_required"))
                    .toBool());

            client.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("connect")},
                    {QStringLiteral("code"),
                     QStringLiteral("123456789")},
                });
            const auto connectAuth =
                client.take(QStringLiteral("error"));
            QVERIFY(connectAuth.has_value());
            QCOMPARE(
                connectAuth->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_token"));
        }
    }

    void signalingApprovalBoundaries()
    {
        {
            SignalingServer::Options options = loopbackOptions();
            options.connectMax = 1000;
            options.connectWindow = 60.0;
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient agent;
            QVERIFY(registerAgent(
                agent, server.signalingPort(),
                QStringLiteral("246813570")));
            TestClient viewer;
            QString requestId;
            QVERIFY(requestViewer(
                viewer, server.signalingPort(),
                QStringLiteral("246813570"),
                QStringLiteral("rejected-viewer"),
                &requestId));
            const auto request =
                agent.take(QStringLiteral("viewer_request"));
            QVERIFY(request.has_value());
            QCOMPARE(
                request->value(QStringLiteral("request_id")).toString(),
                requestId);
            QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());
            QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());

            agent.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("reject")},
                    {QStringLiteral("request_id"), requestId},
                    {QStringLiteral("reason"),
                     QStringLiteral("现在不方便")},
                });
            const auto rejected = viewer.take(QStringLiteral("error"));
            QVERIFY(rejected.has_value());
            QCOMPARE(
                rejected->value(QStringLiteral("reason")).toString(),
                QStringLiteral("rejected"));
            QCOMPARE(
                rejected->value(QStringLiteral("text")).toString(),
                QStringLiteral("现在不方便"));
            QCOMPARE(
                rejected->value(QStringLiteral("request_id")).toString(),
                requestId);
            QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());
            QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());

            agent.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("approve")},
                    {QStringLiteral("request_id"), requestId},
                    {QStringLiteral("access"), QStringLiteral("control")},
                });
            const auto stale = agent.take(QStringLiteral("error"));
            QVERIFY(stale.has_value());
            QCOMPARE(
                stale->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_request"));
        }

        {
            SignalingServer::Options options = loopbackOptions();
            options.connectMax = 1000;
            options.connectWindow = 60.0;
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient agent;
            QVERIFY(registerAgent(
                agent, server.signalingPort(),
                QStringLiteral("246813571")));
            TestClient viewer;
            QString requestId;
            QVERIFY(requestViewer(
                viewer, server.signalingPort(),
                QStringLiteral("246813571"),
                QStringLiteral("cancelling-viewer"),
                &requestId));
            QVERIFY(
                agent.take(QStringLiteral("viewer_request"))
                    .has_value());
            viewer.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("cancel")},
                    {QStringLiteral("request_id"), requestId},
                });
            const auto cancelled =
                agent.take(QStringLiteral("viewer_cancelled"));
            QVERIFY(cancelled.has_value());
            QCOMPARE(
                cancelled->value(QStringLiteral("request_id")).toString(),
                requestId);
            QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());
            QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());

            viewer.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("cancel")},
                    {QStringLiteral("request_id"), requestId},
                });
            const auto repeated = viewer.take(QStringLiteral("error"));
            QVERIFY(repeated.has_value());
            QCOMPARE(
                repeated->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_request"));
        }

        {
            SignalingServer::Options options = loopbackOptions();
            options.connectMax = 1000;
            options.connectWindow = 60.0;
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient agent;
            QVERIFY(registerAgent(
                agent, server.signalingPort(),
                QStringLiteral("246813572")));
            TestClient viewer;
            QString requestId;
            QVERIFY(requestViewer(
                viewer, server.signalingPort(),
                QStringLiteral("246813572"),
                QStringLiteral("access-viewer"),
                &requestId));
            QVERIFY(
                agent.take(QStringLiteral("viewer_request"))
                    .has_value());
            agent.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("approve")},
                    {QStringLiteral("request_id"), requestId},
                    {QStringLiteral("access"), QStringLiteral("view")},
                });
            const auto agentError = agent.take(QStringLiteral("error"));
            const auto viewerError = viewer.take(QStringLiteral("error"));
            QVERIFY(agentError.has_value());
            QVERIFY(viewerError.has_value());
            QCOMPARE(
                agentError->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_access"));
            QCOMPARE(
                viewerError->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_access"));
            QCOMPARE(
                viewerError->value(QStringLiteral("request_id")).toString(),
                requestId);
            QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());
            QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());
        }

        {
            SignalingServer::Options options = loopbackOptions();
            options.connectMax = 1000;
            options.connectWindow = 60.0;
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient agent;
            QVERIFY(registerAgent(
                agent, server.signalingPort(),
                QStringLiteral("246813573")));
            TestClient viewerOne;
            QString requestId;
            QVERIFY(requestViewer(
                viewerOne, server.signalingPort(),
                QStringLiteral("246813573"),
                QStringLiteral("first-viewer"),
                &requestId));
            QVERIFY(
                agent.take(QStringLiteral("viewer_request"))
                    .has_value());

            TestClient viewerTwo;
            QVERIFY(viewerTwo.connectTo(server.signalingPort()));
            viewerTwo.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("connect")},
                    {QStringLiteral("code"),
                     QStringLiteral("246813573")},
                    {QStringLiteral("name"),
                     QStringLiteral("second-viewer")},
                });
            const auto busy = viewerTwo.take(QStringLiteral("error"));
            QVERIFY(busy.has_value());
            QCOMPARE(
                busy->value(QStringLiteral("reason")).toString(),
                QStringLiteral("host_busy"));
            QVERIFY(!viewerTwo.take(QStringLiteral("waiting"), 150).has_value());

            agent.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("approve")},
                    {QStringLiteral("request_id"), requestId},
                    {QStringLiteral("access"), QStringLiteral("control")},
                });
            QVERIFY(agent.take(QStringLiteral("viewer")).has_value());
            QVERIFY(viewerOne.take(QStringLiteral("peer")).has_value());
            QVERIFY(agent.take(QStringLiteral("access")).has_value());
            QVERIFY(viewerOne.take(QStringLiteral("access")).has_value());

            TestClient viewerThree;
            QVERIFY(viewerThree.connectTo(server.signalingPort()));
            viewerThree.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("connect")},
                    {QStringLiteral("code"),
                     QStringLiteral("246813573")},
                    {QStringLiteral("name"),
                     QStringLiteral("third-viewer")},
                });
            const auto pairedBusy =
                viewerThree.take(QStringLiteral("error"));
            QVERIFY(pairedBusy.has_value());
            QCOMPARE(
                pairedBusy->value(QStringLiteral("reason")).toString(),
                QStringLiteral("host_busy"));
        }

        {
            SignalingServer::Options options = loopbackOptions();
            options.connectMax = 1000;
            options.connectWindow = 60.0;
            options.approvalTimeoutMs = 200;
            SignalingServer server(options);
            QString error;
            QVERIFY2(server.start(&error), qPrintable(error));

            TestClient agent;
            QVERIFY(registerAgent(
                agent, server.signalingPort(),
                QStringLiteral("246813574")));
            TestClient viewer;
            QString requestId;
            QVERIFY(requestViewer(
                viewer, server.signalingPort(),
                QStringLiteral("246813574"),
                QStringLiteral("timeout-viewer"),
                &requestId));
            QVERIFY(
                agent.take(QStringLiteral("viewer_request"))
                    .has_value());
            const auto expired =
                agent.take(QStringLiteral("viewer_request_expired"), 3000);
            QVERIFY(expired.has_value());
            QCOMPARE(
                expired->value(QStringLiteral("request_id")).toString(),
                requestId);
            const auto timedOut = viewer.take(QStringLiteral("error"), 3000);
            QVERIFY(timedOut.has_value());
            QCOMPARE(
                timedOut->value(QStringLiteral("reason")).toString(),
                QStringLiteral("approval_timeout"));
            QCOMPARE(
                timedOut->value(QStringLiteral("request_id")).toString(),
                requestId);
            QVERIFY(!viewer.take(QStringLiteral("peer"), 150).has_value());
            QVERIFY(!agent.take(QStringLiteral("viewer"), 150).has_value());

            agent.send(
                QJsonObject{
                    {QStringLiteral("t"), QStringLiteral("approve")},
                    {QStringLiteral("request_id"), requestId},
                    {QStringLiteral("access"), QStringLiteral("control")},
                });
            const auto stale = agent.take(QStringLiteral("error"));
            QVERIFY(stale.has_value());
            QCOMPARE(
                stale->value(QStringLiteral("reason")).toString(),
                QStringLiteral("bad_request"));
        }
    }

    void signalingRateLimit()
    {
        SignalingServer::Options options = loopbackOptions();
        options.connectMax = 1;
        SignalingServer server(options);
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));

        TestClient agent;
        TestClient viewerOne;
        const auto pair = establishPair(server, agent, viewerOne);
        QVERIFY(pair.has_value());

        TestClient viewerTwo;
        QVERIFY(viewerTwo.connectTo(server.signalingPort()));
        viewerTwo.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("connect")},
                {QStringLiteral("code"),
                 QStringLiteral("604173829")},
            });
        const auto limited =
            viewerTwo.take(QStringLiteral("error"));
        QVERIFY(limited.has_value());
        QCOMPARE(
            limited->value(QStringLiteral("reason")).toString(),
            QStringLiteral("rate_limited"));

        viewerTwo.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("connect")},
                {QStringLiteral("code"),
                 QStringLiteral("999888777")},
            });
        const auto limitedAgain =
            viewerTwo.take(QStringLiteral("error"));
        QVERIFY(limitedAgain.has_value());
        QCOMPARE(
            limitedAgain->value(QStringLiteral("reason")).toString(),
            QStringLiteral("rate_limited"));

        viewerOne.send(
            QJsonObject{
                {QStringLiteral("t"), QStringLiteral("reconnect")},
                {QStringLiteral("code"),
                 QStringLiteral("604173829")},
            });
        QVERIFY(
            agent.take(QStringLiteral("viewer_gone")).has_value());
    }

    void udpStunAndRelay()
    {
        SignalingServer server(loopbackOptions());
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));

        TestClient agent;
        TestClient viewer;
        const auto pair = establishPair(server, agent, viewer);
        QVERIFY(pair.has_value());

        QUdpSocket agentUdp;
        QUdpSocket viewerUdp;
        QVERIFY(agentUdp.bind(QHostAddress::LocalHost, 0));
        QVERIFY(viewerUdp.bind(QHostAddress::LocalHost, 0));

        QByteArray transactionId(12, '\x5a');
        const QByteArray request =
            stun::buildBindingRequest(transactionId);
        QCOMPARE(
            agentUdp.writeDatagram(
                request, QHostAddress::LocalHost, server.relayPort()),
            qint64(request.size()));

        QByteArray response;
        QVERIFY(waitForDatagram(agentUdp, &response));
        const auto stunMessage = stun::parseMessage(response);
        QVERIFY(stunMessage.has_value());
        QCOMPARE(stunMessage->type, stun::kBindingSuccess);
        QCOMPARE(stunMessage->transactionId, transactionId);
        const auto mapped = stun::parseMappedAddress(
            stunMessage->attributes, stunMessage->transactionId);
        QVERIFY(mapped.has_value());
        QCOMPARE(mapped->address, QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(mapped->port, agentUdp.localPort());

        const QByteArray agentProbe("agent-probe");
        agentUdp.writeDatagram(
            wrapRelay(pair->agentTag, agentProbe),
            QHostAddress::LocalHost, server.relayPort());
        pumpEvents(40);

        const QByteArray viewerProbe("viewer-probe");
        viewerUdp.writeDatagram(
            wrapRelay(pair->viewerTag, viewerProbe),
            QHostAddress::LocalHost, server.relayPort());
        QByteArray forwarded;
        QVERIFY(waitForDatagram(agentUdp, &forwarded));
        QCOMPARE(forwarded, viewerProbe);

        const QByteArray agentPacket("agent-to-viewer");
        agentUdp.writeDatagram(
            wrapRelay(pair->agentTag, agentPacket),
            QHostAddress::LocalHost, server.relayPort());
        QVERIFY(waitForDatagram(viewerUdp, &forwarded));
        QCOMPARE(forwarded, agentPacket);

        viewerUdp.writeDatagram(
            wrapRelay(0xDEADBEEFU, QByteArray("unknown-tag")),
            QHostAddress::LocalHost, server.relayPort());
        QVERIFY(!waitForDatagram(viewerUdp, &forwarded, 150));
    }

    void relayBudgetDrops()
    {
        SignalingServer::Options options = loopbackOptions();
        options.relayMbps = 0.008;
        SignalingServer server(options);
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));

        TestClient agent;
        TestClient viewer;
        const auto pair = establishPair(server, agent, viewer);
        QVERIFY(pair.has_value());

        QUdpSocket agentUdp;
        QUdpSocket viewerUdp;
        QVERIFY(agentUdp.bind(QHostAddress::LocalHost, 0));
        QVERIFY(viewerUdp.bind(QHostAddress::LocalHost, 0));

        agentUdp.writeDatagram(
            wrapRelay(pair->agentTag, QByteArray("register-agent")),
            QHostAddress::LocalHost, server.relayPort());
        pumpEvents(30);
        viewerUdp.writeDatagram(
            wrapRelay(pair->viewerTag, QByteArray("register-viewer")),
            QHostAddress::LocalHost, server.relayPort());
        QByteArray forwarded;
        QVERIFY(waitForDatagram(agentUdp, &forwarded));
        QCOMPARE(forwarded, QByteArray("register-viewer"));

        const QByteArray oversized(9000, 'x');
        agentUdp.writeDatagram(
            wrapRelay(pair->agentTag, oversized),
            QHostAddress::LocalHost, server.relayPort());
        QVERIFY(!waitForDatagram(viewerUdp, &forwarded, 250));
    }
};

QTEST_GUILESS_MAIN(ServerTest)

#include "server_test.moc"
