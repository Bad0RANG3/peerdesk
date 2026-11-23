#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "platform/startup_registration.h"
#include "session/media_sources.h"
#include "ui/app_window.h"
#include "ui/code_input.h"
#include "ui/connection_page.h"
#include "ui/host_panel.h"
#include "ui/outgoing_file_queue.h"
#include "ui/settings_dialog.h"
#include "ui/ui_theme.h"
#include "ui/viewer_page.h"

#include <QApplication>
#include <QComboBox>
#include <QGraphicsOpacityEffect>
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

class FakeStartupRegistration final : public pd::StartupRegistration {
public:
    [[nodiscard]] bool isSupported() const override
    {
        return supported;
    }

    [[nodiscard]] bool isEnabled() const override
    {
        return enabled;
    }

    [[nodiscard]] QString registeredCommand() const override
    {
        return enabled
            ? QStringLiteral("\"C:\\PeerDesk\\peerdesk-client.exe\" --background")
            : QString();
    }

    bool setEnabled(bool value, QString *error = nullptr) override
    {
        ++setCalls;
        if (failNextSet) {
            failNextSet = false;
            if (error) {
                *error = QStringLiteral("模拟启动项写入失败");
            }
            return false;
        }
        enabled = value;
        if (error) {
            error->clear();
        }
        return true;
    }

    bool supported = true;
    bool enabled = false;
    bool failNextSet = false;
    int setCalls = 0;
};

} // namespace

class UiTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void connectionCodeFormatting();
    void invalidInputIsRejected();
    void validConnectionIsDispatched();
    void connectedStateSwitchesToDisconnect();
    void recentConnectionsSelectionAndClear();
    void navigationSwitchesPagesWithoutAutoStart();
    void hostPanelRendersWithSyntheticSource();
    void viewerCanvasScalesAndNormalizesCoordinates();
    void viewerCanvasBatchesPointerInput();
    void viewerCanvasGameMouseRelativeInput();
    void viewerCanvasCapturesAndReleasesKeyboard();
    void viewerCanvasIgnoresInputWhenDisabled();
    void viewerDemoRendersNativeSession();
    void viewerEndedClearsFrameAndShowsBack();
    void viewerToolbarFadesAndRevealsOnActivity();
    void viewerReconnectKeepsFrameAndIsolatesInput();
    void viewerPageGameMouseLifecycle();
    void outgoingFileQueueRetriesAndContinues();
    void outgoingFileQueuePausesAndResumes();
    void outgoingFileQueueIgnoresStaleCallbacks();
    void viewerFileTransferCancelState();
    void hostFileTransferCancelState();
    void appWindowKeepsViewerDuringLiveSession();
    void windowGeometryRoundTrip();
    void windowGeometryFallsBackWhenOffscreen();
    void closeWithoutTrayQuits();
    void closeToTrayHidesAndExplicitQuitWorks();
    void appWindowLoadsAndClearsRecentConnections();
    void settingsUpdateControllersAndConnectionPage();
    void settingsStartupRegistrationRoundTrip();
    void appWindowStartsInBackgroundOnlyWithTray();
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

