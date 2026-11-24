#include "ui/connection_page.h"

#include "core/util.h"
#include "ui/code_input.h"
#include "ui/ui_theme.h"

#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>

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

QString displayName(const RecentConnection &connection)
{
    const QString name = connection.name.simplified();
    return name.isEmpty() ? QStringLiteral("远端设备") : name;
}

QString displayName(const FavoriteConnection &connection)
{
    const QString name = connection.name.simplified();
    return name.isEmpty() ? QStringLiteral("远端设备") : name;
}

QString quickTileStyle(const QString &objectName)
{
    return QStringLiteral(
        "QPushButton#%1 {"
        "  background: rgba(255, 255, 255, 0.035);"
        "  border: 1px solid rgba(255, 255, 255, 0.085);"
        "  border-radius: 9px;"
        "  padding: 0;"
        "  text-align: left;"
        "}"
        "QPushButton#%1:hover {"
        "  background: rgba(61, 123, 253, 0.11);"
        "  border-color: rgba(91, 155, 255, 0.34);"
        "}"
        "QPushButton#%1:pressed {"
        "  background: rgba(61, 123, 253, 0.17);"
        "}"
        "QPushButton#%1:disabled {"
        "  background: rgba(255, 255, 255, 0.02);"
        "  border-color: rgba(255, 255, 255, 0.06);"
        "}")
        .arg(objectName);
}

