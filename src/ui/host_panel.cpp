#include "ui/host_panel.h"

#include "app/host_controller.h"
#include "app/quality_presets.h"
#include "session/session_types.h"
#include "ui/ui_theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace pd::ui {
namespace {

QLabel *makeSubtitle(const QString &value, QWidget *parent)
{
    auto *label = new QLabel(value, parent);
    label->setFont(uiFont(8));
    label->setStyleSheet(QStringLiteral("color: #69728a;"));
    label->setWordWrap(true);
    return label;
}

QComboBox *makeCombo(const QString &objectName, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setObjectName(objectName);
    combo->setCursor(Qt::PointingHandCursor);
    combo->setMinimumHeight(42);
    return combo;
}

void populateNumbers(
    QComboBox *combo,
    const QList<int> &values,
    const QString &suffix)
{
    for (const int value : values) {
        combo->addItem(
            QStringLiteral("%1 %2").arg(value).arg(suffix),
            value);
    }
}

void addShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(56);
    shadow->setOffset(0, 20);
    shadow->setColor(QColor(0, 0, 0, 155));
    widget->setGraphicsEffect(shadow);
}

QFrame *makeRail(QWidget *parent)
{
    auto *rail = new QFrame(parent);
    rail->setObjectName(QStringLiteral("hostStatusRail"));
    rail->setStyleSheet(QStringLiteral(
        "QFrame#hostStatusRail {"
        "  background: rgba(255, 255, 255, 0.032);"
        "  border: 1px solid rgba(255, 255, 255, 0.075);"
        "  border-radius: 10px;"
        "}"));
    return rail;
}

QString formatTransferSize(qint64 bytes)
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

QString fileFailureText(const QString &reason)
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
    if (reason == QStringLiteral("interrupted")) {
        return QStringLiteral("传输已中断");
    }
    if (reason == QStringLiteral("io")) {
        return QStringLiteral("文件读写失败");
    }
    if (reason == QStringLiteral("data")) {
        return QStringLiteral("文件数据校验失败");
    }
    return reason.isEmpty() ? QStringLiteral("文件传输失败") : reason;
}

} // namespace

HostPanel::HostPanel(
    HostController *controller,
    bool autoStart,
    QWidget *parent)
    : BackdropWidget(parent)
    , controller_(controller)
    , autoStart_(autoStart)
{
    Q_ASSERT(controller_);
    setAcceptDrops(true);
    buildUi();
    connectSignals();
    refreshFromController();
}

void HostPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: 0; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));
    root->addWidget(scroll);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("hostScrollContent"));
    content->setStyleSheet(QStringLiteral(
        "QWidget#hostScrollContent { background: transparent; }"));
    auto *outer = new QVBoxLayout(content);
    outer->setContentsMargins(30, 24, 30, 28);
    outer->setSpacing(0);
    outer->addStretch(1);

    auto *card = new GlassCard(content);
    card->setObjectName(QStringLiteral("hostCard"));
    card->setFixedWidth(620);
    addShadow(card);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(26, 24, 26, 24);
    layout->setSpacing(0);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    auto *headingText = new QWidget(card);
    auto *headingLayout = new QVBoxLayout(headingText);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(3);
    auto *title = makeTitle(QStringLiteral("本机连接码"), headingText, 17);
    title->setObjectName(QStringLiteral("hostTitle"));
    headingLayout->addWidget(title);
    headingLayout->addWidget(makeSubtitle(
        QStringLiteral("在另一台设备的 PeerDesk 中输入此码即可连接"),
        headingText));
    heading->addWidget(headingText, 1);

    auto *headingActions = new QVBoxLayout;
    headingActions->setContentsMargins(0, 0, 0, 0);
    headingActions->setSpacing(6);
    modePill_ = new PillLabel(card);
    modePill_->setText(QStringLiteral("待机"));
    modePill_->setTone(PillLabel::Tone::Neutral);
    headingActions->addWidget(modePill_, 0, Qt::AlignRight);
    powerButton_ = new QPushButton(QStringLiteral("启动被控"), card);
    powerButton_->setObjectName(QStringLiteral("powerButton"));
    powerButton_->setCursor(Qt::PointingHandCursor);
    powerButton_->setMinimumHeight(29);
    powerButton_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  padding: 4px 10px;"
        "  font-size: 11px;"
        "  border-radius: 8px;"
        "  background: rgba(255,255,255,0.055);"
        "  border: 1px solid rgba(255,255,255,0.12);"
        "  color: #9aa5b8;"
        "}"
        "QPushButton:hover { color: #eef2f8; background: rgba(255,255,255,0.09); }"));
    headingActions->addWidget(powerButton_, 0, Qt::AlignRight);
    heading->addLayout(headingActions);
    layout->addLayout(heading);
    layout->addSpacing(20);

    auto *codeBlock = new QFrame(card);
    codeBlock->setObjectName(QStringLiteral("hostCodeBlock"));
    codeBlock->setStyleSheet(QStringLiteral(
        "QFrame#hostCodeBlock {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 rgba(61, 123, 253, 0.15),"
        "    stop:1 rgba(61, 123, 253, 0.035));"
        "  border: 1px solid rgba(61, 123, 253, 0.36);"
        "  border-radius: 11px;"
        "}"));
    auto *codeLayout = new QVBoxLayout(codeBlock);
    codeLayout->setContentsMargins(18, 18, 18, 16);
    codeLayout->setSpacing(0);
    auto *codeCaption = new QLabel(QStringLiteral("CONNECTION CODE"), codeBlock);
    codeCaption->setAlignment(Qt::AlignCenter);
    QFont captionFont = uiFont(8, QFont::DemiBold);
    captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.2);
    codeCaption->setFont(captionFont);
    codeCaption->setStyleSheet(QStringLiteral("color: #8fa4c7;"));
    codeLayout->addWidget(codeCaption);
    codeLayout->addSpacing(8);
    codeValue_ = new QLabel(QStringLiteral("000 000 000"), codeBlock);
    codeValue_->setObjectName(QStringLiteral("hostCodeValue"));
    codeValue_->setAlignment(Qt::AlignCenter);
    QFont codeFont = monoFont(30, QFont::Bold);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 3.5);
    codeValue_->setFont(codeFont);
    codeValue_->setStyleSheet(QStringLiteral(
        "color: #f8fbff;"
        "background: transparent;"));
    codeValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    codeLayout->addWidget(codeValue_);
    codeLayout->addSpacing(7);
    codeHint_ = new QLabel(
        QStringLiteral("登录信令服务器后将显示可连接状态"),
        codeBlock);
    codeHint_->setAlignment(Qt::AlignCenter);
    codeHint_->setFont(uiFont(8));
    codeHint_->setStyleSheet(QStringLiteral("color: #69728a;"));
    codeHint_->setWordWrap(true);
    codeLayout->addWidget(codeHint_);
    layout->addWidget(codeBlock);
    layout->addSpacing(13);

    auto *codeActions = new QHBoxLayout;
    codeActions->setSpacing(8);
    copyButton_ = new QPushButton(QStringLiteral("复制连接码"), card);
    copyButton_->setObjectName(QStringLiteral("copyCodeButton"));
    copyButton_->setCursor(Qt::PointingHandCursor);
    copyButton_->setMinimumHeight(35);
    codeActions->addWidget(copyButton_);
    regenerateButton_ = new QPushButton(QStringLiteral("更换连接码"), card);
    regenerateButton_->setObjectName(QStringLiteral("regenerateButton"));
    regenerateButton_->setCursor(Qt::PointingHandCursor);
    regenerateButton_->setMinimumHeight(35);
    codeActions->addWidget(regenerateButton_);
    codeActions->addStretch(1);
    serverPill_ = new PillLabel(card);
    serverPill_->setText(QStringLiteral("服务器 —"));
    serverPill_->setTone(PillLabel::Tone::Neutral);
    codeActions->addWidget(serverPill_);
    layout->addLayout(codeActions);
    layout->addSpacing(13);

    auto *statusRail = makeRail(card);
    auto *statusLayout = new QHBoxLayout(statusRail);
    statusLayout->setContentsMargins(13, 10, 13, 10);
    statusLayout->setSpacing(9);
    statusDot_ = new StatusDot(statusRail);
    statusDot_->setColor(textFaint());
    statusLayout->addWidget(statusDot_);
    statusText_ = new QLabel(QStringLiteral("被控端未启动"), statusRail);
    statusText_->setObjectName(QStringLiteral("hostStatus"));
    statusText_->setFont(uiFont(8));
    statusText_->setStyleSheet(QStringLiteral("color: #9aa5b8;"));
    statusLayout->addWidget(statusText_, 1);
    pathText_ = new QLabel(QString(), statusRail);
    pathText_->setObjectName(QStringLiteral("linkPath"));
    pathText_->setFont(monoFont(8));
    pathText_->setStyleSheet(QStringLiteral("color: #69728a;"));
    statusLayout->addWidget(pathText_, 0);
    layout->addWidget(statusRail);

    auto *approvalCard = new QFrame(card);
    approvalCard->setObjectName(QStringLiteral("approvalCard"));
    approvalCard->setStyleSheet(QStringLiteral(
        "QFrame#approvalCard {"
        "  background: rgba(240, 179, 76, 0.09);"
        "  border: 1px solid rgba(240, 179, 76, 0.38);"
        "  border-radius: 11px;"
        "}"));
    auto *approvalLayout = new QVBoxLayout(approvalCard);
    approvalLayout->setContentsMargins(14, 12, 14, 12);
    approvalLayout->setSpacing(8);
    auto *approvalHeading = new QLabel(
        QStringLiteral("远程控制请求"), approvalCard);
    approvalHeading->setFont(uiFont(8, QFont::DemiBold));
    approvalHeading->setStyleSheet(QStringLiteral("color: #f0b34c;"));
    approvalLayout->addWidget(approvalHeading);
    approvalName_ = new QLabel(QString(), approvalCard);
    approvalName_->setObjectName(QStringLiteral("approvalName"));
    approvalName_->setFont(uiFont(10, QFont::DemiBold));
    approvalName_->setStyleSheet(QStringLiteral("color: #f5f8ff;"));
    approvalName_->setWordWrap(true);
    approvalLayout->addWidget(approvalName_);
    auto *approvalHint = new QLabel(
        QStringLiteral("对方请求控制本机，批准后才会建立远程会话。"),
        approvalCard);
    approvalHint->setFont(uiFont(8));
    approvalHint->setStyleSheet(QStringLiteral("color: #9aa5b8;"));
    approvalHint->setWordWrap(true);
    approvalLayout->addWidget(approvalHint);
    auto *approvalActions = new QHBoxLayout;
    approvalActions->setContentsMargins(0, 0, 0, 0);
    approvalActions->setSpacing(8);
    rejectButton_ = new QPushButton(
        QStringLiteral("拒绝"), approvalCard);
    rejectButton_->setObjectName(QStringLiteral("rejectButton"));
    rejectButton_->setCursor(Qt::PointingHandCursor);
    rejectButton_->setMinimumHeight(32);
    approvalActions->addWidget(rejectButton_);
    approveButton_ = new QPushButton(
        QStringLiteral("允许控制"), approvalCard);
    approveButton_->setObjectName(QStringLiteral("approveButton"));
    approveButton_->setCursor(Qt::PointingHandCursor);
    approveButton_->setMinimumHeight(32);
    approveButton_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  padding: 5px 14px;"
        "  border-radius: 8px;"
        "  background: #3d7bfd;"
        "  color: #ffffff;"
        "  border: 1px solid #5a8fff;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #4f88ff; }"
        "QPushButton:pressed { background: #2f68e0; }"));
    approvalActions->addWidget(approveButton_);
    approvalActions->addStretch(1);
    approvalLayout->addLayout(approvalActions);
    approvalCard->hide();
    approvalCard_ = approvalCard;
    layout->addWidget(approvalCard);
    layout->addSpacing(20);

    auto *sectionTitle = new QLabel(QStringLiteral("画面与性能"), card);
    sectionTitle->setFont(uiFont(10, QFont::DemiBold));
    sectionTitle->setStyleSheet(QStringLiteral("color: #dfe6f2;"));
    layout->addWidget(sectionTitle);
    layout->addSpacing(10);

    monitorCombo_ = makeCombo(QStringLiteral("monitorCombo"), card);
    fpsCombo_ = makeCombo(QStringLiteral("fpsCombo"), card);
    widthCombo_ = makeCombo(QStringLiteral("widthCombo"), card);
    qualityCombo_ = makeCombo(QStringLiteral("qualityCombo"), card);
    populateNumbers(fpsCombo_, {15, 24, 30, 45, 60}, QStringLiteral("fps"));
    populateNumbers(
        widthCombo_,
        {1280, 1600, 1920, 2560, 3840},
        QStringLiteral("px"));
    for (const QualityPreset &preset : qualityPresets()) {
        qualityCombo_->addItem(preset.name, preset.quality);
    }

    auto *settingsGrid = new QGridLayout;
    settingsGrid->setContentsMargins(0, 0, 0, 0);
    settingsGrid->setHorizontalSpacing(12);
    settingsGrid->setVerticalSpacing(12);
    settingsGrid->addWidget(
        makeField(QStringLiteral("显示器"), monitorCombo_, card),
        0,
        0);
    settingsGrid->addWidget(
        makeField(QStringLiteral("帧率"), fpsCombo_, card),
        0,
        1);
    settingsGrid->addWidget(
        makeField(QStringLiteral("最大宽度"), widthCombo_, card),
        1,
        0);
    settingsGrid->addWidget(
        makeField(QStringLiteral("画质"), qualityCombo_, card),
        1,
        1);
    layout->addLayout(settingsGrid);
    layout->addSpacing(18);

    auto *permissions = new QFrame(card);
    permissions->setObjectName(QStringLiteral("permissionsBox"));
    permissions->setStyleSheet(QStringLiteral(
        "QFrame#permissionsBox {"
        "  background: rgba(255,255,255,0.025);"
        "  border: 1px solid rgba(255,255,255,0.07);"
        "  border-radius: 11px;"
        "}"));
    auto *permissionGrid = new QGridLayout(permissions);
    permissionGrid->setContentsMargins(14, 12, 14, 12);
    permissionGrid->setHorizontalSpacing(22);
    permissionGrid->setVerticalSpacing(11);
    inputToggle_ = new ToggleSwitch(permissions);
    inputToggle_->setObjectName(QStringLiteral("inputToggle"));
    clipboardToggle_ = new ToggleSwitch(permissions);
    clipboardToggle_->setObjectName(QStringLiteral("clipboardToggle"));
    skipStaticToggle_ = new ToggleSwitch(permissions);
    skipStaticToggle_->setObjectName(QStringLiteral("skipStaticToggle"));
    audioToggle_ = new ToggleSwitch(permissions);
    audioToggle_->setObjectName(QStringLiteral("audioToggle"));
    permissionGrid->addWidget(
        makeSwitchRow(
            QStringLiteral("允许键鼠控制"),
            QStringLiteral("授权远端操作"),
            inputToggle_,
            permissions),
        0,
        0);
    permissionGrid->addWidget(
        makeSwitchRow(
            QStringLiteral("剪贴板同步"),
            QStringLiteral("双向文本复制"),
            clipboardToggle_,
            permissions),
        0,
        1);
    permissionGrid->addWidget(
        makeSwitchRow(
            QStringLiteral("静帧省流"),
            QStringLiteral("画面静止时降帧"),
            skipStaticToggle_,
            permissions),
        1,
        0);
    permissionGrid->addWidget(
        makeSwitchRow(
            QStringLiteral("传输本机声音"),
            QStringLiteral("系统回环音频"),
            audioToggle_,
            permissions),
        1,
        1);
    layout->addWidget(permissions);
    layout->addSpacing(18);

    auto *fileTitle = new QLabel(QStringLiteral("文件传输"), card);
    fileTitle->setFont(uiFont(10, QFont::DemiBold));
    fileTitle->setStyleSheet(QStringLiteral("color: #dfe6f2;"));
    layout->addWidget(fileTitle);
    layout->addSpacing(8);
    auto *fileRow = new QHBoxLayout;
    fileRow->setSpacing(8);
    filePathInput_ = new QLineEdit(card);
    filePathInput_->setObjectName(QStringLiteral("hostFilePath"));
    filePathInput_->setPlaceholderText(
        QStringLiteral("选择文件，或拖放到窗口中"));
    filePathInput_->setMinimumHeight(41);
    fileRow->addWidget(filePathInput_, 1);
    auto *browseButton = new QPushButton(QStringLiteral("浏览"), card);
    browseButton->setObjectName(QStringLiteral("browseFileButton"));
    browseButton->setCursor(Qt::PointingHandCursor);
    browseButton->setMinimumHeight(41);
    fileRow->addWidget(browseButton);
    sendFileButton_ = new QPushButton(QStringLiteral("发送"), card);
    sendFileButton_->setObjectName(QStringLiteral("sendFileButton"));
    sendFileButton_->setCursor(Qt::PointingHandCursor);
    sendFileButton_->setMinimumHeight(41);
    fileRow->addWidget(sendFileButton_);
    fileRetryButton_ = new QPushButton(QStringLiteral("重试"), card);
    fileRetryButton_->setObjectName(QStringLiteral("retryFileButton"));
    fileRetryButton_->setCursor(Qt::PointingHandCursor);
    fileRetryButton_->setMinimumHeight(41);
    fileRetryButton_->hide();
    fileRow->addWidget(fileRetryButton_);
    layout->addLayout(fileRow);
    fileStatus_ = new QLabel(card);
    fileStatus_->setObjectName(QStringLiteral("fileStatus"));
    fileStatus_->setFont(uiFont(8));
    fileStatus_->setStyleSheet(QStringLiteral("color: #69728a;"));
    fileStatus_->setWordWrap(true);
    fileStatus_->hide();
    layout->addWidget(fileStatus_);
    layout->addSpacing(18);

    auto *logHeading = new QHBoxLayout;
    auto *logTitle = new QLabel(QStringLiteral("事件日志"), card);
    logTitle->setFont(uiFont(10, QFont::DemiBold));
    logTitle->setStyleSheet(QStringLiteral("color: #dfe6f2;"));
    logHeading->addWidget(logTitle);
    logHeading->addStretch(1);
    auto *clearButton = new QPushButton(QStringLiteral("清空"), card);
    clearButton->setObjectName(QStringLiteral("quietButton"));
    clearButton->setCursor(Qt::PointingHandCursor);
    clearButton->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 11px; padding: 3px 8px; }"));
    logHeading->addWidget(clearButton);
    layout->addLayout(logHeading);
    layout->addSpacing(8);
    logView_ = new QPlainTextEdit(card);
    logView_->setObjectName(QStringLiteral("hostEventLog"));
    logView_->setReadOnly(true);
    logView_->setMinimumHeight(132);
    logView_->setMaximumHeight(132);
    logView_->setPlaceholderText(QStringLiteral("等待信令事件…"));
    layout->addWidget(logView_);

    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(1);
    auto *footer = new QLabel(
        QStringLiteral("PeerDesk · 自托管 P2P 远程桌面 · 无会员 / 无时长限制"),
        content);
    footer->setAlignment(Qt::AlignCenter);
    footer->setFont(uiFont(8));
    footer->setStyleSheet(QStringLiteral("color: #56607a;"));
    outer->addSpacing(15);
    outer->addWidget(footer);
    scroll->setWidget(content);

    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this,
            QStringLiteral("选择要发送的文件"));
        if (!paths.isEmpty()) {
            filePathInput_->setText(
                paths.size() == 1 ? paths.constFirst() : QString());
            sendFiles(paths);
        }
    });
    connect(clearButton, &QPushButton::clicked, logView_, &QPlainTextEdit::clear);
    appendLog(QStringLiteral("客户端已就绪"));
}