void UiTest::recentConnectionsSelectionAndClear()
{
    pd::ui::ConnectionPage page;
    page.resize(900, 760);
    page.show();
    QTest::qWait(20);

    QVERIFY(page.recentConnectionsPanel()->isHidden());
    QVERIFY(page.recentConnectionButtons().constFirst()->isHidden());

    pd::RecentConnection office;
    office.code = QStringLiteral("123456789");
    office.name = QStringLiteral("OFFICE-PC");
    office.server = QStringLiteral("desk.example:7900");
    office.connectedAt = QDateTime::currentDateTimeUtc();
    pd::RecentConnection laptop;
    laptop.code = QStringLiteral("987654321");
    laptop.name = QStringLiteral("Studio Laptop");
    laptop.server = QStringLiteral("lab.example:7901");
    laptop.connectedAt = office.connectedAt.addSecs(-60);
    page.setRecentConnections({office, laptop});
    QTest::qWait(10);

    QVERIFY(page.recentConnectionsPanel()->isVisible());
    const QList<QPushButton *> buttons =
        page.recentConnectionButtons();
    QCOMPARE(buttons.size(), pd::Preferences::kRecentConnectionLimit);
    QVERIFY(buttons.at(0)->isVisible());
    QVERIFY(buttons.at(1)->isVisible());
    QVERIFY(buttons.at(2)->isHidden());
    QCOMPARE(
        buttons.at(0)
            ->findChild<QLabel *>(
                QStringLiteral("recentConnectionName"))
            ->text(),
        QStringLiteral("OFFICE-PC"));
    QCOMPARE(
        buttons.at(0)
            ->findChild<QLabel *>(
                QStringLiteral("recentConnectionCode"))
            ->text(),
        QStringLiteral("123 456 789"));
    QCOMPARE(
        buttons.at(0)
            ->findChild<QLabel *>(
                QStringLiteral("recentConnectionServer"))
            ->text(),
        QStringLiteral("desk.example:7900"));

    page.setToken(QStringLiteral("server-secret"));
    buttons.at(0)->click();
    QCOMPARE(page.code(), QStringLiteral("123456789"));
    QCOMPARE(page.server(), QStringLiteral("desk.example:7900"));
    QCOMPARE(page.token(), QStringLiteral("server-secret"));

    QSignalSpy requests(
        &page,
        &pd::ui::ConnectionPage::connectRequested);
    page.requestConnect();
    QCOMPARE(requests.count(), 1);
    const QList<QVariant> arguments = requests.takeFirst();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("123456789"));
    QCOMPARE(
        arguments.at(1).toString(),
        QStringLiteral("desk.example:7900"));
    QCOMPARE(arguments.at(2).toString(), QStringLiteral("server-secret"));
    page.setBusy(false);

    QSignalSpy cleared(
        &page,
        &pd::ui::ConnectionPage::recentConnectionsCleared);
    page.clearRecentConnectionsButton()->click();
    QCOMPARE(cleared.count(), 1);
    QVERIFY(page.recentConnectionsPanel()->isHidden());
    for (QPushButton *button : page.recentConnectionButtons()) {
        QVERIFY(button->isHidden());
    }
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

