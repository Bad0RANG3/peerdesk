#include "ui/settings_dialog.h"

#include "app/diagnostic_service.h"
#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/quality_presets.h"
#include "app/viewer_controller.h"
#include "core/util.h"
#include "diagnostics/logging.h"
#include "platform/startup_registration.h"
#include "session/session_types.h"
#include "ui/connection_page.h"
#include "ui/ui_theme.h"
#include "ui/ui_widgets.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace pd::ui {
namespace {

QLabel *makeSectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(uiFont(9, QFont::DemiBold));
    label->setStyleSheet(QStringLiteral("color: #dce8ff;"));
    return label;
}

QLabel *makeNote(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(uiFont(8));
    label->setStyleSheet(QStringLiteral("color: #69728a;"));
    label->setWordWrap(true);
    return label;
}

QLineEdit *makeInput(
    const QString &objectName,
    const QString &placeholder,
    QWidget *parent)
{
    auto *input = new QLineEdit(parent);
    input->setObjectName(objectName);
    input->setPlaceholderText(placeholder);
    input->setMinimumHeight(41);
    return input;
}

} // namespace

SettingsDialog::SettingsDialog(
    HostController *host,
    ViewerController *viewer,
    std::shared_ptr<Preferences> preferences,
    ConnectionPage *connectionPage,
    std::shared_ptr<StartupRegistration> startupRegistration,
    QWidget *parent)
    : QDialog(parent)
    , host_(host)
    , viewer_(viewer)
    , preferences_(std::move(preferences))
    , connectionPage_(connectionPage)
    , startupRegistration_(std::move(startupRegistration))
{
    Q_ASSERT(host_);
    Q_ASSERT(viewer_);

    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("设置 · PeerDesk"));
    setModal(false);
    setSizeGripEnabled(false);
    resize(560, 690);
    buildUi();
    applyPreferences();
}

