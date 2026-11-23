#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QWidget>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QLabel;
class QMenu;
class QMouseEvent;
class QPaintEvent;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QToolButton;
class QWheelEvent;

namespace pd {
class ViewerController;
} // namespace pd

namespace pd::ui {

class PillLabel;
class StatusDot;

class ViewerCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit ViewerCanvas(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void clearFrame();
    [[nodiscard]] bool hasFrame() const { return !frame_.isNull(); }
    [[nodiscard]] const QImage &frame() const { return frame_; }

    [[nodiscard]] QRectF frameRect() const;
    [[nodiscard]] QPointF normalizePoint(const QPointF &position) const;
    void setInputEnabled(bool enabled);
    [[nodiscard]] bool inputEnabled() const { return inputEnabled_; }
    [[nodiscard]] QStringList pressedKeys() const;
    void releaseInputs();

signals:
    void mouseMoveRequested(double x, double y);
    void mouseButtonRequested(
        const QString &action,
        double x,
        double y,
        const QString &button);
    void wheelRequested(int delta, bool horizontal);
    void keyRequested(const QString &code, bool down);
    void textRequested(const QString &text);
    void clipboardPasteRequested(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    bool event(QEvent *event) override;

private:
    void scheduleMove();
    void flushMove();
    void scheduleWheel();
    void flushWheel();
    void sendKeyDown(const QString &code);
    void sendKeyUp(const QString &code);
    [[nodiscard]] QString buttonName(Qt::MouseButton button) const;
    [[nodiscard]] QString keyCode(const QKeyEvent *event) const;

    QImage frame_;
    bool inputEnabled_ = false;
    QPointF pendingMove_;
    bool moveScheduled_ = false;
    int pendingWheelY_ = 0;
    int pendingWheelX_ = 0;
    bool wheelScheduled_ = false;
    QSet<QString> pressedKeys_;
    QSet<QString> pressedButtons_;
    QPointF lastPoint_;
    QTimer *moveTimer_ = nullptr;
    QTimer *wheelTimer_ = nullptr;
};

class ViewerPage final : public QWidget {
    Q_OBJECT

public:
    explicit ViewerPage(
        ViewerController *controller,
        QWidget *parent = nullptr);

    void beginSession(const QString &peerName, const QString &code = {});
    void showConnecting(
        const QString &peerName,
        const QString &note = {});
    void showStreaming(const QString &peerName);
    void showReconnecting();
    void showFailed(const QString &reason);
    void showEnded(const QString &reason);
    void resetSession();
    void setPeerName(const QString &peerName);
    void setLinkState(const QString &state);
    void showDemoFrame(
        const QImage &frame,
        const QString &peerName = QStringLiteral("STUDIO-PC"),
        const QJsonArray &monitors = QJsonArray());
    void showToast(const QString &message);
    void sendTextToRemote(const QString &text);
    void sendLocalFiles(const QStringList &paths);
    void setToolbarAutoHideInterval(int milliseconds);

    [[nodiscard]] bool sessionLive() const { return live_; }
    [[nodiscard]] bool demoMode() const { return demoMode_; }
    [[nodiscard]] ViewerCanvas *canvas() const { return canvas_; }
    [[nodiscard]] QWidget *toolbar() const { return toolbar_; }
    [[nodiscard]] QWidget *hud() const { return hud_; }
    [[nodiscard]] QWidget *connectionOverlay() const { return overlay_; }
    [[nodiscard]] QToolButton *qualityButton() const { return qualityButton_; }
    [[nodiscard]] QToolButton *monitorButton() const { return monitorButton_; }
    [[nodiscard]] QToolButton *clipboardButton() const
    {
        return clipboardButton_;
    }
    [[nodiscard]] QToolButton *textButton() const { return textButton_; }
    [[nodiscard]] QToolButton *fileButton() const { return fileButton_; }
    [[nodiscard]] QToolButton *cadButton() const { return cadButton_; }
    [[nodiscard]] QToolButton *audioButton() const { return audioButton_; }
    [[nodiscard]] QToolButton *fullScreenButton() const
    {
        return fullScreenButton_;
    }
    [[nodiscard]] QToolButton *hudButton() const { return hudButton_; }
    [[nodiscard]] QToolButton *hangupButton() const { return hangupButton_; }
    [[nodiscard]] QPushButton *backButton() const { return backButton_; }
    [[nodiscard]] QMenu *qualityMenu() const { return qualityMenu_; }
    [[nodiscard]] QMenu *monitorMenu() const { return monitorMenu_; }
    [[nodiscard]] QTimer *toolbarTimer() const { return toolbarTimer_; }
    [[nodiscard]] QLabel *overlayTitle() const { return overlayTitle_; }
    [[nodiscard]] QLabel *overlayNote() const { return overlayNote_; }
    [[nodiscard]] QLabel *peerLabel() const { return peerLabel_; }
    [[nodiscard]] QLabel *codeLabel() const { return codeLabel_; }
    [[nodiscard]] QLabel *resolutionLabel() const
    {
        return resolutionValue_;
    }
    [[nodiscard]] QLabel *fpsLabel() const { return fpsValue_; }
    [[nodiscard]] QLabel *bitrateLabel() const { return bitrateValue_; }
    [[nodiscard]] QLabel *latencyLabel() const { return latencyValue_; }
    [[nodiscard]] QLabel *audioLabel() const { return audioValue_; }
    [[nodiscard]] QLabel *transferLabel() const { return transferLabel_; }
    [[nodiscard]] QLabel *demoBadge() const { return demoBadge_; }
    [[nodiscard]] QWidget *textOverlay() const { return textOverlay_; }
    [[nodiscard]] QWidget *dropOverlay() const { return dropOverlay_; }
    [[nodiscard]] QString lastPeerName() const { return peerName_; }

public slots:
    void toggleFullScreen();
    void openTextInput();

signals:
    void toastRequested(const QString &message);
    void hangupRequested();
    void backRequested();
    void fullScreenRequested(bool enter);
    void exitFullScreenRequested();
    void textSendRequested(const QString &text);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void connectSignals();
    void buildQualityMenu();
    void updateMonitors();
    void updatePeerUi();
    void updateLinkUi();
    void updateStatisticsUi();
    void updateAudioUi();
    void updateResolutionUi();
    void updateToolbarVisibility();
    void showOverlay(
        const QString &title,
        const QString &note,
        bool showBack);
    void hideOverlay();
    void wakeToolbar();
    void scheduleToolbarHide();
    void setMenuPinned(bool pinned);
    void flushFileQueue();
    void installMenuPin(QMenu *menu);
    void layoutFloatingWidgets();

    ViewerController *controller_ = nullptr;
    ViewerCanvas *canvas_ = nullptr;
    QWidget *overlay_ = nullptr;
    QLabel *overlayTitle_ = nullptr;
    QLabel *overlayNote_ = nullptr;
    StatusDot *overlayDot_ = nullptr;
    QPushButton *backButton_ = nullptr;

    QWidget *toolbar_ = nullptr;
    QToolButton *qualityButton_ = nullptr;
    QToolButton *monitorButton_ = nullptr;
    QToolButton *clipboardButton_ = nullptr;
    QToolButton *textButton_ = nullptr;
    QToolButton *fileButton_ = nullptr;
    QToolButton *cadButton_ = nullptr;
    QToolButton *audioButton_ = nullptr;
    QToolButton *fullScreenButton_ = nullptr;
    QToolButton *hudButton_ = nullptr;
    QToolButton *hangupButton_ = nullptr;
    QMenu *qualityMenu_ = nullptr;
    QMenu *monitorMenu_ = nullptr;
    QTimer *toolbarTimer_ = nullptr;

    QWidget *hud_ = nullptr;
    QLabel *peerLabel_ = nullptr;
    QLabel *codeLabel_ = nullptr;
    PillLabel *linkPill_ = nullptr;
    QLabel *resolutionValue_ = nullptr;
    QLabel *fpsValue_ = nullptr;
    QLabel *bitrateValue_ = nullptr;
    QLabel *latencyValue_ = nullptr;
    QLabel *audioValue_ = nullptr;

    QWidget *dropOverlay_ = nullptr;
    QWidget *textOverlay_ = nullptr;
    QPlainTextEdit *textInput_ = nullptr;
    QPushButton *textSendButton_ = nullptr;
    QLabel *transferLabel_ = nullptr;
    QLabel *demoBadge_ = nullptr;

    QString peerName_;
    QString sessionCode_;
    QString linkState_;
    QStringList pendingFiles_;
    QJsonObject info_;
    QJsonObject statistics_;
    QJsonObject audioState_;
    QString currentFileName_;
    int lastFilePercent_ = -1;
    int toolbarAutoHideMs_ = 3000;
    bool live_ = false;
    bool demoMode_ = false;
    bool fileBusy_ = false;
    bool hudVisible_ = true;
    bool menuPinned_ = false;
    bool lastAudioEnabled_ = true;
    bool dragActive_ = false;
};

} // namespace pd::ui
