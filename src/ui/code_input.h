#pragma once

#include <QLineEdit>
#include <QString>

namespace pd::ui {

class CodeInput final : public QLineEdit {
    Q_OBJECT

public:
    explicit CodeInput(QWidget *parent = nullptr);

    [[nodiscard]] QString digits() const;
    void setDigits(const QString &digits);
    void setInvalid(bool invalid);

signals:
    void digitsChanged(const QString &digits);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void reformat(const QString &text, qsizetype cursor);

    bool formatting_ = false;
    bool invalid_ = false;
};

} // namespace pd::ui
