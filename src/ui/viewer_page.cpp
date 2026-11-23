#include "ui/viewer_page.h"

#include "app/quality_presets.h"
#include "app/viewer_controller.h"
#include "ui/ui_theme.h"
#include "ui/ui_widgets.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace pd::ui {
namespace {

constexpr int kMouseMoveIntervalMs = 16;
constexpr int kWheelStep = 120;

QString jsonString(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

double jsonNumber(
    const QJsonObject &object,
    const char *key,
    double fallback = 0.0)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toDouble() : fallback;
}

bool jsonBool(
    const QJsonObject &object,
    const char *key,
    bool fallback = false)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

QString formatSize(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB")
            .arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB")
        .arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

QString failureText(const QString &reason)
{
    if (reason == QStringLiteral("offline")) {
        return QStringLiteral("传输通道尚未建立");
    }
    if (reason == QStringLiteral("missing")) {
        return QStringLiteral("文件不存在");
    }
    if (reason == QStringLiteral("size")) {
        return QStringLiteral("文件为空或超过 512 MB");
    }
    if (reason == QStringLiteral("busy")) {
        return QStringLiteral("另一个文件仍在发送");
    }
    if (reason == QStringLiteral("cancelled")) {
        return QStringLiteral("传输已取消");
    }
    return reason.isEmpty() ? QStringLiteral("文件传输失败") : reason;
}

QToolButton *makeToolButton(
    const QString &text,
    const QString &objectName,
    QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(false);
    button->setMinimumHeight(34);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QLabel *makeHudCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("hudCaption"));
    label->setFont(uiFont(8));
    label->setStyleSheet(QStringLiteral("color: #69728a;"));
    return label;
}

QLabel *makeHudValue(QWidget *parent, const QString &objectName)
{
    auto *label = new QLabel(QStringLiteral("—"), parent);
    label->setObjectName(objectName);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setFont(monoFont(9, QFont::DemiBold));
    label->setStyleSheet(QStringLiteral("color: #e8eef9;"));
    return label;
}

QFrame *makeOverlayCard(QWidget *parent, const QString &objectName)
{
    auto *card = new QFrame(parent);
    card->setObjectName(objectName);
    card->setStyleSheet(QStringLiteral(
        "QFrame#%1 {"
        "  background: rgba(17, 21, 30, 0.98);"
        "  border: 1px solid rgba(255, 255, 255, 0.14);"
        "  border-radius: 18px;"
        "}").arg(objectName));
    return card;
}

} // namespace

ViewerCanvas::ViewerCanvas(QWidget *parent)
    : QWidget(parent)
    , moveTimer_(new QTimer(this))
    , wheelTimer_(new QTimer(this))
{
    setObjectName(QStringLiteral("viewerCanvas"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);

    moveTimer_->setSingleShot(true);
    moveTimer_->setInterval(kMouseMoveIntervalMs);
    connect(moveTimer_, &QTimer::timeout, this, &ViewerCanvas::flushMove);

    wheelTimer_->setSingleShot(true);
    wheelTimer_->setInterval(0);
    connect(wheelTimer_, &QTimer::timeout, this, &ViewerCanvas::flushWheel);
}

void ViewerCanvas::setFrame(const QImage &frame)
{
    frame_ = frame;
    update();
}

void ViewerCanvas::clearFrame()
{
    frame_ = QImage();
    update();
}

QRectF ViewerCanvas::frameRect() const
{
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return {};
    }
    const QSizeF available(width(), height());
    const QSizeF imageSize(frame_.size());
    if (available.isEmpty() || imageSize.isEmpty()) {
        return {};
    }
    const QSizeF scaled = imageSize.scaled(available, Qt::KeepAspectRatio);
    return QRectF(
        QPointF(
            (available.width() - scaled.width()) / 2.0,
            (available.height() - scaled.height()) / 2.0),
        scaled);
}

QPointF ViewerCanvas::normalizePoint(const QPointF &position) const
{
    const QRectF target = frameRect();
    if (!target.isValid() || target.width() <= 0.0 || target.height() <= 0.0) {
        return QPointF(0.5, 0.5);
    }
    const double x = std::clamp(
        (position.x() - target.left()) / target.width(),
        0.0,
        1.0);
    const double y = std::clamp(
        (position.y() - target.top()) / target.height(),
        0.0,
        1.0);
    return QPointF(x, y);
}

void ViewerCanvas::setInputEnabled(bool enabled)
{
    if (inputEnabled_ == enabled) {
        return;
    }
    if (!enabled) {
        releaseInputs();
    }
    inputEnabled_ = enabled;
    setCursor(enabled ? Qt::ArrowCursor : Qt::ArrowCursor);
    if (enabled) {
        setFocus(Qt::OtherFocusReason);
    }
}

QStringList ViewerCanvas::pressedKeys() const
{
    return QStringList(pressedKeys_.begin(), pressedKeys_.end());
}

void ViewerCanvas::releaseInputs()
{
    const QStringList keys(pressedKeys_.begin(), pressedKeys_.end());
    for (const QString &code : keys) {
        emit keyRequested(code, false);
    }
    pressedKeys_.clear();

    const QStringList buttons(pressedButtons_.begin(), pressedButtons_.end());
    const QPointF point = normalizePoint(lastPoint_);
    for (const QString &button : buttons) {
        emit mouseButtonRequested(
            QStringLiteral("up"),
            point.x(),
            point.y(),
            button);
    }
    pressedButtons_.clear();

    if (moveTimer_) {
        moveTimer_->stop();
    }
    moveScheduled_ = false;
    if (wheelTimer_) {
        wheelTimer_->stop();
    }
    wheelScheduled_ = false;
    pendingWheelX_ = 0;
    pendingWheelY_ = 0;
}

void ViewerCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#05070b")));
    const QRectF target = frameRect();
    if (frame_.isNull() || target.isEmpty()) {
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, frame_);
}

void ViewerCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!inputEnabled_) {
        event->ignore();
        return;
    }
    pendingMove_ = normalizePoint(event->position());
    lastPoint_ = event->position();
    if (!moveScheduled_) {
        moveScheduled_ = true;
        moveTimer_->start();
    }
    event->accept();
}

void ViewerCanvas::mousePressEvent(QMouseEvent *event)
{
    if (!inputEnabled_) {
        event->ignore();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    lastPoint_ = event->position();
    const QPointF point = normalizePoint(event->position());
    const QString button = buttonName(event->button());
    if (!button.isEmpty()) {
        pressedButtons_.insert(button);
        emit mouseButtonRequested(
            QStringLiteral("down"),
            point.x(),
            point.y(),
            button);
    }
    event->accept();
}

void ViewerCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    lastPoint_ = event->position();
    if (!inputEnabled_) {
        event->ignore();
        return;
    }
    const QString button = buttonName(event->button());
    if (!button.isEmpty()) {
        pressedButtons_.remove(button);
        const QPointF point = normalizePoint(event->position());
        emit mouseButtonRequested(
            QStringLiteral("up"),
            point.x(),
            point.y(),
            button);
    }
    event->accept();
}

void ViewerCanvas::wheelEvent(QWheelEvent *event)
{
    if (!inputEnabled_) {
        event->ignore();
        return;
    }
    const QPoint angle = event->angleDelta();
    const bool horizontal =
        event->modifiers().testFlag(Qt::ShiftModifier)
        || std::abs(angle.x()) > std::abs(angle.y());
    const int delta = horizontal
        ? (angle.x() != 0 ? angle.x() : angle.y())
        : (angle.y() != 0 ? angle.y() : angle.x());
    if (delta == 0) {
        event->ignore();
        return;
    }
    if (horizontal) {
        pendingWheelX_ += delta;
    } else {
        pendingWheelY_ += delta;
    }
    scheduleWheel();
    event->accept();
}

void ViewerCanvas::keyPressEvent(QKeyEvent *event)
{
    if (!inputEnabled_) {
        event->ignore();
        return;
    }
    if (event->isAutoRepeat()) {
        const QString repeatCode = keyCode(event);
        const bool repeatable =
            repeatCode == QStringLiteral("Backspace")
            || repeatCode == QStringLiteral("Delete")
            || repeatCode.startsWith(QStringLiteral("Arrow"));
        if (repeatable) {
            emit keyRequested(repeatCode, true);
            event->accept();
        } else {
            event->accept();
        }
        return;
    }

    const bool paste =
        event->key() == Qt::Key_V
        && event->modifiers().testFlag(Qt::ControlModifier)
        && !event->modifiers().testFlag(Qt::AltModifier)
        && !event->modifiers().testFlag(Qt::MetaModifier);
    if (paste) {
        const QString text = QApplication::clipboard()->text();
        emit clipboardPasteRequested(text);
        event->accept();
        return;
    }

    sendKeyDown(keyCode(event));
    event->accept();
}

void ViewerCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    sendKeyUp(keyCode(event));
    event->accept();
}

void ViewerCanvas::focusOutEvent(QFocusEvent *event)
{
    releaseInputs();
    QWidget::focusOutEvent(event);
}

void ViewerCanvas::inputMethodEvent(QInputMethodEvent *event)
{
    if (inputEnabled_ && !event->commitString().isEmpty()) {
        emit textRequested(event->commitString());
        event->accept();
        return;
    }
    QWidget::inputMethodEvent(event);
}

QVariant ViewerCanvas::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        return inputEnabled_;
    }
    if (query == Qt::ImCursorRectangle) {
        return frameRect().toRect();
    }
    return QWidget::inputMethodQuery(query);
}

bool ViewerCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride && inputEnabled_) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::WindowDeactivate) {
        releaseInputs();
    }
    if (event->type() == QEvent::MouseButtonDblClick && inputEnabled_) {
        mousePressEvent(static_cast<QMouseEvent *>(event));
        return true;
    }
    return QWidget::event(event);
}

void ViewerCanvas::scheduleMove()
{
    if (!moveScheduled_) {
        moveScheduled_ = true;
        moveTimer_->start();
    }
}

void ViewerCanvas::flushMove()
{
    moveScheduled_ = false;
    if (!inputEnabled_) {
        return;
    }
    emit mouseMoveRequested(pendingMove_.x(), pendingMove_.y());
}

void ViewerCanvas::scheduleWheel()
{
    if (!wheelScheduled_) {
        wheelScheduled_ = true;
        wheelTimer_->start();
    }
}

void ViewerCanvas::flushWheel()
{
    wheelScheduled_ = false;
    if (!inputEnabled_) {
        pendingWheelX_ = 0;
        pendingWheelY_ = 0;
        return;
    }
    while (std::abs(pendingWheelY_) >= kWheelStep) {
        const int delta =
            pendingWheelY_ > 0 ? kWheelStep : -kWheelStep;
        emit wheelRequested(delta, false);
        pendingWheelY_ -= delta;
    }
    while (std::abs(pendingWheelX_) >= kWheelStep) {
        const int delta =
            pendingWheelX_ > 0 ? kWheelStep : -kWheelStep;
        emit wheelRequested(delta, true);
        pendingWheelX_ -= delta;
    }
}

void ViewerCanvas::sendKeyDown(const QString &code)
{
    if (code.isEmpty() || pressedKeys_.contains(code)) {
        return;
    }
    pressedKeys_.insert(code);
    emit keyRequested(code, true);
}

void ViewerCanvas::sendKeyUp(const QString &code)
{
    if (code.isEmpty() || !pressedKeys_.remove(code)) {
        return;
    }
    emit keyRequested(code, false);
}

QString ViewerCanvas::buttonName(Qt::MouseButton button) const
{
    switch (button) {
    case Qt::LeftButton:
        return QStringLiteral("left");
    case Qt::MiddleButton:
        return QStringLiteral("middle");
    case Qt::RightButton:
        return QStringLiteral("right");
    case Qt::BackButton:
        return QStringLiteral("back");
    case Qt::ForwardButton:
        return QStringLiteral("forward");
    default:
        return {};
    }
}

QString ViewerCanvas::keyCode(const QKeyEvent *event) const
{
    const int key = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QStringLiteral("Key%1")
            .arg(QChar(QLatin1Char('A' + (key - Qt::Key_A))));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        const QChar digit(QLatin1Char('0' + (key - Qt::Key_0)));
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("Numpad%1").arg(digit)
            : QStringLiteral("Digit%1").arg(digit);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    }

    switch (key) {
    case Qt::Key_Escape:
        return QStringLiteral("Escape");
    case Qt::Key_Tab:
        return QStringLiteral("Tab");
    case Qt::Key_Backtab:
        return QStringLiteral("Tab");
    case Qt::Key_Backspace:
        return QStringLiteral("Backspace");
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadEnter")
            : QStringLiteral("Enter");
    case Qt::Key_Insert:
        return QStringLiteral("Insert");
    case Qt::Key_Delete:
        return QStringLiteral("Delete");
    case Qt::Key_Home:
        return QStringLiteral("Home");
    case Qt::Key_End:
        return QStringLiteral("End");
    case Qt::Key_PageUp:
        return QStringLiteral("PageUp");
    case Qt::Key_PageDown:
        return QStringLiteral("PageDown");
    case Qt::Key_Left:
        return QStringLiteral("ArrowLeft");
    case Qt::Key_Right:
        return QStringLiteral("ArrowRight");
    case Qt::Key_Up:
        return QStringLiteral("ArrowUp");
    case Qt::Key_Down:
        return QStringLiteral("ArrowDown");
    case Qt::Key_Shift:
        if (event->nativeVirtualKey() == 0xA1
            || event->nativeVirtualKey() == 0xA1 + 0x100) {
            return QStringLiteral("ShiftRight");
        }
        return QStringLiteral("ShiftLeft");
    case Qt::Key_Control:
        if (event->nativeVirtualKey() == 0xA3
            || event->nativeVirtualKey() == 0xA3 + 0x100) {
            return QStringLiteral("ControlRight");
        }
        return QStringLiteral("ControlLeft");
    case Qt::Key_Alt:
        if (event->nativeVirtualKey() == 0xA5
            || event->nativeVirtualKey() == 0xA5 + 0x100) {
            return QStringLiteral("AltRight");
        }
        return QStringLiteral("AltLeft");
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
        return QStringLiteral("MetaLeft");
    case Qt::Key_Super_R:
        return QStringLiteral("MetaRight");
    case Qt::Key_Menu:
        return QStringLiteral("ContextMenu");
    case Qt::Key_CapsLock:
        return QStringLiteral("CapsLock");
    case Qt::Key_NumLock:
        return QStringLiteral("NumLock");
    case Qt::Key_ScrollLock:
        return QStringLiteral("ScrollLock");
    case Qt::Key_Print:
        return QStringLiteral("PrintScreen");
    case Qt::Key_Pause:
        return QStringLiteral("Pause");
    case Qt::Key_Space:
        return QStringLiteral("Space");
    case Qt::Key_Minus:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadSubtract")
            : QStringLiteral("Minus");
    case Qt::Key_Plus:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadAdd")
            : QStringLiteral("Equal");
    case Qt::Key_Equal:
        return QStringLiteral("Equal");
    case Qt::Key_Asterisk:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadMultiply")
            : QStringLiteral("Digit8");
    case Qt::Key_Slash:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadDivide")
            : QStringLiteral("Slash");
    case Qt::Key_Period:
        return modifiers.testFlag(Qt::KeypadModifier)
            ? QStringLiteral("NumpadDecimal")
            : QStringLiteral("Period");
    case Qt::Key_Comma:
        return QStringLiteral("Comma");
    case Qt::Key_Semicolon:
        return QStringLiteral("Semicolon");
    case Qt::Key_Apostrophe:
        return QStringLiteral("Quote");
    case Qt::Key_BracketLeft:
        return QStringLiteral("BracketLeft");
    case Qt::Key_BracketRight:
        return QStringLiteral("BracketRight");
    case Qt::Key_Backslash:
        return QStringLiteral("Backslash");
    case Qt::Key_QuoteLeft:
        return QStringLiteral("Backquote");
    default:
        return {};
    }
}