void HostPanel::connectSignals()
{
    connect(copyButton_, &QPushButton::clicked, this, [this]() {
        const QString code = controller_->formattedCode();
        if (code.isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(code);
        appendLog(QStringLiteral("已复制连接码 %1").arg(code));
        emit toastRequested(QStringLiteral("连接码已复制"));
    });
    connect(regenerateButton_, &QPushButton::clicked, this, [this]() {
        QString error;
        if (!controller_->regenerateCode(&error)) {
            appendLog(error);
            emit toastRequested(error);
            return;
        }
        appendLog(QStringLiteral("连接码已更换"));
        emit toastRequested(QStringLiteral("连接码已更换"));
    });
    connect(powerButton_, &QPushButton::clicked, this, [this]() {
        if (controller_->active()) {
            stopHosting();
        } else {
            startHosting();
        }
    });
    connect(approveButton_, &QPushButton::clicked, this, [this]() {
        if (!controller_->approveViewer()) {
            appendLog(QStringLiteral("批准连接请求失败"));
            emit toastRequested(QStringLiteral("批准连接请求失败"));
            return;
        }
        appendLog(QStringLiteral("已批准远程控制请求"));
        emit toastRequested(QStringLiteral("已批准远程控制请求"));
    });
    connect(rejectButton_, &QPushButton::clicked, this, [this]() {
        if (!controller_->rejectViewer()) {
            appendLog(QStringLiteral("拒绝连接请求失败"));
            emit toastRequested(QStringLiteral("拒绝连接请求失败"));
            return;
        }
        appendLog(QStringLiteral("已拒绝远程控制请求"));
        emit toastRequested(QStringLiteral("已拒绝远程控制请求"));
    });
    connect(sendFileButton_, &QPushButton::clicked, this, [this]() {
        if (fileBusy_) {
            if (!controller_->cancelFile()) {
                cancelPending_ = false;
                updateFileTransferUi(
                    QStringLiteral("取消传输失败"),
                    QStringLiteral("#f2555a"));
                appendLog(QStringLiteral("取消传输失败"));
                emit toastRequested(QStringLiteral("取消传输失败"));
                return;
            }
            cancelPending_ = true;
            updateFileTransferUi();
            return;
        }
        const QString path = filePathInput_->text().trimmed();
        if (path.isEmpty()) {
            chooseAndSendFile();
            return;
        }
        sendFile(path);
    });
    connect(filePathInput_, &QLineEdit::returnPressed, this, [this]() {
        if (!fileBusy_) {
            sendFile(filePathInput_->text().trimmed());
        }
    });
    connect(
        fileRetryButton_,
        &QPushButton::clicked,
        this,
        [this]() {
            if (fileQueue_.active() || fileQueue_.hasPending()
                || !fileQueue_.hasFailed()) {
                return;
            }
            if (!fileQueue_.retryFailed()) {
                return;
            }
            lastFilePercent_ = -1;
            updateFileTransferUi();
            flushFileQueue();
        });

    connect(
        controller_,
        &HostController::stateChanged,
        this,
        [this](HostController::State state) {
            if (state == HostController::State::Streaming) {
                if (!fileQueue_.empty()) {
                    const auto *task = fileQueue_.activeTask();
                    lastFilePercent_ =
                        task && task->size > 0
                        ? static_cast<int>(
                            (task->sent * 100) / task->size)
                        : -1;
                    fileQueue_.resume();
                    updateFileTransferUi();
                    if (!fileQueue_.active()
                        && fileQueue_.hasPending()) {
                        QTimer::singleShot(
                            0,
                            this,
                            &HostPanel::flushFileQueue);
                    }
                }
            } else if (!fileQueue_.empty()) {
                cancelPending_ = false;
                if (fileQueue_.active()) {
                    fileQueue_.interruptActive(
                        QStringLiteral("interrupted"));
                }
                fileQueue_.pause();
                updateFileTransferUi();
            }
            updateState();
        });
    connect(
        controller_,
        &HostController::codeChanged,
        this,
        [this](const QString &, const QString &formatted) {
            codeValue_->setText(formatted);
        });
    connect(
        controller_,
        &HostController::peerChanged,
        this,
        [this](const QString &name, bool connected) {
            peerName_ = connected ? name : QString();
            updateState();
            updatePeer();
        });
    connect(
        controller_,
        &HostController::approvalChanged,
        this,
        [this] {
            updateApprovalUi();
            updateState();
        });
    connect(
        controller_,
        &HostController::errorOccurred,
        this,
        [this](const QString &message) {
            appendLog(message);
            updateState();
        });
    connect(
        controller_,
        &HostController::videoSettingsChanged,
        this,
        &HostPanel::updateVideoSettings);
    connect(
        controller_,
        &HostController::monitorsChanged,
        this,
        &HostPanel::updateMonitors);
    connect(
        controller_,
        &HostController::statisticsChanged,
        this,
        &HostPanel::updateStatistics);
    connect(
        controller_,
        &HostController::linkStateChanged,
        this,
        &HostPanel::updateLinkState);
    connect(
        controller_,
        &HostController::logMessage,
        this,
        &HostPanel::appendLog);
    connect(
        controller_,
        &HostController::fileReceived,
        this,
        [this](const QString &path, qint64 size) {
            appendLog(QStringLiteral("已接收文件 %1 (%2 KB)")
                          .arg(QFileInfo(path).fileName())
                          .arg(size / 1024));
        });
    connect(
        controller_,
        &HostController::outgoingFileStarted,
        this,
        [this](
            const QString &id,
            const QString &name,
            qint64 size,
            const QString &sourcePath) {
            if (!fileQueue_.markStarted(
                    id,
                    name,
                    size,
                    sourcePath)) {
                return;
            }
            const auto *task = fileQueue_.activeTask();
            cancelPending_ = false;
            lastFilePercent_ =
                task && task->size > 0
                ? std::clamp(
                    static_cast<int>((task->sent * 100) / task->size),
                    0,
                    100)
                : -1;
            updateFileTransferUi();
            const QString displayName =
                task && !task->name.isEmpty() ? task->name : name;
            appendLog(
                task && task->sent > 0
                ? QStringLiteral("继续发送文件 %1").arg(displayName)
                : QStringLiteral("开始发送文件 %1").arg(displayName));
        });
    connect(
        controller_,
        &HostController::outgoingFileProgress,
        this,
        [this](const QString &id, qint64 sent, qint64 total) {
            if (!fileQueue_.markProgress(id, sent, total)
                || total <= 0) {
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
            updateFileTransferUi();
        });
    connect(
        controller_,
        &HostController::outgoingFileFinished,
        this,
        [this](const QString &id, const QString &name, qint64 size) {
            if (!fileQueue_.markFinished(id, name, size)) {
                return;
            }
            cancelPending_ = false;
            lastFilePercent_ = -1;
            appendLog(QStringLiteral("文件发送完成 %1").arg(name));
            emit toastRequested(QStringLiteral("文件已发送：%1").arg(name));
            updateFileTransferUi();
            if (fileQueue_.hasPending()) {
                QTimer::singleShot(
                    0,
                    this,
                    &HostPanel::flushFileQueue);
            }
        });
    connect(
        controller_,
        &HostController::outgoingFileFailed,
        this,
        [this](const QString &id, const QString &reason) {
            if (!fileQueue_.markFailed(id, reason)) {
                return;
            }
            cancelPending_ = false;
            lastFilePercent_ = -1;
            const QString text = QStringLiteral("发送失败：%1")
                .arg(fileFailureText(reason));
            appendLog(text);
            emit toastRequested(text);
            updateFileTransferUi(text, QStringLiteral("#f2555a"));
            if (fileQueue_.hasPending()) {
                QTimer::singleShot(
                    0,
                    this,
                    &HostPanel::flushFileQueue);
            }
        });
    connect(
        controller_,
        &HostController::outgoingFileCancelled,
        this,
        [this](const QString &id, const QString &reason) {
            const auto *active = fileQueue_.activeTask();
            const QString name = active ? active->name : QString();
            if (!fileQueue_.markCancelled(id, reason)) {
                return;
            }
            cancelPending_ = false;
            lastFilePercent_ = -1;
            const QString text = name.isEmpty()
                ? QStringLiteral("传输已取消")
                : QStringLiteral("传输已取消：%1").arg(name);
            appendLog(text);
            emit toastRequested(text);
            updateFileTransferUi(text, QStringLiteral("#ffd9a3"));
            if (fileQueue_.hasPending()) {
                QTimer::singleShot(
                    0,
                    this,
                    &HostPanel::flushFileQueue);
            }
        });
    connect(
        controller_,
        &HostController::outgoingFileInterrupted,
        this,
        [this](const QString &id, const QString &reason) {
            if (!fileQueue_.markInterrupted(id, reason)) {
                return;
            }
            cancelPending_ = false;
            lastFilePercent_ = -1;
            fileQueue_.pause();
            updateFileTransferUi(
                QStringLiteral("传输已中断，恢复后自动续传"),
                QStringLiteral("#ffb38a"));
            appendLog(QStringLiteral("文件传输已中断"));
        });

    connect(monitorCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (syncing_) {
            return;
        }
        VideoSettings settings = controller_->videoSettings();
        settings.monitor = monitorCombo_->currentData().toInt();
        controller_->setVideoSettings(settings);
    });
    connect(fpsCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (syncing_) {
            return;
        }
        VideoSettings settings = controller_->videoSettings();
        settings.fps = fpsCombo_->currentData().toInt();
        controller_->setVideoSettings(settings);
    });
    connect(widthCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (syncing_) {
            return;
        }
        VideoSettings settings = controller_->videoSettings();
        settings.maxWidth = widthCombo_->currentData().toInt();
        controller_->setVideoSettings(settings);
    });
    connect(qualityCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (syncing_) {
            return;
        }
        VideoSettings settings = controller_->videoSettings();
        settings.quality = qualityCombo_->currentData().toInt();
        controller_->setVideoSettings(settings);
    });
    connect(inputToggle_, &QAbstractButton::toggled, this, [this](bool enabled) {
        if (!syncing_) {
            controller_->setAllowInput(enabled);
        }
    });
    connect(
        clipboardToggle_,
        &QAbstractButton::toggled,
        this,
        [this](bool enabled) {
            if (!syncing_) {
                controller_->setClipboardEnabled(enabled);
            }
        });
    connect(
        skipStaticToggle_,
        &QAbstractButton::toggled,
        this,
        [this](bool enabled) {
            if (syncing_) {
                return;
            }
            VideoSettings settings = controller_->videoSettings();
            settings.skipStatic = enabled;
            controller_->setVideoSettings(settings);
        });
    connect(audioToggle_, &QAbstractButton::toggled, this, [this](bool enabled) {
        if (!syncing_) {
            controller_->setAudioEnabled(enabled);
        }
    });
}