void UiTest::viewerCanvasGameMouseRelativeInput()
{
    pd::ui::ViewerCanvas canvas;
    canvas.resize(1000, 600);
    QImage frame(800, 400, QImage::Format_RGB32);
    frame.fill(Qt::black);
    canvas.setFrame(frame);
    canvas.show();
    canvas.setInputEnabled(true);

    QSignalSpy absoluteMoves(
        &canvas,
        &pd::ui::ViewerCanvas::mouseMoveRequested);
    QSignalSpy absoluteButtons(
        &canvas,
        &pd::ui::ViewerCanvas::mouseButtonRequested);
    QSignalSpy relativeMoves(
        &canvas,
        &pd::ui::ViewerCanvas::mouseRelativeRequested);
    QSignalSpy relativeButtons(
        &canvas,
        &pd::ui::ViewerCanvas::mouseRelativeButtonRequested);
    QSignalSpy modeChanges(
        &canvas,
        &pd::ui::ViewerCanvas::relativeMouseModeChanged);

    canvas.setRelativeMouseMode(true);
    QVERIFY(canvas.relativeMouseMode());
    QCOMPARE(canvas.cursor().shape(), Qt::BlankCursor);
    QCOMPARE(modeChanges.count(), 1);
    QCOMPARE(modeChanges.constFirst().constFirst().toBool(), true);

    const QPoint localCenter = canvas.rect().center();
    const QPoint globalCenter = canvas.mapToGlobal(localCenter);
    QMouseEvent baseline(
        QEvent::MouseMove,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &baseline);

    QMouseEvent firstMove(
        QEvent::MouseMove,
        QPointF(localCenter + QPoint(7, -4)),
        QPointF(globalCenter + QPoint(7, -4)),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QMouseEvent secondMove(
        QEvent::MouseMove,
        QPointF(localCenter + QPoint(10, -2)),
        QPointF(globalCenter + QPoint(10, -2)),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &firstMove);
    QApplication::sendEvent(&canvas, &secondMove);
    QTRY_COMPARE_WITH_TIMEOUT(relativeMoves.count(), 1, 1000);
    QCOMPARE(relativeMoves.constFirst().at(0).toInt(), 10);
    QCOMPARE(relativeMoves.constFirst().at(1).toInt(), -2);
    QCOMPARE(absoluteMoves.count(), 0);

    QMouseEvent leftPress(
        QEvent::MouseButtonPress,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QMouseEvent leftRelease(
        QEvent::MouseButtonRelease,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &leftPress);
    QApplication::sendEvent(&canvas, &leftRelease);
    QCOMPARE(relativeButtons.count(), 2);
    QCOMPARE(
        relativeButtons.at(0).at(0).toString(),
        QStringLiteral("reldown"));
    QCOMPARE(
        relativeButtons.at(0).at(1).toString(),
        QStringLiteral("left"));
    QCOMPARE(
        relativeButtons.at(1).at(0).toString(),
        QStringLiteral("relup"));
    QCOMPARE(
        relativeButtons.at(1).at(1).toString(),
        QStringLiteral("left"));
    QCOMPARE(absoluteButtons.count(), 0);

    QMouseEvent rightPress(
        QEvent::MouseButtonPress,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    QApplication::sendEvent(&canvas, &rightPress);
    QCOMPARE(relativeButtons.count(), 3);

    canvas.setInputEnabled(false);
    QVERIFY(!canvas.relativeMouseMode());
    QVERIFY(!canvas.inputEnabled());
    QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(relativeButtons.count(), 4);
    QCOMPARE(
        relativeButtons.constLast().at(0).toString(),
        QStringLiteral("relup"));
    QCOMPARE(
        relativeButtons.constLast().at(1).toString(),
        QStringLiteral("right"));
    QCOMPARE(absoluteButtons.count(), 0);
    QCOMPARE(absoluteMoves.count(), 0);
    QCOMPARE(modeChanges.count(), 2);
    QCOMPARE(modeChanges.constLast().constFirst().toBool(), false);
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

void UiTest::viewerToolbarFadesAndRevealsOnActivity()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(960, 600);
    page.show();
    QTest::qWait(20);

    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(QColor(QStringLiteral("#203a54")));
    page.beginSession(QStringLiteral("DESKTOP"), QStringLiteral("123456789"));
    page.canvas()->setFrame(frame);
    page.showStreaming(QStringLiteral("DESKTOP"));
    QTRY_VERIFY_WITH_TIMEOUT(page.toolbar()->isVisible(), 500);
    QTRY_VERIFY_WITH_TIMEOUT(
        page.toolbarOpacityEffect()->opacity() > 0.95,
        1000);

    page.setToolbarAutoHideInterval(30);
    QTRY_VERIFY_WITH_TIMEOUT(
        page.toolbarAnimation()->state() == QAbstractAnimation::Running,
        500);
    QTRY_VERIFY_WITH_TIMEOUT(page.toolbar()->isHidden(), 1000);
    QCOMPARE(page.toolbarOpacityEffect()->opacity(), 0.0);

    page.setToolbarAutoHideInterval(3000);
    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(page.canvas(), &enter);
    QTRY_VERIFY_WITH_TIMEOUT(page.toolbar()->isVisible(), 500);
    QTRY_VERIFY_WITH_TIMEOUT(
        page.toolbarOpacityEffect()->opacity() > 0.95,
        1000);

    page.showReconnecting();
    QTest::qWait(10);
    QVERIFY(page.toolbar()->isHidden());
    QCOMPARE(page.toolbarOpacityEffect()->opacity(), 0.0);
    QVERIFY(page.canvas()->hasFrame());
}

void UiTest::viewerReconnectKeepsFrameAndIsolatesInput()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(960, 600);
    page.show();
    QTest::qWait(20);

    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(QColor(QStringLiteral("#28425f")));
    page.beginSession(QStringLiteral("DESKTOP"), QStringLiteral("123456789"));
    page.canvas()->setFrame(frame);
    page.showStreaming(QStringLiteral("DESKTOP"));
    QTest::qWait(10);

    QVERIFY(page.sessionLive());
    QVERIFY(page.canvas()->hasFrame());
    QVERIFY(page.canvas()->inputEnabled());
    page.canvas()->setFocus(Qt::OtherFocusReason);
    QTest::keyPress(page.canvas(), Qt::Key_A);
    QCOMPARE(
        page.canvas()->pressedKeys(),
        QStringList{QStringLiteral("KeyA")});

    page.toolbar()->show();
    page.textOverlay()->show();
    page.dropOverlay()->show();
    page.transferLabel()->show();
    controller.outgoingFileStarted(
        QStringLiteral("reconnect-file"),
        QStringLiteral("reconnect.bin"),
        4096);
    QTest::qWait(10);
    QVERIFY(page.toolbar()->isVisible());
    QVERIFY(page.textOverlay()->isVisible());
    QVERIFY(page.dropOverlay()->isVisible());
    QVERIFY(page.transferLabel()->isVisible());
    QVERIFY(page.fileCancelButton()->isVisible());

    page.showReconnecting();
    QTest::qWait(10);

    QVERIFY(page.sessionLive());
    QVERIFY(page.canvas()->hasFrame());
    QCOMPARE(page.canvas()->frame().size(), frame.size());
    QCOMPARE(
        page.canvas()->frame().pixelColor(QPoint(20, 20)),
        frame.pixelColor(QPoint(20, 20)));
    QVERIFY(!page.canvas()->inputEnabled());
    QVERIFY(page.canvas()->pressedKeys().isEmpty());
    QVERIFY(page.connectionOverlay()->isVisible());
    QVERIFY(page.overlayTitle()->text().contains(QStringLiteral("恢复")));
    QVERIFY(page.backButton()->isHidden());
    QVERIFY(page.toolbar()->isHidden());
    QVERIFY(page.textOverlay()->isHidden());
    QVERIFY(page.dropOverlay()->isHidden());
    QVERIFY(page.transferLabel()->isHidden());
    QVERIFY(page.fileCancelButton()->isHidden());
    QVERIFY(page.fileRetryButton()->isHidden());
    QCOMPARE(page.fileQueueSize(), 1);

    page.showStreaming(QStringLiteral("DESKTOP"));
    QTest::qWait(10);
    QVERIFY(page.transferLabel()->isVisible());
    QVERIFY(page.transferLabel()->text().contains(
        QStringLiteral("等待发送")));
    QVERIFY(page.fileCancelButton()->isHidden());
    QCOMPARE(page.fileQueueSize(), 1);

    page.showEnded(QStringLiteral("恢复超时"));
    QVERIFY(!page.sessionLive());
    QVERIFY(!page.canvas()->hasFrame());
    QVERIFY(page.backButton()->isVisible());
    QCOMPARE(page.fileQueueSize(), 0);
}

void UiTest::viewerPageGameMouseLifecycle()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(960, 600);
    page.show();
    QTest::qWait(20);

    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(QColor(QStringLiteral("#31506f")));
    page.beginSession(QStringLiteral("DESKTOP"), QStringLiteral("123456789"));
    page.canvas()->setFrame(frame);
    page.showStreaming(QStringLiteral("DESKTOP"));
    QTest::qWait(10);
    QVERIFY(page.gameMouseButton()->isEnabled());

    QSignalSpy exitFullScreen(
        &page,
        &pd::ui::ViewerPage::exitFullScreenRequested);
    QSignalSpy relativeButtons(
        page.canvas(),
        &pd::ui::ViewerCanvas::mouseRelativeButtonRequested);
    page.gameMouseButton()->click();
    QVERIFY(page.canvas()->relativeMouseMode());
    QVERIFY(page.gameMouseButton()->isChecked());
    QVERIFY(page.toolbar()->isHidden());
    QCOMPARE(page.canvas()->cursor().shape(), Qt::BlankCursor);

    QTest::keyPress(page.canvas(), Qt::Key_Escape);
    QVERIFY(!page.canvas()->relativeMouseMode());
    QVERIFY(!page.gameMouseButton()->isChecked());
    QCOMPARE(page.canvas()->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(exitFullScreen.count(), 0);

    page.gameMouseButton()->click();
    QVERIFY(page.canvas()->relativeMouseMode());
    const QPoint localCenter = page.canvas()->rect().center();
    const QPoint globalCenter = page.canvas()->mapToGlobal(localCenter);
    QMouseEvent leftPress(
        QEvent::MouseButtonPress,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(page.canvas(), &leftPress);
    QCOMPARE(relativeButtons.count(), 1);

    page.showReconnecting();
    QTest::qWait(10);
    QVERIFY(page.sessionLive());
    QVERIFY(page.canvas()->hasFrame());
    QCOMPARE(
        page.canvas()->frame().pixelColor(QPoint(20, 20)),
        frame.pixelColor(QPoint(20, 20)));
    QVERIFY(!page.canvas()->relativeMouseMode());
    QVERIFY(!page.canvas()->inputEnabled());
    QVERIFY(!page.gameMouseButton()->isChecked());
    QVERIFY(!page.gameMouseButton()->isEnabled());
    QCOMPARE(page.canvas()->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(relativeButtons.count(), 2);
    QCOMPARE(
        relativeButtons.constLast().at(0).toString(),
        QStringLiteral("relup"));
    QCOMPARE(
        relativeButtons.constLast().at(1).toString(),
        QStringLiteral("left"));
    QVERIFY(page.connectionOverlay()->isVisible());

    page.showStreaming(QStringLiteral("DESKTOP"));
    QTRY_VERIFY_WITH_TIMEOUT(page.gameMouseButton()->isEnabled(), 500);
    page.gameMouseButton()->click();
    QVERIFY(page.canvas()->relativeMouseMode());
    QMouseEvent rightPress(
        QEvent::MouseButtonPress,
        QPointF(localCenter),
        QPointF(globalCenter),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    QApplication::sendEvent(page.canvas(), &rightPress);
    QCOMPARE(relativeButtons.count(), 3);

    page.showFailed(QStringLiteral("测试失败"));
    QVERIFY(!page.sessionLive());
    QVERIFY(!page.canvas()->relativeMouseMode());
    QVERIFY(!page.canvas()->inputEnabled());
    QCOMPARE(page.canvas()->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(relativeButtons.count(), 4);
    QCOMPARE(
        relativeButtons.constLast().at(0).toString(),
        QStringLiteral("relup"));
    QCOMPARE(
        relativeButtons.constLast().at(1).toString(),
        QStringLiteral("right"));

    page.showStreaming(QStringLiteral("DESKTOP"));
    QTRY_VERIFY_WITH_TIMEOUT(page.gameMouseButton()->isEnabled(), 500);
    page.gameMouseButton()->click();
    QVERIFY(page.canvas()->relativeMouseMode());
    page.showEnded(QStringLiteral("测试结束"));
    QVERIFY(!page.sessionLive());
    QVERIFY(!page.canvas()->hasFrame());
    QVERIFY(!page.canvas()->relativeMouseMode());
    QVERIFY(!page.gameMouseButton()->isChecked());
    QCOMPARE(page.canvas()->cursor().shape(), Qt::ArrowCursor);
}

void UiTest::outgoingFileQueueRetriesAndContinues()
{
    pd::ui::OutgoingFileQueue queue;
    QCOMPARE(
        queue.enqueue({
            QStringLiteral("C:/tmp/alpha.bin"),
            QStringLiteral("C:/tmp/beta.bin"),
            QStringLiteral("C:/tmp/gamma.bin"),
        }),
        3);
    QCOMPARE(
        queue.enqueue({QStringLiteral("C:/tmp/alpha.bin")}),
        0);

    QStringList sent;
    const auto sender = [&sent](const QString &path, QString *error) {
        sent.append(path);
        if (error) {
            error->clear();
        }
        return true;
    };

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QCOMPARE(queue.activePosition(), 1);
    QVERIFY(queue.markStarted(QStringLiteral("alpha-1"), {}, 100));
    QVERIFY(queue.markProgress(QStringLiteral("alpha-1"), 40, 100));
    QCOMPARE(queue.activeTask()->sent, 40);
    QVERIFY(queue.markFinished(QStringLiteral("alpha-1"), {}, 100));
    QVERIFY(!queue.active());
    QVERIFY(queue.hasPending());

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("beta-1"), {}, 200));
    QVERIFY(queue.markFailed(QStringLiteral("beta-1"), QStringLiteral("io")));
    QCOMPARE(queue.failedCount(), 1);
    QVERIFY(queue.hasFailed());
    QVERIFY(queue.retryFailed());
    QVERIFY(!queue.hasFailed());

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("beta-2"), {}, 200));
    QVERIFY(queue.markCancelled(
        QStringLiteral("beta-2"),
        QStringLiteral("cancelled")));
    QCOMPARE(queue.cancelledCount(), 1);
    QVERIFY(queue.hasPending());

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("gamma-1"), {}, 300));
    QVERIFY(queue.markFinished(QStringLiteral("gamma-1"), {}, 300));
    QVERIFY(!queue.active());
    QVERIFY(!queue.hasPending());
    QCOMPARE(queue.succeededCount(), 2);
    QCOMPARE(queue.cancelledCount(), 1);
    QCOMPARE(queue.succeededBytes(), 400);
    QCOMPARE(sent.size(), 4);
    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::NoTask);
}