ViewerPage::ViewerPage(
    ViewerController *controller,
    QWidget *parent)
    : QWidget(parent)
    , controller_(controller)
{
    Q_ASSERT(controller_);
    setObjectName(QStringLiteral("viewerPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    setMinimumSize(640, 420);
    buildUi();
    connectSignals();
    resetSession();
}

void ViewerPage::buildUi()
{
    setStyleSheet(QStringLiteral(R"(
QWidget#viewerPage {
    background: #05070b;
}
QWidget#viewerToolbar {
    background: rgba(10, 13, 19, 0.96);
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 13px;
}
QWidget#viewerToolbar QToolButton {
    color: #d8e1ef;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 8px;
    padding: 6px 9px;
    font-size: 11px;
}
QWidget#viewerToolbar QToolButton:hover {
    color: #ffffff;
    background: rgba(255, 255, 255, 0.08);
    border-color: rgba(255, 255, 255, 0.09);
}
QWidget#viewerToolbar QToolButton:pressed,
QWidget#viewerToolbar QToolButton:checked {
    color: #ffffff;
    background: rgba(61, 123, 253, 0.28);
    border-color: rgba(91, 155, 255, 0.42);
}
QWidget#viewerToolbar QToolButton#hangupButton {
    color: #ffd9db;
    background: rgba(242, 85, 90, 0.16);
    border-color: rgba(242, 85, 90, 0.28);
}
QWidget#viewerToolbar QToolButton#hangupButton:hover {
    background: rgba(242, 85, 90, 0.27);
}
QWidget#viewerHud {
    background: rgba(12, 16, 24, 0.92);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 13px;
}
QWidget#connectionOverlay {
    background: rgba(3, 5, 9, 0.84);
}
QWidget#dropOverlay {
    background: rgba(6, 10, 17, 0.90);
    border: 2px dashed rgba(91, 155, 255, 0.72);
    border-radius: 16px;
}
QWidget#textOverlay {
    background: rgba(3, 5, 9, 0.78);
}
QLabel#demoBadge {
    color: #ffe2a6;
    background: rgba(76, 53, 14, 0.92);
    border: 1px solid rgba(244, 183, 64, 0.44);
    border-radius: 8px;
    padding: 6px 10px;
    font-size: 10px;
    font-weight: 600;
}
QLabel#transferLabel {
    color: #cfd9e8;
    background: rgba(12, 16, 24, 0.92);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    padding: 6px 10px;
    font-size: 10px;
}
QMenu {
    color: #eef2f8;
    background: #171b26;
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 9px;
    padding: 6px;
}
QMenu::item {
    min-width: 150px;
    padding: 7px 26px 7px 12px;
    border-radius: 6px;
}
QMenu::item:selected {
    background: rgba(61, 123, 253, 0.28);
}
QMenu::indicator:checked {
    background: #5b9bff;
    border-radius: 3px;
}
)"));

    canvas_ = new ViewerCanvas(this);

    overlay_ = new QWidget(this);
    overlay_->setObjectName(QStringLiteral("connectionOverlay"));
    auto *overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(26, 26, 26, 26);
    overlayLayout->addStretch(1);
    auto *overlayRow = new QHBoxLayout;
    overlayRow->addStretch(1);
    auto *overlayCard = makeOverlayCard(
        overlay_,
        QStringLiteral("viewerOverlayCard"));
    overlayCard->setFixedWidth(470);
    auto *overlayCardLayout = new QVBoxLayout(overlayCard);
    overlayCardLayout->setContentsMargins(28, 24, 28, 24);
    overlayCardLayout->setSpacing(10);
    auto *overlayHeading = new QHBoxLayout;
    overlayHeading->setSpacing(10);
    overlayDot_ = new StatusDot(overlayCard);
    overlayDot_->setFixedSize(10, 10);
    overlayHeading->addWidget(overlayDot_, 0, Qt::AlignVCenter);
    overlayTitle_ = new QLabel(
        QStringLiteral("正在建立远程会话…"),
        overlayCard);
    overlayTitle_->setObjectName(QStringLiteral("viewerOverlayTitle"));
    overlayTitle_->setFont(uiFont(15, QFont::DemiBold));
    overlayTitle_->setStyleSheet(QStringLiteral("color: #f5f8ff;"));
    overlayHeading->addWidget(overlayTitle_, 1);
    overlayCardLayout->addLayout(overlayHeading);
    overlayNote_ = new QLabel(overlayCard);
    overlayNote_->setObjectName(QStringLiteral("viewerOverlayNote"));
    overlayNote_->setWordWrap(true);
    overlayNote_->setFont(uiFont(9));
    overlayNote_->setStyleSheet(QStringLiteral("color: #9aa5b8;"));
    overlayCardLayout->addWidget(overlayNote_);
    overlayCardLayout->addSpacing(5);
    auto *overlayActions = new QHBoxLayout;
    overlayActions->addStretch(1);
    backButton_ = new QPushButton(QStringLiteral("返回连接页"), overlayCard);
    backButton_->setObjectName(QStringLiteral("backButton"));
    backButton_->setCursor(Qt::PointingHandCursor);
    backButton_->setMinimumHeight(34);
    backButton_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  padding: 6px 15px;"
        "  font-size: 11px;"
        "  border-radius: 8px;"
        "}"));
    overlayActions->addWidget(backButton_);
    overlayCardLayout->addLayout(overlayActions);
    overlayRow->addWidget(overlayCard);
    overlayRow->addStretch(1);
    overlayLayout->addLayout(overlayRow);
    overlayLayout->addStretch(1);
    overlay_->hide();

    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("viewerToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar_);
    toolbarLayout->setContentsMargins(6, 5, 6, 5);
    toolbarLayout->setSpacing(1);

    qualityMenu_ = new QMenu(toolbar_);
    qualityButton_ = makeToolButton(
        QStringLiteral("画质 · 均衡"),
        QStringLiteral("qualityButton"),
        toolbar_);
    qualityButton_->setMenu(qualityMenu_);
    qualityButton_->setPopupMode(QToolButton::InstantPopup);
    toolbarLayout->addWidget(qualityButton_);

    monitorMenu_ = new QMenu(toolbar_);
    monitorButton_ = makeToolButton(
        QStringLiteral("显示器"),
        QStringLiteral("monitorButton"),
        toolbar_);
    monitorButton_->setMenu(monitorMenu_);
    monitorButton_->setPopupMode(QToolButton::InstantPopup);
    toolbarLayout->addWidget(monitorButton_);

    clipboardButton_ = makeToolButton(
        QStringLiteral("剪贴板"),
        QStringLiteral("clipboardButton"),
        toolbar_);
    toolbarLayout->addWidget(clipboardButton_);
    textButton_ = makeToolButton(
        QStringLiteral("文本"),
        QStringLiteral("textButton"),
        toolbar_);
    toolbarLayout->addWidget(textButton_);
    fileButton_ = makeToolButton(
        QStringLiteral("文件"),
        QStringLiteral("fileButton"),
        toolbar_);
    toolbarLayout->addWidget(fileButton_);
    cadButton_ = makeToolButton(
        QStringLiteral("Ctrl+Alt+Del"),
        QStringLiteral("cadButton"),
        toolbar_);
    toolbarLayout->addWidget(cadButton_);
    audioButton_ = makeToolButton(
        QStringLiteral("声音"),
        QStringLiteral("audioButton"),
        toolbar_);
    audioButton_->setCheckable(true);
    toolbarLayout->addWidget(audioButton_);
    fullScreenButton_ = makeToolButton(
        QStringLiteral("全屏"),
        QStringLiteral("fullScreenButton"),
        toolbar_);
    toolbarLayout->addWidget(fullScreenButton_);
    hudButton_ = makeToolButton(
        QStringLiteral("信息"),
        QStringLiteral("hudButton"),
        toolbar_);
    hudButton_->setCheckable(true);
    hudButton_->setChecked(true);
    toolbarLayout->addWidget(hudButton_);
    hangupButton_ = makeToolButton(
        QStringLiteral("断开"),
        QStringLiteral("hangupButton"),
        toolbar_);
    toolbarLayout->addWidget(hangupButton_);

    hud_ = new QWidget(this);
    hud_->setObjectName(QStringLiteral("viewerHud"));
    hud_->setFixedWidth(286);
    auto *hudLayout = new QVBoxLayout(hud_);
    hudLayout->setContentsMargins(15, 13, 15, 13);
    hudLayout->setSpacing(7);
    auto *hudHeading = new QHBoxLayout;
    hudHeading->setSpacing(8);
    peerLabel_ = new QLabel(QStringLiteral("远端设备"), hud_);
    peerLabel_->setObjectName(QStringLiteral("viewerPeer"));
    peerLabel_->setFont(uiFont(10, QFont::DemiBold));
    peerLabel_->setStyleSheet(QStringLiteral("color: #f2f6fc;"));
    hudHeading->addWidget(peerLabel_, 1);
    linkPill_ = new PillLabel(hud_);
    linkPill_->setText(QStringLiteral("连接中"));
    linkPill_->setTone(PillLabel::Tone::Neutral);
    hudHeading->addWidget(linkPill_, 0, Qt::AlignVCenter);
    hudLayout->addLayout(hudHeading);
    codeLabel_ = new QLabel(QStringLiteral("连接码 —"), hud_);
    codeLabel_->setObjectName(QStringLiteral("viewerSessionCode"));
    codeLabel_->setFont(monoFont(8));
    codeLabel_->setStyleSheet(QStringLiteral("color: #69728a;"));
    hudLayout->addWidget(codeLabel_);
    auto *hudGrid = new QGridLayout;
    hudGrid->setHorizontalSpacing(12);
    hudGrid->setVerticalSpacing(5);
    resolutionValue_ = makeHudValue(
        hud_,
        QStringLiteral("hudResolution"));
    fpsValue_ = makeHudValue(hud_, QStringLiteral("hudFps"));
    bitrateValue_ = makeHudValue(hud_, QStringLiteral("hudBitrate"));
    latencyValue_ = makeHudValue(hud_, QStringLiteral("hudLatency"));
    audioValue_ = makeHudValue(hud_, QStringLiteral("hudAudio"));
    hudGrid->addWidget(makeHudCaption(QStringLiteral("分辨率"), hud_), 0, 0);
    hudGrid->addWidget(resolutionValue_, 0, 1);
    hudGrid->addWidget(makeHudCaption(QStringLiteral("帧率"), hud_), 1, 0);
    hudGrid->addWidget(fpsValue_, 1, 1);
    hudGrid->addWidget(makeHudCaption(QStringLiteral("码率"), hud_), 2, 0);
    hudGrid->addWidget(bitrateValue_, 2, 1);
    hudGrid->addWidget(makeHudCaption(QStringLiteral("延迟"), hud_), 3, 0);
    hudGrid->addWidget(latencyValue_, 3, 1);
    hudGrid->addWidget(makeHudCaption(QStringLiteral("声音"), hud_), 4, 0);
    hudGrid->addWidget(audioValue_, 4, 1);
    hudLayout->addLayout(hudGrid);

    dropOverlay_ = new QWidget(this);
    dropOverlay_->setObjectName(QStringLiteral("dropOverlay"));
    dropOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *dropLayout = new QVBoxLayout(dropOverlay_);
    dropLayout->setContentsMargins(32, 32, 32, 32);
    dropLayout->addStretch(1);
    auto *dropTitle = new QLabel(
        QStringLiteral("松开鼠标以发送文件"),
        dropOverlay_);
    dropTitle->setAlignment(Qt::AlignCenter);
    dropTitle->setFont(uiFont(17, QFont::DemiBold));
    dropTitle->setStyleSheet(QStringLiteral("color: #f5f8ff;"));
    dropLayout->addWidget(dropTitle);
    auto *dropNote = new QLabel(
        QStringLiteral("文件将通过当前加密会话传输到远端设备"),
        dropOverlay_);
    dropNote->setAlignment(Qt::AlignCenter);
    dropNote->setFont(uiFont(9));
    dropNote->setStyleSheet(QStringLiteral("color: #91a0b8;"));
    dropLayout->addWidget(dropNote);
    dropLayout->addStretch(1);
    dropOverlay_->hide();

    textOverlay_ = new QWidget(this);
    textOverlay_->setObjectName(QStringLiteral("textOverlay"));
    auto *textOverlayLayout = new QVBoxLayout(textOverlay_);
    textOverlayLayout->setContentsMargins(28, 28, 28, 28);
    textOverlayLayout->addStretch(1);
    auto *textRow = new QHBoxLayout;
    textRow->addStretch(1);
    auto *textCard = makeOverlayCard(
        textOverlay_,
        QStringLiteral("viewerTextCard"));
    textCard->setFixedWidth(520);
    auto *textCardLayout = new QVBoxLayout(textCard);
    textCardLayout->setContentsMargins(22, 20, 22, 20);
    textCardLayout->setSpacing(10);
    auto *textTitle = new QLabel(QStringLiteral("发送文本到远端"), textCard);
    textTitle->setFont(uiFont(13, QFont::DemiBold));
    textTitle->setStyleSheet(QStringLiteral("color: #f5f8ff;"));
    textCardLayout->addWidget(textTitle);
    auto *textHint = new QLabel(
        QStringLiteral("文本会输入到远端当前获得焦点的窗口"),
        textCard);
    textHint->setFont(uiFont(8));
    textHint->setStyleSheet(QStringLiteral("color: #7f8ca3;"));
    textCardLayout->addWidget(textHint);
    textInput_ = new QPlainTextEdit(textCard);
    textInput_->setObjectName(QStringLiteral("textInput"));
    textInput_->setPlaceholderText(
        QStringLiteral("输入要发送的文本，Ctrl+Enter 快速发送"));
    textInput_->setMinimumHeight(132);
    textCardLayout->addWidget(textInput_);
    auto *textActions = new QHBoxLayout;
    textActions->addStretch(1);
    auto *textCancelButton = new QPushButton(
        QStringLiteral("取消"),
        textCard);
    textCancelButton->setCursor(Qt::PointingHandCursor);
    textActions->addWidget(textCancelButton);
    textSendButton_ = new QPushButton(QStringLiteral("发送"), textCard);
    textSendButton_->setObjectName(QStringLiteral("primaryButton"));
    textSendButton_->setCursor(Qt::PointingHandCursor);
    textActions->addWidget(textSendButton_);
    textCardLayout->addLayout(textActions);
    textRow->addWidget(textCard);
    textRow->addStretch(1);
    textOverlayLayout->addLayout(textRow);
    textOverlayLayout->addStretch(1);
    textOverlay_->hide();

    transferLabel_ = new QLabel(this);
    transferLabel_->setObjectName(QStringLiteral("transferLabel"));
    transferLabel_->setFixedSize(380, 28);
    transferLabel_->hide();

    demoBadge_ = new QLabel(
        QStringLiteral("合成演示画面 · 不是真实远端"),
        this);
    demoBadge_->setObjectName(QStringLiteral("demoBadge"));
    demoBadge_->setAttribute(Qt::WA_TransparentForMouseEvents);
    demoBadge_->adjustSize();
    demoBadge_->hide();

    toolbarTimer_ = new QTimer(this);
    toolbarTimer_->setSingleShot(true);
    toolbarTimer_->setInterval(toolbarAutoHideMs_);

    buildQualityMenu();
    updateMonitors();
    updatePeerUi();
    updateLinkUi();
    updateStatisticsUi();
    updateAudioUi();
    updateResolutionUi();
    updateToolbarVisibility();

    setAcceptDrops(true);
}

