#pragma once

#include <QDialog>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace pd {
class HostController;
class Preferences;
class ViewerController;
} // namespace pd

namespace pd::ui {

class ConnectionPage;
class ToggleSwitch;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(
        HostController *host,
        ViewerController *viewer,
        std::shared_ptr<Preferences> preferences,
        ConnectionPage *connectionPage,
        QWidget *parent = nullptr);

    [[nodiscard]] QLineEdit *serverInput() const { return serverInput_; }
    [[nodiscard]] QLineEdit *tokenInput() const { return tokenInput_; }
    [[nodiscard]] QComboBox *qualityCombo() const { return qualityCombo_; }
    [[nodiscard]] ToggleSwitch *audioToggle() const { return audioToggle_; }
    [[nodiscard]] QLineEdit *receiveDirectoryInput() const
    {
        return receiveDirectoryInput_;
    }
    [[nodiscard]] QPushButton *browseButton() const { return browseButton_; }
    [[nodiscard]] ToggleSwitch *closeToTrayToggle() const
    {
        return closeToTrayToggle_;
    }
    [[nodiscard]] QLabel *errorLabel() const { return errorLabel_; }
    [[nodiscard]] QPushButton *saveButton() const { return saveButton_; }

public slots:
    void reload();
    void save();

signals:
    void settingsSaved();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void buildUi();
    void applyPreferences();
    void showError(const QString &message);

    HostController *host_ = nullptr;
    ViewerController *viewer_ = nullptr;
    std::shared_ptr<Preferences> preferences_;
    ConnectionPage *connectionPage_ = nullptr;

    QLineEdit *serverInput_ = nullptr;
    QLineEdit *tokenInput_ = nullptr;
    QComboBox *qualityCombo_ = nullptr;
    ToggleSwitch *audioToggle_ = nullptr;
    QLineEdit *receiveDirectoryInput_ = nullptr;
    QPushButton *browseButton_ = nullptr;
    ToggleSwitch *closeToTrayToggle_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QPushButton *saveButton_ = nullptr;
};

} // namespace pd::ui