void UiTest::outgoingFileQueuePausesAndResumes()
{
    pd::ui::OutgoingFileQueue queue;
    QCOMPARE(queue.enqueue({QStringLiteral("C:/tmp/paused.bin")}), 1);

    int starts = 0;
    const auto sender = [&starts](const QString &, QString *error) {
        ++starts;
        if (error) {
            error->clear();
        }
        return true;
    };

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("paused-1"), {}, 512));
    queue.pause();
    QVERIFY(queue.interruptActive(QStringLiteral("interrupted")));
    QVERIFY(queue.paused());
    QVERIFY(!queue.active());
    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::NoTask);
    QCOMPARE(starts, 1);

    queue.resume();
    QVERIFY(!queue.paused());
    QVERIFY(queue.hasPending());
    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QCOMPARE(starts, 2);
    QVERIFY(queue.markStarted(QStringLiteral("paused-2"), {}, 512));
    QVERIFY(queue.markFinished(QStringLiteral("paused-2"), {}, 512));
    QCOMPARE(queue.succeededCount(), 1);
}

void UiTest::outgoingFileQueueIgnoresStaleCallbacks()
{
    pd::ui::OutgoingFileQueue queue;
    QCOMPARE(
        queue.enqueue({
            QStringLiteral("C:/tmp/first.bin"),
            QStringLiteral("C:/tmp/second.bin"),
        }),
        2);
    const auto sender = [](const QString &, QString *error) {
        if (error) {
            error->clear();
        }
        return true;
    };

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("first-1"), {}, 128));
    QVERIFY(!queue.markStarted(QStringLiteral("first-1"), {}, 128));
    QVERIFY(queue.markFinished(QStringLiteral("first-1"), {}, 128));

    QCOMPARE(
        queue.startNext(sender),
        pd::ui::OutgoingFileQueue::StartResult::Started);
    QVERIFY(queue.markStarted(QStringLiteral("second-1"), {}, 256));
    QVERIFY(!queue.markProgress(QStringLiteral("first-1"), 64, 128));
    QVERIFY(!queue.markFinished(QStringLiteral("first-1"), {}, 128));
    QVERIFY(!queue.markFailed(
        QStringLiteral("first-1"),
        QStringLiteral("stale")));
    QVERIFY(!queue.markCancelled(
        QStringLiteral("first-1"),
        QStringLiteral("stale")));
    QVERIFY(!queue.markInterrupted(
        QStringLiteral("first-1"),
        QStringLiteral("stale")));
    QVERIFY(queue.active());
    QVERIFY(queue.markProgress(QStringLiteral("second-1"), 64, 256));
    QVERIFY(queue.markFinished(QStringLiteral("second-1"), {}, 256));
    QVERIFY(!queue.markStarted(QStringLiteral("first-1"), {}, 128));
    QCOMPARE(queue.succeededCount(), 2);
}