void ViewerPage::connectSignals()
{
    connect(
        canvas_,
        &ViewerCanvas::mouseMoveRequested,
        controller_,
        &ViewerController::sendMouseMove);
    connect(
        canvas_,
        &ViewerCanvas::mouseButtonRequested,
        controller_,
        &ViewerController::sendMouseButton);
    connect(
        canvas_,
        &ViewerCanvas::wheelRequested,
        controller_,
        &ViewerController::sendMouseWheel);
    connect(
        canvas_,
        &ViewerCanvas::keyRequested,
        controller_,
        [this](const QString &code, bool down) {
            if (live_ && !demoMode_) {
                (void)controller_->sendKey(code, down);
                wakeToolbar();
            }
        });
    connect(
        canvas_,
        &ViewerCanvas::textRequested,
        this,
        &ViewerPage::sendTextToRemote);
    connect(
        canvas_,
        &ViewerCanvas::clipboardPasteRequested,
        this,
        [this](const QString &text) {
            if (!live_ || demoMode_) {
                return;
            }
            if (text.isEmpty()) {
                showToast(QStringLiteral("本机剪贴板为空"));
                return;
            }
            if (controller_->sendClipboard(text)) {
                showToast(QStringLiteral("已粘贴本机剪贴板内容"));
            } else {
                showToast(QStringLiteral("剪贴板发送失败"));
            }
        });
    connect(
        canvas_,
        &ViewerCanvas::mouseMoveRequested,
        this,
        [this](double, double) {
            wakeToolbar();
        });

    connect(
        controller_,
        &ViewerController::frameDecoded,
        this,
        [this](const QImage &image, bool) {
            canvas_->setFrame(image);
            updateResolutionUi();
            if (live_
                && controller_->state() == ViewerController::State::Streaming
                && overlay_->isVisible()) {
                showStreaming(
                    peerName_.isEmpty() ? controller_->peerName() : peerName_);
            }
        });
    connect(
        controller_,
        &ViewerController::stateChanged,
        this,
        [this](ViewerController::State state) {
            if (state == ViewerController::State::Streaming) {
                showStreaming(
                    peerName_.isEmpty() ? controller_->peerName() : peerName_);
            } else if (state == ViewerController::State::Reconnecting) {
                showReconnecting();
            } else if (state == ViewerController::State::Connecting
                       && live_) {
                if (!overlay_->isVisible() || demoMode_) {
                    showConnecting(
                        peerName_.isEmpty()
                            ? QStringLiteral("远端设备")
                            : peerName_);
                }
            }
        });
    connect(
        controller_,
        &ViewerController::inputReleaseRequested,
        canvas_,
        &ViewerCanvas::releaseInputs);
    connect(
        controller_,
        &ViewerController::peerChanged,
        this,
        [this](const QString &name, bool connected) {
            if (connected && !name.isEmpty()) {
                setPeerName(name);
            }
        });
    connect(
        controller_,
        &ViewerController::errorOccurred,
        this,
        [this](const QString &message) {
            if (live_ || overlay_->isVisible()) {
                showFailed(message);
            }
        });
    connect(
        controller_,
        &ViewerController::sessionEnded,
        this,
        &ViewerPage::showEnded);
    connect(
        controller_,
        &ViewerController::linkStateChanged,
        this,
        &ViewerPage::setLinkState);
    connect(
        controller_,
        &ViewerController::infoChanged,
        this,
        [this](const QJsonObject &info) {
            info_ = info;
            updateMonitors();
            updateResolutionUi();
            if (live_ && !demoMode_
                && controller_->state()
                    == ViewerController::State::Streaming) {
                canvas_->setInputEnabled(
                    info.value(QStringLiteral("input")).toBool(true));
            }
        });
    connect(
        controller_,
        &ViewerController::statisticsChanged,
        this,
        [this](const QJsonObject &statistics) {
            statistics_ = statistics;
            updateStatisticsUi();
            updateLinkUi();
        });
    connect(
        controller_,
        &ViewerController::audioStateChanged,
        this,
        [this](const QJsonObject &state) {
            audioState_ = state;
            updateAudioUi();
        });
    connect(
        controller_,
        &ViewerController::audioEnabledChanged,
        this,
        [this](bool enabled) {
            lastAudioEnabled_ = enabled;
            updateAudioUi();
        });
    connect(
        controller_,
        &ViewerController::eventReceived,
        this,
        [this](const QJsonObject &message) {
            const QString kind = jsonString(message, "t");
            if (kind == QStringLiteral("clip")
                || kind == QStringLiteral("clipboard")) {
                const QString text = jsonString(message, "s");
                QApplication::clipboard()->setText(text);
                showToast(QStringLiteral("已同步远端剪贴板"));
                return;
            }
            if (kind == QStringLiteral("link")) {
                setLinkState(jsonString(message, "state"));
                return;
            }
            if (kind == QStringLiteral("info")) {
                info_ = message;
                updateMonitors();
                updateResolutionUi();
                return;
            }
            if (kind == QStringLiteral("stats")) {
                statistics_ = message;
                updateStatisticsUi();
                updateLinkUi();
                return;
            }
            if (kind == QStringLiteral("audio")) {
                audioState_ = message;
                updateAudioUi();
                return;
            }
            if (kind == QStringLiteral("file_in")) {
                const QString state = jsonString(message, "state");
                if (state == QStringLiteral("done")) {
                    showToast(
                        QStringLiteral("已接收文件：%1")
                            .arg(jsonString(message, "name")));
                }
            }
        });
    connect(
        controller_,
        &ViewerController::outgoingFileStarted,
        this,
        [this](const QString &, const QString &name, qint64) {
            fileBusy_ = true;
            currentFileName_ = name;
            lastFilePercent_ = -1;
            transferLabel_->setText(
                QStringLiteral("正在发送 %1 · 0%").arg(name));
            transferLabel_->setStyleSheet(QStringLiteral("color: #cfd9e8;"));
            transferLabel_->show();
            transferLabel_->raise();
            scheduleToolbarHide();
        });
    connect(
        controller_,
        &ViewerController::outgoingFileProgress,
        this,
        [this](const QString &, qint64 sent, qint64 total) {
            if (total <= 0) {
                return;
            }
            const int percent = std::clamp(
                static_cast<int>((sent * 100) / total),
                0,
                100);
            if (percent == lastFilePercent_) {
                return;
            }
            lastFilePercent_ = percent;
            transferLabel_->setText(
                QStringLiteral("正在发送 %1 · %2%")
                    .arg(
                        currentFileName_.isEmpty()
                            ? QStringLiteral("文件")
                            : currentFileName_)
                    .arg(percent));
            transferLabel_->show();
            transferLabel_->raise();
        });
    connect(
        controller_,
        &ViewerController::outgoingFileFinished,
        this,
        [this](const QString &, const QString &name, qint64 size) {
            fileBusy_ = false;
            currentFileName_.clear();
            lastFilePercent_ = -1;
            transferLabel_->setText(
                QStringLiteral("已发送 %1 · %2")
                    .arg(name, formatSize(size)));
            transferLabel_->setStyleSheet(QStringLiteral("color: #6ee7a8;"));
            transferLabel_->show();
            showToast(QStringLiteral("文件已发送：%1").arg(name));
            QTimer::singleShot(0, this, &ViewerPage::flushFileQueue);
        });
    connect(
        controller_,
        &ViewerController::outgoingFileFailed,
        this,
        [this](const QString &, const QString &reason) {
            fileBusy_ = false;
            currentFileName_.clear();
            lastFilePercent_ = -1;
            const QString text =
                QStringLiteral("发送失败：%1").arg(failureText(reason));
            transferLabel_->setText(text);
            transferLabel_->setStyleSheet(QStringLiteral("color: #ff9a9e;"));
            transferLabel_->show();
            showToast(text);
            QTimer::singleShot(0, this, &ViewerPage::flushFileQueue);
        });

    connect(backButton_, &QPushButton::clicked, this, &ViewerPage::backRequested);
    connect(
        qualityMenu_,
        &QMenu::triggered,
        this,
        [this](QAction *action) {
            const QString id = action->data().toString();
            if (id.isEmpty()) {
                return;
            }
            controller_->setQualityPreset(id);
            qualityButton_->setText(
                QStringLiteral("画质 · %1")
                    .arg(qualityPreset(id).name));
            for (QAction *candidate : qualityMenu_->actions()) {
                candidate->setChecked(candidate == action);
            }
            showToast(
                QStringLiteral("画质已切换为 %1")
                    .arg(qualityPreset(id).name));
        });
    connect(
        monitorMenu_,
        &QMenu::triggered,
        this,
        [this](QAction *action) {
            bool ok = false;
            const int index = action->data().toInt(&ok);
            if (!ok) {
                return;
            }
            controller_->selectMonitor(index);
            updateMonitors();
            updateResolutionUi();
            showToast(QStringLiteral("已切换远端显示器"));
        });

    connect(
        clipboardButton_,
        &QToolButton::clicked,
        this,
        [this]() {
            if (!live_ || demoMode_) {
                showToast(QStringLiteral("演示画面不会发送真实输入"));
                return;
            }
            const QString text = QApplication::clipboard()->text();
            if (text.isEmpty()) {
                showToast(QStringLiteral("本机剪贴板为空"));
                return;
            }
            if (controller_->sendClipboard(text)) {
                showToast(QStringLiteral("已发送剪贴板内容"));
            } else {
                showToast(QStringLiteral("剪贴板发送失败"));
            }
        });
    connect(textButton_, &QToolButton::clicked, this, &ViewerPage::openTextInput);
    connect(
        fileButton_,
        &QToolButton::clicked,
        this,
        [this]() {
            if (!live_ || demoMode_) {
                showToast(QStringLiteral("演示画面不会发送真实文件"));
                return;
            }
            const QStringList paths = QFileDialog::getOpenFileNames(
                this,
                QStringLiteral("选择要发送到远端的文件"));
            sendLocalFiles(paths);
        });
    connect(
        cadButton_,
        &QToolButton::clicked,
        this,
        [this]() {
            if (!live_ || demoMode_) {
                showToast(QStringLiteral("演示画面不会发送真实输入"));
                return;
            }
            controller_->sendCtrlAltDelete();
            showToast(QStringLiteral("已发送 Ctrl+Alt+Del"));
        });
    connect(
        audioButton_,
        &QToolButton::toggled,
        this,
        [this](bool enabled) {
            if (enabled == controller_->audioEnabled()) {
                return;
            }
            if (demoMode_) {
                lastAudioEnabled_ = enabled;
                updateAudioUi();
                return;
            }
            controller_->setAudioEnabled(enabled);
            lastAudioEnabled_ = enabled;
            updateAudioUi();
            showToast(
                enabled ? QStringLiteral("已开启远端声音")
                        : QStringLiteral("已静音远端声音"));
        });
    connect(
        fullScreenButton_,
        &QToolButton::clicked,
        this,
        &ViewerPage::toggleFullScreen);
    connect(
        hudButton_,
        &QToolButton::toggled,
        this,
        [this](bool checked) {
            hudVisible_ = checked;
            hud_->setVisible(checked);
            if (checked) {
                hud_->raise();
            }
        });
    connect(hangupButton_, &QToolButton::clicked, this, [this]() {
        emit hangupRequested();
    });
    connect(textSendButton_, &QPushButton::clicked, this, [this]() {
        sendTextToRemote(textInput_->toPlainText());
    });
    for (QPushButton *button : textOverlay_->findChildren<QPushButton *>()) {
        if (button == textSendButton_) {
            continue;
        }
        connect(button, &QPushButton::clicked, this, [this]() {
            textOverlay_->hide();
            updateToolbarVisibility();
            wakeToolbar();
            canvas_->setFocus(Qt::OtherFocusReason);
        });
    }
    connect(textInput_, &QPlainTextEdit::textChanged, this, [this]() {
        textSendButton_->setEnabled(
            !textInput_->toPlainText().trimmed().isEmpty());
    });

    connect(
        toolbarTimer_,
        &QTimer::timeout,
        this,
        [this]() {
            if (!live_
                || menuPinned_
                || dragActive_
                || !textOverlay_->isHidden()
                || !overlay_->isHidden()) {
                return;
            }
            toolbar_->hide();
        });

    installMenuPin(qualityMenu_);
    installMenuPin(monitorMenu_);

    canvas_->installEventFilter(this);
    toolbar_->installEventFilter(this);
    textInput_->installEventFilter(this);
    installEventFilter(this);
    setToolbarAutoHideInterval(toolbarAutoHideMs_);
    textSendButton_->setEnabled(false);
}

