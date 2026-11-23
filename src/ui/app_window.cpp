#include "ui/app_window.h"

#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/viewer_controller.h"
#include "ui/connection_page.h"
#include "ui/host_panel.h"
#include "ui/settings_dialog.h"
#include "ui/ui_theme.h"
#include "ui/viewer_page.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMoveEvent>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace pd::ui {
namespace {

constexpr int kHeaderHeight = 72;
constexpr int kWindowSaveDelayMs = 400;

QIcon makeWindowIcon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, 0, 64);
    gradient.setColorAt(0.0, QColor(QStringLiteral("#5b9bff")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#3d7bfd")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 17, 17);
    QFont font = uiFont(24, QFont::Bold);
    font.setPixelSize(35);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(
        QRectF(4, 1, 56, 56),
        Qt::AlignCenter,
        QStringLiteral("P"));
    return QIcon(pixmap);
}

bool intersectsAnyScreen(const QRect &geometry)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    return std::any_of(
        screens.cbegin(),
        screens.cend(),
        [&geometry](const QScreen *screen) {
            return screen
                && screen->availableGeometry().intersects(geometry);
        });
}

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
    , preferences_(
          host_->preferences() ? host_->preferences()
                               : viewer_->preferences())
{
    Q_ASSERT(host_);
    Q_ASSERT(viewer_);
    setObjectName(QStringLiteral("appWindow"));
    setWindowTitle(QStringLiteral("PeerDesk · 对端桌面"));
    setWindowIcon(makeWindowIcon());
    setMinimumSize(820, 640);
    resize(1080, 840);
    buildUi();
    buildTray();
    connectSignals();
    restoringWindow_ = true;
    restoreWindowState();
    restoringWindow_ = false;
    if (windowSaveTimer_) {
        windowSaveTimer_->stop();
    }
    updateViewerState();
    updateNavigation();
}

void AppWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("appHeader"));
    header_->setFixedHeight(kHeaderHeight);
    header_->setStyleSheet(QStringLiteral(
        "QWidget#appHeader {"
        "  background: rgba(11, 13, 18, 0.72);"
        "  border-bottom: 1px solid rgba(255, 255, 255, 0.065);"
        "}"));
    auto *headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(24, 0, 24, 0);
    headerLayout->setSpacing(18);

    auto *brand = new BrandMark(header_);
    headerLayout->addWidget(brand, 0, Qt::AlignVCenter);

    auto *brandText = new QWidget(header_);
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

    auto *navigation = new QWidget(header_);
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
    settingsButton_ = new QPushButton(QStringLiteral("设置"), header_);
    settingsButton_->setObjectName(
        QStringLiteral("headerSettingsButton"));
    settingsButton_->setCursor(Qt::PointingHandCursor);
    settingsButton_->setMinimumHeight(31);
    settingsButton_->setAccessibleName(QStringLiteral("设置"));
    settingsButton_->setToolTip(
        QStringLiteral("连接、画质、文件与窗口设置"));
    headerLayout->addWidget(settingsButton_, 0, Qt::AlignVCenter);
    privacyPill_ = new PillLabel(header_);
    privacyPill_->setText(QStringLiteral("端到端加密"));
    privacyPill_->setTone(PillLabel::Tone::Green);
    headerLayout->addWidget(privacyPill_, 0, Qt::AlignVCenter);
    root->addWidget(header_);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("pageStack"));
    stack_->setStyleSheet(QStringLiteral(
        "QStackedWidget#pageStack { background: transparent; }"));
    connectionPage_ = new ConnectionPage(stack_);
    hostPanel_ = new HostPanel(host_, true, stack_);
    viewerPage_ = new ViewerPage(viewer_, stack_);
    stack_->addWidget(connectionPage_);
    stack_->addWidget(hostPanel_);
    stack_->addWidget(viewerPage_);
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