QString favoriteStarStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  color: #8bb8ff;"
        "  background: transparent;"
        "  border: 0;"
        "  border-radius: 7px;"
        "  padding: 0;"
        "  font-size: 13px;"
        "}"
        "QPushButton:hover {"
        "  color: #d9e8ff;"
        "  background: rgba(91, 155, 255, 0.14);"
        "}"
        "QPushButton:pressed {"
        "  background: rgba(91, 155, 255, 0.22);"
        "}"
        "QPushButton:disabled { color: #56607a; }");
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

    favoritePanel_ = new QWidget(card);
    favoritePanel_->setObjectName(QStringLiteral("favoriteDevices"));
    favoritePanel_->hide();
    auto *favoriteLayout = new QVBoxLayout(favoritePanel_);
    favoriteLayout->setContentsMargins(0, 16, 0, 0);
    favoriteLayout->setSpacing(8);

    auto *favoriteHeading = new QHBoxLayout;
    favoriteHeading->setSpacing(8);
    favoriteHeading->addWidget(makeCaption(
        QStringLiteral("常用设备"),
        favoritePanel_));
    favoriteHeading->addStretch(1);
    auto *favoriteHint = new QLabel(
        QStringLiteral("单击直接连接"),
        favoritePanel_);
    favoriteHint->setFont(uiFont(7));
    favoriteHint->setStyleSheet(QStringLiteral("color: #56607a;"));
    favoriteHeading->addWidget(favoriteHint);
    favoriteLayout->addLayout(favoriteHeading);

    auto *favoriteGrid = new QGridLayout;
    favoriteGrid->setContentsMargins(0, 0, 0, 0);
    favoriteGrid->setHorizontalSpacing(7);
    favoriteGrid->setVerticalSpacing(7);
    for (int index = 0;
         index < Preferences::kFavoriteConnectionLimit;
         ++index) {
        FavoriteTile tile;
        tile.button = new QPushButton(favoritePanel_);
        tile.button->setObjectName(
            QStringLiteral("favoriteConnectionButton"));
        tile.button->setProperty("favoriteIndex", index);
        tile.button->setCursor(Qt::PointingHandCursor);
        tile.button->setMinimumHeight(46);
        tile.button->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Fixed);
        tile.button->setStyleSheet(
            quickTileStyle(QStringLiteral("favoriteConnectionButton")));

        auto *tileLayout = new QVBoxLayout(tile.button);
        tileLayout->setContentsMargins(9, 5, 6, 5);
        tileLayout->setSpacing(1);
        auto *topLine = new QHBoxLayout;
        topLine->setSpacing(5);
        tile.name = new QLabel(tile.button);
        tile.name->setObjectName(
            QStringLiteral("favoriteConnectionName"));
        tile.name->setFont(uiFont(9, QFont::DemiBold));
        tile.name->setStyleSheet(
            QStringLiteral("color: #e8eef9; background: transparent;"));
        tile.name->setAttribute(Qt::WA_TransparentForMouseEvents);
        topLine->addWidget(tile.name, 1);
        tile.remove = new QPushButton(QStringLiteral("★"), tile.button);
        tile.remove->setObjectName(
            QStringLiteral("favoriteRemoveButton"));
        tile.remove->setProperty("favoriteIndex", index);
        tile.remove->setCursor(Qt::PointingHandCursor);
        tile.remove->setFixedSize(20, 20);
        tile.remove->setStyleSheet(favoriteStarStyle());
        topLine->addWidget(tile.remove, 0, Qt::AlignTop);
        tileLayout->addLayout(topLine);

        tile.code = new QLabel(tile.button);
        tile.code->setObjectName(
            QStringLiteral("favoriteConnectionCode"));
        tile.code->setFont(uiFont(8, QFont::DemiBold));
        tile.code->setStyleSheet(
            QStringLiteral("color: #8bb8ff; background: transparent;"));
        tile.code->setAttribute(Qt::WA_TransparentForMouseEvents);
        tileLayout->addWidget(tile.code);

        tile.server = new QLabel(tile.button);
        tile.server->setObjectName(
            QStringLiteral("favoriteConnectionServer"));
        tile.server->setFont(uiFont(7));
        tile.server->setStyleSheet(
            QStringLiteral("color: #69728a; background: transparent;"));
        tile.server->setAttribute(Qt::WA_TransparentForMouseEvents);
        tileLayout->addWidget(tile.server);

        favoriteGrid->addWidget(
            tile.button,
            index / 2,
            index % 2);
        favoriteTiles_.append(tile);
        connect(
            tile.button,
            &QPushButton::clicked,
            this,
            [this, index]() { chooseFavoriteConnection(index); });
        connect(
            tile.remove,
            &QPushButton::clicked,
            this,
            [this, index]() {
                if (index < 0 || index >= favoriteConnections_.size()) {
                    return;
                }
                const FavoriteConnection &connection =
                    favoriteConnections_.at(index);
                emit favoriteToggleRequested(
                    connection.code,
                    displayName(connection),
                    connection.server,
                    false);
            });
    }
    favoriteGrid->setColumnStretch(0, 1);
    favoriteGrid->setColumnStretch(1, 1);
    favoriteLayout->addLayout(favoriteGrid);
    cardLayout->addWidget(favoritePanel_);

    recentPanel_ = new QWidget(card);
    recentPanel_->setObjectName(QStringLiteral("recentConnections"));
    recentPanel_->hide();
    auto *recentLayout = new QVBoxLayout(recentPanel_);
    recentLayout->setContentsMargins(0, 16, 0, 0);
    recentLayout->setSpacing(8);

    auto *recentHeading = new QHBoxLayout;
    recentHeading->setSpacing(8);
    auto *recentCaption = makeCaption(
        QStringLiteral("最近连接"),
        recentPanel_);
    recentHeading->addWidget(recentCaption);
    recentHeading->addStretch(1);
    clearRecentButton_ = new QPushButton(
        QStringLiteral("清除"),
        recentPanel_);
    clearRecentButton_->setObjectName(
        QStringLiteral("clearRecentConnections"));
    clearRecentButton_->setCursor(Qt::PointingHandCursor);
    clearRecentButton_->setAccessibleName(
        QStringLiteral("清除最近连接"));
    clearRecentButton_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  color: #7fa9e8;"
        "  background: transparent;"
        "  border: 0;"
        "  padding: 1px 4px;"
        "  font-size: 11px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover { color: #b7d2ff; }"
        "QPushButton:disabled { color: #56607a; }"));
    recentHeading->addWidget(clearRecentButton_);
    recentLayout->addLayout(recentHeading);

    auto *recentGrid = new QGridLayout;
    recentGrid->setContentsMargins(0, 0, 0, 0);
    recentGrid->setHorizontalSpacing(7);
    recentGrid->setVerticalSpacing(7);
    for (int index = 0;
         index < Preferences::kRecentConnectionLimit;
         ++index) {
        RecentTile tile;
        tile.button = new QPushButton(recentPanel_);
        tile.button->setObjectName(
            QStringLiteral("recentConnectionButton"));
        tile.button->setProperty("recentIndex", index);
        tile.button->setCursor(Qt::PointingHandCursor);
        tile.button->setMinimumHeight(46);
        tile.button->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Fixed);
        tile.button->setStyleSheet(
            quickTileStyle(QStringLiteral("recentConnectionButton")));

        auto *tileLayout = new QVBoxLayout(tile.button);
        tileLayout->setContentsMargins(9, 5, 9, 5);
        tileLayout->setSpacing(1);
        auto *topLine = new QHBoxLayout;
        topLine->setSpacing(6);
        tile.name = new QLabel(tile.button);
        tile.name->setObjectName(
            QStringLiteral("recentConnectionName"));
        tile.name->setFont(uiFont(9, QFont::DemiBold));
        tile.name->setStyleSheet(
            QStringLiteral("color: #e8eef9; background: transparent;"));
        tile.name->setAttribute(Qt::WA_TransparentForMouseEvents);
        topLine->addWidget(tile.name, 1);
        tile.code = new QLabel(tile.button);
        tile.code->setObjectName(
            QStringLiteral("recentConnectionCode"));
        tile.code->setFont(uiFont(8, QFont::DemiBold));
        tile.code->setStyleSheet(
            QStringLiteral("color: #8bb8ff; background: transparent;"));
        tile.code->setAttribute(Qt::WA_TransparentForMouseEvents);
        topLine->addWidget(tile.code, 0, Qt::AlignRight);
        tile.favorite = new QPushButton(QStringLiteral("☆"), tile.button);
        tile.favorite->setObjectName(
            QStringLiteral("recentFavoriteButton"));
        tile.favorite->setProperty("recentIndex", index);
        tile.favorite->setCursor(Qt::PointingHandCursor);
        tile.favorite->setFixedSize(20, 20);
        tile.favorite->setStyleSheet(favoriteStarStyle());
        topLine->addWidget(tile.favorite, 0, Qt::AlignTop);
        tileLayout->addLayout(topLine);

        tile.server = new QLabel(tile.button);
        tile.server->setObjectName(
            QStringLiteral("recentConnectionServer"));
        tile.server->setFont(uiFont(7));
        tile.server->setStyleSheet(
            QStringLiteral("color: #69728a; background: transparent;"));
        tile.server->setAttribute(Qt::WA_TransparentForMouseEvents);
        tileLayout->addWidget(tile.server);

        recentGrid->addWidget(
            tile.button,
            index / 2,
            index % 2);
        recentTiles_.append(tile);
        connect(
            tile.button,
            &QPushButton::clicked,
            this,
            [this, index]() { chooseRecentConnection(index); });
        connect(
            tile.favorite,
            &QPushButton::clicked,
            this,
            [this, index]() {
                if (busy_
                    || connected_
                    || index < 0
                    || index >= recentConnections_.size()) {
                    return;
                }
                const RecentConnection &connection =
                    recentConnections_.at(index);
                emit favoriteToggleRequested(
                    connection.code,
                    displayName(connection),
                    connection.server,
                    !isFavorite(connection.code, connection.server));
            });
    }
    recentGrid->setColumnStretch(0, 1);
    recentGrid->setColumnStretch(1, 1);
    recentLayout->addLayout(recentGrid);
    cardLayout->addWidget(recentPanel_);

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
    connect(
        clearRecentButton_,
        &QPushButton::clicked,
        this,
        [this]() {
            setRecentConnections({});
            emit recentConnectionsCleared();
        });

    setRecentConnections({});
    setFavoriteConnections({});
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
    updateRecentControls();
    updateFavoriteControls();
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
    updateRecentControls();
    updateFavoriteControls();
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