void ViewerPage::buildQualityMenu()
{
    for (const QualityPreset &preset : qualityPresets()) {
        auto *action = qualityMenu_->addAction(preset.name);
        action->setCheckable(true);
        action->setData(preset.id);
        action->setChecked(preset.id == controller_->qualityId());
    }
    qualityButton_->setText(
        QStringLiteral("画质 · %1")
            .arg(qualityPreset(controller_->qualityId()).name));
}

void ViewerPage::updateMonitors()
{
    monitorMenu_->clear();
    const QJsonArray monitors =
        info_.value(QStringLiteral("monitors")).toArray();
    if (monitors.isEmpty()) {
        auto *empty = monitorMenu_->addAction(
            QStringLiteral("等待远端显示器信息"));
        empty->setEnabled(false);
        monitorButton_->setText(QStringLiteral("显示器"));
        return;
    }

    QString selectedName;
    for (const QJsonValue &value : monitors) {
        const QJsonObject monitor = value.toObject();
        const int index = static_cast<int>(
            jsonNumber(monitor, "index", 0.0));
        QString name = jsonString(monitor, "name");
        if (name.isEmpty()) {
            name = QStringLiteral("显示器 %1").arg(index + 1);
        }
        if (jsonBool(monitor, "primary")) {
            name += QStringLiteral(" · 主屏");
        }
        auto *action = monitorMenu_->addAction(name);
        action->setCheckable(true);
        action->setData(index);
        const bool selected = index == controller_->monitor();
        action->setChecked(selected);
        if (selected) {
            selectedName = name;
        }
    }
    if (selectedName.isEmpty()) {
        const QJsonObject first =
            monitors.first().toObject();
        const int index = static_cast<int>(jsonNumber(first, "index", 0.0));
        selectedName = jsonString(first, "name");
        if (selectedName.isEmpty()) {
            selectedName = QStringLiteral("显示器 %1").arg(index + 1);
        }
    }
    monitorButton_->setText(selectedName);
}

