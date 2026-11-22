#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "session/media_sources.h"
#include "ui/app_window.h"
#include "ui/code_input.h"
#include "ui/connection_page.h"
#include "ui/host_panel.h"
#include "ui/settings_dialog.h"
#include "ui/ui_theme.h"
#include "ui/viewer_page.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

std::shared_ptr<pd::Preferences> testPreferences()
{
    return std::make_shared<pd::Preferences>(
        pd::Preferences::Mode::Ephemeral);
}

} // namespace

class UiTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void connectionCodeFormatting();
    void invalidInputIsRejected();
    void validConnectionIsDispatched();
    void connectedStateSwitchesToDisconnect();
    void navigationSwitchesPagesWithoutAutoStart();
    void hostPanelRendersWithSyntheticSource();
    void viewerCanvasScalesAndNormalizesCoordinates();
    void viewerCanvasBatchesPointerInput();
    void viewerCanvasCapturesAndReleasesKeyboard();
    void viewerCanvasIgnoresInputWhenDisabled();
    void viewerDemoRendersNativeSession();
    void viewerEndedClearsFrameAndShowsBack();
    void appWindowKeepsViewerDuringLiveSession();
    void windowGeometryRoundTrip();
    void windowGeometryFallsBackWhenOffscreen();
    void closeWithoutTrayQuits();
    void closeToTrayHidesAndExplicitQuitWorks();
    void settingsUpdateControllersAndConnectionPage();
};

void UiTest::initTestCase()
{
    QApplication::setQuitOnLastWindowClosed(false);
    pd::ui::applyTheme(*qApp);
}

void UiTest::connectionCodeFormatting()
{
    pd::ui::ConnectionPage page;
    page.setCode(QStringLiteral("123456789"));

    QCOMPARE(page.codeInput()->text(), QStringLiteral("123 456 789"));
    QCOMPARE(page.code(), QStringLiteral("123456789"));

    page.setCode(QStringLiteral("12a34-56 789"));
    QCOMPARE(page.codeInput()->text(), QStringLiteral("123 456 789"));
    QCOMPARE(page.code(), QStringLiteral("123456789"));

    page.codeInput()->clear();
    QTest::keyClicks(page.codeInput(), QStringLiteral("987654321"));
    QCOMPARE(page.codeInput()->text(), QStringLiteral("987 654 321"));
    QCOMPARE(page.code(), QStringLiteral("987654321"));
}

void UiTest::invalidInputIsRejected()
{
    pd::ui::ConnectionPage page;
    page.setCode(QStringLiteral("12345"));
    page.setServer(QStringLiteral("127.0.0.1:7900"));
    QSignalSpy requests(&page, &pd::ui::ConnectionPage::connectRequested);

    page.requestConnect();

    QCOMPARE(requests.count(), 0);
    QVERIFY(!page.busy());
    auto *error = page.findChild<QLabel *>(QStringLiteral("connectionError"));
    QVERIFY(error);
    QVERIFY(error->text().contains(QStringLiteral("9 位")));

    page.setCode(QStringLiteral("123456789"));
    page.setServer(QStringLiteral("127.0.0.1:invalid"));
    page.requestConnect();
    QCOMPARE(requests.count(), 0);
    QVERIFY(!page.busy());
    QVERIFY(error->text().contains(QStringLiteral("服务器")));
}

void UiTest::validConnectionIsDispatched()
{
    pd::ui::ConnectionPage page;
    page.setCode(QStringLiteral("123456789"));
    page.setServer(QStringLiteral("127.0.0.1:7900"));
    page.setToken(QStringLiteral("secret"));
    QSignalSpy requests(&page, &pd::ui::ConnectionPage::connectRequested);

    page.requestConnect();

    QCOMPARE(requests.count(), 1);
    const QList<QVariant> arguments = requests.takeFirst();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("123456789"));
    QCOMPARE(arguments.at(1).toString(), QStringLiteral("127.0.0.1:7900"));
    QCOMPARE(arguments.at(2).toString(), QStringLiteral("secret"));
    QVERIFY(page.busy());
    QVERIFY(!page.connectButton()->isEnabled());
}