void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(26, 24, 26, 22);
    root->setSpacing(0);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    auto *headingText = new QWidget(this);
    auto *headingLayout = new QVBoxLayout(headingText);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(3);
    auto *title = makeTitle(QStringLiteral("设置"), headingText, 18);
    title->setObjectName(QStringLiteral("settingsTitle"));
    headingLayout->addWidget(title);
    headingLayout->addWidget(makeNote(
        QStringLiteral("连接、画面与本地行为集中在这里管理"),
        headingText));
    heading->addWidget(headingText, 1);
    auto *pill = new PillLabel(this);
    pill->setText(QStringLiteral("本机配置"));
    pill->setTone(PillLabel::Tone::Blue);
    heading->addWidget(pill, 0, Qt::AlignTop);
    root->addLayout(heading);
    root->addSpacing(18);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(QStringLiteral("连接"), content));
    serverInput_ = makeInput(
        QStringLiteral("settingsServerInput"),
        QStringLiteral("192.168.1.2:7900"),
        content);
    serverInput_->setAccessibleName(QStringLiteral("信令服务器"));
    layout->addWidget(makeField(
        QStringLiteral("信令服务器"),
        serverInput_,
        content));

    tokenInput_ = makeInput(
        QStringLiteral("settingsTokenInput"),
        QStringLiteral("服务器未设置令牌时留空"),
        content);
    tokenInput_->setEchoMode(QLineEdit::Password);
    tokenInput_->setAccessibleName(QStringLiteral("服务器令牌"));
    layout->addWidget(makeField(
        QStringLiteral("服务器令牌（可选）"),
        tokenInput_,
        content));

    qualityCombo_ = new QComboBox(content);
    qualityCombo_->setObjectName(QStringLiteral("settingsQualityCombo"));
    qualityCombo_->setMinimumHeight(41);
    qualityCombo_->setAccessibleName(QStringLiteral("默认画质"));
    for (const QualityPreset &preset : qualityPresets()) {
        qualityCombo_->addItem(
            QStringLiteral("%1 · %2 fps / %3 px")
                .arg(preset.name)
                .arg(preset.fps)
                .arg(preset.maxWidth),
            preset.id);
    }
    layout->addWidget(makeField(
        QStringLiteral("控制端默认画质"),
        qualityCombo_,
        content));

    audioToggle_ = new ToggleSwitch(content);
    audioToggle_->setObjectName(QStringLiteral("settingsAudioToggle"));
    audioToggle_->setAccessibleName(QStringLiteral("接收远端声音"));
    layout->addWidget(makeSwitchRow(
        QStringLiteral("接收远端声音"),
        QStringLiteral("会话中也会同步开启或停止被控端声音采集"),
        audioToggle_,
        content));

    layout->addSpacing(2);
    layout->addWidget(makeSectionTitle(
        QStringLiteral("文件"),
        content));
    auto *directoryRow = new QWidget(content);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(8);
    receiveDirectoryInput_ = makeInput(
        QStringLiteral("settingsReceiveDirectoryInput"),
        QStringLiteral("留空使用系统下载目录"),
        directoryRow);
    receiveDirectoryInput_->setAccessibleName(QStringLiteral("文件接收目录"));
    directoryLayout->addWidget(receiveDirectoryInput_, 1);
    browseButton_ = new QPushButton(QStringLiteral("浏览…"), directoryRow);
    browseButton_->setObjectName(QStringLiteral("settingsBrowseButton"));
    browseButton_->setCursor(Qt::PointingHandCursor);
    browseButton_->setMinimumHeight(41);
    directoryLayout->addWidget(browseButton_);
    layout->addWidget(makeField(
        QStringLiteral("接收的文件保存到"),
        directoryRow,
        content));
    layout->addWidget(makeNote(
        QStringLiteral("更改目录会中断正在进行的文件接收；留空则恢复默认下载目录。"),
        content));

    layout->addSpacing(2);
    layout->addWidget(makeSectionTitle(
        QStringLiteral("窗口"),
        content));
    closeToTrayToggle_ = new ToggleSwitch(content);
    closeToTrayToggle_->setObjectName(
        QStringLiteral("settingsCloseToTrayToggle"));
    closeToTrayToggle_->setAccessibleName(QStringLiteral("关闭到系统托盘"));
    layout->addWidget(makeSwitchRow(
        QStringLiteral("关闭到系统托盘"),
        QStringLiteral("关闭主窗口后继续运行，可从托盘恢复或退出"),
        closeToTrayToggle_,
        content));

    layout->addSpacing(2);
    layout->addWidget(makeSectionTitle(
        QStringLiteral("系统"),
        content));
    startupToggle_ = new ToggleSwitch(content);
    startupToggle_->setObjectName(
        QStringLiteral("settingsStartupToggle"));
    startupToggle_->setAccessibleName(
        QStringLiteral("登录时启动 PeerDesk"));
    layout->addWidget(makeSwitchRow(
        QStringLiteral("登录时启动 PeerDesk"),
        QStringLiteral("登录后驻留系统托盘并保持本机可被控"),
        startupToggle_,
        content));
    startupNote_ = makeNote(QString(), content);
    startupNote_->setObjectName(
        QStringLiteral("settingsStartupNote"));
    layout->addWidget(startupNote_);

    layout->addSpacing(2);
    layout->addWidget(makeSectionTitle(
        QStringLiteral("日志与诊断"),
        content));
    logDirectoryLabel_ = makeNote(QString(), content);
    logDirectoryLabel_->setObjectName(
        QStringLiteral("settingsLogDirectoryLabel"));
    logDirectoryLabel_->setTextInteractionFlags(
        Qt::TextSelectableByMouse);
    layout->addWidget(logDirectoryLabel_);

    diagnosticsCrashNote_ = makeNote(
        QStringLiteral(
            "上次运行未正常退出。可先打开日志目录，或导出一份脱敏诊断包供排查。"),
        content);
    diagnosticsCrashNote_->setObjectName(
        QStringLiteral("settingsDiagnosticsCrashNote"));
    diagnosticsCrashNote_->setStyleSheet(
        QStringLiteral("color: #f0b45a;"));
    layout->addWidget(diagnosticsCrashNote_);

    auto *diagnosticsActions = new QHBoxLayout;
    diagnosticsActions->setContentsMargins(0, 0, 0, 0);
    diagnosticsActions->setSpacing(8);
    openLogsButton_ = new QPushButton(
        QStringLiteral("打开日志目录"),
        content);
    openLogsButton_->setObjectName(
        QStringLiteral("settingsOpenLogsButton"));
    openLogsButton_->setCursor(Qt::PointingHandCursor);
    openLogsButton_->setMinimumHeight(34);
    diagnosticsActions->addWidget(openLogsButton_);
    exportDiagnosticsButton_ = new QPushButton(
        QStringLiteral("导出诊断包…"),
        content);
    exportDiagnosticsButton_->setObjectName(
        QStringLiteral("settingsExportDiagnosticsButton"));
    exportDiagnosticsButton_->setCursor(Qt::PointingHandCursor);
    exportDiagnosticsButton_->setMinimumHeight(34);
    diagnosticsActions->addWidget(exportDiagnosticsButton_);
    diagnosticsActions->addStretch(1);
    layout->addLayout(diagnosticsActions);

    diagnosticsStatusLabel_ = makeNote(QString(), content);
    diagnosticsStatusLabel_->setObjectName(
        QStringLiteral("settingsDiagnosticsStatusLabel"));
    diagnosticsStatusLabel_->hide();
    layout->addWidget(diagnosticsStatusLabel_);
    layout->addWidget(makeNote(
        QStringLiteral(
            "诊断包只包含当前客户端日志、应用版本和系统环境；服务器地址与网络端点会被替换为占位符，不包含令牌、连接码、文件名或接收目录。"),
        content));

    layout->addWidget(makeNote(
        QStringLiteral("服务器和令牌会用于下一次连接，不会中断当前会话；画质和声音立即生效。"),
        content));
    layout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("settingsErrorLabel"));
    errorLabel_->setFont(uiFont(8));
    errorLabel_->setStyleSheet(QStringLiteral("color: #f2555a;"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    root->addWidget(errorLabel_);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(9);
    buttons->addStretch(1);
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    cancelButton->setObjectName(QStringLiteral("settingsCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(cancelButton);
    saveButton_ = new QPushButton(QStringLiteral("保存设置"), this);
    saveButton_->setObjectName(QStringLiteral("primaryButton"));
    saveButton_->setAccessibleName(QStringLiteral("保存设置"));
    saveButton_->setCursor(Qt::PointingHandCursor);
    saveButton_->setDefault(true);
    buttons->addWidget(saveButton_);
    root->addSpacing(14);
    root->addLayout(buttons);

    connect(browseButton_, &QPushButton::clicked, this, [this]() {
        const QString directory = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("选择文件接收目录"),
            receiveDirectoryInput_->text().trimmed());
        if (!directory.isEmpty()) {
            receiveDirectoryInput_->setText(directory);
            errorLabel_->hide();
        }
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::hide);
    connect(saveButton_, &QPushButton::clicked, this, &SettingsDialog::save);
    connect(serverInput_, &QLineEdit::returnPressed, this, &SettingsDialog::save);
    connect(tokenInput_, &QLineEdit::returnPressed, this, &SettingsDialog::save);
    connect(
        serverInput_,
        &QLineEdit::textChanged,
        this,
        [this]() { errorLabel_->hide(); });
    connect(
        tokenInput_,
        &QLineEdit::textChanged,
        this,
        [this]() { errorLabel_->hide(); });
    connect(
        startupToggle_,
        &QAbstractButton::toggled,
        this,
        [this]() { errorLabel_->hide(); });
    connect(
        openLogsButton_,
        &QPushButton::clicked,
        this,
        &SettingsDialog::openLogDirectory);
    connect(
        exportDiagnosticsButton_,
        &QPushButton::clicked,
        this,
        &SettingsDialog::exportDiagnostics);
}

void SettingsDialog::reload()
{
    errorLabel_->hide();
    applyPreferences();
}

void SettingsDialog::applyPreferences()
{
    const ViewerPreferences viewerPreferences =
        preferences_ ? preferences_->viewer() : ViewerPreferences();
    const QString server = viewer_->server().isEmpty()
        ? viewerPreferences.server
        : viewer_->server();
    const QString token = viewer_->token().isEmpty()
        ? viewerPreferences.token
        : viewer_->token();
    serverInput_->setText(server);
    tokenInput_->setText(token);
    qualityCombo_->setCurrentIndex(
        std::max(0, qualityCombo_->findData(viewer_->qualityId())));
    audioToggle_->setCheckedSilently(viewer_->audioEnabled());
    receiveDirectoryInput_->setText(viewerPreferences.receiveDirectory);
    closeToTrayToggle_->setCheckedSilently(
        preferences_ ? preferences_->window().closeToTray : true);

    const bool startupSupported =
        startupRegistration_ && startupRegistration_->isSupported();
    startupToggle_->setEnabled(startupSupported);
    startupToggle_->setCursor(
        startupSupported ? Qt::PointingHandCursor : Qt::ArrowCursor);
    startupToggle_->setCheckedSilently(
        startupSupported && startupRegistration_->isEnabled());
    startupNote_->setText(
        startupSupported
            ? QStringLiteral("仅对当前 Windows 用户生效，不需要管理员权限；可随时在这里关闭。")
            : QStringLiteral("当前平台不支持登录时启动。"));
    refreshDiagnosticsInfo();
}

void SettingsDialog::showError(const QString &message)
{
    errorLabel_->setText(message);
    errorLabel_->show();
}

void SettingsDialog::refreshDiagnosticsInfo()
{
    const QString directory = DiagnosticService::logDirectory();
    logDirectoryLabel_->setText(
        QStringLiteral("日志目录：%1").arg(directory));
    diagnosticsCrashNote_->setVisible(
        diagnostics::LogManager::instance().previousSessionCrashed());
}

void SettingsDialog::openLogDirectory()
{
    const QString directory = DiagnosticService::logDirectory();
    if (!QDir().mkpath(directory)) {
        diagnosticsStatusLabel_->setStyleSheet(
            QStringLiteral("color: #f2555a;"));
        diagnosticsStatusLabel_->setText(
            QStringLiteral("无法创建日志目录。"));
        diagnosticsStatusLabel_->show();
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        diagnosticsStatusLabel_->setStyleSheet(
            QStringLiteral("color: #f2555a;"));
        diagnosticsStatusLabel_->setText(
            QStringLiteral("无法打开日志目录，可从上方路径手动访问。"));
        diagnosticsStatusLabel_->show();
        return;
    }
    diagnosticsStatusLabel_->setStyleSheet(
        QStringLiteral("color: #70d69c;"));
    diagnosticsStatusLabel_->setText(
        QStringLiteral("已在文件资源管理器中打开日志目录。"));
    diagnosticsStatusLabel_->show();
}

void SettingsDialog::exportDiagnostics()
{
    if (!preferences_) {
        diagnosticsStatusLabel_->setStyleSheet(
            QStringLiteral("color: #f2555a;"));
        diagnosticsStatusLabel_->setText(
            QStringLiteral("无法读取本机设置，诊断包未导出。"));
        diagnosticsStatusLabel_->show();
        return;
    }

    const QString downloads = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    const QString initialPath = QDir(
        downloads.isEmpty() ? QDir::homePath() : downloads)
                                    .filePath(
                                        DiagnosticService::defaultFileName());
    QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出诊断包"),
        initialPath,
        QStringLiteral("ZIP 诊断包 (*.zip)"));
    if (outputPath.isEmpty()) {
        return;
    }
    if (!outputPath.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        outputPath += QStringLiteral(".zip");
    }

    QString error;
    if (!DiagnosticService::exportBundle(
            *preferences_,
            outputPath,
            &error)) {
        diagnosticsStatusLabel_->setStyleSheet(
            QStringLiteral("color: #f2555a;"));
        diagnosticsStatusLabel_->setText(
            error.isEmpty()
                ? QStringLiteral("诊断包导出失败。")
                : QStringLiteral("诊断包导出失败：%1").arg(error));
        diagnosticsStatusLabel_->show();
        return;
    }
    diagnosticsStatusLabel_->setStyleSheet(
        QStringLiteral("color: #70d69c;"));
    diagnosticsStatusLabel_->setText(
        QStringLiteral("诊断包已导出：%1").arg(
            QFileInfo(outputPath).fileName()));
    diagnosticsStatusLabel_->show();
}

void SettingsDialog::save()
{
    const QString server = serverInput_->text().trimmed();
    if (!parseServerAddress(server).has_value()) {
        showError(QStringLiteral("信令服务器地址无效，例如 192.168.1.2:7900"));
        serverInput_->setFocus(Qt::OtherFocusReason);
        serverInput_->selectAll();
        return;
    }

    const QString token = tokenInput_->text().trimmed();
    const QString qualityId =
        qualityCombo_->currentData().toString();
    const bool audio = audioToggle_->isChecked();
    const bool closeToTray = closeToTrayToggle_->isChecked();
    const bool startupEnabled = startupToggle_->isChecked();
    QString receiveDirectory = receiveDirectoryInput_->text().trimmed();
    if (receiveDirectory.isEmpty()) {
        receiveDirectory = defaultReceiveDirectory();
        receiveDirectoryInput_->setText(receiveDirectory);
    }

    if (startupRegistration_ && startupRegistration_->isSupported()
        && startupRegistration_->isEnabled() != startupEnabled) {
        QString startupError;
        if (!startupRegistration_->setEnabled(
                startupEnabled,
                &startupError)) {
            showError(startupError.isEmpty()
                    ? QStringLiteral("无法更新登录时启动设置")
                    : startupError);
            return;
        }
    }

    host_->setServer(server);
    host_->setToken(token);
    viewer_->setServer(server);
    viewer_->setToken(token);
    viewer_->setQualityPreset(qualityId);
    viewer_->setAudioEnabled(audio);

    if (preferences_) {
        const HostPreferences hostPreferences = preferences_->host();
        const ViewerPreferences viewerPreferences = preferences_->viewer();
        if (hostPreferences.receiveDirectory != receiveDirectory) {
            host_->setReceiveDirectory(receiveDirectory);
        }
        if (viewerPreferences.receiveDirectory != receiveDirectory) {
            viewer_->setReceiveDirectory(receiveDirectory);
        }

        WindowPreferences window = preferences_->window();
        window.closeToTray = closeToTray;
        preferences_->setWindow(window);
    }

    if (connectionPage_) {
        connectionPage_->setServer(server);
        connectionPage_->setToken(token);
    }

    errorLabel_->hide();
    emit settingsSaved();
    accept();
}

void SettingsDialog::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    paintBackdrop(painter, size());
}

} // namespace pd::ui