void ViewerPage::updatePeerUi()
{
    const QString name = peerName_.isEmpty()
        ? QStringLiteral("远端设备")
        : peerName_;
    peerLabel_->setText(name);
    if (demoMode_) {
        codeLabel_->setText(
            QStringLiteral("合成演示 · 无真实链路"));
    } else if (sessionCode_.isEmpty()) {
        codeLabel_->setText(QStringLiteral("连接码 —"));
    } else {
        codeLabel_->setText(
            QStringLiteral("连接码 %1")
                .arg(formatConnectionCode(sessionCode_)));
    }
}

void ViewerPage::updateLinkUi()
{
    QString text = QStringLiteral("连接中");
    PillLabel::Tone tone = PillLabel::Tone::Neutral;
    if (linkState_ == QStringLiteral("direct")) {
        text = QStringLiteral("P2P 直连");
        tone = PillLabel::Tone::Green;
    } else if (linkState_ == QStringLiteral("relay")) {
        text = QStringLiteral("中继模式");
        tone = PillLabel::Tone::Blue;
    } else if (linkState_ == QStringLiteral("punching")) {
        text = QStringLiteral("正在穿透");
        tone = PillLabel::Tone::Amber;
    } else if (linkState_ == QStringLiteral("reconnecting")) {
        text = QStringLiteral("正在恢复");
        tone = PillLabel::Tone::Amber;
    } else if (linkState_ == QStringLiteral("closed")) {
        text = QStringLiteral("已断开");
        tone = PillLabel::Tone::Red;
    } else if (!statistics_.isEmpty()) {
        if (jsonBool(statistics_, "relay")) {
            text = QStringLiteral("中继模式");
            tone = PillLabel::Tone::Blue;
        } else {
            text = QStringLiteral("P2P 直连");
            tone = PillLabel::Tone::Green;
        }
    }
    linkPill_->setText(text);
    linkPill_->setTone(tone);
}

void ViewerPage::updateStatisticsUi()
{
    if (statistics_.isEmpty()) {
        fpsValue_->setText(QStringLiteral("—"));
        bitrateValue_->setText(QStringLiteral("—"));
        latencyValue_->setText(QStringLiteral("—"));
        return;
    }

    const double fps = jsonNumber(statistics_, "fps");
    const double sfps = jsonNumber(statistics_, "sfps");
    const double cap = jsonNumber(statistics_, "cap");
    QString fpsText = QStringLiteral("—");
    if (fps > 0.0) {
        fpsText = QStringLiteral("%1 fps").arg(fps, 0, 'f', 1);
    }
    if (cap > 0.0 && sfps > 0.0) {
        fpsText += QStringLiteral(" (限 %1)")
            .arg(static_cast<int>(std::lround(sfps)));
    }
    fpsValue_->setText(fpsText);

    const double mbps = jsonNumber(statistics_, "mbps");
    QString bitrateText = QStringLiteral("—");
    if (mbps > 0.0) {
        bitrateText = QStringLiteral("%1 Mbps").arg(mbps, 0, 'f', 1);
        if (cap > 0.0) {
            bitrateText += QStringLiteral(" / 限 %1")
                .arg(cap, 0, 'f', 1);
        }
    }
    bitrateValue_->setText(bitrateText);

    const double rtt = jsonNumber(statistics_, "rtt");
    latencyValue_->setText(
        rtt > 0.0
            ? QStringLiteral("%1 ms").arg(rtt, 0, 'f', 1)
            : QStringLiteral("—"));
}

void ViewerPage::updateAudioUi()
{
    audioButton_->setChecked(lastAudioEnabled_);
    if (!live_) {
        audioValue_->setText(QStringLiteral("—"));
        return;
    }
    if (jsonBool(audioState_, "pending")) {
        audioValue_->setText(QStringLiteral("切换中"));
        return;
    }
    if (!lastAudioEnabled_) {
        audioValue_->setText(QStringLiteral("静音"));
        return;
    }
    const QString error = jsonString(audioState_, "error");
    if (!error.isEmpty() && !jsonBool(audioState_, "on")) {
        audioValue_->setText(QStringLiteral("不可用"));
        return;
    }
    if (audioState_.isEmpty()) {
        audioValue_->setText(QStringLiteral("开"));
        return;
    }
    audioValue_->setText(
        jsonBool(audioState_, "on")
            ? QStringLiteral("开")
            : QStringLiteral("关"));
}

void ViewerPage::updateResolutionUi()
{
    QString resolution;
    if (canvas_->hasFrame()) {
        resolution = QStringLiteral("%1 × %2")
            .arg(canvas_->frame().width())
            .arg(canvas_->frame().height());
    } else {
        const QJsonArray monitors =
            info_.value(QStringLiteral("monitors")).toArray();
        for (const QJsonValue &value : monitors) {
            const QJsonObject monitor = value.toObject();
            if (static_cast<int>(jsonNumber(monitor, "index", 0.0))
                != controller_->monitor()) {
                continue;
            }
            const int width =
                static_cast<int>(jsonNumber(monitor, "w", 0.0));
            const int height =
                static_cast<int>(jsonNumber(monitor, "h", 0.0));
            if (width > 0 && height > 0) {
                resolution = QStringLiteral("%1 × %2")
                    .arg(width)
                    .arg(height);
            }
            break;
        }
    }
    resolutionValue_->setText(
        resolution.isEmpty() ? QStringLiteral("—") : resolution);
}