void HostPanel::activate()
{
    if (autoStart_ && !controller_->active()) {
        startHosting();
    }
    refreshFromController();
}

void HostPanel::startHosting()
{
    if (controller_->active()) {
        return;
    }
    QString error;
    appendLog(QStringLiteral("正在连接信令服务器 %1…")
                  .arg(controller_->server()));
    if (!controller_->start(&error)) {
        appendLog(error);
        emit toastRequested(error);
    }
    updateState();
}

void HostPanel::stopHosting()
{
    if (!controller_->active() && controller_->state()
        == HostController::State::Idle) {
        return;
    }
    controller_->stop();
    clearFileTransferState();
    appendLog(QStringLiteral("被控端已停止"));
    updateState();
}

void HostPanel::refreshFromController()
{
    codeValue_->setText(controller_->formattedCode());
    serverPill_->setText(
        QStringLiteral("服务器 %1").arg(controller_->server()));
    updateMonitors(controller_->monitors());
    updateVideoSettings(controller_->videoSettings());
    updateStatistics(controller_->statistics());
    updateApprovalUi();
    updateState();
    updatePeer();
}

void HostPanel::updateApprovalUi()
{
    const bool pending = controller_->approvalPending();
    approvalCard_->setVisible(pending);
    if (!pending) {
        approvalName_->clear();
        return;
    }
    const QString name = controller_->pendingViewerName();
    approvalName_->setText(
        name.isEmpty()
            ? QStringLiteral("有新的设备请求控制本机")
            : QStringLiteral("%1 请求控制本机").arg(name));
}

