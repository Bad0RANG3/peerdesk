#include "ui/app_window.h"

#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "ui/connection_page.h"
#include "ui/host_panel.h"
#include "ui/ui_theme.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace pd::ui {
namespace {

constexpr int kHeaderHeight = 72;

void setNavChecked(QPushButton *button, bool checked)
{
    button->setChecked(checked);
    button->setProperty("active", checked);
    button->style()->unpolish(button);
    button->style()->polish(button);
}

} // namespace

AppWindow::AppWindow(
    HostController *host,
    ViewerController *viewer,
    QWidget *parent)
    : BackdropWidget(parent)
    , host_(host)
    , viewer_(viewer)
{
    Q_ASSERT(host_);
    Q_ASSERT(viewer_);
    setObjectName(QStringLiteral("appWindow"));
    setWindowTitle(QStringLiteral("PeerDesk · 对端桌面"));
    setMinimumSize(820, 640);
    resize(1080, 840);
    buildUi();
    connectSignals();
    updateViewerState();
    updateNavigation();
}

void AppWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("appHeader"));
    header->setFixedHeight(kHeaderHeight);
    header->setStyleSheet(QStringLiteral(
        "QWidget#appHeader {"
        "  background: rgba(11, 13, 18, 0.72);"
        "  border-bottom: 1px solid rgba(255, 255, 255, 0.065);"
        "}"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(24, 0, 24, 0);
    headerLayout->setSpacing(18);

    auto *brand = new BrandMark(header);
    headerLayout->addWidget(brand, 0, Qt::AlignVCenter);

    auto *brandText = new QWidget(header);
    auto *brandTextLayout = new QVBoxLayout(brandText);
    brandTextLayout->setContentsMargins(0, 0, 0, 0);
    brandTextLayout->setSpacing(0);
    auto *brandTitle = new QLabel(QStringLiteral("PeerDesk"), brandText);
    brandTitle->setFont(uiFont(12, QFont::DemiBold));
    brandTitle->setStyleSheet(QStringLiteral("color: #f4f7fc;"));
    brandTextLayout->addWidget(brandTitle);
    auto *brandSubtitle = new QLabel(
        QStringLiteral("对端桌面 · 自托管"),
        brandText);
    brandSubtitle->setFont(uiFont(8));
    brandSubtitle->setStyleSheet(QStringLiteral("color: #69728a;"));
    brandTextLayout->addWidget(brandSubtitle);
    headerLayout->addWidget(brandText, 0, Qt::AlignVCenter);
    headerLayout->addStretch(1);

    auto *navigation = new QWidget(header);
    navigation->setObjectName(QStringLiteral("appNavigation"));
    navigation->setStyleSheet(QStringLiteral(
        "QWidget#appNavigation {"
        "  background: rgba(255, 255, 255, 0.035);"
        "  border: 1px solid rgba(255, 255, 255, 0.085);"
        "  border-radius: 11px;"
        "}"));
    auto *navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(3, 3, 3, 3);
    navigationLayout->setSpacing(2);
    remoteNavButton_ = new QPushButton(QStringLiteral("远程协助"), navigation);
    remoteNavButton_->setObjectName(QStringLiteral("navButton"));
    remoteNavButton_->setCheckable(true);
    remoteNavButton_->setCursor(Qt::PointingHandCursor);
    remoteNavButton_->setMinimumHeight(31);
    remoteNavButton_->setAccessibleName(QStringLiteral("远程协助"));
    navigationLayout->addWidget(remoteNavButton_);
    hostNavButton_ = new QPushButton(QStringLiteral("本机被控"), navigation);
    hostNavButton_->setObjectName(QStringLiteral("navButton"));
    hostNavButton_->setCheckable(true);
    hostNavButton_->setCursor(Qt::PointingHandCursor);
    hostNavButton_->setMinimumHeight(31);
    hostNavButton_->setAccessibleName(QStringLiteral("本机被控"));
    navigationLayout->addWidget(hostNavButton_);
    headerLayout->addWidget(navigation, 0, Qt::AlignVCenter);

    headerLayout->addStretch(1);
    privacyPill_ = new PillLabel(header);
    privacyPill_->setText(QStringLiteral("端到端加密"));
    privacyPill_->setTone(PillLabel::Tone::Green);
    headerLayout->addWidget(privacyPill_, 0, Qt::AlignVCenter);
    root->addWidget(header);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("pageStack"));
    stack_->setStyleSheet(QStringLiteral(
        "QStackedWidget#pageStack { background: transparent; }"));
    connectionPage_ = new ConnectionPage(stack_);
    hostPanel_ = new HostPanel(host_, true, stack_);
    stack_->addWidget(connectionPage_);
    stack_->addWidget(hostPanel_);
    root->addWidget(stack_, 1);

    const ViewerPreferences viewerPreferences = viewer_->preferences()
        ? viewer_->preferences()->viewer()
        : ViewerPreferences();
    connectionPage_->setServer(
        viewer_->server().isEmpty() ? viewerPreferences.server
                                    : viewer_->server());
    connectionPage_->setCode(
        viewer_->code().isEmpty() ? viewerPreferences.code : viewer_->code());
    connectionPage_->setToken(
        viewer_->token().isEmpty() ? viewerPreferences.token
                                   : viewer_->token());

    toast_ = new QLabel(this);
    toast_->setObjectName(QStringLiteral("toastMessage"));
    toast_->setAlignment(Qt::AlignCenter);
    toast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    toast_->setStyleSheet(QStringLiteral(
        "QLabel#toastMessage {"
        "  color: #f6f9ff;"
        "  background: rgba(24, 30, 43, 0.97);"
        "  border: 1px solid rgba(255, 255, 255, 0.16);"
        "  border-radius: 12px;"
        "  padding: 9px 18px;"
        "  font-size: 12px;"
        "}"));
    toast_->hide();
    auto *opacity = new QGraphicsOpacityEffect(toast_);
    opacity->setOpacity(1.0);
    toast_->setGraphicsEffect(opacity);
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    toastTimer_->setInterval(2400);
    connect(toastTimer_, &QTimer::timeout, this, [this, opacity]() {
        auto *fade = new QPropertyAnimation(opacity, "opacity", this);
        fade->setDuration(180);
        fade->setStartValue(opacity->opacity());
        fade->setEndValue(0.0);
        connect(fade, &QPropertyAnimation::finished, toast_, &QLabel::hide);
        connect(fade, &QPropertyAnimation::finished, fade, &QObject::deleteLater);
        fade->start();
    });

    connectionPage_->focusCode();
}