void AppWindow::buildTray()
{
    trayMenu_ = new QMenu(this);

    auto *showAction = trayMenu_->addAction(
        QStringLiteral("显示 PeerDesk"));
    connect(
        showAction,
        &QAction::triggered,
        this,
        &AppWindow::showMainWindow);

    auto *remoteAction = trayMenu_->addAction(
        QStringLiteral("远程协助"));
    connect(remoteAction, &QAction::triggered, this, [this]() {
        showMainWindow();
        showRemotePage();
    });

    auto *hostAction = trayMenu_->addAction(
        QStringLiteral("本机被控"));
    connect(hostAction, &QAction::triggered, this, [this]() {
        showMainWindow();
        showHostPage();
    });

    auto *settingsAction = trayMenu_->addAction(
        QStringLiteral("设置"));
    connect(
        settingsAction,
        &QAction::triggered,
        this,
        &AppWindow::showSettings);

    trayMenu_->addSeparator();
    auto *quitAction = trayMenu_->addAction(
        QStringLiteral("退出 PeerDesk"));
    connect(
        quitAction,
        &QAction::triggered,
        this,
        &AppWindow::requestQuit);

    trayIcon_ = new QSystemTrayIcon(windowIcon(), this);
    trayIcon_->setToolTip(QStringLiteral("PeerDesk · 对端桌面"));
    trayIcon_->setContextMenu(trayMenu_);
    connect(
        trayIcon_,
        &QSystemTrayIcon::activated,
        this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger
                || reason == QSystemTrayIcon::DoubleClick) {
                showMainWindow();
            }
        });
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon_->show();
    }
}