void UiTest::connectedStateSwitchesToDisconnect()
{
    pd::ui::ConnectionPage page;
    page.setCode(QStringLiteral("123456789"));
    page.setConnected(true, QStringLiteral("OFFICE-PC"));

    QVERIFY(page.connected());
    QCOMPARE(
        page.connectButton()->text(),
        QStringLiteral("断开远程会话"));
    QVERIFY(!page.codeInput()->isEnabled());

    QSignalSpy disconnects(
        &page,
        &pd::ui::ConnectionPage::disconnectRequested);
    page.requestConnect();
    QCOMPARE(disconnects.count(), 1);

    page.setConnected(false);
    QVERIFY(!page.connected());
    QCOMPARE(
        page.connectButton()->text(),
        QStringLiteral("连接远程设备"));
    QVERIFY(page.codeInput()->isEnabled());
}

void UiTest::navigationSwitchesPagesWithoutAutoStart()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);

    window.showRemotePage();
    QCOMPARE(window.currentPage(), window.connectionPage());
    QVERIFY(window.remoteNavButton()->isChecked());
    QVERIFY(!window.hostNavButton()->isChecked());

    window.showHostPage();
    QCOMPARE(window.currentPage(), window.hostPanel());
    QVERIFY(window.hostNavButton()->isChecked());
    QVERIFY(!window.remoteNavButton()->isChecked());
    QVERIFY(!host.active());
    QCOMPARE(host.state(), pd::HostController::State::Idle);
}

void UiTest::hostPanelRendersWithSyntheticSource()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    host.setPreferences(preferences);
    host.setScreenSource(
        std::make_shared<pd::SyntheticScreenSource>(1280, 720, true));

    pd::ui::HostPanel panel(&host, false);
    panel.resize(800, 900);
    panel.show();
    QTest::qWait(20);

    const QRegularExpression codePattern(
        QStringLiteral("^\\d{3} \\d{3} \\d{3}$"));
    QVERIFY2(
        codePattern.match(panel.codeLabel()->text()).hasMatch(),
        qPrintable(panel.codeLabel()->text()));
    QVERIFY(panel.monitorCombo()->count() >= 1);
    QVERIFY(!host.active());

    const QPixmap rendered = panel.grab();
    QVERIFY(!rendered.isNull());
    QVERIFY(rendered.width() >= 700);
    QVERIFY(rendered.height() >= 800);
}

void UiTest::viewerCanvasScalesAndNormalizesCoordinates()
{
    pd::ui::ViewerCanvas canvas;
    canvas.resize(1000, 600);

    QImage frame(800, 400, QImage::Format_RGB32);
    frame.fill(Qt::darkCyan);
    canvas.setFrame(frame);

    const QRectF target = canvas.frameRect();
    QCOMPARE(target, QRectF(0.0, 50.0, 1000.0, 500.0));
    QCOMPARE(canvas.normalizePoint(QPointF(500.0, 300.0)), QPointF(0.5, 0.5));
    QCOMPARE(canvas.normalizePoint(QPointF(-50.0, -50.0)), QPointF(0.0, 0.0));
    QCOMPARE(canvas.normalizePoint(QPointF(1200.0, 800.0)), QPointF(1.0, 1.0));

    canvas.clearFrame();
    QVERIFY(!canvas.hasFrame());
    QCOMPARE(canvas.normalizePoint(QPointF(0.0, 0.0)), QPointF(0.5, 0.5));
}