void UiTest::viewerFileTransferCancelState()
{
    const auto preferences = testPreferences();
    pd::ViewerController controller;
    controller.setPreferences(preferences);
    pd::ui::ViewerPage page(&controller);
    page.resize(960, 600);
    page.show();
    QTest::qWait(20);

    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(QColor(QStringLiteral("#31506f")));
    page.beginSession(QStringLiteral("DESKTOP"), QStringLiteral("123456789"));
    page.canvas()->setFrame(frame);
    page.showStreaming(QStringLiteral("DESKTOP"));
    QTest::qWait(10);

    QVERIFY(page.fileCancelButton()->isHidden());
    controller.outgoingFileStarted(
        QStringLiteral("cancel-file"),
        QStringLiteral("演示文件.bin"),
        8192);
    QTest::qWait(10);
    QVERIFY(page.fileCancelButton()->isVisible());
    QVERIFY(page.fileCancelButton()->isEnabled());
    QVERIFY(page.transferLabel()->text().contains(
        QStringLiteral("演示文件.bin")));

    controller.outgoingFileCancelled(
        QStringLiteral("cancel-file"),
        QStringLiteral("cancelled"));
    QTest::qWait(10);
    QVERIFY(page.fileCancelButton()->isHidden());
    QVERIFY(page.transferLabel()->text().contains(QStringLiteral("已取消")));
    QVERIFY(page.sessionLive());
    QVERIFY(page.canvas()->hasFrame());

    controller.outgoingFileStarted(
        QStringLiteral("retry-file"),
        QStringLiteral("retry.bin"),
        2048);
    controller.outgoingFileFailed(
        QStringLiteral("retry-file"),
        QStringLiteral("io"));
    QTest::qWait(10);
    QVERIFY(page.fileRetryButton()->isVisible());
    QCOMPARE(page.fileRetryButton()->x(), page.fileCancelButton()->x());
    QCOMPARE(page.fileRetryButton()->y(), page.fileCancelButton()->y());

    controller.outgoingFileStarted(
        QStringLiteral("interrupt-file"),
        QStringLiteral("interrupt.bin"),
        4096);
    QTest::qWait(10);
    QVERIFY(page.fileCancelButton()->isVisible());
    controller.outgoingFileInterrupted(
        QStringLiteral("interrupt-file"),
        QStringLiteral("network"));
    QTest::qWait(10);
    QVERIFY(page.fileCancelButton()->isHidden());
    QVERIFY(page.fileRetryButton()->isHidden());
    QVERIFY(page.transferLabel()->text().contains(
        QStringLiteral("传输已中断")));
    QVERIFY(page.canvas()->hasFrame());
}

