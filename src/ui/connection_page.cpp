#include "ui/connection_page.h"

#include "core/util.h"
#include "ui/code_input.h"
#include "ui/ui_theme.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace pd::ui {
namespace {

QLabel *makeHint(const QString &value, QWidget *parent)
{
    auto *label = new QLabel(value, parent);
    label->setFont(uiFont(8));
    label->setStyleSheet(QStringLiteral("color: #69728a;"));
    label->setWordWrap(true);
    return label;
}

void addShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(54);
    shadow->setOffset(0, 18);
    shadow->setColor(QColor(0, 0, 0, 150));
    widget->setGraphicsEffect(shadow);
}

} // namespace

ConnectionPage::ConnectionPage(QWidget *parent)
    : BackdropWidget(parent)
{
    buildUi();
}

void ConnectionPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 28);
    root->setSpacing(0);
    root->addStretch(1);

    auto *card = new GlassCard(this);
    card->setObjectName(QStringLiteral("connectCard"));
    card->setFixedWidth(460);
    addShadow(card);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(26, 24, 26, 24);
    cardLayout->setSpacing(0);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    auto *headingText = new QWidget(card);
    auto *headingLayout = new QVBoxLayout(headingText);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(3);
    auto *title = makeTitle(QStringLiteral("远程协助"), headingText, 17);
    title->setObjectName(QStringLiteral("connectTitle"));
    headingLayout->addWidget(title);
    headingLayout->addWidget(makeHint(
        QStringLiteral("输入对端 9 位连接码，立即建立加密会话"),
        headingText));
    heading->addWidget(headingText, 1);
    auto *protocolPill = new PillLabel(card);
    protocolPill->setText(QStringLiteral("P2P / PDTP"));
    protocolPill->setTone(PillLabel::Tone::Blue);
    heading->addWidget(protocolPill, 0, Qt::AlignTop);
    cardLayout->addLayout(heading);
    cardLayout->addSpacing(22);

    cardLayout->addWidget(makeCaption(QStringLiteral("连接码"), card));
    cardLayout->addSpacing(7);
    codeInput_ = new CodeInput(card);
    codeInput_->setObjectName(QStringLiteral("connectionCode"));
    codeInput_->setMinimumHeight(54);
    codeInput_->setAccessibleName(QStringLiteral("9 位连接码"));
    cardLayout->addWidget(codeInput_);
    cardLayout->addSpacing(16);

    serverInput_ = new QLineEdit(card);
    serverInput_->setObjectName(QStringLiteral("serverInput"));
    serverInput_->setPlaceholderText(QStringLiteral("127.0.0.1:7900"));
    serverInput_->setMinimumHeight(43);
    serverInput_->setClearButtonEnabled(false);
    serverInput_->setAccessibleName(QStringLiteral("信令服务器"));
    cardLayout->addWidget(makeField(
        QStringLiteral("信令服务器"),
        serverInput_,
        card));
    cardLayout->addSpacing(15);

    tokenInput_ = new QLineEdit(card);
    tokenInput_->setObjectName(QStringLiteral("tokenInput"));
    tokenInput_->setPlaceholderText(QStringLiteral("服务器未设置令牌时留空"));
    tokenInput_->setEchoMode(QLineEdit::Password);
    tokenInput_->setMinimumHeight(43);
    tokenInput_->setAccessibleName(QStringLiteral("服务器令牌"));
    cardLayout->addWidget(makeField(
        QStringLiteral("服务器令牌（可选）"),
        tokenInput_,
        card));
    cardLayout->addSpacing(20);

    connectButton_ = new QPushButton(QStringLiteral("连接远程设备"), card);
    connectButton_->setObjectName(QStringLiteral("primaryButton"));
    connectButton_->setCursor(Qt::PointingHandCursor);
    connectButton_->setMinimumHeight(46);
    cardLayout->addWidget(connectButton_);

    errorText_ = new QLabel(card);
    errorText_->setObjectName(QStringLiteral("connectionError"));
    errorText_->setFont(uiFont(8));
    errorText_->setStyleSheet(QStringLiteral("color: #f2555a;"));
    errorText_->setWordWrap(true);
    errorText_->hide();
    cardLayout->addWidget(errorText_);

    auto *statusRail = new QFrame(card);
    statusRail->setObjectName(QStringLiteral("statusRail"));
    statusRail->setStyleSheet(QStringLiteral(
        "QFrame#statusRail {"
        "  background: rgba(255, 255, 255, 0.035);"
        "  border: 1px solid rgba(255, 255, 255, 0.075);"
        "  border-radius: 10px;"
        "}"));
    auto *statusLayout = new QHBoxLayout(statusRail);
    statusLayout->setContentsMargins(13, 10, 13, 10);
    statusLayout->setSpacing(9);
    statusDot_ = new StatusDot(statusRail);
    statusDot_->setColor(textFaint());
    statusLayout->addWidget(statusDot_);
    statusText_ = new QLabel(QStringLiteral("准备连接"), statusRail);
    statusText_->setObjectName(QStringLiteral("connectionStatus"));
    statusText_->setFont(uiFont(8));
    statusText_->setStyleSheet(QStringLiteral("color: #9aa5b8;"));
    statusLayout->addWidget(statusText_, 1);
    auto *secureText = new QLabel(QStringLiteral("端到端加密"), statusRail);
    secureText->setFont(uiFont(8));
    secureText->setStyleSheet(QStringLiteral("color: #69728a;"));
    statusLayout->addWidget(secureText, 0);
    cardLayout->addSpacing(14);
    cardLayout->addWidget(statusRail);

    cardLayout->addSpacing(17);
    auto *ownerRow = new QHBoxLayout;
    ownerRow->setSpacing(4);
    auto *ownerText = new QLabel(
        QStringLiteral("想让别人连接这台电脑？"),
        card);
    ownerText->setFont(uiFont(8));
    ownerText->setStyleSheet(QStringLiteral("color: #69728a;"));
    ownerRow->addWidget(ownerText);
    auto *ownerButton = new QPushButton(
        QStringLiteral("查看本机连接码"),
        card);
    ownerButton->setObjectName(QStringLiteral("hostModeButton"));
    ownerButton->setCursor(Qt::PointingHandCursor);
    ownerButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  color: #78adff;"
        "  background: transparent;"
        "  border: 0;"
        "  padding: 2px 4px;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover { color: #a7c9ff; }"));
    ownerRow->addWidget(ownerButton);
    ownerRow->addStretch(1);
    cardLayout->addLayout(ownerRow);

    root->addWidget(card, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *footer = new QLabel(
        QStringLiteral("自托管信令 · 优先点对点直连 · 无会员与时长限制"),
        this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setFont(uiFont(8));
    footer->setStyleSheet(QStringLiteral("color: #56607a;"));
    root->addSpacing(16);
    root->addWidget(footer);

    connect(
        connectButton_,
        &QPushButton::clicked,
        this,
        &ConnectionPage::requestConnect);
    connect(
        codeInput_,
        &QLineEdit::textChanged,
        this,
        [this]() {
            errorText_->hide();
            codeInput_->setInvalid(false);
        });
    connect(
        codeInput_,
        &QLineEdit::returnPressed,
        this,
        &ConnectionPage::requestConnect);
    connect(
        serverInput_,
        &QLineEdit::returnPressed,
        this,
        &ConnectionPage::requestConnect);
    connect(
        tokenInput_,
        &QLineEdit::returnPressed,
        this,
        &ConnectionPage::requestConnect);
    connect(
        ownerButton,
        &QPushButton::clicked,
        this,
        &ConnectionPage::hostModeRequested);
}

void ConnectionPage::setCode(const QString &code)
{
    codeInput_->setDigits(code);
}

void ConnectionPage::setServer(const QString &server)
{
    serverInput_->setText(server);
}

void ConnectionPage::setToken(const QString &token)
{
    tokenInput_->setText(token);
}

void ConnectionPage::setBusy(bool busy)
{
    busy_ = busy;
    codeInput_->setEnabled(!busy);
    serverInput_->setEnabled(!busy);
    tokenInput_->setEnabled(!busy);
    connectButton_->setEnabled(!busy);
    if (connected_ && !busy) {
        connectButton_->setText(QStringLiteral("断开远程会话"));
    } else {
        connectButton_->setText(
            busy ? QStringLiteral("正在建立连接…")
                 : QStringLiteral("连接远程设备"));
    }
    if (busy) {
        errorText_->hide();
        setStatus(
            QStringLiteral("正在寻找并连接远端设备…"),
            PillLabel::Tone::Amber);
    }
}

void ConnectionPage::setConnected(bool connected, const QString &peerName)
{
    connected_ = connected;
    busy_ = false;
    codeInput_->setEnabled(!connected);
    serverInput_->setEnabled(!connected);
    tokenInput_->setEnabled(!connected);
    connectButton_->setEnabled(true);
    connectButton_->setText(
        connected ? QStringLiteral("断开远程会话")
                  : QStringLiteral("连接远程设备"));
    errorText_->hide();
    if (connected) {
        setStatus(
            peerName.isEmpty()
                ? QStringLiteral("已连接 · 正在接收远端画面")
                : QStringLiteral("已连接 · %1").arg(peerName),
            PillLabel::Tone::Green);
    } else {
        setStatus(QStringLiteral("准备连接"));
    }
}

void ConnectionPage::setStatus(
    const QString &textValue,
    PillLabel::Tone tone)
{
    statusText_->setText(textValue);
    QColor color = textFaint();
    QString textColor = QStringLiteral("#9aa5b8");
    switch (tone) {
    case PillLabel::Tone::Blue:
        color = blueBright();
        textColor = QStringLiteral("#dce8ff");
        break;
    case PillLabel::Tone::Green:
        color = green();
        textColor = QStringLiteral("#baf3d6");
        break;
    case PillLabel::Tone::Amber:
        color = amber();
        textColor = QStringLiteral("#f8dda0");
        break;
    case PillLabel::Tone::Red:
        color = red();
        textColor = QStringLiteral("#ffd0d2");
        break;
    case PillLabel::Tone::Neutral:
        break;
    }
    statusDot_->setColor(color);
    statusText_->setStyleSheet(
        QStringLiteral("color: %1;").arg(textColor));
}

void ConnectionPage::setError(const QString &message)
{
    connected_ = false;
    busy_ = false;
    setBusy(false);
    errorText_->setText(message);
    errorText_->show();
    setStatus(message, PillLabel::Tone::Red);
    if (compactConnectionCode(codeInput_->text()).size() < 9) {
        codeInput_->setInvalid(true);
    }
}

void ConnectionPage::focusCode()
{
    codeInput_->setFocus(Qt::OtherFocusReason);
    codeInput_->selectAll();
}

QString ConnectionPage::code() const
{
    return codeInput_->digits();
}

QString ConnectionPage::server() const
{
    return serverInput_->text().trimmed();
}

QString ConnectionPage::token() const
{
    return tokenInput_->text().trimmed();
}

void ConnectionPage::requestConnect()
{
    if (connected_) {
        emit disconnectRequested();
        return;
    }
    if (busy_) {
        return;
    }
    const QString normalized = code();
    if (normalized.size() != 9) {
        codeInput_->setInvalid(true);
        setError(QStringLiteral("请输入完整的 9 位数字连接码"));
        codeInput_->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (!parseServerAddress(server()).has_value()) {
        setError(QStringLiteral("信令服务器地址无效，例如 192.168.1.2:7900"));
        serverInput_->setFocus(Qt::OtherFocusReason);
        serverInput_->selectAll();
        return;
    }

    errorText_->hide();
    codeInput_->setInvalid(false);
    setBusy(true);
    setStatus(
        QStringLiteral("正在寻找并连接远端设备…"),
        PillLabel::Tone::Amber);
    emit connectRequested(normalized, server(), token());
}

} // namespace pd::ui