void ConnectionPage::setRecentConnections(
    const QList<RecentConnection> &connections)
{
    recentConnections_ = connections.mid(
        0,
        Preferences::kRecentConnectionLimit);
    for (int index = 0; index < recentTiles_.size(); ++index) {
        RecentTile &tile = recentTiles_[index];
        const bool visible = index < recentConnections_.size();
        tile.button->setVisible(visible);
        if (!visible) {
            continue;
        }

        const RecentConnection &connection =
            recentConnections_.at(index);
        const QString name = displayName(connection);
        const QString code = formatCode(connection.code);
        const QString server = connection.server;
        tile.name->setText(name);
        tile.code->setText(code);
        tile.server->setText(server);
        QStringList tooltip{
            name,
            code,
            server,
        };
        if (connection.connectedAt.isValid()) {
            tooltip.append(
                QStringLiteral("最近连接于 %1")
                    .arg(connection.connectedAt
                             .toLocalTime()
                             .toString(
                                 QStringLiteral("yyyy-MM-dd HH:mm"))));
        }
        tile.button->setToolTip(tooltip.join(u'\n'));
        tile.button->setAccessibleName(
            QStringLiteral("%1，连接码 %2，服务器 %3")
                .arg(name, code, server));
        tile.button->setAccessibleDescription(
            QStringLiteral("填入此设备的连接码和服务器"));
    }
    recentPanel_->setVisible(!recentConnections_.isEmpty());
    updateRecentControls();
}

