#include "ui/ui_theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QPainter>
#include <QPalette>
#include <QRadialGradient>
#include <QRect>
#include <QSize>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>

namespace pd::ui {
namespace {

QString preferredFamily(
    const QStringList &families,
    const QString &fallback)
{
    const QStringList available = QFontDatabase::families();
    for (const QString &family : families) {
        if (available.contains(family)) {
            return family;
        }
    }
    return fallback;
}

QString uiFamily()
{
    static const QString family = preferredFamily(
        {
            QStringLiteral("Microsoft YaHei UI"),
            QStringLiteral("Microsoft YaHei"),
            QStringLiteral("PingFang SC"),
            QStringLiteral("Segoe UI"),
        },
        QStringLiteral("Sans Serif"));
    return family;
}

QString monoFamily()
{
    static const QString family = preferredFamily(
        {
            QStringLiteral("Cascadia Mono"),
            QStringLiteral("JetBrains Mono"),
            QStringLiteral("Consolas"),
        },
        QStringLiteral("Monospace"));
    return family;
}

QColor withAlpha(const QColor &color, int alpha)
{
    QColor result = color;
    result.setAlpha(std::clamp(alpha, 0, 255));
    return result;
}

} // namespace

QColor background() { return QColor(QStringLiteral("#0b0d12")); }
QColor surface() { return QColor(QStringLiteral("#12151d")); }
QColor surfaceRaised() { return QColor(QStringLiteral("#171b26")); }
QColor text() { return QColor(QStringLiteral("#eef2f8")); }
QColor textDim() { return QColor(QStringLiteral("#9aa5b8")); }
QColor textFaint() { return QColor(QStringLiteral("#69728a")); }
QColor blue() { return QColor(QStringLiteral("#3d7bfd")); }
QColor blueBright() { return QColor(QStringLiteral("#5b9bff")); }
QColor cyan() { return QColor(QStringLiteral("#43d9e8")); }
QColor green() { return QColor(QStringLiteral("#35d07f")); }
QColor amber() { return QColor(QStringLiteral("#f4b740")); }
QColor red() { return QColor(QStringLiteral("#f2555a")); }

QFont uiFont(int pointSize, int weight)
{
    QFont font(uiFamily());
    font.setPointSizeF(static_cast<qreal>(pointSize));
    if (weight >= 0) {
        font.setWeight(static_cast<QFont::Weight>(weight));
    }
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

QFont monoFont(int pointSize, int weight)
{
    QFont font(monoFamily());
    font.setPointSizeF(static_cast<qreal>(pointSize));
    if (weight >= 0) {
        font.setWeight(static_cast<QFont::Weight>(weight));
    }
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

void applyTheme(QApplication &application)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    QPalette palette;
    palette.setColor(QPalette::Window, background());
    palette.setColor(QPalette::WindowText, text());
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#0d1118")));
    palette.setColor(QPalette::AlternateBase, surface());
    palette.setColor(QPalette::ToolTipBase, surfaceRaised());
    palette.setColor(QPalette::ToolTipText, text());
    palette.setColor(QPalette::Text, text());
    palette.setColor(QPalette::Button, surfaceRaised());
    palette.setColor(QPalette::ButtonText, text());
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, blueBright());
    palette.setColor(QPalette::Highlight, blue());
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, textFaint());
    palette.setColor(QPalette::Disabled, QPalette::Text, textFaint());
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, textFaint());
    application.setPalette(palette);
    application.setFont(uiFont(10));
    application.setStyleSheet(styleSheet());
}

void paintBackdrop(QPainter &painter, const QSize &size)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(QPoint(0, 0), size), background());

    const qreal width = std::max(1, size.width());
    const qreal height = std::max(1, size.height());

    QRadialGradient blueGlow(
        QPointF(width * 0.18, -height * 0.08),
        width * 0.82);
    blueGlow.setColorAt(0.0, withAlpha(blue(), 62));
    blueGlow.setColorAt(0.56, withAlpha(blue(), 20));
    blueGlow.setColorAt(1.0, withAlpha(blue(), 0));
    painter.fillRect(QRect(QPoint(0, 0), size), blueGlow);

    QRadialGradient cyanGlow(
        QPointF(width * 0.98, height * 0.02),
        width * 0.62);
    cyanGlow.setColorAt(0.0, withAlpha(cyan(), 34));
    cyanGlow.setColorAt(0.52, withAlpha(cyan(), 10));
    cyanGlow.setColorAt(1.0, withAlpha(cyan(), 0));
    painter.fillRect(QRect(QPoint(0, 0), size), cyanGlow);

    QRadialGradient lowerGlow(
        QPointF(width * 0.52, height * 1.08),
        width * 0.66);
    lowerGlow.setColorAt(0.0, withAlpha(blue(), 26));
    lowerGlow.setColorAt(0.62, withAlpha(blue(), 6));
    lowerGlow.setColorAt(1.0, withAlpha(blue(), 0));
    painter.fillRect(QRect(QPoint(0, 0), size), lowerGlow);
}