void AppWindow::connectSignals()
{
    connect(
        remoteNavButton_,
        &QPushButton::clicked,
        this,
        &AppWindow::showRemotePage);
    connect(
        hostNavButton_,
        &QPushButton::clicked,
        this,
        &AppWindow::showHostPage);
    connect(
        connectionPage_,
        &ConnectionPage::hostModeRequested,
        this,
        &AppWindow::showHostPage);
    connect(
        connectionPage_,
        &ConnectionPage::connectRequested,
        this,
        [this](
            const QString &code,
            const QString &server,
            const QString &token) {
            QString error;
            if (!viewer_->connectTo(
                    code,
                    server,
                    token,
                    QStringLiteral("PeerDesk 控制端"),
                    &error)) {
                connectionPage_->setError(error);
                showToast(error);
            }
        });
    connect(
        connectionPage_,
        &ConnectionPage::disconnectRequested,
        this,
        [this]() {
            viewer_->hangUp();
            connectionPage_->setConnected(false);
            showToast(QStringLiteral("远程会话已断开"));
        });
    connect(
        viewer_,
        &ViewerController::stateChanged,
        this,
        [this](ViewerController::State) {
            updateViewerState();
        });
    connect(
        viewer_,
        &ViewerController::peerChanged,
        this,
        [this](const QString &, bool) {
            updateViewerState();
        });
    connect(
        viewer_,
        &ViewerController::errorOccurred,
        this,
        [this](const QString &message) {
            connectionPage_->setError(message);
            showToast(message);
        });
    connect(
        viewer_,
        &ViewerController::sessionEnded,
        this,
        [this](const QString &reason) {
            connectionPage_->setConnected(false);
            connectionPage_->setStatus(reason, PillLabel::Tone::Amber);
            showToast(reason);
        });
    connect(
        viewer_,
        &ViewerController::linkStateChanged,
        this,
        [this](const QString &state) {
            if (viewer_->state() != ViewerController::State::Streaming) {
                return;
            }
            if (state == QStringLiteral("direct")) {
                connectionPage_->setStatus(
                    QStringLiteral("已连接 · P2P 直连"),
                    PillLabel::Tone::Green);
            } else if (state == QStringLiteral("relay")) {
                connectionPage_->setStatus(
                    QStringLiteral("已连接 · 中继模式"),
                    PillLabel::Tone::Blue);
            } else if (state == QStringLiteral("punching")) {
                connectionPage_->setStatus(
                    QStringLiteral("正在建立 P2P 通道…"),
                    PillLabel::Tone::Amber);
            }
        });
    connect(
        hostPanel_,
        &HostPanel::toastRequested,
        this,
        &AppWindow::showToast);
}

