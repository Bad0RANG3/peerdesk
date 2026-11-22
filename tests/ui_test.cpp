#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "session/media_sources.h"
#include "ui/app_window.h"
#include "ui/code_input.h"
#include "ui/connection_page.h"
#include "ui/host_panel.h"
#include "ui/ui_theme.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>

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
};

void UiTest::initTestCase()
{
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

QTEST_MAIN(UiTest)

#include "ui_test.moc"
