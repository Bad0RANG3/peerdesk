#include "app/audio_output.h"
#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "core/util.h"
#include "server/server.h"
#include "session/audio_codec.h"
#include "session/media_sources.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QThread>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>

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

QString serverAddress(const SignalingServer &server)
{
    return QStringLiteral("127.0.0.1:%1").arg(server.signalingPort());
}

void configureHostPreferences(
    const std::shared_ptr<Preferences> &preferences,
    const QString &server,
    const QString &code,
    const QString &token = {},
    const QString &receiveDirectory = {})
{
    HostPreferences value = preferences->host();
    value.server = server;
    value.token = token;
    value.code = code;
    value.video.fps = 20;
    value.video.maxWidth = 640;
    value.video.quality = 50;
    value.video.monitor = 0;
    value.video.skipStatic = false;
    value.video.clipboard = true;
    value.video.audio = true;
    value.allowInput = true;
    value.forceRelay = false;
    value.receiveDirectory = receiveDirectory;
    preferences->setHost(value);
}

void configureViewerPreferences(
    const std::shared_ptr<Preferences> &preferences,
    const QString &server,
    const QString &token = {},
    const QString &receiveDirectory = {})
{
    ViewerPreferences value = preferences->viewer();
    value.server = server;
    value.token = token;
    value.code.clear();
    value.qualityId = QStringLiteral("fluent");
    value.audio = true;
    value.receiveDirectory = receiveDirectory;
    preferences->setViewer(value);
}

class FakeInputSink final : public IInputSink {
public:
    [[nodiscard]] QRect virtualDesktop() const override
    {
        return QRect(0, 0, 2304, 768);
    }

    void moveAbsolute(double x, double y) override
    {
        absoluteMoves.append(QPointF(x, y));
    }

    void moveRelative(int dx, int dy) override
    {
        relativeMoves.append(QPoint(dx, dy));
    }

    void mouseButton(const QString &button, bool down) override
    {
        buttons.append(qMakePair(button, down));
    }

    void click(const QString &button) override
    {
        clicks.append(button);
    }

    void wheel(int delta, bool horizontal) override
    {
        wheels.append(qMakePair(delta, horizontal));
    }

    void key(const QString &code, bool down) override
    {
        keys.append(qMakePair(code, down));
    }

    void typeText(const QString &text) override
    {
        typed.append(text);
    }

    void ctrlAltDelete() override
    {
        ++cadCount;
    }

    QList<QPointF> absoluteMoves;
    QList<QPoint> relativeMoves;
    QList<QPair<QString, bool>> buttons;
    QList<QString> clicks;
    QList<QPair<int, bool>> wheels;
    QList<QPair<QString, bool>> keys;
    QString typed;
    int cadCount = 0;
};

class FakeClipboard final : public IClipboard {
public:
    [[nodiscard]] QString text() const override { return value; }

    bool setText(const QString &text) override
    {
        value = text;
        ++writes;
        return true;
    }

    QString value;
    int writes = 0;
};

class FakeAudioPlayback final : public IAudioPlayback {
public:
    void setEnabled(bool enabled) override
    {
        if (enabled_ != enabled) {
            enabled_ = enabled;
            ++enabledChanges;
        }
    }

    [[nodiscard]] bool enabled() const override { return enabled_; }

    void pushPcm16(const QByteArray &pcm) override
    {
        ++pushes;
        bytes += pcm.size();
        lastFrame = pcm;
    }

    void reset() override
    {
        ++resets;
        lastFrame.clear();
    }

    [[nodiscard]] QString errorString() const override { return {}; }

    int pushes = 0;
    int bytes = 0;
    int resets = 0;
    int enabledChanges = 0;
    QByteArray lastFrame;

private:
    bool enabled_ = true;
};

bool hasEvent(
    const QList<QJsonObject> &events,
    const QString &type,
    const std::function<bool(const QJsonObject &)> &predicate = {})
{
    return std::any_of(
        events.cbegin(),
        events.cend(),
        [&type, &predicate](const QJsonObject &event) {
            return event.value(QStringLiteral("t")).toString() == type
                && (!predicate || predicate(event));
        });
}

} // namespace