void HostPanel::updateState()
{
    QString pillText;
    QString status;
    QString hint;
    QString path;
    QColor accent = textFaint();
    PillLabel::Tone tone = PillLabel::Tone::Neutral;

    switch (controller_->state()) {
    case HostController::State::Idle:
        pillText = QStringLiteral("待机");
        status = QStringLiteral("被控端未启动");
        hint = QStringLiteral("点击右上角“启动被控”开始接受远程协助");
        powerButton_->setText(QStringLiteral("启动被控"));
        break;
    case HostController::State::Connecting:
        pillText = QStringLiteral("连接中");
        status = QStringLiteral("正在注册连接码…");
        hint = QStringLiteral("正在与信令服务器建立安全连接");
        accent = amber();
        tone = PillLabel::Tone::Amber;
        powerButton_->setText(QStringLiteral("停止被控"));
        break;
    case HostController::State::Waiting:
        if (controller_->approvalPending()) {
            pillText = QStringLiteral("请求接入");
            status = controller_->pendingViewerName().isEmpty()
                ? QStringLiteral("有设备请求远程控制")
                : QStringLiteral("%1 请求远程控制")
                      .arg(controller_->pendingViewerName());
            hint = QStringLiteral("批准后对方才能查看并控制本机");
            accent = amber();
            tone = PillLabel::Tone::Amber;
        } else {
            pillText = QStringLiteral("在线");
            status = QStringLiteral("等待控制端输入连接码");
            hint = QStringLiteral("本机已上线，可以接受远程协助");
            accent = green();
            tone = PillLabel::Tone::Green;
        }
        powerButton_->setText(QStringLiteral("停止被控"));
        break;
    case HostController::State::Streaming:
        pillText = QStringLiteral("远控中");
        status = peerName_.isEmpty()
            ? QStringLiteral("控制端已连接")
            : QStringLiteral("%1 正在控制本机").arg(peerName_);
        hint = QStringLiteral("远程会话已建立 · 端到端加密");
        path = linkState_.isEmpty()
            ? QString()
            : QStringLiteral("已连接");
        accent = blueBright();
        tone = PillLabel::Tone::Blue;
        powerButton_->setText(QStringLiteral("停止被控"));
        break;
    case HostController::State::Error:
        pillText = QStringLiteral("异常");
        status = controller_->lastError().isEmpty()
            ? QStringLiteral("被控端发生错误")
            : controller_->lastError();
        hint = QStringLiteral("请检查服务器地址、网络与令牌后重试");
        accent = red();
        tone = PillLabel::Tone::Red;
        powerButton_->setText(QStringLiteral("重新启动"));
        break;
    }

    modePill_->setText(pillText);
    modePill_->setTone(tone);
    statusText_->setText(status);
    statusText_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg(accent == textFaint()
                     ? QStringLiteral("#9aa5b8")
                     : accent.name(QColor::HexRgb)));
    statusDot_->setColor(accent);
    codeHint_->setText(hint);
    pathText_->setText(path);
    powerButton_->setText(
        controller_->active() ? QStringLiteral("停止被控")
                              : (controller_->state() == HostController::State::Error
                                     ? QStringLiteral("重新启动")
                                     : QStringLiteral("启动被控")));
}