void ConnectionPage::setFavoriteConnections(
    const QList<FavoriteConnection> &connections)
{
    favoriteConnections_ = connections.mid(
        0,
        Preferences::kFavoriteConnectionLimit);
    for (int index = 0; index < favoriteTiles_.size(); ++index) {
        FavoriteTile &tile = favoriteTiles_[index];
        const bool visible = index < favoriteConnections_.size();
        tile.button->setVisible(visible);
        if (!visible) {
            continue;
        }

        const FavoriteConnection &connection =
            favoriteConnections_.at(index);
        const QString name = displayName(connection);
        const QString code = formatCode(connection.code);
        const QString server = connection.server;
        tile.name->setText(name);
        tile.code->setText(code);
        tile.server->setText(server);
        tile.button->setToolTip(
            QStringLiteral("%1\n%2\n%3").arg(name, code, server));
        tile.button->setAccessibleName(
            QStringLiteral("%1，连接码 %2，服务器 %3")
                .arg(name, code, server));
        tile.button->setAccessibleDescription(
            QStringLiteral("立即连接此常用设备"));
        tile.remove->setAccessibleName(
            QStringLiteral("取消收藏 %1").arg(name));
        tile.remove->setAccessibleDescription(
            QStringLiteral("从常用设备中移除此连接"));
        tile.remove->setToolTip(QStringLiteral("取消收藏"));
    }
    favoritePanel_->setVisible(!favoriteConnections_.isEmpty());
    updateFavoriteControls();
    updateRecentControls();
}

QList<QPushButton *> ConnectionPage::recentConnectionButtons() const
{
    QList<QPushButton *> buttons;
    buttons.reserve(recentTiles_.size());
    for (const RecentTile &tile : recentTiles_) {
        buttons.append(tile.button);
    }
    return buttons;
}

QList<QPushButton *> ConnectionPage::recentFavoriteButtons() const
{
    QList<QPushButton *> buttons;
    buttons.reserve(recentTiles_.size());
    for (const RecentTile &tile : recentTiles_) {
        buttons.append(tile.favorite);
    }
    return buttons;
}