class AppControllerTest : public QObject {
    Q_OBJECT

private slots:
    void controllersStreamEndToEnd()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));
        QVERIFY(server.isListening());

        const QString address = serverAddress(server);
        const QString code = QStringLiteral("135792468");
        QTemporaryDir hostConfigDirectory;
        QTemporaryDir viewerConfigDirectory;
        QTemporaryDir hostReceive;
        QTemporaryDir viewerReceive;
        QVERIFY(hostConfigDirectory.isValid());
        QVERIFY(viewerConfigDirectory.isValid());
        QVERIFY(hostReceive.isValid());
        QVERIFY(viewerReceive.isValid());

        HostController host;
        auto hostPreferences = std::make_shared<Preferences>(
            hostConfigDirectory.filePath(QStringLiteral("host.ini")));
        configureHostPreferences(
            hostPreferences,
            address,
            code,
            {},
            hostReceive.path());
        host.setPreferences(hostPreferences);
        host.setBindAddress(QStringLiteral("127.0.0.1"));
        host.setDeviceId(QStringLiteral("controller-host"));

        auto screen =
            std::make_shared<SyntheticScreenSource>(1280, 720, true);
        auto audioSource = std::make_shared<ToneAudioSource>(523.25);
        auto input = std::make_shared<FakeInputSink>();
        auto clipboard = std::make_shared<FakeClipboard>();
        clipboard->value = QStringLiteral("host initial clipboard");
        host.setScreenSource(screen);
        host.setAudioSource(audioSource);
        host.setInputSink(input);
        host.setClipboard(clipboard);

        ViewerController viewer;
        auto viewerPreferences = std::make_shared<Preferences>(
            viewerConfigDirectory.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(
            viewerPreferences,
            address,
            {},
            viewerReceive.path());
        viewer.setPreferences(viewerPreferences);
        viewer.setBindAddress(QStringLiteral("127.0.0.1"));
        auto playback = std::make_shared<FakeAudioPlayback>();
        viewer.setAudioPlayback(playback);

        QList<QImage> frames;
        QList<QJsonObject> viewerEvents;
        QList<QJsonObject> viewerStats;
        QList<QJsonObject> hostEvents;
        QList<QString> viewerErrors;
        QList<QString> hostErrors;
        auto hostTrace = std::make_shared<QStringList>();
        auto viewerTrace = std::make_shared<QStringList>();
        connect(
            &viewer,
            &ViewerController::frameDecoded,
            [&frames](const QImage &image, bool) {
                frames.append(image);
            });
        connect(
            &viewer,
            &ViewerController::eventReceived,
            [&viewerEvents](const QJsonObject &event) {
                viewerEvents.append(event);
            });
        connect(
            &viewer,
            &ViewerController::statisticsChanged,
            [&viewerStats](const QJsonObject &stats) {
                viewerStats.append(stats);
            });
        connect(
            &host,
            &HostController::eventReceived,
            [&hostEvents](const QJsonObject &event) {
                hostEvents.append(event);
            });
        connect(
            &viewer,
            &ViewerController::errorOccurred,
            [&viewerErrors](const QString &message) {
                viewerErrors.append(message);
            });
        connect(
            &host,
            &HostController::errorOccurred,
            [&hostErrors](const QString &message) {
                hostErrors.append(message);
            });
        connect(
            &host,
            &HostController::logMessage,
            [hostTrace](const QString &message) {
                hostTrace->append(message);
            });
        connect(
            &host,
            &HostController::stateChanged,
            [hostTrace](HostController::State state) {
                hostTrace->append(
                    QStringLiteral("controller state=%1").arg(
                        static_cast<int>(state)));
            });
        connect(
            &host,
            &HostController::peerChanged,
            [hostTrace](const QString &name, bool connected) {
                hostTrace->append(
                    QStringLiteral("peer=%1 connected=%2")
                        .arg(name)
                        .arg(connected));
            });
        connect(
            &host,
            &HostController::linkStateChanged,
            [hostTrace](const QString &state) {
                hostTrace->append(
                    QStringLiteral("link state=%1").arg(state));
            });
        connect(
            &viewer,
            &ViewerController::logMessage,
            [viewerTrace](const QString &message) {
                viewerTrace->append(message);
            });
        connect(
            &viewer,
            &ViewerController::stateChanged,
            [viewerTrace](ViewerController::State state) {
                viewerTrace->append(
                    QStringLiteral("controller state=%1").arg(
                        static_cast<int>(state)));
            });
        connect(
            &viewer,
            &ViewerController::peerChanged,
            [viewerTrace](const QString &name, bool connected) {
                viewerTrace->append(
                    QStringLiteral("peer=%1 connected=%2")
                        .arg(name)
                        .arg(connected));
            });
        connect(
            &viewer,
            &ViewerController::linkStateChanged,
            [viewerTrace](const QString &state) {
                viewerTrace->append(
                    QStringLiteral("link state=%1").arg(state));
            });

        QString error;
        QVERIFY2(host.start(&error), qPrintable(error));
        QVERIFY(host.active());
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));
        QCOMPARE(host.code(), code);

        QVERIFY2(
            viewer.connectTo(code, address, {}, QStringLiteral("tester"), &error),
            qPrintable(error));
        QVERIFY(viewer.active());
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && viewer.state() == ViewerController::State::Streaming
                    && host.streaming() && viewer.streaming();
            },
            10000));
        QCOMPARE(viewer.code(), code);
        QVERIFY(!viewer.peerName().isEmpty());

        QVERIFY(waitUntil([&]() { return !frames.isEmpty(); }, 8000));
        QVERIFY(!frames.constFirst().isNull());
        QVERIFY(waitUntil([&]() { return playback->pushes > 0; }, 6000));
        QCOMPARE(playback->lastFrame.size(), qsizetype(kAudioPcmBytes));
        QVERIFY(waitUntil(
            [&]() {
                return !viewer.info().isEmpty()
                    && viewer.info()
                           .value(QStringLiteral("monitors"))
                           .toArray()
                           .size()
                        == 2;
            },
            5000));
        QVERIFY(waitUntil([&]() { return !viewerStats.isEmpty(); }, 6000));
        QCOMPARE(
            viewerStats.constLast().value(QStringLiteral("t")).toString(),
            QStringLiteral("stats"));
        QVERIFY(waitUntil(
            [&]() {
                return !host.statistics().isEmpty()
                    && host.statistics()
                           .value(QStringLiteral("t"))
                           .toString()
                        == QStringLiteral("stats");
            },
            6000));
        QCOMPARE(host.monitors().size(), qsizetype(2));
        QVERIFY(!host.viewerInfo().isEmpty());

        viewer.setMaxWidth(480);
        QVERIFY(waitUntil(
            [&]() { return host.videoSettings().maxWidth == 480; },
            4000));
        viewer.setFps(24);
        QVERIFY(waitUntil(
            [&]() { return host.videoSettings().fps == 24; },
            4000));
        viewer.setQuality(57);
        QVERIFY(waitUntil(
            [&]() { return host.videoSettings().quality == 57; },
            4000));
        viewer.selectMonitor(1);
        QCOMPARE(viewer.monitor(), 1);
        QVERIFY(waitUntil(
            [&]() { return host.videoSettings().monitor == 1; },
            4000));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin(),
                    frames.cend(),
                    [](const QImage &frame) {
                        return frame.width() <= 480
                            && frame.width() > 0
                            && frame.height() > 0;
                    });
            },
            5000));

        const qsizetype movesBefore = input->absoluteMoves.size();
        viewer.sendMouseMove(0.25, 0.75);
        QVERIFY(waitUntil(
            [&]() { return input->absoluteMoves.size() > movesBefore; },
            3000));
        viewer.sendMouseButton(
            QStringLiteral("down"),
            0.4,
            0.6,
            QStringLiteral("left"));
        viewer.sendMouseButton(
            QStringLiteral("up"),
            0.4,
            0.6,
            QStringLiteral("left"));
        QVERIFY(waitUntil(
            [&]() { return input->buttons.size() >= 2; },
            3000));
        QCOMPARE(input->buttons.at(input->buttons.size() - 2),
                 qMakePair(QStringLiteral("left"), true));
        QCOMPARE(input->buttons.constLast(),
                 qMakePair(QStringLiteral("left"), false));
        viewer.sendMouseWheel(120, false);
        QVERIFY(waitUntil(
            [&]() { return !input->wheels.isEmpty(); },
            3000));
        QCOMPARE(input->wheels.constLast(), qMakePair(120, false));
        viewer.sendMouseRelative(7, -4);
        QVERIFY(waitUntil(
            [&]() { return !input->relativeMoves.isEmpty(); },
            3000));
        QCOMPARE(input->relativeMoves.constLast(), QPoint(7, -4));

        QVERIFY(viewer.sendKey(QStringLiteral("KeyA"), true));
        QVERIFY(viewer.sendKey(QStringLiteral("KeyA"), false));
        QVERIFY(waitUntil(
            [&]() {
                return std::find(
                           input->keys.cbegin(),
                           input->keys.cend(),
                           qMakePair(QStringLiteral("KeyA"), true))
                        != input->keys.cend()
                    && std::find(
                           input->keys.cbegin(),
                           input->keys.cend(),
                           qMakePair(QStringLiteral("KeyA"), false))
                        != input->keys.cend();
            },
            3000));
        viewer.sendText(QStringLiteral("PeerDesk controller 输入"));
        QVERIFY(waitUntil(
            [&]() {
                return input->typed
                    == QStringLiteral("PeerDesk controller 输入");
            },
            3000));
        viewer.sendCtrlAltDelete();
        QVERIFY(waitUntil([&]() { return input->cadCount == 1; }, 3000));

        const QString viewerClipboard =
            QStringLiteral("viewer clipboard 剪贴板");
        QVERIFY(viewer.sendClipboard(viewerClipboard));
        QVERIFY(waitUntil(
            [&]() { return clipboard->value == viewerClipboard; },
            3000));
        clipboard->value = QStringLiteral("host clipboard back");
        QVERIFY(waitUntil(
            [&]() {
                return hasEvent(
                    viewerEvents,
                    QStringLiteral("clip"),
                    [](const QJsonObject &event) {
                        return event.value(QStringLiteral("s")).toString()
                            == QStringLiteral("host clipboard back");
                    });
            },
            4000));

        const int pushesBeforeMute = playback->pushes;
        viewer.setAudioEnabled(false);
        QVERIFY(waitUntil(
            [&]() { return !host.audioEnabled(); },
            4000));
        QVERIFY(waitUntil(
            [&]() { return !playback->enabled(); },
            2000));
        pumpEvents(300);
        const int pushesAfterMute = playback->pushes;
        pumpEvents(800);
        QCOMPARE(playback->pushes, pushesAfterMute);
        QVERIFY(pushesAfterMute >= pushesBeforeMute);

        viewer.setAudioEnabled(true);
        QVERIFY(waitUntil(
            [&]() { return host.audioEnabled(); },
            4000));
        QVERIFY(waitUntil(
            [&]() { return playback->enabled(); },
            2000));
        QVERIFY(waitUntil(
            [&]() { return playback->pushes > pushesAfterMute; },
            5000));
        QVERIFY(hostErrors.isEmpty());
        QVERIFY(viewerErrors.isEmpty());

        viewer.hangUp();
        QCOMPARE(viewer.state(), ViewerController::State::Idle);
        QVERIFY(!viewer.active());
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));
        QVERIFY(host.active());
        QVERIFY(host.code() == code);

        ViewerController secondViewer;
        QTemporaryDir secondViewerConfig;
        QTemporaryDir secondViewerReceive;
        QVERIFY(secondViewerConfig.isValid());
        QVERIFY(secondViewerReceive.isValid());
        auto secondPreferences = std::make_shared<Preferences>(
            secondViewerConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(
            secondPreferences,
            address,
            {},
            secondViewerReceive.path());
        secondViewer.setPreferences(secondPreferences);
        secondViewer.setBindAddress(QStringLiteral("127.0.0.1"));
        auto secondPlayback = std::make_shared<FakeAudioPlayback>();
        secondViewer.setAudioPlayback(secondPlayback);
        auto secondViewerTrace = std::make_shared<QStringList>();
        connect(
            &secondViewer,
            &ViewerController::logMessage,
            [secondViewerTrace](const QString &message) {
                secondViewerTrace->append(message);
            });
        connect(
            &secondViewer,
            &ViewerController::stateChanged,
            [secondViewerTrace](ViewerController::State state) {
                secondViewerTrace->append(
                    QStringLiteral("controller state=%1").arg(
                        static_cast<int>(state)));
            });
        connect(
            &secondViewer,
            &ViewerController::peerChanged,
            [secondViewerTrace](const QString &name, bool connected) {
                secondViewerTrace->append(
                    QStringLiteral("peer=%1 connected=%2")
                        .arg(name)
                        .arg(connected));
            });
        connect(
            &secondViewer,
            &ViewerController::linkStateChanged,
            [secondViewerTrace](const QString &state) {
                secondViewerTrace->append(
                    QStringLiteral("link state=%1").arg(state));
            });
        QVERIFY2(
            secondViewer.connectTo(
                code,
                address,
                {},
                QStringLiteral("second"),
                &error),
            qPrintable(error));
        const bool secondConnected = waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && secondViewer.state()
                        == ViewerController::State::Streaming
                    && secondViewer.streaming();
            },
            10000);
        const QString secondFailure =
            QStringLiteral(
                "host state=%1 error=%2\nviewer state=%3 error=%4 "
                "active=%5\nhost trace:\n%6\nviewer trace:\n%7")
                .arg(static_cast<int>(host.state()))
                .arg(host.lastError())
                .arg(static_cast<int>(secondViewer.state()))
                .arg(secondViewer.lastError())
                .arg(secondViewer.active())
                .arg(hostTrace
                         ->mid(std::max<qsizetype>(
                             0, hostTrace->size() - 40))
                         .join('\n'))
                .arg(secondViewerTrace
                         ->mid(std::max<qsizetype>(
                             0, secondViewerTrace->size() - 40))
                         .join('\n'));
        QVERIFY2(secondConnected, qPrintable(secondFailure));
        secondViewer.hangUp();
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));

        const QString oldCode = host.code();
        QVERIFY2(host.regenerateCode(&error), qPrintable(error));
        QVERIFY(host.code() != oldCode);
        QVERIFY(codeIsValid(host.code()));
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            6000));

        ViewerController staleViewer;
        QTemporaryDir staleConfig;
        auto stalePreferences = std::make_shared<Preferences>(
            staleConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(stalePreferences, address);
        staleViewer.setPreferences(stalePreferences);
        staleViewer.setBindAddress(QStringLiteral("127.0.0.1"));
        auto stalePlayback = std::make_shared<FakeAudioPlayback>();
        staleViewer.setAudioPlayback(stalePlayback);
        staleViewer.setReconnectTimeoutMs(1500);
        QSignalSpy staleEnded(
            &staleViewer, &ViewerController::sessionEnded);
        QVERIFY(staleViewer.connectTo(oldCode, address, {}, {}, &error));
        QVERIFY(waitUntil(
            [&]() {
                return staleViewer.state() == ViewerController::State::Error
                    && !staleViewer.active()
                    && !staleViewer.lastError().isEmpty();
            },
            6000));
        QCOMPARE(staleEnded.count(), 0);

        QVERIFY2(
            staleViewer.connectTo(
                host.code(),
                address,
                {},
                QStringLiteral("fresh"),
                &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && staleViewer.state()
                        == ViewerController::State::Streaming
                    && staleViewer.streaming();
            },
            10000));
        staleViewer.hangUp();
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));
        QVERIFY(host.active());
    }

    void viewerRecoversAfterHostReconnect()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        options.connectMax = 1000;
        options.connectWindow = 60.0;
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        const QString address = serverAddress(server);
        const QString code = QStringLiteral("314159265");
        QTemporaryDir hostConfig;
        QTemporaryDir viewerConfig;
        QVERIFY(hostConfig.isValid());
        QVERIFY(viewerConfig.isValid());

        HostController host;
        auto hostPreferences = std::make_shared<Preferences>(
            hostConfig.filePath(QStringLiteral("host.ini")));
        configureHostPreferences(
            hostPreferences, address, code);
        host.setPreferences(hostPreferences);
        host.setBindAddress(QStringLiteral("127.0.0.1"));
        host.setDeviceId(QStringLiteral("recovery-host"));
        host.setScreenSource(
            std::make_shared<SyntheticScreenSource>(640, 360, false));
        host.setAudioSource(std::make_shared<ToneAudioSource>());
        host.setInputSink(std::make_shared<FakeInputSink>());
        host.setClipboard(std::make_shared<FakeClipboard>());

        ViewerController viewer;
        auto viewerPreferences = std::make_shared<Preferences>(
            viewerConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(viewerPreferences, address);
        viewer.setPreferences(viewerPreferences);
        viewer.setBindAddress(QStringLiteral("127.0.0.1"));
        viewer.setAudioPlayback(std::make_shared<FakeAudioPlayback>());
        viewer.setReconnectTimeoutMs(6000);

        QSignalSpy releaseRequests(
            &viewer, &ViewerController::inputReleaseRequested);
        QSignalSpy sessionEnds(
            &viewer, &ViewerController::sessionEnded);

        QString error;
        QVERIFY2(host.start(&error), qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));
        QVERIFY2(
            viewer.connectTo(
                code,
                address,
                {},
                QStringLiteral("recovery-viewer"),
                &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && viewer.state() == ViewerController::State::Streaming
                    && viewer.streaming();
            },
            10000));

        host.stop();
        QVERIFY(waitUntil(
            [&]() {
                return viewer.state()
                    == ViewerController::State::Reconnecting;
            },
            5000));
        QVERIFY(viewer.active());
        QCOMPARE(releaseRequests.count(), 1);
        QCOMPARE(sessionEnds.count(), 0);

        QVERIFY2(host.start(&error), qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && viewer.state() == ViewerController::State::Streaming
                    && viewer.streaming();
            },
            10000));
        QVERIFY(viewer.active());
        QCOMPARE(releaseRequests.count(), 1);
        QCOMPARE(sessionEnds.count(), 0);

        viewer.hangUp();
        QCOMPARE(viewer.state(), ViewerController::State::Idle);
        host.stop();
    }

    void viewerRecoveryFailureAndHangUpStaySilent()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        options.connectMax = 1000;
        options.connectWindow = 60.0;
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));

        const QString address = serverAddress(server);
        const QString code = QStringLiteral("271828182");
        QTemporaryDir hostConfig;
        QTemporaryDir viewerConfig;
        QTemporaryDir quietViewerConfig;
        QVERIFY(hostConfig.isValid());
        QVERIFY(viewerConfig.isValid());
        QVERIFY(quietViewerConfig.isValid());

        HostController host;
        auto hostPreferences = std::make_shared<Preferences>(
            hostConfig.filePath(QStringLiteral("host.ini")));
        configureHostPreferences(
            hostPreferences, address, code);
        host.setPreferences(hostPreferences);
        host.setBindAddress(QStringLiteral("127.0.0.1"));
        host.setDeviceId(QStringLiteral("timeout-host"));
        host.setScreenSource(
            std::make_shared<SyntheticScreenSource>(640, 360, false));
        host.setAudioSource(std::make_shared<ToneAudioSource>());
        host.setInputSink(std::make_shared<FakeInputSink>());
        host.setClipboard(std::make_shared<FakeClipboard>());

        ViewerController viewer;
        auto viewerPreferences = std::make_shared<Preferences>(
            viewerConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(viewerPreferences, address);
        viewer.setPreferences(viewerPreferences);
        viewer.setBindAddress(QStringLiteral("127.0.0.1"));
        viewer.setAudioPlayback(std::make_shared<FakeAudioPlayback>());
        viewer.setReconnectTimeoutMs(1400);

        QSignalSpy releaseRequests(
            &viewer, &ViewerController::inputReleaseRequested);
        QSignalSpy sessionEnds(
            &viewer, &ViewerController::sessionEnded);

        QString error;
        QVERIFY2(host.start(&error), qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));
        QVERIFY2(
            viewer.connectTo(
                code,
                address,
                {},
                QStringLiteral("timeout-viewer"),
                &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && viewer.state() == ViewerController::State::Streaming
                    && viewer.streaming();
            },
            10000));

        host.stop();
        QVERIFY(waitUntil(
            [&]() {
                return viewer.state()
                    == ViewerController::State::Reconnecting;
            },
            5000));
        QCOMPARE(releaseRequests.count(), 1);
        QCOMPARE(sessionEnds.count(), 0);
        QVERIFY(waitUntil(
            [&]() {
                return viewer.state() == ViewerController::State::Error
                    && !viewer.active();
            },
            5000));
        QCOMPARE(releaseRequests.count(), 1);
        QCOMPARE(sessionEnds.count(), 1);

        QVERIFY2(host.start(&error), qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));

        ViewerController quietViewer;
        auto quietPreferences = std::make_shared<Preferences>(
            quietViewerConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(quietPreferences, address);
        quietViewer.setPreferences(quietPreferences);
        quietViewer.setBindAddress(QStringLiteral("127.0.0.1"));
        quietViewer.setAudioPlayback(std::make_shared<FakeAudioPlayback>());
        quietViewer.setReconnectTimeoutMs(1400);
        QSignalSpy quietReleaseRequests(
            &quietViewer, &ViewerController::inputReleaseRequested);
        QSignalSpy quietSessionEnds(
            &quietViewer, &ViewerController::sessionEnded);
        QVERIFY2(
            quietViewer.connectTo(
                code,
                address,
                {},
                QStringLiteral("quiet-viewer"),
                &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return host.state() == HostController::State::Streaming
                    && quietViewer.state()
                        == ViewerController::State::Streaming
                    && quietViewer.streaming();
            },
            10000));

        host.stop();
        QVERIFY(waitUntil(
            [&]() {
                return quietViewer.state()
                    == ViewerController::State::Reconnecting;
            },
            5000));
        QCOMPARE(quietReleaseRequests.count(), 1);
        quietViewer.hangUp();
        QCOMPARE(quietViewer.state(), ViewerController::State::Idle);
        QVERIFY(!quietViewer.active());
        pumpEvents(1800);
        QCOMPARE(quietViewer.state(), ViewerController::State::Idle);
        QCOMPARE(quietReleaseRequests.count(), 1);
        QCOMPARE(quietSessionEnds.count(), 0);
    }

    void invalidParametersAndTokenRecovery()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        options.token = QStringLiteral("test-token");
        SignalingServer server(options);
        QString serverError;
        QVERIFY2(server.start(&serverError), qPrintable(serverError));
        const QString address = serverAddress(server);
        const QString code = QStringLiteral("246813579");

        QTemporaryDir hostConfig;
        QTemporaryDir viewerConfig;
        QVERIFY(hostConfig.isValid());
        QVERIFY(viewerConfig.isValid());

        HostController host;
        auto hostPreferences = std::make_shared<Preferences>(
            hostConfig.filePath(QStringLiteral("host.ini")));
        configureHostPreferences(
            hostPreferences,
            address,
            code,
            QStringLiteral("test-token"));
        host.setPreferences(hostPreferences);
        host.setBindAddress(QStringLiteral("127.0.0.1"));
        host.setScreenSource(
            std::make_shared<SyntheticScreenSource>(640, 360, false));
        host.setAudioSource(std::make_shared<ToneAudioSource>());
        host.setInputSink(std::make_shared<FakeInputSink>());
        host.setClipboard(std::make_shared<FakeClipboard>());
        QVERIFY(host.start());
        QVERIFY(waitUntil(
            [&]() { return host.state() == HostController::State::Waiting; },
            5000));

        ViewerController viewer;
        auto viewerPreferences = std::make_shared<Preferences>(
            viewerConfig.filePath(QStringLiteral("viewer.ini")));
        configureViewerPreferences(viewerPreferences, address);
        viewer.setPreferences(viewerPreferences);
        viewer.setBindAddress(QStringLiteral("127.0.0.1"));
        viewer.setAudioPlayback(std::make_shared<FakeAudioPlayback>());

        QList<QString> errors;
        connect(
            &viewer,
            &ViewerController::errorOccurred,
            [&errors](const QString &message) {
                errors.append(message);
            });

        QString error;
        QVERIFY(!viewer.connectTo(
            QStringLiteral("12"),
            address,
            QStringLiteral("test-token"),
            {},
            &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(viewer.lastError(), error);
        QCOMPARE(viewer.state(), ViewerController::State::Error);
        QVERIFY(!viewer.active());
        QVERIFY(!errors.isEmpty());

        errors.clear();
        error.clear();
        QVERIFY(!viewer.connectTo(
            code,
            QStringLiteral("127.0.0.1:not-a-port"),
            QStringLiteral("test-token"),
            {},
            &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(viewer.state(), ViewerController::State::Error);
        QVERIFY(!viewer.active());
        QVERIFY(!errors.isEmpty());

        errors.clear();
        error.clear();
        QVERIFY(viewer.connectTo(
            code,
            address,
            QStringLiteral("wrong-token"),
            QStringLiteral("recovering"),
            &error));
        QVERIFY(error.isEmpty());
        QVERIFY(viewer.active());
        QVERIFY(waitUntil(
            [&]() {
                return viewer.state() == ViewerController::State::Error
                    && !viewer.active()
                    && !viewer.lastError().isEmpty();
            },
            6000));
        QVERIFY(!errors.isEmpty());

        QVERIFY(viewer.connectTo(
            code,
            address,
            QStringLiteral("test-token"),
            QStringLiteral("recovered"),
            &error));
        QVERIFY(error.isEmpty());
        QVERIFY(waitUntil(
            [&]() {
                return viewer.state() == ViewerController::State::Streaming
                    && host.state() == HostController::State::Streaming
                    && viewer.streaming();
            },
            10000));
        viewer.hangUp();
        QCOMPARE(viewer.state(), ViewerController::State::Idle);
    }

    void preferencesRoundTrip()
    {
        QTemporaryDir hostConfig;
        QTemporaryDir viewerConfig;
        QTemporaryDir hostReceive;
        QTemporaryDir viewerReceive;
        QVERIFY(hostConfig.isValid());
        QVERIFY(viewerConfig.isValid());
        QVERIFY(hostReceive.isValid());
        QVERIFY(viewerReceive.isValid());

        const QString hostIni =
            hostConfig.filePath(QStringLiteral("host.ini"));
        const QString viewerIni =
            viewerConfig.filePath(QStringLiteral("viewer.ini"));
        HostController host;
        auto hostPreferences =
            std::make_shared<Preferences>(hostIni);
        configureHostPreferences(
            hostPreferences,
            QStringLiteral("peerdesk.example:7900"),
            QStringLiteral("112233445"),
            QStringLiteral("secret"),
            hostReceive.path());
        host.setPreferences(hostPreferences);
        host.setServer(QStringLiteral("remote.example:8100"));
        host.setToken(QStringLiteral("updated-secret"));
        VideoSettings hostVideo = host.videoSettings();
        hostVideo.fps = 17;
        hostVideo.maxWidth = 512;
        hostVideo.quality = 44;
        hostVideo.clipboard = false;
        hostVideo.audio = false;
        host.setVideoSettings(hostVideo);
        host.setAllowInput(false);
        host.setForceRelay(true);
        host.setReceiveDirectory(hostReceive.path());

        ViewerController viewer;
        auto viewerPreferences =
            std::make_shared<Preferences>(viewerIni);
        ViewerPreferences initialViewer = viewerPreferences->viewer();
        initialViewer.server = QStringLiteral("viewer.example:7900");
        initialViewer.token = QStringLiteral("viewer-secret");
        initialViewer.code = QStringLiteral("998877665");
        initialViewer.receiveDirectory = viewerReceive.path();
        viewerPreferences->setViewer(initialViewer);
        viewer.setPreferences(viewerPreferences);
        viewer.setQualityPreset(QStringLiteral("hd"));
        viewer.setMaxWidth(1920);
        viewer.setFps(40);
        viewer.setQuality(72);
        viewer.setAudioEnabled(false);
        viewer.setReceiveDirectory(viewerReceive.path());

        QVERIFY(QFileInfo::exists(hostIni));
        QVERIFY(QFileInfo::exists(viewerIni));
        Preferences storedHost(hostIni);
        const HostPreferences hostValues = storedHost.host();
        QCOMPARE(hostValues.server, QStringLiteral("remote.example:8100"));
        QCOMPARE(hostValues.token, QStringLiteral("updated-secret"));
        QCOMPARE(hostValues.code, QStringLiteral("112233445"));
        QCOMPARE(hostValues.video.fps, 17);
        QCOMPARE(hostValues.video.maxWidth, 512);
        QCOMPARE(hostValues.video.quality, 44);
        QVERIFY(!hostValues.video.clipboard);
        QVERIFY(!hostValues.video.audio);
        QVERIFY(!hostValues.allowInput);
        QVERIFY(hostValues.forceRelay);
        QCOMPARE(hostValues.receiveDirectory, hostReceive.path());

        Preferences storedViewer(viewerIni);
        const ViewerPreferences viewerValues = storedViewer.viewer();
        QCOMPARE(
            viewerValues.server,
            QStringLiteral("viewer.example:7900"));
        QCOMPARE(viewerValues.token, QStringLiteral("viewer-secret"));
        QCOMPARE(viewerValues.code, QStringLiteral("998877665"));
        QCOMPARE(viewerValues.qualityId, viewer.qualityId());
        QCOMPARE(viewerValues.qualityId, QStringLiteral("hd"));
        QVERIFY(!viewerValues.audio);
        QCOMPARE(viewerValues.receiveDirectory, viewerReceive.path());

        pd::WindowPreferences windowValues;
        windowValues.geometry =
            QByteArray("peerdesk-window-geometry");
        windowValues.maximized = true;
        windowValues.closeToTray = false;
        storedHost.setWindow(windowValues);
        Preferences reloadedWindow(hostIni);
        const WindowPreferences restoredWindow = reloadedWindow.window();
        QCOMPARE(restoredWindow.geometry, windowValues.geometry);
        QVERIFY(restoredWindow.maximized);
        QVERIFY(!restoredWindow.closeToTray);
    }
};

QTEST_MAIN(AppControllerTest)

#include "app_controller_test.moc"
