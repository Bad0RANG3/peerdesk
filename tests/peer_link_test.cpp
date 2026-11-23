#include "transport/peer_link.h"

#include "server/server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <optional>

using namespace pd;

namespace {

void pumpEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (timer.elapsed() < milliseconds) {
            QThread::msleep(1);
        }
    } while (timer.elapsed() < milliseconds);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        pumpEvents(20);
    }
    return predicate();
}

class SignalClient {
public:
    bool connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil(
            [this]() {
                return socket.state() == QAbstractSocket::ConnectedState;
            },
            3000);
    }

    void send(const QJsonObject &object)
    {
        QByteArray wire =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        wire.append('\n');
        socket.write(wire);
        socket.flush();
    }

    [[nodiscard]] std::optional<QJsonObject> take(
        const QString &type,
        int timeoutMs = 3000)
    {
        return waitUntil(
                   [this, &type]() {
                       poll();
                       return std::any_of(
                           inbox.cbegin(),
                           inbox.cend(),
                           [&type](const QJsonObject &message) {
                               return message.value(QStringLiteral("t"))
                                          .toString()
                                   == type;
                           });
                   },
                   timeoutMs)
            ? takeNow(type)
            : std::nullopt;
    }

    QTcpSocket socket;

private:
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
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) {
                inbox.append(document.object());
            }
        }
    }

    std::optional<QJsonObject> takeNow(const QString &type)
    {
        for (qsizetype index = 0; index < inbox.size(); ++index) {
            if (inbox.at(index).value(QStringLiteral("t")).toString() == type) {
                return inbox.takeAt(index);
            }
        }
        return std::nullopt;
    }

    QByteArray buffer;
    QList<QJsonObject> inbox;
};

quint32 relayTag(const QJsonObject &message)
{
    return static_cast<quint32>(
        static_cast<quint64>(
            message.value(QStringLiteral("relay_tag")).toDouble()));
}

} // namespace