QList<QPushButton *> ConnectionPage::favoriteConnectionButtons() const
{
    QList<QPushButton *> buttons;
    buttons.reserve(favoriteTiles_.size());
    for (const FavoriteTile &tile : favoriteTiles_) {
        buttons.append(tile.button);
    }
    return buttons;
}

QList<QPushButton *> ConnectionPage::favoriteRemoveButtons() const
{
    QList<QPushButton *> buttons;
    buttons.reserve(favoriteTiles_.size());
    for (const FavoriteTile &tile : favoriteTiles_) {
        buttons.append(tile.remove);
    }
    return buttons;
}

void ConnectionPage::chooseRecentConnection(int index)
{
    if (busy_
        || connected_
        || index < 0
        || index >= recentConnections_.size()) {
        return;
    }
    const RecentConnection &connection =
        recentConnections_.at(index);
    setCode(connection.code);
    setServer(connection.server);
    errorText_->hide();
    codeInput_->setInvalid(false);
    setStatus(
        QStringLiteral("已载入 %1，确认后连接")
            .arg(displayName(connection)),
        PillLabel::Tone::Blue);
    connectButton_->setFocus(Qt::TabFocusReason);
}

void ConnectionPage::chooseFavoriteConnection(int index)
{
    if (busy_
        || connected_
        || index < 0
        || index >= favoriteConnections_.size()) {
        return;
    }
    const FavoriteConnection &connection =
        favoriteConnections_.at(index);
    if (!parseServerAddress(connection.server).has_value()) {
        setError(QStringLiteral("常用设备的信令服务器地址无效"));
        return;
    }
    setCode(connection.code);
    setServer(connection.server);
    errorText_->hide();
    codeInput_->setInvalid(false);
    setBusy(true);
    setStatus(
        QStringLiteral("正在连接常用设备 %1…")
            .arg(displayName(connection)),
        PillLabel::Tone::Amber);
    emit connectRequested(connection.code, connection.server, token());
}

void ConnectionPage::updateRecentControls()
{
    const bool enabled = !busy_ && !connected_;
    for (int index = 0; index < recentTiles_.size(); ++index) {
        RecentTile &tile = recentTiles_[index];
        const bool visible = index < recentConnections_.size();
        tile.button->setEnabled(enabled && visible);
        tile.favorite->setVisible(visible);
        if (!visible) {
            continue;
        }
        const RecentConnection &connection =
            recentConnections_.at(index);
        const bool favorite =
            isFavorite(connection.code, connection.server);
        tile.favorite->setText(
            favorite ? QStringLiteral("★") : QStringLiteral("☆"));
        tile.favorite->setEnabled(enabled);
        tile.favorite->setToolTip(
            favorite ? QStringLiteral("取消收藏")
                     : QStringLiteral("收藏为常用设备"));
        tile.favorite->setAccessibleName(
            favorite
                ? QStringLiteral("取消收藏 %1").arg(
                      displayName(connection))
                : QStringLiteral("收藏 %1").arg(
                      displayName(connection)));
        tile.favorite->setAccessibleDescription(
            favorite
                ? QStringLiteral("从常用设备中移除")
                : QStringLiteral("收藏后可一键连接"));
    }
    if (clearRecentButton_) {
        clearRecentButton_->setEnabled(enabled);
    }
}

void ConnectionPage::updateFavoriteControls()
{
    const bool enabled = !busy_ && !connected_;
    for (int index = 0; index < favoriteTiles_.size(); ++index) {
        FavoriteTile &tile = favoriteTiles_[index];
        const bool visible = index < favoriteConnections_.size();
        tile.button->setEnabled(enabled && visible);
        tile.remove->setEnabled(enabled && visible);
    }
}

bool ConnectionPage::isFavorite(
    const QString &code,
    const QString &server) const
{
    return std::any_of(
        favoriteConnections_.cbegin(),
        favoriteConnections_.cend(),
        [&code, &server](const FavoriteConnection &connection) {
            return connection.code == code
                && connection.server.compare(
                       server,
                       Qt::CaseInsensitive)
                    == 0;
        });
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