void AppWindow::connectSignals()
{
    windowSaveTimer_ = new QTimer(this);
    windowSaveTimer_->setSingleShot(true);
    windowSaveTimer_->setInterval(kWindowSaveDelayMs);
    connect(
        windowSaveTimer_,
        &QTimer::timeout,
        this,
        &AppWindow::saveWindowState);

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
        settingsButton_,
        &QPushButton::clicked,
        this,
        &AppWindow::showSettings);
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
                return;
            }
            const QString peer = viewer_->peerName().isEmpty()
                ? QStringLiteral("远端设备")
                : viewer_->peerName();
            viewerPage_->beginSession(peer, code);
            viewerPage_->showConnecting(peer);
            showViewerPage();
        });
    connect(
        connectionPage_,
        &ConnectionPage::disconnectRequested,
        this,
        [this]() {
            viewer_->hangUp();
            viewerPage_->resetSession();
            connectionPage_->setConnected(false);
            showToast(QStringLiteral("远程会话已断开"));
        });
    connect(
        viewer_,
        &ViewerController::stateChanged,
        this,
        [this](ViewerController::State) {
            updateViewerState();
            updateNavigation();
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
            if (viewerPage_->connectionOverlay()->isVisible()) {
                showViewerPage();
            }
        });
    connect(
        viewer_,
        &ViewerController::sessionEnded,
        this,
        [this](const QString &reason) {
            connectionPage_->setConnected(false);
            connectionPage_->setStatus(reason, PillLabel::Tone::Amber);
            showToast(reason);
            showViewerPage();
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
    connect(
        viewerPage_,
        &ViewerPage::toastRequested,
        this,
        &AppWindow::showToast);
    connect(
        viewerPage_,
        &ViewerPage::hangupRequested,
        this,
        [this]() {
            viewer_->hangUp();
            viewerPage_->resetSession();
            connectionPage_->setConnected(false);
            connectionPage_->setStatus(QStringLiteral("远程会话已断开"));
            showRemotePage();
            showToast(QStringLiteral("远程会话已断开"));
        });
    connect(
        viewerPage_,
        &ViewerPage::backRequested,
        this,
        [this]() {
            viewer_->hangUp();
            viewerPage_->resetSession();
            connectionPage_->setConnected(false);
            connectionPage_->setStatus(QStringLiteral("准备连接"));
            showRemotePage();
        });
    connect(
        viewerPage_,
        &ViewerPage::fullScreenRequested,
        this,
        &AppWindow::setFullScreen);
    connect(
        viewerPage_,
        &ViewerPage::exitFullScreenRequested,
        this,
        [this]() {
            setFullScreen(false);
        });
    connect(
        viewerPage_,
        &ViewerPage::textSendRequested,
        this,
        [this](const QString &text) {
            if (!text.isEmpty()) {
                showToast(QStringLiteral("文本已发送到远端"));
            }
        });
}

void AppWindow::showRemotePage()
{
    if (viewerPage_ && viewerPage_->sessionLive()) {
        showViewerPage();
        return;
    }
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

void AppWindow::showViewerPage()
{
    stack_->setCurrentWidget(viewerPage_);
    updateNavigation();
    if (viewerPage_->sessionLive()
        && viewerPage_->connectionOverlay()->isHidden()) {
        viewerPage_->canvas()->setFocus(Qt::OtherFocusReason);
    }
}

QWidget *AppWindow::currentPage() const
{
    return stack_->currentWidget();
}

bool AppWindow::trayAvailable() const
{
    if (trayAvailabilityOverridden_) {
        return trayAvailableForTesting_;
    }
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void AppWindow::setTrayAvailableForTesting(bool available)
{
    trayAvailabilityOverridden_ = true;
    trayAvailableForTesting_ = available;
    if (!available && trayIcon_) {
        trayIcon_->hide();
    }
}

void AppWindow::showMainWindow()
{
    const bool maximized =
        wasMaximizedBeforeHide_
        || windowState().testFlag(Qt::WindowMaximized);
    if (!isVisible() || isMinimized()) {
        if (maximized) {
            showMaximized();
        } else {
            showNormal();
        }
    }
    wasMaximizedBeforeHide_ = false;
    raise();
    activateWindow();
}

void AppWindow::showSettings()
{
    showMainWindow();
    ensureSettingsDialog();
    settingsDialog_->reload();
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}

void AppWindow::ensureSettingsDialog()
{
    if (settingsDialog_) {
        return;
    }
    settingsDialog_ = new SettingsDialog(
        host_,
        viewer_,
        preferences_,
        connectionPage_,
        this);
    connect(
        settingsDialog_,
        &SettingsDialog::settingsSaved,
        this,
        [this]() {
            showToast(QStringLiteral("设置已保存"));
        });
}

void AppWindow::saveWindowState()
{
    if (!preferences_) {
        return;
    }
    if (windowSaveTimer_) {
        windowSaveTimer_->stop();
    }

    WindowPreferences state = preferences_->window();
    const Qt::WindowStates windowState = this->windowState();
    if (windowState.testFlag(Qt::WindowFullScreen)) {
        state.maximized = wasMaximized_;
    } else if (windowState.testFlag(Qt::WindowMaximized)) {
        state.maximized = true;
    } else if (!windowState.testFlag(Qt::WindowMinimized)) {
        state.geometry = saveGeometry();
        state.maximized = false;
    }
    preferences_->setWindow(state);
}

void AppWindow::requestQuit()
{
    if (quitting_) {
        return;
    }
    quitting_ = true;
    saveWindowState();
    if (!close()) {
        hide();
    }
    emit quitRequested();
}

void AppWindow::restoreWindowState()
{
    if (!preferences_) {
        centerOnPrimaryScreen();
        return;
    }

    const WindowPreferences state = preferences_->window();
    bool restored = false;
    if (!state.geometry.isEmpty()) {
        restored = restoreGeometry(state.geometry);
    }
    if (!restored || !intersectsAnyScreen(frameGeometry())) {
        centerOnPrimaryScreen();
    }
    if (state.maximized) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void AppWindow::centerOnPrimaryScreen()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const QRect available = screen
        ? screen->availableGeometry()
        : QRect(0, 0, 1080, 840);
    resize(
        std::min(width(), available.width()),
        std::min(height(), available.height()));
    const QRect frame = frameGeometry();
    move(
        available.center().x() - frame.width() / 2,
        available.center().y() - frame.height() / 2);
}

void AppWindow::scheduleWindowStateSave()
{
    if (restoringWindow_
        || !windowSaveTimer_
        || !isVisible()
        || isMinimized()
        || isMaximized()
        || isFullScreen()) {
        return;
    }
    windowSaveTimer_->start();
}

bool AppWindow::prepareTray()
{
    if (!trayAvailable() || !trayIcon_) {
        return false;
    }
    if (trayAvailabilityOverridden_) {
        return true;
    }
    trayIcon_->show();
    return trayIcon_->isVisible();
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
            if (!viewerPage_->sessionLive()) {
                connectionPage_->setStatus(QStringLiteral("准备连接"));
            }
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
        if (viewerPage_->sessionLive() && !viewerPage_->demoMode()) {
            viewerPage_->showConnecting(
                viewer_->peerName().isEmpty()
                    ? QStringLiteral("远端设备")
                    : viewer_->peerName());
        }
        break;
    case ViewerController::State::Streaming:
        connectionPage_->setConnected(true, viewer_->peerName());
        if (!viewerPage_->sessionLive()) {
            viewerPage_->beginSession(
                viewer_->peerName().isEmpty()
                    ? QStringLiteral("远端设备")
                    : viewer_->peerName(),
                viewer_->code());
        }
        viewerPage_->showStreaming(viewer_->peerName());
        if (stack_->currentWidget() == connectionPage_) {
            showViewerPage();
        }
        break;
    case ViewerController::State::Reconnecting:
        connectionPage_->setStatus(
            QStringLiteral("连接中断，正在恢复…"),
            PillLabel::Tone::Amber);
        viewerPage_->showReconnecting();
        if (stack_->currentWidget() == connectionPage_) {
            showViewerPage();
        }
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

void AppWindow::setFullScreen(bool enter)
{
    if (enter == isFullScreen()) {
        syncFullScreenState();
        return;
    }
    viewerPage_->canvas()->releaseInputs();
    if (enter) {
        wasMaximized_ = isMaximized();
        showFullScreen();
    } else if (wasMaximized_) {
        showMaximized();
    } else {
        showNormal();
    }
    syncFullScreenState();
}

void AppWindow::syncFullScreenState()
{
    const bool fullScreen = isFullScreen();
    if (header_) {
        header_->setVisible(!fullScreen);
    }
    if (fullScreen) {
        viewerPage_->canvas()->releaseInputs();
    }
    if (viewerPage_ && viewerPage_->fullScreenButton()) {
        viewerPage_->fullScreenButton()->setText(
            fullScreen ? QStringLiteral("退出全屏")
                       : QStringLiteral("全屏"));
    }
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
    scheduleWindowStateSave();
}

void AppWindow::moveEvent(QMoveEvent *event)
{
    BackdropWidget::moveEvent(event);
    scheduleWindowStateSave();
}

void AppWindow::changeEvent(QEvent *event)
{
    BackdropWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && viewerPage_) {
        syncFullScreenState();
    }
}

void AppWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (!quitting_) {
        saveWindowState();
    }
}

void AppWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    if (quitting_) {
        event->accept();
        return;
    }

    const bool closeToTray = preferences_
        ? preferences_->window().closeToTray
        : true;
    if (closeToTray && prepareTray()) {
        wasMaximizedBeforeHide_ =
            windowState().testFlag(Qt::WindowMaximized);
        if (settingsDialog_) {
            settingsDialog_->hide();
        }
        event->ignore();
        hide();
        if (!trayAvailabilityOverridden_
            && !trayHintShown_
            && QSystemTrayIcon::supportsMessages()) {
            trayHintShown_ = true;
            trayIcon_->showMessage(
                QStringLiteral("PeerDesk 仍在运行"),
                QStringLiteral("远程协助与本机被控会保持在线，可从托盘恢复或退出。"),
                QSystemTrayIcon::Information,
                3500);
        }
        return;
    }

    quitting_ = true;
    event->accept();
    emit quitRequested();
}

} // namespace pd::ui