void UiTest::viewerCanvasBatchesPointerInput()
{
    pd::ui::ViewerCanvas canvas;
    canvas.resize(1000, 600);
    QImage frame(800, 400, QImage::Format_RGB32);
    frame.fill(Qt::black);
    canvas.setFrame(frame);
    canvas.show();
    canvas.setInputEnabled(true);

    QSignalSpy moves(&canvas, &pd::ui::ViewerCanvas::mouseMoveRequested);
    QSignalSpy wheel(&canvas, &pd::ui::ViewerCanvas::wheelRequested);

    QMouseEvent firstMove(
        QEvent::MouseMove,
        QPointF(100.0, 100.0),
        QPointF(100.0, 100.0),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QMouseEvent secondMove(
        QEvent::MouseMove,
        QPointF(400.0, 300.0),
        QPointF(400.0, 300.0),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &firstMove);
    QApplication::sendEvent(&canvas, &secondMove);
    QCOMPARE(moves.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(moves.count(), 1, 1000);
    const QList<QVariant> move = moves.takeFirst();
    QVERIFY(std::abs(move.at(0).toDouble() - 0.4) < 1e-9);
    QVERIFY(std::abs(move.at(1).toDouble() - 0.5) < 1e-9);

    QWheelEvent vertical(
        QPointF(500.0, 300.0),
        QPointF(500.0, 300.0),
        QPoint(),
        QPoint(0, 240),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false);
    QApplication::sendEvent(&canvas, &vertical);
    QTRY_COMPARE_WITH_TIMEOUT(wheel.count(), 2, 1000);
    for (int i = 0; i < wheel.count(); ++i) {
        const QList<QVariant> delta = wheel.at(i);
        QCOMPARE(delta.at(0).toInt(), 120);
        QCOMPARE(delta.at(1).toBool(), false);
    }

    wheel.clear();
    QWheelEvent horizontal(
        QPointF(500.0, 300.0),
        QPointF(500.0, 300.0),
        QPoint(),
        QPoint(-240, 0),
        Qt::NoButton,
        Qt::ShiftModifier,
        Qt::ScrollUpdate,
        false);
    QApplication::sendEvent(&canvas, &horizontal);
    QTRY_COMPARE_WITH_TIMEOUT(wheel.count(), 2, 1000);
    for (int i = 0; i < wheel.count(); ++i) {
        const QList<QVariant> delta = wheel.at(i);
        QCOMPARE(delta.at(0).toInt(), -120);
        QCOMPARE(delta.at(1).toBool(), true);
    }
}

void UiTest::viewerCanvasCapturesAndReleasesKeyboard()
{
    pd::ui::ViewerCanvas canvas;
    canvas.resize(800, 600);
    canvas.show();
    canvas.setInputEnabled(true);
    QSignalSpy keys(&canvas, &pd::ui::ViewerCanvas::keyRequested);

    QTest::keyPress(&canvas, Qt::Key_A);
    QCOMPARE(canvas.pressedKeys(), QStringList{QStringLiteral("KeyA")});
    QCOMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(0).toString(), QStringLiteral("KeyA"));
    QCOMPARE(keys.at(0).at(1).toBool(), true);

    QKeyEvent repeatEvent(
        QEvent::KeyPress,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        true);
    QApplication::sendEvent(&canvas, &repeatEvent);
    QCOMPARE(keys.count(), 1);
    QCOMPARE(canvas.pressedKeys(), QStringList{QStringLiteral("KeyA")});

    QTest::keyRelease(&canvas, Qt::Key_A);
    QCOMPARE(keys.count(), 2);
    QCOMPARE(keys.at(1).at(0).toString(), QStringLiteral("KeyA"));
    QCOMPARE(keys.at(1).at(1).toBool(), false);
    QVERIFY(canvas.pressedKeys().isEmpty());

    QTest::keyPress(&canvas, Qt::Key_Control);
    QCOMPARE(keys.count(), 3);
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&canvas, &deactivate);
    QCOMPARE(keys.count(), 4);
    QCOMPARE(keys.at(3).at(0).toString(), QStringLiteral("ControlLeft"));
    QCOMPARE(keys.at(3).at(1).toBool(), false);
    QVERIFY(canvas.pressedKeys().isEmpty());
}