void HostPanel::updatePeer()
{
    if (!peerName_.isEmpty()) {
        pathText_->setText(
            QStringLiteral("已连接 · %1").arg(peerName_));
    } else {
        pathText_->setText(QString());
    }
}

void HostPanel::updateMonitors(const QList<MonitorInfo> &monitors)
{
    const int selected = controller_->videoSettings().monitor;
    const QSignalBlocker blocker(monitorCombo_);
    monitorCombo_->clear();
    for (const MonitorInfo &monitor : monitors) {
        QString label = QStringLiteral("显示器 %1").arg(monitor.index + 1);
        if (monitor.primary) {
            label.append(QStringLiteral("（主）"));
        }
        if (monitor.width > 0 && monitor.height > 0) {
            label.append(QStringLiteral(" · %1×%2")
                             .arg(monitor.width)
                             .arg(monitor.height));
        }
        monitorCombo_->addItem(label, monitor.index);
    }
    if (monitorCombo_->count() == 0) {
        monitorCombo_->addItem(QStringLiteral("显示器 1"), 0);
    }
    selectComboValue(monitorCombo_, selected);
}

void HostPanel::updateVideoSettings(const VideoSettings &settings)
{
    setSyncing(true);
    selectComboValue(fpsCombo_, settings.fps);
    selectComboValue(widthCombo_, settings.maxWidth);
    selectComboValue(qualityCombo_, settings.quality);
    selectComboValue(monitorCombo_, settings.monitor);
    inputToggle_->setCheckedSilently(controller_->allowInput());
    clipboardToggle_->setCheckedSilently(settings.clipboard);
    skipStaticToggle_->setCheckedSilently(settings.skipStatic);
    audioToggle_->setCheckedSilently(settings.audio);
    setSyncing(false);
}