class PeerLinkTest : public QObject {
    Q_OBJECT

private slots:
    void directHandshakeControlVideoAndAudio()
    {
        const QByteArray psk = QByteArrayLiteral("shared-test-secret");
        PeerLink host(true, psk);
        PeerLink client(false, psk);
        host.setLabel(QStringLiteral("host"));
        client.setLabel(QStringLiteral("client"));
        host.setDeviceId(QStringLiteral("host-device"));
        client.setDeviceId(QStringLiteral("client-device"));

        QString error;
        QVERIFY2(host.start(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        QVERIFY2(client.start(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));

        QList<QJsonObject> hostControls;
        QList<QJsonObject> clientControls;
        QList<QByteArray> clientFrames;
        QList<bool> clientKeyframes;
        QList<QByteArray> hostAudio;
        connect(&host, &PeerLink::controlMessageReceived,
                [&hostControls](const QJsonObject &message) {
                    hostControls.append(message);
                });
        connect(&client, &PeerLink::controlMessageReceived,
                [&clientControls](const QJsonObject &message) {
                    clientControls.append(message);
                });
        connect(&client, &PeerLink::videoFrameReceived,
                [&clientFrames, &clientKeyframes](const QByteArray &frame, bool keyframe) {
                    clientFrames.append(frame);
                    clientKeyframes.append(keyframe);
                });
        connect(&host, &PeerLink::audioReceived,
                [&hostAudio](const QByteArray &payload) {
                    hostAudio.append(payload);
                });

        host.addCandidates(client.localCandidates());
        client.addCandidates(host.localCandidates());
        host.beginHandshake();
        client.beginHandshake();

        QVERIFY(waitUntil([&]() { return host.established() && client.established(); }));
        QTRY_COMPARE_WITH_TIMEOUT(host.state(), QStringLiteral("direct"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QStringLiteral("direct"), 3000);

        QJsonObject large;
        large.insert(QStringLiteral("t"), QStringLiteral("test"));
        large.insert(QStringLiteral("body"), QString(4000, QChar(u'x')));
        client.sendControl(large);
        QVERIFY(waitUntil([&]() {
            return std::any_of(
                hostControls.begin(),
                hostControls.end(),
                [](const QJsonObject &message) {
                    return message.value(QStringLiteral("t")).toString()
                        == QStringLiteral("test");
                });
        }));
        const auto received = std::find_if(
            hostControls.begin(),
            hostControls.end(),
            [](const QJsonObject &message) {
                return message.value(QStringLiteral("t")).toString()
                    == QStringLiteral("test");
            });
        QVERIFY(received != hostControls.end());
        QCOMPARE(
            received->value(QStringLiteral("body")).toString(),
            QString(4000, QChar(u'x')));

        QByteArray jpeg(24000, '\0');
        for (qsizetype index = 0; index < jpeg.size(); ++index) {
            jpeg[index] = static_cast<char>((index * 31) & 0xFF);
        }
        host.sendVideo(jpeg, true);
        QVERIFY(waitUntil([&]() { return !clientFrames.isEmpty(); }));
        QCOMPARE(clientFrames.constFirst(), jpeg);
        QCOMPARE(clientKeyframes.constFirst(), true);
        QCOMPARE(
            host.videoPacketizerThreadName(),
            QStringLiteral("PeerDeskVideoSend"));
        QVERIFY(
            host.videoPacketizerThreadName()
            != QThread::currentThread()->objectName());

        AudioPacket audio;
        audio.sequence = 7;
        audio.timestampMs = 140;
        audio.samples = 160;
        audio.previous = QByteArray(160, '\x01');
        audio.current = QByteArray(160, '\x02');
        const QByteArray audioPayload = packAudio(audio);
        client.sendAudio(audioPayload);
        QVERIFY(waitUntil([&]() { return !hostAudio.isEmpty(); }));
        QCOMPARE(hostAudio.constFirst(), audioPayload);

        QVERIFY(waitUntil([&]() { return host.stats().rttMs > 0.0; }, 6000));
        QVERIFY(!host.stats().path.isEmpty());
        QVERIFY(host.stats().mbps >= 0.0);

        client.close();
        QTRY_COMPARE_WITH_TIMEOUT(host.state(), QStringLiteral("closed"), 2000);
    }

    void videoPacketizerBuildsAndRetransmitsOffThread()
    {
        VideoSendPipeline pipeline;
        QList<VideoPacketBatch> batches;
        connect(
            &pipeline,
            &VideoSendPipeline::packetsReady,
            [&batches](const VideoPacketBatch &batch) {
                batches.append(batch);
            });
        pipeline.start();

        pipeline.submit(QByteArray(24000, 'v'), true);
        QVERIFY(waitUntil([&]() { return !batches.isEmpty(); }));
        const VideoPacketBatch original = batches.constFirst();
        QCOMPARE(original.keyframe, true);
        QCOMPARE(original.retransmit, false);
        QVERIFY(original.frameId > 0);
        QCOMPARE(
            pipeline.packetizerThreadName(),
            QStringLiteral("PeerDeskVideoSend"));
        QVERIFY(
            pipeline.packetizerThreadName()
            != QThread::currentThread()->objectName());

        QList<WirePacket> fragments;
        for (const WirePacket &packet : original.packets) {
            if (packet.type == PacketType::Data) {
                fragments.append(packet);
            }
        }
        QVERIFY(fragments.size() >= 3);

        const qsizetype before = batches.size();
        pipeline.retransmit(original.frameId, QList<quint16>{0, 2});
        QVERIFY(waitUntil([&]() {
            return batches.size() > before;
        }));
        const VideoPacketBatch retry = batches.constLast();
        QCOMPARE(retry.frameId, original.frameId);
        QCOMPARE(retry.retransmit, true);
        QCOMPARE(retry.packets.size(), qsizetype(2));
        QCOMPARE(retry.packets.at(0).payload, fragments.at(0).payload);
        QCOMPARE(retry.packets.at(1).payload, fragments.at(2).payload);
    }

    void videoPacketizerIsolatesRestartedGenerations()
    {
        VideoSendPipeline pipeline;
        QList<VideoPacketBatch> batches;
        connect(
            &pipeline,
            &VideoSendPipeline::packetsReady,
            [&batches](const VideoPacketBatch &batch) {
                batches.append(batch);
            });
        pipeline.start();
        pipeline.submit(QByteArray(2 * 1024 * 1024, 'a'), false);
        pipeline.stop();

        batches.clear();
        pipeline.start();
        pipeline.submit(QByteArray(4000, 'b'), true);
        QVERIFY(waitUntil([&]() { return !batches.isEmpty(); }));
        pumpEvents(100);

        QVERIFY(!batches.isEmpty());
        for (const VideoPacketBatch &batch : std::as_const(batches)) {
            for (const WirePacket &packet : batch.packets) {
                if (packet.type != PacketType::Data) {
                    continue;
                }
                const auto fragment = unpackVideoFragment(packet.payload);
                QVERIFY(fragment.has_value());
                QVERIFY(!fragment->data.isEmpty());
                QCOMPARE(fragment->data.at(0), 'b');
            }
        }
    }

    void signalingRelayHandshakeControlVideoAndAudio()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        SignalClient agent;
        SignalClient viewer;
        QVERIFY(agent.connectTo(server.signalingPort()));
        QVERIFY(viewer.connectTo(server.signalingPort()));

        agent.send(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("register")},
            {QStringLiteral("code"), QStringLiteral("604173829")},
            {QStringLiteral("name"), QStringLiteral("cpp-host")},
            {QStringLiteral("cands"), QJsonArray{}},
        });
        QVERIFY(agent.take(QStringLiteral("registered")).has_value());
        viewer.send(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("connect")},
            {QStringLiteral("code"), QStringLiteral("604173829")},
            {QStringLiteral("name"), QStringLiteral("cpp-client")},
            {QStringLiteral("cands"), QJsonArray{}},
        });
        const auto agentViewer = agent.take(QStringLiteral("viewer"));
        const auto viewerPeer = viewer.take(QStringLiteral("peer"));
        QVERIFY(agentViewer.has_value());
        QVERIFY(viewerPeer.has_value());

        const QByteArray psk = QByteArrayLiteral("relay-test-secret");
        PeerLink host(true, psk);
        PeerLink client(false, psk);
        host.setForceRelay(true);
        client.setForceRelay(true);
        QString error;
        QVERIFY2(host.start(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        QVERIFY2(client.start(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        host.offerRelay(server.publicHost(), server.relayPort());
        client.offerRelay(server.publicHost(), server.relayPort());
        host.setRelayOutTag(relayTag(*agentViewer));
        client.setRelayOutTag(relayTag(*viewerPeer));

        QList<QJsonObject> hostControls;
        QList<QByteArray> clientFrames;
        QList<QByteArray> hostAudio;
        connect(
            &host,
            &PeerLink::controlMessageReceived,
            [&hostControls](const QJsonObject &message) {
                hostControls.append(message);
            });
        connect(
            &client,
            &PeerLink::videoFrameReceived,
            [&clientFrames](const QByteArray &frame, bool) {
                clientFrames.append(frame);
            });
        connect(
            &host,
            &PeerLink::audioReceived,
            [&hostAudio](const QByteArray &payload) {
                hostAudio.append(payload);
            });

        host.beginHandshake();
        client.beginHandshake();
        QVERIFY(waitUntil(
            [&]() { return host.established() && client.established(); },
            7000));
        QTRY_COMPARE_WITH_TIMEOUT(host.state(), QStringLiteral("relay"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QStringLiteral("relay"), 2000);
        QVERIFY(host.stats().usingRelay);
        QVERIFY(client.stats().usingRelay);

        client.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("relay-control")},
            {QStringLiteral("body"), QString(3000, QChar(u'r'))},
        });
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    hostControls.cbegin(),
                    hostControls.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                            == QStringLiteral("relay-control");
                    });
            }));

        const QByteArray jpeg(18000, 'v');
        host.sendVideo(jpeg, true);
        QVERIFY(waitUntil([&]() { return !clientFrames.isEmpty(); }));
        QCOMPARE(clientFrames.constFirst(), jpeg);

        const QByteArray audio = QByteArrayLiteral("relay-audio-frame");
        client.sendAudio(audio);
        QVERIFY(waitUntil([&]() { return !hostAudio.isEmpty(); }));
        QCOMPARE(hostAudio.constFirst(), audio);
    }

    void peerResetReusesSocketAndReconnects()
    {
        const QByteArray psk = QByteArrayLiteral("reset-test-secret");
        PeerLink host(true, psk);
        PeerLink client(false, psk);
        QString error;
        QVERIFY2(
            host.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            client.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        const quint16 hostPort = host.localPort();
        const quint16 clientPort = client.localPort();

        const auto connectPeers = [&]() {
            host.addCandidates(client.localCandidates());
            client.addCandidates(host.localCandidates());
            host.beginHandshake();
            client.beginHandshake();
            return waitUntil(
                [&]() { return host.established() && client.established(); });
        };

        QVERIFY(connectPeers());
        host.resetPeer();
        client.resetPeer();
        QCOMPARE(host.localPort(), hostPort);
        QCOMPARE(client.localPort(), clientPort);
        QCOMPARE(host.state(), QStringLiteral("idle"));
        QCOMPARE(client.state(), QStringLiteral("idle"));
        QVERIFY(!host.established());
        QVERIFY(!client.established());
        QVERIFY(host.peerCandidates().isEmpty());
        QVERIFY(client.peerCandidates().isEmpty());

        QList<QByteArray> frames;
        connect(
            &client,
            &PeerLink::videoFrameReceived,
            [&frames](const QByteArray &frame, bool) {
                frames.append(frame);
            });
        QVERIFY(connectPeers());
        QTRY_COMPARE_WITH_TIMEOUT(
            host.state(), QStringLiteral("direct"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            client.state(), QStringLiteral("direct"), 3000);

        const QByteArray jpeg(12000, 'r');
        host.sendVideo(jpeg, true);
        QVERIFY(waitUntil([&]() { return !frames.isEmpty(); }));
        QCOMPARE(frames.constFirst(), jpeg);
    }
};

QTEST_GUILESS_MAIN(PeerLinkTest)

#include "peer_link_test.moc"
