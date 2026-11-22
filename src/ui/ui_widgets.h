#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QString>

class QPropertyAnimation;

namespace pd::ui {

class BackdropWidget : public QWidget {
    Q_OBJECT

public:
    explicit BackdropWidget(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};

class GlassCard final : public QFrame {
    Q_OBJECT

public:
    explicit GlassCard(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};

class PillLabel final : public QWidget {
    Q_OBJECT

public:
    enum class Tone {
        Blue,
        Green,
        Amber,
        Red,
        Neutral,
    };

    explicit PillLabel(QWidget *parent = nullptr);

    void setText(const QString &text);
    [[nodiscard]] QString text() const { return text_; }
    void setTone(Tone tone);
    [[nodiscard]] Tone tone() const { return tone_; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString text_ = QStringLiteral("待机");
    Tone tone_ = Tone::Blue;
};

class StatusDot final : public QWidget {
    Q_OBJECT

public:
    explicit StatusDot(QWidget *parent = nullptr);

    void setColor(const QColor &color);
    [[nodiscard]] QColor color() const { return color_; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor color_ = QColor(QStringLiteral("#69728a"));
};

class ToggleSwitch final : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knobPosition READ knobPosition WRITE setKnobPosition)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] qreal knobPosition() const;
    void setKnobPosition(qreal position);
    void setCheckedSilently(bool checked);

protected:
    void paintEvent(QPaintEvent *event) override;
    void checkStateSet() override;
    void nextCheckState() override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void animateTo(bool checked);

    qreal knobPosition_ = 0.0;
    QPropertyAnimation *animation_ = nullptr;
    bool hovered_ = false;
};

class BrandMark final : public QWidget {
    Q_OBJECT

public:
    explicit BrandMark(QWidget *parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

[[nodiscard]] QLabel *makeCaption(
    const QString &text,
    QWidget *parent = nullptr);
[[nodiscard]] QLabel *makeTitle(
    const QString &text,
    QWidget *parent = nullptr,
    int pointSize = 16);
[[nodiscard]] QWidget *makeField(
    const QString &label,
    QWidget *control,
    QWidget *parent = nullptr);
[[nodiscard]] QWidget *makeSwitchRow(
    const QString &title,
    const QString &subtitle,
    ToggleSwitch *toggle,
    QWidget *parent = nullptr);

} // namespace pd::ui