void AppWindow::showRemotePage()
{
    stack_->setCurrentWidget(connectionPage_);
    updateNavigation();
    connectionPage_->focusCode();
}

void AppWindow::showHostPage()
{
    stack_->setCurrentWidget(hostPanel_);
    updateNavigation();
    hostPanel_->activate();
}

QWidget *AppWindow::currentPage() const
{
    return stack_->currentWidget();
}

void AppWindow::showToast(const QString &message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    if (auto *opacity =
            qobject_cast<QGraphicsOpacityEffect *>(toast_->graphicsEffect())) {
        opacity->setOpacity(1.0);
    }
    toast_->setText(message);
    toast_->adjustSize();
    positionToast();
    toast_->show();
    toast_->raise();
    toastTimer_->start();
}

void AppWindow::updateViewerState()
{
    switch (viewer_->state()) {
    case ViewerController::State::Idle:
        if (!connectionPage_->connected()) {
            connectionPage_->setBusy(false);
            connectionPage_->setStatus(QStringLiteral("准备连接"));
        }
        break;
    case ViewerController::State::Connecting:
        connectionPage_->setConnected(false);
        connectionPage_->setBusy(true);
        connectionPage_->setStatus(
            viewer_->peerName().isEmpty()
                ? QStringLiteral("正在寻找并连接远端设备…")
                : QStringLiteral("正在与 %1 建立加密通道…")
                      .arg(viewer_->peerName()),
            PillLabel::Tone::Amber);
        break;
    case ViewerController::State::Streaming:
        connectionPage_->setConnected(true, viewer_->peerName());
        break;
    case ViewerController::State::Error:
        connectionPage_->setError(
            viewer_->lastError().isEmpty()
                ? QStringLiteral("连接失败，请稍后重试")
                : viewer_->lastError());
        break;
    }
}

void AppWindow::updateNavigation()
{
    const bool hostPage = stack_->currentWidget() == hostPanel_;
    setNavChecked(remoteNavButton_, !hostPage);
    setNavChecked(hostNavButton_, hostPage);
}

void AppWindow::positionToast()
{
    if (!toast_ || toast_->isHidden()) {
        return;
    }
    toast_->move(
        (width() - toast_->width()) / 2,
        height() - toast_->height() - 28);
}

void AppWindow::resizeEvent(QResizeEvent *event)
{
    BackdropWidget::resizeEvent(event);
    positionToast();
}

} // namespace pd::ui
