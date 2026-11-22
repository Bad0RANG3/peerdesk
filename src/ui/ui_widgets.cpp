#include "ui/ui_widgets.h"

#include "ui/ui_theme.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

namespace pd::ui {

BackdropWidget::BackdropWidget(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
}

void BackdropWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    paintBackdrop(painter, size());
}

GlassCard::GlassCard(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_TranslucentBackground);
}

void GlassCard::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient fill(bounds.topLeft(), bounds.bottomLeft());
    fill.setColorAt(0.0, QColor(25, 30, 43, 244));
    fill.setColorAt(1.0, QColor(17, 20, 29, 246));
    painter.setPen(QPen(QColor(255, 255, 255, 32), 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(bounds, 16.0, 16.0);

    painter.setPen(Qt::NoPen);
    QLinearGradient sheen(
        QPointF(bounds.left(), bounds.top()),
        QPointF(bounds.right(), bounds.top() + 70));
    sheen.setColorAt(0.0, QColor(91, 155, 255, 16));
    sheen.setColorAt(0.55, QColor(91, 155, 255, 4));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(sheen);
    painter.drawRoundedRect(
        QRectF(bounds.left() + 1, bounds.top() + 1, bounds.width() - 2, 70),
        15.0,
        15.0);
}

PillLabel::PillLabel(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void PillLabel::setText(const QString &text)
{
    if (text_ == text) {
        return;
    }
    text_ = text;
    updateGeometry();
    update();
}

void PillLabel::setTone(Tone tone)
{
    if (tone_ == tone) {
        return;
    }
    tone_ = tone;
    update();
}

QSize PillLabel::sizeHint() const
{
    const QFontMetrics metrics(font());
    return QSize(metrics.horizontalAdvance(text_) + 20, 24);
}

void PillLabel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QColor accent = blueBright();
    switch (tone_) {
    case Tone::Green:
        accent = green();
        break;
    case Tone::Amber:
        accent = amber();
        break;
    case Tone::Red:
        accent = red();
        break;
    case Tone::Neutral:
        accent = textFaint();
        break;
    case Tone::Blue:
        break;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = accent;
    fill.setAlpha(36);
    QColor border = accent;
    border.setAlpha(82);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(
        QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
        12.0,
        12.0);
    painter.setPen(accent);
    painter.setFont(uiFont(8, QFont::DemiBold));
    painter.drawText(rect(), Qt::AlignCenter, text_);
}

StatusDot::StatusDot(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusDot::setColor(const QColor &color)
{
    if (color_ == color) {
        return;
    }
    color_ = color;
    update();
}

QSize StatusDot::sizeHint() const
{
    return QSize(12, 12);
}

void StatusDot::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRadialGradient halo(QPointF(width() / 2.0, height() / 2.0), 6.0);
    QColor glow = color_;
    glow.setAlpha(48);
    halo.setColorAt(0.0, glow);
    halo.setColorAt(0.5, glow);
    halo.setColorAt(1.0, QColor(glow.red(), glow.green(), glow.blue(), 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(halo);
    painter.drawEllipse(rect());

    painter.setBrush(color_);
    painter.drawEllipse(QRectF(3.0, 3.0, 6.0, 6.0));
}

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
    , animation_(new QPropertyAnimation(this, "knobPosition", this))
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    animation_->setDuration(150);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QAbstractButton::toggled, this, &ToggleSwitch::animateTo);
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(40, 23);
}

qreal ToggleSwitch::knobPosition() const
{
    return knobPosition_;
}

void ToggleSwitch::setKnobPosition(qreal position)
{
    knobPosition_ = std::clamp(position, 0.0, 1.0);
    update();
}

void ToggleSwitch::setCheckedSilently(bool checked)
{
    const QSignalBlocker blocker(this);
    setChecked(checked);
    animation_->stop();
    setKnobPosition(checked ? 1.0 : 0.0);
}

void ToggleSwitch::checkStateSet()
{
    QAbstractButton::checkStateSet();
    if (!animation_ || animation_->state() != QAbstractAnimation::Running) {
        setKnobPosition(isChecked() ? 1.0 : 0.0);
    }
}

void ToggleSwitch::nextCheckState()
{
    setChecked(!isChecked());
}

void ToggleSwitch::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ToggleSwitch::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track =
        QRectF(rect()).adjusted(0.5, 1.5, -0.5, -1.5);
    QColor off = QColor(255, 255, 255, hovered_ ? 34 : 24);
    QColor on = blue();
    QColor trackColor;
    trackColor.setRedF(off.redF() + (on.redF() - off.redF()) * knobPosition_);
    trackColor.setGreenF(
        off.greenF() + (on.greenF() - off.greenF()) * knobPosition_);
    trackColor.setBlueF(
        off.blueF() + (on.blueF() - off.blueF()) * knobPosition_);
    trackColor.setAlphaF(
        off.alphaF() + (on.alphaF() - off.alphaF()) * knobPosition_);

    painter.setPen(QPen(
        isEnabled()
            ? QColor(255, 255, 255, 42)
            : QColor(255, 255, 255, 18),
        1.0));
    painter.setBrush(trackColor);
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    const qreal diameter = track.height() - 5.0;
    const qreal left = track.left() + 2.5;
    const qreal right = track.right() - diameter - 2.5;
    const QPointF center(
        left + diameter / 2.0
            + (right - left) * knobPosition_,
        track.center().y());
    painter.setPen(Qt::NoPen);
    painter.setBrush(
        isEnabled() ? QColor(250, 252, 255) : QColor(160, 166, 180));
    painter.drawEllipse(center, diameter / 2.0, diameter / 2.0);
}

void ToggleSwitch::animateTo(bool checked)
{
    if (!isVisible()) {
        setKnobPosition(checked ? 1.0 : 0.0);
        return;
    }
    animation_->stop();
    animation_->setStartValue(knobPosition_);
    animation_->setEndValue(checked ? 1.0 : 0.0);
    animation_->start();
}

BrandMark::BrandMark(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(36, 36);
}

QSize BrandMark::sizeHint() const
{
    return QSize(36, 36);
}

void BrandMark::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient fill(body.topLeft(), body.bottomRight());
    fill.setColorAt(0.0, blueBright());
    fill.setColorAt(0.55, blue());
    fill.setColorAt(1.0, QColor(QStringLiteral("#2a5fd0")));
    painter.setPen(QPen(QColor(255, 255, 255, 45), 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(body, 10.0, 10.0);

    QFont logoFont = monoFont(15, QFont::Black);
    painter.setFont(logoFont);
    painter.setPen(Qt::white);
    painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("P"));
}

QLabel *makeCaption(const QString &value, QWidget *parent)
{
    auto *label = new QLabel(value, parent);
    label->setFont(uiFont(8));
    label->setStyleSheet(QStringLiteral("color: #9aa5b8;"));
    return label;
}

QLabel *makeTitle(const QString &value, QWidget *parent, int pointSize)
{
    auto *label = new QLabel(value, parent);
    label->setFont(uiFont(pointSize, QFont::DemiBold));
    label->setStyleSheet(QStringLiteral("color: #eef2f8;"));
    return label;
}

QWidget *makeField(
    const QString &labelText,
    QWidget *control,
    QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    layout->addWidget(makeCaption(labelText, container));
    layout->addWidget(control);
    return container;
}

QWidget *makeSwitchRow(
    const QString &title,
    const QString &subtitle,
    ToggleSwitch *toggle,
    QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *labels = new QWidget(container);
    auto *labelLayout = new QVBoxLayout(labels);
    labelLayout->setContentsMargins(0, 0, 0, 0);
    labelLayout->setSpacing(2);
    auto *titleLabel = new QLabel(title, labels);
    titleLabel->setFont(uiFont(9, QFont::Medium));
    titleLabel->setStyleSheet(QStringLiteral("color: #dfe6f2;"));
    labelLayout->addWidget(titleLabel);
    if (!subtitle.isEmpty()) {
        auto *subLabel = new QLabel(subtitle, labels);
        subLabel->setFont(uiFont(8));
        subLabel->setStyleSheet(QStringLiteral("color: #69728a;"));
        labelLayout->addWidget(subLabel);
    }
    layout->addWidget(labels, 1);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    return container;
}

} // namespace pd::ui