void HostPanel::updateStatistics(const QJsonObject &statistics)
{
    if (statistics.isEmpty() || !controller_->streaming()) {
        return;
    }
    const double fps = statistics.value(QStringLiteral("fps")).toDouble();
    const double mbps =
        statistics.value(QStringLiteral("mbps")).toDouble();
    pathText_->setText(QStringLiteral("%1 fps · %2 Mbps")
                           .arg(fps, 0, 'f', 1)
                           .arg(mbps, 0, 'f', 1));
}

void HostPanel::updateLinkState(const QString &state)
{
    linkState_ = state;
    if (state == QStringLiteral("direct")) {
        pathText_->setText(QStringLiteral("P2P 直连"));
    } else if (state == QStringLiteral("relay")) {
        pathText_->setText(QStringLiteral("中继模式"));
    } else if (state == QStringLiteral("punching")) {
        pathText_->setText(QStringLiteral("正在打洞…"));
    } else if (state == QStringLiteral("closed")) {
        pathText_->setText(QString());
    }
}

void HostPanel::chooseAndSendFile()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("选择要发送的文件"));
    if (!paths.isEmpty()) {
        filePathInput_->setText(
            paths.size() == 1 ? paths.constFirst() : QString());
        sendFiles(paths);
    }
}

void HostPanel::sendFile(const QString &pathValue)
{
    const QString path = pathValue.trimmed();
    if (path.isEmpty()) {
        return;
    }
    sendFiles({path});
}

void HostPanel::sendFiles(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return;
    }
    QStringList validPaths;
    int unavailable = 0;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QFileInfo info(trimmed);
        if (!info.isFile() || !info.isReadable() || info.size() <= 0) {
            ++unavailable;
            continue;
        }
        validPaths.append(info.absoluteFilePath());
    }
    if (!validPaths.isEmpty()) {
        const int added = fileQueue_.enqueue(validPaths);
        lastFilePercent_ = -1;
        cancelPending_ = false;
        if (added > 0) {
            filePathInput_->clear();
            const QString text = added == 1
                ? QStringLiteral("已加入发送队列：%1")
                    .arg(QFileInfo(validPaths.constFirst()).fileName())
                : QStringLiteral("已加入 %1 个文件到发送队列")
                    .arg(added);
            appendLog(text);
            emit toastRequested(text);
            updateFileTransferUi();
            flushFileQueue();
        } else if (!fileQueue_.active()) {
            const QString text = QStringLiteral("文件已在发送队列中");
            appendLog(text);
            updateFileTransferUi(text, QStringLiteral("#ffd9a3"));
        }
    }
    if (unavailable > 0) {
        const QString text = QStringLiteral("%1 个文件不可读或为空")
            .arg(unavailable);
        appendLog(text);
        emit toastRequested(text);
        if (validPaths.isEmpty()) {
            updateFileTransferUi(text, QStringLiteral("#f2555a"));
        }
    }
}