void ViewerPage::updateToolbarVisibility()
{
    const bool available = live_;
    toolbar_->setEnabled(available);
    if (!available
        || !overlay_->isHidden()
        || !textOverlay_->isHidden()) {
        toolbar_->hide();
        toolbarTimer_->stop();
        return;
    }
    if (toolbar_->isVisible()) {
        scheduleToolbarHide();
    }
}

void ViewerPage::showOverlay(
    const QString &title,
    const QString &note,
    bool showBack)
{
    overlayTitle_->setText(title);
    overlayNote_->setText(note);
    backButton_->setVisible(showBack);
    overlayDot_->setColor(showBack ? red() : amber());
    overlay_->setGeometry(rect());
    overlay_->show();
    overlay_->raise();
    toolbar_->hide();
    toolbarTimer_->stop();
}

void ViewerPage::hideOverlay()
{
    overlay_->hide();
    if (live_) {
        wakeToolbar();
    }
}

void ViewerPage::wakeToolbar()
{
    if (!live_
        || !overlay_->isHidden()
        || !textOverlay_->isHidden()) {
        return;
    }
    toolbar_->setEnabled(true);
    toolbar_->show();
    toolbar_->adjustSize();
    toolbar_->move(
        (width() - toolbar_->width()) / 2,
        height() - toolbar_->height() - 24);
    toolbar_->raise();
    scheduleToolbarHide();
}

void ViewerPage::scheduleToolbarHide()
{
    if (!live_
        || toolbarAutoHideMs_ <= 0
        || menuPinned_
        || dragActive_
        || !textOverlay_->isHidden()
        || !overlay_->isHidden()
        || !toolbar_->isVisible()) {
        toolbarTimer_->stop();
        return;
    }
    toolbarTimer_->start(toolbarAutoHideMs_);
}

void ViewerPage::setMenuPinned(bool pinned)
{
    menuPinned_ = pinned;
    if (pinned) {
        toolbarTimer_->stop();
        if (live_ && overlay_->isHidden()) {
            toolbar_->show();
            toolbar_->raise();
        }
    } else {
        scheduleToolbarHide();
    }
}

void ViewerPage::installMenuPin(QMenu *menu)
{
    connect(menu, &QMenu::aboutToShow, this, [this]() {
        setMenuPinned(true);
    });
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            setMenuPinned(
                (qualityMenu_ && qualityMenu_->isVisible())
                || (monitorMenu_ && monitorMenu_->isVisible()));
        });
    });
}

void ViewerPage::flushFileQueue()
{
    if (fileBusy_ || pendingFiles_.isEmpty()) {
        return;
    }
    if (!live_ || demoMode_ || !controller_->active()) {
        pendingFiles_.clear();
        transferLabel_->setText(
            QStringLiteral("文件未发送：远程会话未建立"));
        transferLabel_->setStyleSheet(QStringLiteral("color: #ff9a9e;"));
        transferLabel_->show();
        showToast(QStringLiteral("文件未发送：远程会话未建立"));
        return;
    }

    while (!pendingFiles_.isEmpty()) {
        const QString path = pendingFiles_.takeFirst();
        QString error;
        if (controller_->sendFile(path, &error)) {
            fileBusy_ = true;
            currentFileName_ = QFileInfo(path).fileName();
            if (!currentFileName_.isEmpty()) {
                transferLabel_->setText(
                    QStringLiteral("正在发送 %1 · 0%")
                        .arg(currentFileName_));
                transferLabel_->setStyleSheet(
                    QStringLiteral("color: #cfd9e8;"));
                transferLabel_->show();
            }
            return;
        }
        const QString text =
            QStringLiteral("发送失败：%1").arg(failureText(error));
        transferLabel_->setText(text);
        transferLabel_->setStyleSheet(QStringLiteral("color: #ff9a9e;"));
        transferLabel_->show();
        showToast(text);
    }
}

void ViewerPage::beginSession(
    const QString &peerName,
    const QString &code)
{
    resetSession();
    live_ = true;
    peerName_ = peerName.trimmed();
    sessionCode_ = code.trimmed().isEmpty()
        ? controller_->code()
        : code.trimmed();
    lastAudioEnabled_ = controller_->audioEnabled();
    updatePeerUi();
    updateLinkUi();
    updateAudioUi();
    updateToolbarVisibility();
    wakeToolbar();
}

void ViewerPage::showConnecting(
    const QString &peerName,
    const QString &note)
{
    if (!live_ || demoMode_) {
        beginSession(
            peerName,
            controller_->code());
    }
    live_ = true;
    demoMode_ = false;
    demoBadge_->hide();
    if (!peerName.trimmed().isEmpty()) {
        peerName_ = peerName.trimmed();
    }
    if (sessionCode_.isEmpty()) {
        sessionCode_ = controller_->code();
    }
    canvas_->setInputEnabled(false);
    updatePeerUi();
    updateLinkUi();
    updateAudioUi();
    const QString target = peerName_.isEmpty()
        ? QStringLiteral("远端设备")
        : peerName_;
    showOverlay(
        QStringLiteral("正在与 %1 建立加密通道…").arg(target),
        note.isEmpty()
            ? QStringLiteral("正在协商 P2P 通道并准备首帧画面…")
            : note,
        false);
}

void ViewerPage::showStreaming(const QString &peerName)
{
    if (demoMode_) {
        return;
    }
    if (!live_) {
        beginSession(
            peerName,
            controller_->code());
    }
    live_ = true;
    if (!peerName.trimmed().isEmpty()) {
        peerName_ = peerName.trimmed();
    } else if (peerName_.isEmpty()) {
        peerName_ = controller_->peerName();
    }
    sessionCode_ = controller_->code().isEmpty()
        ? sessionCode_
        : controller_->code();
    updatePeerUi();
    updateLinkUi();
    updateAudioUi();
    hideOverlay();
    canvas_->setInputEnabled(
        info_.value(QStringLiteral("input")).toBool(true));
    canvas_->setFocus(Qt::OtherFocusReason);
    updateToolbarVisibility();
    wakeToolbar();
}

void ViewerPage::showReconnecting()
{
    live_ = true;
    demoMode_ = false;
    demoBadge_->hide();
    dragActive_ = false;
    dropOverlay_->hide();
    textOverlay_->hide();
    transferLabel_->hide();
    canvas_->setInputEnabled(false);
    canvas_->releaseInputs();
    linkState_ = QStringLiteral("reconnecting");
    updateLinkUi();
    updateAudioUi();
    showOverlay(
        QStringLiteral("连接中断，正在恢复…"),
        QStringLiteral(
            "正在重新配对并恢复加密通道，已保留最后一帧画面。"),
        false);
    updateToolbarVisibility();
}

void ViewerPage::showFailed(const QString &reason)
{
    live_ = false;
    demoMode_ = false;
    demoBadge_->hide();
    canvas_->setInputEnabled(false);
    canvas_->releaseInputs();
    dragActive_ = false;
    dropOverlay_->hide();
    textOverlay_->hide();
    transferLabel_->hide();
    toolbar_->hide();
    toolbarTimer_->stop();
    updateAudioUi();
    showOverlay(
        QStringLiteral("远程连接出现问题"),
        reason.isEmpty()
            ? QStringLiteral("连接已中断，请返回后重试。")
            : reason,
        true);
}

void ViewerPage::showEnded(const QString &reason)
{
    live_ = false;
    demoMode_ = false;
    demoBadge_->hide();
    pendingFiles_.clear();
    fileBusy_ = false;
    currentFileName_.clear();
    lastFilePercent_ = -1;
    canvas_->setInputEnabled(false);
    canvas_->releaseInputs();
    canvas_->clearFrame();
    dragActive_ = false;
    dropOverlay_->hide();
    toolbar_->hide();
    toolbarTimer_->stop();
    updateStatisticsUi();
    updateAudioUi();
    const QString target = peerName_.isEmpty()
        ? QStringLiteral("远端设备")
        : peerName_;
    showOverlay(
        QStringLiteral("与 %1 的会话已结束").arg(target),
        reason.isEmpty()
            ? QStringLiteral("传输通道已关闭，请返回连接页重新连接。")
            : reason,
        true);
}

