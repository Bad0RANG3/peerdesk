#pragma once

#include "ui/ui_widgets.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace pd {
class HostController;
class ViewerController;
} // namespace pd

namespace pd::ui {

class ConnectionPage;
class HostPanel;
class PillLabel;

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
    [[nodiscard]] QPushButton *remoteNavButton() const
    {
        return remoteNavButton_;
    }
    [[nodiscard]] QPushButton *hostNavButton() const
    {
        return hostNavButton_;
    }
    [[nodiscard]] QWidget *currentPage() const;

public slots:
    void showRemotePage();
    void showHostPage();
    void showToast(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    void connectSignals();
    void updateViewerState();
    void updateNavigation();
    void positionToast();

    HostController *host_ = nullptr;
    ViewerController *viewer_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    ConnectionPage *connectionPage_ = nullptr;
    HostPanel *hostPanel_ = nullptr;
    QPushButton *remoteNavButton_ = nullptr;
    QPushButton *hostNavButton_ = nullptr;
    PillLabel *privacyPill_ = nullptr;
    QLabel *toast_ = nullptr;
    QTimer *toastTimer_ = nullptr;
};

} // namespace pd::ui
