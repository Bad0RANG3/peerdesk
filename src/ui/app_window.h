#pragma once

#include "ui/ui_widgets.h"

#include <memory>

#include <QWidget>

class QLabel;
class QCloseEvent;
class QHideEvent;
class QMenu;
class QMoveEvent;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class QTimer;

namespace pd {
class HostController;
class Preferences;
class ViewerController;
} // namespace pd

namespace pd::ui {

class ConnectionPage;
class HostPanel;
class PillLabel;
class SettingsDialog;
class ViewerPage;

class AppWindow final : public BackdropWidget {
    Q_OBJECT

public:
    explicit AppWindow(
        HostController *host,
        ViewerController *viewer,
        QWidget *parent = nullptr);

    [[nodiscard]] ConnectionPage *connectionPage() const
    {
        return connectionPage_;
    }
    [[nodiscard]] HostPanel *hostPanel() const { return hostPanel_; }
    [[nodiscard]] ViewerPage *viewerPage() const { return viewerPage_; }
    [[nodiscard]] QWidget *headerWidget() const { return header_; }
    [[nodiscard]] QPushButton *settingsButton() const
    {
        return settingsButton_;
    }
    [[nodiscard]] QSystemTrayIcon *trayIcon() const { return trayIcon_; }
    [[nodiscard]] SettingsDialog *settingsDialog() const
    {
        return settingsDialog_;
    }
    [[nodiscard]] bool trayAvailable() const;
    [[nodiscard]] QPushButton *remoteNavButton() const
    {
        return remoteNavButton_;
    }
    [[nodiscard]] QPushButton *hostNavButton() const
    {
        return hostNavButton_;
    }
    [[nodiscard]] QWidget *currentPage() const;
    void setTrayAvailableForTesting(bool available);

public slots:
    void showRemotePage();
    void showHostPage();
    void showViewerPage();
    void showMainWindow();
    void showSettings();
    void showToast(const QString &message);
    void saveWindowState();
    void requestQuit();

signals:
    void quitRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void buildUi();
    void buildTray();
    void connectSignals();
    void restoreWindowState();
    void centerOnPrimaryScreen();
    void ensureSettingsDialog();
    void scheduleWindowStateSave();
    void refreshRecentConnections();
    void rememberCurrentConnection();
    [[nodiscard]] bool prepareTray();
    void updateViewerState();
    void updateNavigation();
    void positionToast();
    void setFullScreen(bool enter);
    void syncFullScreenState();

    HostController *host_ = nullptr;
    ViewerController *viewer_ = nullptr;
    std::shared_ptr<Preferences> preferences_;
    QStackedWidget *stack_ = nullptr;
    QWidget *header_ = nullptr;
    ConnectionPage *connectionPage_ = nullptr;
    HostPanel *hostPanel_ = nullptr;
    ViewerPage *viewerPage_ = nullptr;
    SettingsDialog *settingsDialog_ = nullptr;
    QSystemTrayIcon *trayIcon_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QPushButton *remoteNavButton_ = nullptr;
    QPushButton *hostNavButton_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    PillLabel *privacyPill_ = nullptr;
    QLabel *toast_ = nullptr;
    QTimer *toastTimer_ = nullptr;
    QTimer *windowSaveTimer_ = nullptr;
    bool wasMaximized_ = false;
    bool wasMaximizedBeforeHide_ = false;
    bool quitting_ = false;
    bool restoringWindow_ = false;
    bool trayAvailabilityOverridden_ = false;
    bool trayAvailableForTesting_ = false;
    bool trayHintShown_ = false;
};

} // namespace pd::ui