void ViewerPage::resetSession()
{
    live_ = false;
    demoMode_ = false;
    hudVisible_ = true;
    menuPinned_ = false;
    dragActive_ = false;
    fileBusy_ = false;
    lastFilePercent_ = -1;
    lastAudioEnabled_ = controller_->audioEnabled();
    peerName_.clear();
    sessionCode_.clear();
    linkState_.clear();
    pendingFiles_.clear();
    currentFileName_.clear();
    info_ = {};
    statistics_ = {};
    audioState_ = {};

    canvas_->clearFrame();
    canvas_->setInputEnabled(false);
    overlay_->hide();
    toolbar_->hide();
    toolbarTimer_->stop();
    dropOverlay_->hide();
    textOverlay_->hide();
    transferLabel_->hide();
    demoBadge_->hide();
    hud_->setVisible(true);
    hudButton_->setChecked(true);
    audioButton_->setChecked(lastAudioEnabled_);
    qualityButton_->setText(
        QStringLiteral("画质 · %1")
            .arg(qualityPreset(controller_->qualityId()).name));
    for (QAction *action : qualityMenu_->actions()) {
        action->setChecked(
            action->data().toString() == controller_->qualityId());
    }
    updateMonitors();
    updatePeerUi();
    updateLinkUi();
    updateStatisticsUi();
    updateAudioUi();
    updateResolutionUi();
    updateToolbarVisibility();
}

void ViewerPage::setPeerName(const QString &peerName)
{
    if (peerName.trimmed().isEmpty()) {
        return;
    }
    peerName_ = peerName.trimmed();
    updatePeerUi();
}

void ViewerPage::setLinkState(const QString &state)
{
    linkState_ = state;
    updateLinkUi();
}

void ViewerPage::showDemoFrame(
    const QImage &frame,
    const QString &peerName,
    const QJsonArray &monitors)
{
    const bool first = !demoMode_;
    if (first) {
        resetSession();
        demoMode_ = true;
        live_ = true;
        peerName_ = peerName.trimmed().isEmpty()
            ? QStringLiteral("STUDIO-PC")
            : peerName.trimmed();
        sessionCode_ = QStringLiteral("DEMO");
        linkState_ = QStringLiteral("direct");
        QJsonArray monitorObjects = monitors;
        if (monitorObjects.isEmpty()) {
            monitorObjects.append(QJsonObject{
                {QStringLiteral("index"), 0},
                {QStringLiteral("x"), 0},
                {QStringLiteral("y"), 0},
                {QStringLiteral("w"), frame.width()},
                {QStringLiteral("h"), frame.height()},
                {QStringLiteral("primary"), true},
                {QStringLiteral("name"), QStringLiteral("主显示器")},
            });
        }
        info_ = QJsonObject{
            {QStringLiteral("t"), QStringLiteral("info")},
            {QStringLiteral("name"), peerName_},
            {QStringLiteral("os"), QStringLiteral("Windows 11 Pro")},
            {QStringLiteral("input"), false},
            {QStringLiteral("monitors"), monitorObjects},
        };
        statistics_ = QJsonObject{
            {QStringLiteral("t"), QStringLiteral("stats")},
            {QStringLiteral("fps"), 59.8},
            {QStringLiteral("sfps"), 60},
            {QStringLiteral("mbps"), 22.6},
            {QStringLiteral("rtt"), 6.4},
            {QStringLiteral("relay"), false},
            {QStringLiteral("cap"), 0},
            {QStringLiteral("quality"), 82},
        };
        audioState_ = QJsonObject{
            {QStringLiteral("on"), true},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("codec"), QStringLiteral("pcmu")},
            {QStringLiteral("rate"), 48000},
            {QStringLiteral("pending"), false},
        };
        lastAudioEnabled_ = true;
        demoBadge_->show();
        demoBadge_->raise();
        overlay_->hide();
        updatePeerUi();
        updateMonitors();
        updateLinkUi();
        updateStatisticsUi();
        updateAudioUi();
        updateToolbarVisibility();
        wakeToolbar();
    }

    canvas_->setFrame(frame);
    canvas_->setInputEnabled(false);
    updateResolutionUi();
}

void ViewerPage::showToast(const QString &message)
{
    if (!message.trimmed().isEmpty()) {
        emit toastRequested(message);
    }
}

void ViewerPage::sendTextToRemote(const QString &text)
{
    if (text.isEmpty()) {
        showToast(QStringLiteral("请输入要发送的文本"));
        return;
    }
    if (!live_ || demoMode_) {
        showToast(QStringLiteral("演示画面不会发送真实输入"));
        return;
    }
    controller_->sendText(text);
    emit textSendRequested(text);
}

void ViewerPage::sendLocalFiles(const QStringList &paths)
{
    if (!live_ || demoMode_) {
        showToast(QStringLiteral("演示画面不会发送真实文件"));
        return;
    }
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !pendingFiles_.contains(trimmed)) {
            pendingFiles_.append(trimmed);
        }
    }
    flushFileQueue();
}

void ViewerPage::setToolbarAutoHideInterval(int milliseconds)
{
    toolbarAutoHideMs_ = std::max(0, milliseconds);
    toolbarTimer_->setInterval(toolbarAutoHideMs_);
    scheduleToolbarHide();
}

void ViewerPage::toggleFullScreen()
{
    canvas_->releaseInputs();
    QWidget *top = window();
    const bool fullScreen = top && top->isFullScreen();
    if (fullScreen) {
        emit fullScreenRequested(false);
        fullScreenButton_->setText(QStringLiteral("全屏"));
    } else {
        emit fullScreenRequested(true);
        fullScreenButton_->setText(QStringLiteral("退出全屏"));
    }
}

void ViewerPage::openTextInput()
{
    if (!live_ || demoMode_) {
        showToast(QStringLiteral("演示画面不会发送真实输入"));
        return;
    }
    canvas_->setInputEnabled(false);
    toolbarTimer_->stop();
    textOverlay_->setGeometry(rect());
    textOverlay_->show();
    textOverlay_->raise();
    textInput_->clear();
    textInput_->setFocus(Qt::OtherFocusReason);
}

void ViewerPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutFloatingWidgets();
}

void ViewerPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    layoutFloatingWidgets();
}

void ViewerPage::layoutFloatingWidgets()
{
    canvas_->setGeometry(rect());
    overlay_->setGeometry(rect());
    dropOverlay_->setGeometry(rect());
    textOverlay_->setGeometry(rect());

    if (!toolbar_->isHidden()) {
        toolbar_->adjustSize();
        toolbar_->move(
            (width() - toolbar_->width()) / 2,
            height() - toolbar_->height() - 24);
    }

    hud_->adjustSize();
    hud_->move(width() - hud_->width() - 20, 18);
    demoBadge_->adjustSize();
    demoBadge_->move(
        width() - demoBadge_->width() - 20,
        height() - demoBadge_->height() - 20);
    demoBadge_->raise();
    transferLabel_->move(
        20,
        height() - transferLabel_->height() - 24);
    transferLabel_->raise();
}

void ViewerPage::dragEnterEvent(QDragEnterEvent *event)
{
    if (!live_ || demoMode_
        || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    dragActive_ = true;
    dropOverlay_->setGeometry(rect());
    dropOverlay_->show();
    dropOverlay_->raise();
    toolbarTimer_->stop();
    event->acceptProposedAction();
}

void ViewerPage::dragMoveEvent(QDragMoveEvent *event)
{
    if (dragActive_ && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void ViewerPage::dragLeaveEvent(QDragLeaveEvent *event)
{
    dragActive_ = false;
    dropOverlay_->hide();
    scheduleToolbarHide();
    event->accept();
}

void ViewerPage::dropEvent(QDropEvent *event)
{
    dragActive_ = false;
    dropOverlay_->hide();
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        sendLocalFiles(paths);
    }
    scheduleToolbarHide();
    event->acceptProposedAction();
}

bool ViewerPage::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == this || watched == canvas_ || watched == toolbar_)
        && (event->type() == QEvent::MouseMove
            || event->type() == QEvent::Enter)) {
        wakeToolbar();
    }

    if (watched == canvas_
        && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool escape =
            keyEvent->key() == Qt::Key_Escape
            && !keyEvent->modifiers().testFlag(Qt::ShiftModifier);
        if (escape && window() && window()->isFullScreen()) {
            emit exitFullScreenRequested();
            fullScreenButton_->setText(QStringLiteral("全屏"));
            return true;
        }
        if (keyEvent->key() == Qt::Key_F11) {
            toggleFullScreen();
            return true;
        }
    }

    if (watched == textInput_
        && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            textOverlay_->hide();
            updateToolbarVisibility();
            wakeToolbar();
            canvas_->setFocus(Qt::OtherFocusReason);
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return
            || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers().testFlag(Qt::ControlModifier)) {
                sendTextToRemote(textInput_->toPlainText());
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace pd::ui