void UiTest::hostFileTransferCancelState()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    host.setPreferences(preferences);
    pd::ui::HostPanel panel(&host, false);
    panel.resize(800, 900);
    panel.show();
    QTest::qWait(20);

    QVERIFY(!panel.fileBusy());
    QCOMPARE(panel.sendFileButton()->text(), QStringLiteral("发送"));
    host.outgoingFileStarted(
        QStringLiteral("host-file"),
        QStringLiteral("host.bin"),
        8192);
    QTest::qWait(10);
    QVERIFY(panel.fileBusy());
    QCOMPARE(panel.sendFileButton()->text(), QStringLiteral("取消发送"));
    QVERIFY(panel.fileStatus()->text().contains(QStringLiteral("host.bin")));

    panel.sendFileButton()->click();
    QTest::qWait(10);
    QVERIFY(!panel.fileBusy());
    QCOMPARE(panel.sendFileButton()->text(), QStringLiteral("发送"));
    QCOMPARE(
        panel.fileStatus()->text(),
        QStringLiteral("取消传输失败"));

    host.outgoingFileStarted(
        QStringLiteral("host-file-2"),
        QStringLiteral("host-2.bin"),
        4096);
    QTest::qWait(10);
    QVERIFY(panel.fileBusy());
    host.outgoingFileCancelled(
        QStringLiteral("host-file-2"),
        QStringLiteral("cancelled"));
    QTest::qWait(10);
    QVERIFY(!panel.fileBusy());
    QCOMPARE(panel.sendFileButton()->text(), QStringLiteral("发送"));
    QCOMPARE(
        panel.fileStatus()->text(),
        QStringLiteral("传输已取消"));

    host.outgoingFileStarted(
        QStringLiteral("host-file-3"),
        QStringLiteral("host-3.bin"),
        2048);
    QTest::qWait(10);
    host.outgoingFileInterrupted(
        QStringLiteral("host-file-3"),
        QStringLiteral("network"));
    QTest::qWait(10);
    QVERIFY(!panel.fileBusy());
    QCOMPARE(panel.sendFileButton()->text(), QStringLiteral("发送"));
    QCOMPARE(
        panel.fileStatus()->text(),
        QStringLiteral("传输已中断"));
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