QString styleSheet()
{
    return QStringLiteral(R"(
QWidget {
    color: #eef2f8;
    font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI";
    font-size: 14px;
    background: transparent;
}
QToolTip {
    color: #eef2f8;
    background: #171b26;
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 6px;
    padding: 6px 8px;
}
QLineEdit, QComboBox, QPlainTextEdit {
    color: #eef2f8;
    background: rgba(8, 10, 15, 0.72);
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 10px;
    padding: 10px 13px;
    selection-background-color: #3d7bfd;
    selection-color: #ffffff;
}
QLineEdit:hover, QComboBox:hover, QPlainTextEdit:hover {
    border-color: rgba(255, 255, 255, 0.25);
}
QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {
    border-color: #3d7bfd;
    background: rgba(10, 13, 20, 0.88);
}
QLineEdit:disabled, QComboBox:disabled {
    color: #69728a;
    background: rgba(8, 10, 15, 0.42);
}
QComboBox {
    padding-right: 30px;
}
QComboBox::drop-down {
    width: 30px;
    border: 0;
}
QComboBox::down-arrow {
    image: none;
    width: 0;
    height: 0;
}
QComboBox QAbstractItemView {
    color: #eef2f8;
    background: #171b26;
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 10px;
    padding: 6px;
    selection-background-color: rgba(61, 123, 253, 0.28);
    selection-color: #ffffff;
    outline: 0;
}
QComboBox QAbstractItemView::item {
    min-height: 28px;
    padding: 2px 8px;
    border-radius: 6px;
}
QPushButton {
    color: #eef2f8;
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 10px;
    padding: 9px 15px;
    font-weight: 600;
}
QPushButton:hover {
    background: rgba(255, 255, 255, 0.11);
    border-color: rgba(255, 255, 255, 0.24);
}
QPushButton:pressed {
    background: rgba(255, 255, 255, 0.04);
}
QPushButton:disabled {
    color: #69728a;
    background: rgba(255, 255, 255, 0.025);
    border-color: rgba(255, 255, 255, 0.07);
}
QPushButton#primaryButton {
    color: #ffffff;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
        stop:0 #5b9bff, stop:1 #3d7bfd);
    border: 1px solid rgba(91, 155, 255, 0.7);
    border-radius: 10px;
    padding: 11px 18px;
    font-size: 14px;
    font-weight: 700;
}
QPushButton#primaryButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
        stop:0 #6ea7ff, stop:1 #4a86ff);
}
QPushButton#quietButton {
    background: transparent;
    border-color: transparent;
    color: #9aa5b8;
}
QPushButton#quietButton:hover {
    background: rgba(255, 255, 255, 0.07);
    color: #eef2f8;
}
QPushButton#navButton {
    background: transparent;
    border: 1px solid transparent;
    color: #9aa5b8;
    padding: 8px 13px;
}
QPushButton#navButton:hover {
    background: rgba(255, 255, 255, 0.07);
    color: #eef2f8;
}
QPushButton#navButton:checked {
    background: rgba(61, 123, 253, 0.18);
    border-color: rgba(61, 123, 253, 0.34);
    color: #dce8ff;
}
QScrollArea {
    border: 0;
    background: transparent;
}
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: rgba(255, 255, 255, 0.16);
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
QPlainTextEdit {
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 11px;
    line-height: 1.6;
}
QFileDialog {
    background: #12151d;
}
)");
}

QString formatConnectionCode(const QString &value)
{
    QString digits;
    digits.reserve(9);
    for (const QChar character : value) {
        if (character.isDigit()) {
            digits.append(character);
            if (digits.size() == 9) {
                break;
            }
        }
    }
    QString formatted;
    formatted.reserve(11);
    for (qsizetype index = 0; index < digits.size(); ++index) {
        if (index == 3 || index == 6) {
            formatted.append(QLatin1Char(' '));
        }
        formatted.append(digits.at(index));
    }
    return formatted;
}

QString compactConnectionCode(const QString &value)
{
    QString digits;
    digits.reserve(9);
    for (const QChar character : value) {
        if (character.isDigit()) {
            digits.append(character);
            if (digits.size() == 9) {
                break;
            }
        }
    }
    return digits;
}

} // namespace pd::ui