void UiTest::viewerCanvasIgnoresInputWhenDisabled()
{
    pd::ui::ViewerCanvas canvas;
    canvas.resize(800, 600);
    QImage frame(800, 600, QImage::Format_RGB32);
    frame.fill(Qt::black);
    canvas.setFrame(frame);
    canvas.show();
    canvas.setInputEnabled(false);

    QSignalSpy moves(&canvas, &pd::ui::ViewerCanvas::mouseMoveRequested);
    QSignalSpy buttons(&canvas, &pd::ui::ViewerCanvas::mouseButtonRequested);
    QSignalSpy wheel(&canvas, &pd::ui::ViewerCanvas::wheelRequested);
    QSignalSpy keys(&canvas, &pd::ui::ViewerCanvas::keyRequested);

    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(400.0, 300.0),
        QPointF(400.0, 300.0),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QMouseEvent move(
        QEvent::MouseMove,
        QPointF(420.0, 320.0),
        QPointF(420.0, 320.0),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QWheelEvent scroll(
        QPointF(400.0, 300.0),
        QPointF(400.0, 300.0),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false);
    QKeyEvent key(
        QEvent::KeyPress,
        Qt::Key_B,
        Qt::NoModifier,
        QStringLiteral("b"));
    QApplication::sendEvent(&canvas, &press);
    QApplication::sendEvent(&canvas, &move);
    QApplication::sendEvent(&canvas, &scroll);
    QApplication::sendEvent(&canvas, &key);
    QTest::qWait(25);

    QCOMPARE(moves.count(), 0);
    QCOMPARE(buttons.count(), 0);
    QCOMPARE(wheel.count(), 0);
    QCOMPARE(keys.count(), 0);
}

void UiTest::viewerDemoRendersNativeSession()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(1280, 720);
    page.show();
    QTest::qWait(20);

    QImage frame(800, 450, QImage::Format_RGB32);
    frame.fill(QColor(QStringLiteral("#18324f")));
    const QJsonArray monitors{
        QJsonObject{
            {QStringLiteral("index"), 0},
            {QStringLiteral("w"), 800},
            {QStringLiteral("h"), 450},
            {QStringLiteral("primary"), true},
            {QStringLiteral("name"), QStringLiteral("合成主屏")},
        },
        QJsonObject{
            {QStringLiteral("index"), 1},
            {QStringLiteral("w"), 1280},
            {QStringLiteral("h"), 720},
            {QStringLiteral("primary"), false},
            {QStringLiteral("name"), QStringLiteral("合成副屏")},
        },
    };
    QSignalSpy toasts(&page, &pd::ui::ViewerPage::toastRequested);
    page.showDemoFrame(frame, QStringLiteral("DESIGN-STUDIO"), monitors);
    QTest::qWait(10);

    QVERIFY(page.sessionLive());
    QVERIFY(page.demoMode());
    QVERIFY(page.canvas()->hasFrame());
    QCOMPARE(page.canvas()->frame().size(), QSize(800, 450));
    QCOMPARE(page.peerLabel()->text(), QStringLiteral("DESIGN-STUDIO"));
    QVERIFY(page.codeLabel()->text().contains(QStringLiteral("合成演示")));
    QCOMPARE(page.resolutionLabel()->text(), QStringLiteral("800 × 450"));
    QCOMPARE(page.fpsLabel()->text(), QStringLiteral("59.8 fps"));
    QCOMPARE(page.bitrateLabel()->text(), QStringLiteral("22.6 Mbps"));
    QCOMPARE(page.latencyLabel()->text(), QStringLiteral("6.4 ms"));
    QCOMPARE(page.audioLabel()->text(), QStringLiteral("开"));
    QCOMPARE(page.qualityMenu()->actions().size(), 5);
    QCOMPARE(page.monitorMenu()->actions().size(), 2);
    QCOMPARE(
        page.monitorButton()->text(),
        QStringLiteral("合成主屏 · 主屏"));
    QVERIFY(page.connectionOverlay()->isHidden());
    QVERIFY(page.toolbar()->isVisible());
    QVERIFY(page.demoBadge()->isVisible());
    QVERIFY(!page.canvas()->inputEnabled());

    page.sendLocalFiles(
        QStringList{QStringLiteral("C:/演示/不会发送.txt")});
    QCOMPARE(toasts.count(), 1);
    QVERIFY(toasts.takeFirst().at(0).toString().contains(QStringLiteral("演示")));

    page.openTextInput();
    QVERIFY(page.textOverlay()->isHidden());
    QCOMPARE(controller.state(), pd::ViewerController::State::Idle);
    QCOMPARE(controller.monitor(), 0);
}