void HostPanel::flushFileQueue()
{
    if (fileQueue_.active() || fileQueue_.paused()
        || !controller_->active()
        || controller_->state() != HostController::State::Streaming) {
        return;
    }

    QString rejectedText;
    while (true) {
        QString error;
        const OutgoingFileQueue::StartResult result =
            fileQueue_.startNext(
                [this](const QString &path, QString *sendError) {
                    return controller_->sendFile(path, sendError);
                },
                &error);
        if (result == OutgoingFileQueue::StartResult::Started) {
            const auto *task = fileQueue_.activeTask();
            cancelPending_ = false;
            lastFilePercent_ =
                task && task->size > 0
                ? std::clamp(
                    static_cast<int>((task->sent * 100) / task->size),
                    0,
                    100)
                : -1;
            updateFileTransferUi();
            return;
        }
        if (result == OutgoingFileQueue::StartResult::NoTask) {
            break;
        }
        rejectedText = QStringLiteral("发送失败：%1")
            .arg(fileFailureText(error));
    }
    lastFilePercent_ = -1;
    if (!rejectedText.isEmpty()) {
        appendLog(rejectedText);
        emit toastRequested(rejectedText);
        updateFileTransferUi(rejectedText, QStringLiteral("#f2555a"));
    } else {
        updateFileTransferUi();
    }
}

void HostPanel::updateFileTransferUi(
    const QString &message,
    const QString &color)
{
    const auto *task = fileQueue_.activeTask();
    QString text = message;
    QString effectiveColor = color;
    if (text.isEmpty()) {
        if (task && !fileQueue_.paused()) {
            const int percent = task->size > 0
                ? std::clamp(
                    static_cast<int>((task->sent * 100) / task->size),
                    0,
                    100)
                : 0;
            text = QStringLiteral("正在发送 %1/%2 · %3 · %4%")
                .arg(fileQueue_.activePosition())
                .arg(fileQueue_.size())
                .arg(
                    task->name.isEmpty()
                        ? QStringLiteral("文件")
                        : task->name)
                .arg(percent);
            effectiveColor = QStringLiteral("#9aa5b8");
        } else if (fileQueue_.hasPending()) {
            text = fileQueue_.paused()
                ? QStringLiteral("发送队列已暂停 · 等待连接恢复")
                : QStringLiteral("等待发送 · %1 个文件")
                    .arg(fileQueue_.size());
            effectiveColor = QStringLiteral("#ffd9a3");
        } else if (!fileQueue_.empty()) {
            const int succeeded = fileQueue_.succeededCount();
            const int failed = fileQueue_.failedCount();
            const int cancelled = fileQueue_.cancelledCount();
            if (failed > 0) {
                text = QStringLiteral("发送完成 · 成功 %1 · 失败 %2")
                    .arg(succeeded)
                    .arg(failed);
                if (cancelled > 0) {
                    text += QStringLiteral(" · 取消 %1").arg(cancelled);
                }
                effectiveColor = QStringLiteral("#f2555a");
            } else if (succeeded > 0) {
                text = QStringLiteral("已发送 %1 个文件 · %2")
                    .arg(succeeded)
                    .arg(formatTransferSize(fileQueue_.succeededBytes()));
                if (cancelled > 0) {
                    text += QStringLiteral(" · 取消 %1").arg(cancelled);
                }
                effectiveColor = QStringLiteral("#35d07f");
            } else if (cancelled > 0) {
                text = QStringLiteral("已取消 %1 个文件").arg(cancelled);
                effectiveColor = QStringLiteral("#ffd9a3");
            } else {
                text = fileQueue_.paused()
                    ? QStringLiteral("发送队列已暂停 · 恢复后继续")
                    : QStringLiteral("等待发送 · %1 个文件")
                        .arg(fileQueue_.size());
                effectiveColor = QStringLiteral("#ffd9a3");
            }
        }
    }

    setFileBusy(fileQueue_.active());
    sendFileButton_->setEnabled(!cancelPending_);

    const bool retryVisible =
        !fileQueue_.active() && !fileQueue_.hasPending()
        && fileQueue_.hasFailed();
    fileRetryButton_->setVisible(retryVisible);
    fileRetryButton_->setEnabled(retryVisible);

    if (text.isEmpty()) {
        fileStatus_->hide();
        return;
    }
    fileStatus_->setText(text);
    if (!effectiveColor.isEmpty()) {
        fileStatus_->setStyleSheet(
            QStringLiteral("color: %1;").arg(effectiveColor));
    }
    fileStatus_->show();
}

void HostPanel::clearFileTransferState()
{
    fileQueue_.clear();
    cancelPending_ = false;
    lastFilePercent_ = -1;
    setFileBusy(false);
    sendFileButton_->setEnabled(true);
    fileRetryButton_->hide();
    fileStatus_->hide();
}

void HostPanel::setFileBusy(bool busy)
{
    fileBusy_ = busy;
    sendFileButton_->setText(
        busy ? QStringLiteral("取消发送") : QStringLiteral("发送"));
}

void HostPanel::appendLog(const QString &message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logView_->appendPlainText(
        QStringLiteral("%1  %2").arg(timestamp, message));
    if (logView_->blockCount() > 240) {
        QTextCursor cursor(logView_->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(
            QTextCursor::Down,
            QTextCursor::KeepAnchor,
            logView_->blockCount() - 200);
        cursor.removeSelectedText();
    }
    logView_->verticalScrollBar()->setValue(
        logView_->verticalScrollBar()->maximum());
}

void HostPanel::setSyncing(bool syncing)
{
    syncing_ = syncing;
}

void HostPanel::selectComboValue(QComboBox *combo, int value)
{
    int index = combo->findData(value);
    if (index < 0) {
        if (combo == qualityCombo_) {
            combo->addItem(
                QStringLiteral("%1%").arg(value),
                value);
        } else if (combo == fpsCombo_) {
            combo->addItem(
                QStringLiteral("%1 fps").arg(value),
                value);
        } else if (combo == widthCombo_) {
            combo->addItem(
                QStringLiteral("%1 px").arg(value),
                value);
        } else {
            combo->addItem(
                QStringLiteral("显示器 %1").arg(value + 1),
                value);
        }
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}

void HostPanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void HostPanel::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    filePathInput_->setText(
        paths.size() == 1 ? paths.constFirst() : QString());
    sendFiles(paths);
    event->acceptProposedAction();
}

} // namespace pd::ui
