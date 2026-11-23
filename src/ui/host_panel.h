#pragma once

#include "ui/outgoing_file_queue.h"
#include "ui/ui_widgets.h"
#include "session/session_types.h"

#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace pd {
class HostController;
} // namespace pd

namespace pd::ui {

class PillLabel;
class StatusDot;
class ToggleSwitch;

class HostPanel final : public BackdropWidget {
    Q_OBJECT

public:
    explicit HostPanel(
        HostController *controller,
        bool autoStart = true,
        QWidget *parent = nullptr);

    void activate();
    void setAutoStart(bool autoStart) { autoStart_ = autoStart; }
    void startHosting();
    void stopHosting();
    void appendLog(const QString &message);

    [[nodiscard]] bool autoStart() const { return autoStart_; }
    [[nodiscard]] QLabel *codeLabel() const { return codeValue_; }
    [[nodiscard]] QPushButton *copyButton() const { return copyButton_; }
    [[nodiscard]] QPushButton *regenerateButton() const
    {
        return regenerateButton_;
    }
    [[nodiscard]] QComboBox *monitorCombo() const { return monitorCombo_; }
    [[nodiscard]] ToggleSwitch *inputToggle() const { return inputToggle_; }
    [[nodiscard]] QPushButton *sendFileButton() const { return sendFileButton_; }
    [[nodiscard]] QPushButton *fileRetryButton() const
    {
        return fileRetryButton_;
    }
    [[nodiscard]] QLabel *fileStatus() const { return fileStatus_; }
    [[nodiscard]] QLineEdit *filePathInput() const { return filePathInput_; }
    [[nodiscard]] QPlainTextEdit *logView() const { return logView_; }
    [[nodiscard]] bool fileBusy() const { return fileBusy_; }
    [[nodiscard]] int fileQueueSize() const { return fileQueue_.size(); }
    [[nodiscard]] bool fileQueuePaused() const { return fileQueue_.paused(); }

public slots:
    void refreshFromController();
    void chooseAndSendFile();
    void sendFiles(const QStringList &paths);

signals:
    void toastRequested(const QString &message);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void connectSignals();
    void updateState();
    void updatePeer();
    void updateMonitors(const QList<MonitorInfo> &monitors);
    void updateVideoSettings(const VideoSettings &settings);
    void updateStatistics(const QJsonObject &statistics);
    void updateLinkState(const QString &state);
    void sendFile(const QString &path);
    void flushFileQueue();
    void updateFileTransferUi(
        const QString &message = {},
        const QString &color = {});
    void clearFileTransferState();
    void setFileBusy(bool busy);
    void setSyncing(bool syncing);
    void selectComboValue(QComboBox *combo, int value);

    HostController *controller_ = nullptr;
    bool autoStart_ = true;
    bool syncing_ = false;
    bool fileBusy_ = false;
    bool cancelPending_ = false;
    int lastFilePercent_ = -1;
    QString peerName_;
    QString linkState_;
    OutgoingFileQueue fileQueue_;

    PillLabel *modePill_ = nullptr;
    PillLabel *serverPill_ = nullptr;
    QLabel *codeValue_ = nullptr;
    QLabel *codeHint_ = nullptr;
    QLabel *statusText_ = nullptr;
    QLabel *pathText_ = nullptr;
    QLabel *fileStatus_ = nullptr;
    StatusDot *statusDot_ = nullptr;
    QPushButton *copyButton_ = nullptr;
    QPushButton *regenerateButton_ = nullptr;
    QPushButton *powerButton_ = nullptr;
    QComboBox *monitorCombo_ = nullptr;
    QComboBox *fpsCombo_ = nullptr;
    QComboBox *widthCombo_ = nullptr;
    QComboBox *qualityCombo_ = nullptr;
    ToggleSwitch *inputToggle_ = nullptr;
    ToggleSwitch *clipboardToggle_ = nullptr;
    ToggleSwitch *skipStaticToggle_ = nullptr;
    ToggleSwitch *audioToggle_ = nullptr;
    QLineEdit *filePathInput_ = nullptr;
    QPushButton *sendFileButton_ = nullptr;
    QPushButton *fileRetryButton_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
};

} // namespace pd::ui