void UiTest::viewerEndedClearsFrameAndShowsBack()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(960, 600);
    page.show();
    QTest::qWait(20);

    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::darkGreen);
    page.showDemoFrame(frame);
    QVERIFY(page.canvas()->hasFrame());

    page.showEnded(QStringLiteral("对端主动结束会话"));

    QVERIFY(!page.sessionLive());
    QVERIFY(!page.demoMode());
    QVERIFY(!page.canvas()->hasFrame());
    QVERIFY(!page.canvas()->inputEnabled());
    QVERIFY(page.toolbar()->isHidden());
    QVERIFY(page.backButton()->isVisible());
    QVERIFY(page.overlayTitle()->text().contains(QStringLiteral("会话已结束")));
    QCOMPARE(page.overlayNote()->text(), QStringLiteral("对端主动结束会话"));
}

void UiTest::appWindowKeepsViewerDuringLiveSession()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.resize(1200, 760);
    window.show();
    QTest::qWait(20);

    QImage frame(800, 450, QImage::Format_RGB32);
    frame.fill(Qt::darkBlue);
    window.viewerPage()->showDemoFrame(frame);
    window.showViewerPage();
    QCOMPARE(window.currentPage(), window.viewerPage());
    QVERIFY(window.viewerPage()->sessionLive());

    window.remoteNavButton()->click();
    QCOMPARE(window.currentPage(), window.viewerPage());
    QVERIFY(window.remoteNavButton()->isChecked());

    window.hostNavButton()->click();
    QCOMPARE(window.currentPage(), window.hostPanel());
    QVERIFY(!host.active());

    window.remoteNavButton()->click();
    QCOMPARE(window.currentPage(), window.viewerPage());
    QVERIFY(window.viewerPage()->sessionLive());

    window.viewerPage()->showEnded(QStringLiteral("测试结束"));
    window.remoteNavButton()->click();
    QCOMPARE(window.currentPage(), window.connectionPage());

    window.viewerPage()->showDemoFrame(frame);
    window.showViewerPage();
    window.viewerPage()->hangupButton()->click();
    QCOMPARE(window.currentPage(), window.connectionPage());
    QVERIFY(!window.viewerPage()->sessionLive());
    QVERIFY(!viewer.active());
}

void UiTest::windowGeometryRoundTrip()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow first(&host, &viewer);
    first.hostPanel()->setAutoStart(false);
    first.resize(820, 680);
    first.move(24, 24);
    first.show();
    QTest::qWait(30);
    const QSize expectedSize = first.size();
    const QByteArray expectedGeometry = first.saveGeometry();
    first.saveWindowState();
    first.hide();

    const pd::WindowPreferences saved = preferences->window();
    QCOMPARE(saved.geometry, expectedGeometry);
    QVERIFY(!saved.maximized);

    pd::ui::AppWindow second(&host, &viewer);
    second.hostPanel()->setAutoStart(false);
    QCOMPARE(second.size(), expectedSize);
    const QRect restored = second.frameGeometry();
    const QList<QScreen *> screens = QGuiApplication::screens();
    QVERIFY(std::any_of(
        screens.cbegin(),
        screens.cend(),
        [&restored](const QScreen *screen) {
            return screen
                && screen->availableGeometry().intersects(restored);
        }));
    second.hide();
}