void UiTest::appWindowLoadsAndClearsRecentConnections()
{
    const auto preferences = testPreferences();
    pd::RecentConnection connection;
    connection.code = QStringLiteral("246813579");
    connection.name = QStringLiteral("DESIGN-STUDIO");
    connection.server = QStringLiteral("studio.example:7900");
    connection.connectedAt = QDateTime::currentDateTimeUtc();
    preferences->recordRecentConnection(
        connection.code,
        connection.name,
        connection.server,
        connection.connectedAt);

    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);
    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.resize(1000, 820);
    window.show();
    QTest::qWait(20);

    auto *page = window.connectionPage();
    QVERIFY(page->recentConnectionsPanel()->isVisible());
    const QList<QPushButton *> buttons =
        page->recentConnectionButtons();
    QCOMPARE(
        buttons.constFirst()
            ->findChild<QLabel *>(
                QStringLiteral("recentConnectionCode"))
            ->text(),
        QStringLiteral("246 813 579"));
    buttons.constFirst()->click();
    QCOMPARE(page->code(), QStringLiteral("246813579"));
    QCOMPARE(page->server(), QStringLiteral("studio.example:7900"));

    page->clearRecentConnectionsButton()->click();
    QVERIFY(page->recentConnectionsPanel()->isHidden());
    QVERIFY(preferences->recentConnections().isEmpty());

    viewer.setReconnectTimeoutMs(400);
    QString error;
    QVERIFY2(
        viewer.connectTo(
            QStringLiteral("135792468"),
            QStringLiteral("127.0.0.1:1"),
            {},
            QStringLiteral("tester"),
            &error),
        qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(
        viewer.state(),
        pd::ViewerController::State::Error,
        4000);
    QVERIFY(preferences->recentConnections().isEmpty());
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

void UiTest::settingsStartupRegistrationRoundTrip()
{
    const auto preferences = testPreferences();
    auto startup = std::make_shared<FakeStartupRegistration>();
    startup->enabled = true;
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer, startup);
    window.hostPanel()->setAutoStart(false);
    window.setTrayAvailableForTesting(false);
    window.show();
    QTest::qWait(20);
    window.settingsButton()->click();

    auto *dialog = window.settingsDialog();
    QVERIFY(dialog);
    QVERIFY(dialog->startupToggle());
    QVERIFY(dialog->startupToggle()->isEnabled());
    QVERIFY(dialog->startupToggle()->isChecked());
    QSignalSpy saved(dialog, &pd::ui::SettingsDialog::settingsSaved);

    dialog->startupToggle()->setChecked(false);
    dialog->saveButton()->click();
    QCOMPARE(startup->setCalls, 1);
    QVERIFY(!startup->enabled);
    QCOMPARE(saved.count(), 1);
    QVERIFY(dialog->isHidden());

    window.settingsButton()->click();
    QVERIFY(!dialog->startupToggle()->isChecked());
    startup->failNextSet = true;
    dialog->startupToggle()->setChecked(true);
    dialog->saveButton()->click();
    QCOMPARE(startup->setCalls, 2);
    QVERIFY(!startup->enabled);
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->errorLabel()->isVisible());
    QVERIFY(dialog->errorLabel()->text().contains(QStringLiteral("失败")));
    QCOMPARE(saved.count(), 1);

    dialog->saveButton()->click();
    QCOMPARE(startup->setCalls, 3);
    QVERIFY(startup->enabled);
    QCOMPARE(saved.count(), 2);
    QVERIFY(dialog->isHidden());

    startup->supported = false;
    window.settingsButton()->click();
    QVERIFY(!dialog->startupToggle()->isEnabled());
    QVERIFY(!dialog->startupToggle()->isChecked());
    QVERIFY(dialog->startupNote()->text().contains(QStringLiteral("不支持")));
}

void UiTest::appWindowStartsInBackgroundOnlyWithTray()
{
    const auto preferences = testPreferences();
    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);

    pd::ui::AppWindow window(&host, &viewer);
    window.hostPanel()->setAutoStart(false);
    window.setTrayAvailableForTesting(true);
    QVERIFY(window.startInBackground());
    QVERIFY(!window.isVisible());

    pd::ui::AppWindow fallback(&host, &viewer);
    fallback.hostPanel()->setAutoStart(false);
    fallback.setTrayAvailableForTesting(false);
    QVERIFY(!fallback.startInBackground());
    QVERIFY(!fallback.isVisible());
}

QTEST_MAIN(UiTest)

#include "ui_test.moc"
