#include "ui/code_input.h"

#include "ui/ui_theme.h"

#include <QKeyEvent>
#include <QSignalBlocker>

#include <algorithm>

namespace pd::ui {

CodeInput::CodeInput(QWidget *parent)
    : QLineEdit(parent)
{
    setPlaceholderText(QStringLiteral("000 000 000"));
    setMaxLength(11);
    setClearButtonEnabled(false);
    setInputMethodHints(Qt::ImhDigitsOnly);
    setFont(monoFont(17, QFont::DemiBold));
    setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  letter-spacing: 5px;"
        "  color: #f7faff;"
        "  padding: 12px 14px;"
        "  background: rgba(8, 10, 15, 0.78);"
        "  border: 1px solid rgba(61, 123, 253, 0.42);"
        "  border-radius: 10px;"
        "}"
        "QLineEdit:focus {"
        "  border: 1px solid #5b9bff;"
        "  background: rgba(10, 13, 21, 0.94);"
        "}"));
    connect(
        this,
        &QLineEdit::textChanged,
        this,
        [this](const QString &value) {
            if (formatting_) {
                return;
            }
            reformat(value, cursorPosition());
        });
}

QString CodeInput::digits() const
{
    return compactConnectionCode(text());
}

void CodeInput::setDigits(const QString &digits)
{
    formatting_ = true;
    setText(formatConnectionCode(digits));
    formatting_ = false;
    setCursorPosition(text().size());
    setInvalid(false);
    emit digitsChanged(this->digits());
}

void CodeInput::setInvalid(bool invalid)
{
    if (invalid_ == invalid) {
        return;
    }
    invalid_ = invalid;
    const QString border = invalid
        ? QStringLiteral("#f2555a")
        : QStringLiteral("rgba(61, 123, 253, 0.42)");
    const QString focusBorder = invalid
        ? QStringLiteral("#ff7075")
        : QStringLiteral("#5b9bff");
    setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  letter-spacing: 5px;"
        "  color: #f7faff;"
        "  padding: 12px 14px;"
        "  background: rgba(8, 10, 15, 0.78);"
        "  border: 1px solid %1;"
        "  border-radius: 10px;"
        "}"
        "QLineEdit:focus {"
        "  border: 1px solid %2;"
        "  background: rgba(10, 13, 21, 0.94);"
        "}")
                    .arg(border, focusBorder));
}

void CodeInput::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        return;
    }
    if (!event->text().isEmpty()
        && !event->text().at(0).isDigit()
        && event->key() != Qt::Key_Backspace
        && event->key() != Qt::Key_Delete
        && event->key() != Qt::Key_Left
        && event->key() != Qt::Key_Right
        && event->key() != Qt::Key_Home
        && event->key() != Qt::Key_End
        && event->key() != Qt::Key_Tab
        && event->key() != Qt::Key_Return
        && event->key() != Qt::Key_Enter) {
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void CodeInput::reformat(const QString &value, qsizetype cursor)
{
    qsizetype digitsBeforeCursor = 0;
    const qsizetype boundedCursor = std::clamp<qsizetype>(
        cursor,
        0,
        value.size());
    for (qsizetype index = 0; index < boundedCursor; ++index) {
        if (value.at(index).isDigit()) {
            ++digitsBeforeCursor;
        }
    }

    const QString compact = compactConnectionCode(value);
    const QString formatted = formatConnectionCode(compact);
    if (formatted != value) {
        formatting_ = true;
        {
            const QSignalBlocker blocker(this);
            setText(formatted);
        }
        formatting_ = false;
    }

    qsizetype desired = 0;
    qsizetype seen = 0;
    while (desired < formatted.size() && seen < digitsBeforeCursor) {
        if (formatted.at(desired).isDigit()) {
            ++seen;
        }
        ++desired;
    }
    if (desired < formatted.size()
        && !formatted.at(desired).isDigit()
        && seen == digitsBeforeCursor) {
        desired = std::min<qsizetype>(desired + 1, formatted.size());
    }
    setCursorPosition(static_cast<int>(desired));
    if (invalid_ && !compact.isEmpty()) {
        setInvalid(false);
    }
    emit digitsChanged(compact);
}

} // namespace pd::ui