void UiTest::windowGeometryFallsBackWhenOffscreen()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow first(&host, &viewer);
    first.hostPanel()->setAutoStart(false);
    first.show();
    first.move(5000, 5000);
    first.saveWindowState();
    first.hide();

    pd::ui::AppWindow second(&host, &viewer);
    second.hostPanel()->setAutoStart(false);
    const QRect restored = second.frameGeometry();
    const QList<QScreen *> screens = QGuiApplication::screens();
    QVERIFY(std::any_of(
        screens.cbegin(),
        screens.cend(),
        [&restored](const QScreen *screen) {
            return screen
                && screen->availableGeometry().intersects(restored);
        }));
    second.hide();
}

void UiTest::closeWithoutTrayQuits()
{
    const auto preferences = testPreferences();
    pd::WindowPreferences windowPreferences = preferences->window();
    windowPreferences.closeToTray = false;
    preferences->setWindow(windowPreferences);

    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.setTrayAvailableForTesting(true);
    window.show();
    QTest::qWait(20);

    QSignalSpy quits(&window, &pd::ui::AppWindow::quitRequested);
    QVERIFY(window.close());
    QCOMPARE(quits.count(), 1);
    QVERIFY(!window.isVisible());

    window.requestQuit();
    QCOMPARE(quits.count(), 1);
}

void UiTest::closeToTrayHidesAndExplicitQuitWorks()
{
    const auto preferences = testPreferences();
    pd::WindowPreferences windowPreferences = preferences->window();
    windowPreferences.closeToTray = true;
    preferences->setWindow(windowPreferences);

    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.setTrayAvailableForTesting(true);
    window.show();
    QTest::qWait(20);

    QSignalSpy quits(&window, &pd::ui::AppWindow::quitRequested);
    QVERIFY(!window.close());
    QCOMPARE(quits.count(), 0);
    QVERIFY(!window.isVisible());

    window.showMainWindow();
    QVERIFY(window.isVisible());

    window.requestQuit();
    QCOMPARE(quits.count(), 1);
    QVERIFY(!window.isVisible());
    window.requestQuit();
    QCOMPARE(quits.count(), 1);
}

void UiTest::settingsUpdateControllersAndConnectionPage()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.setTrayAvailableForTesting(false);
    window.show();
    QTest::qWait(20);
    window.settingsButton()->click();

    auto *dialog = window.settingsDialog();
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QSignalSpy saved(dialog, &pd::ui::SettingsDialog::settingsSaved);

    dialog->serverInput()->setText(
        QStringLiteral("127.0.0.1:not-a-port"));
    dialog->saveButton()->click();
    QCOMPARE(saved.count(), 0);
    QVERIFY(dialog->errorLabel()->isVisible());

    QTemporaryDir receiveDirectory;
    QVERIFY(receiveDirectory.isValid());
    const QString server = QStringLiteral("remote.example:8100");
    const QString token = QStringLiteral("new-secret");
    dialog->serverInput()->setText(server);
    dialog->tokenInput()->setText(token);
    const int qualityIndex =
        dialog->qualityCombo()->findData(QStringLiteral("hd"));
    QVERIFY(qualityIndex >= 0);
    dialog->qualityCombo()->setCurrentIndex(qualityIndex);
    dialog->audioToggle()->setChecked(false);
    dialog->receiveDirectoryInput()->setText(receiveDirectory.path());
    dialog->closeToTrayToggle()->setChecked(false);
    dialog->saveButton()->click();

    QCOMPARE(saved.count(), 1);
    QCOMPARE(host.server(), server);
    QCOMPARE(host.token(), token);
    QCOMPARE(viewer.server(), server);
    QCOMPARE(viewer.token(), token);
    QCOMPARE(viewer.qualityId(), QStringLiteral("hd"));
    QVERIFY(!viewer.audioEnabled());
    QCOMPARE(window.connectionPage()->server(), server);
    QCOMPARE(window.connectionPage()->token(), token);
    QCOMPARE(preferences->host().receiveDirectory, receiveDirectory.path());
    QCOMPARE(preferences->viewer().receiveDirectory, receiveDirectory.path());
    QVERIFY(!preferences->window().closeToTray);
    QVERIFY(dialog->isHidden());
}

QTEST_MAIN(UiTest)

#include "ui_test.moc"
