#pragma once

#include "app/preferences.h"
#include "ui/ui_widgets.h"

#include <QList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace pd::ui {

class CodeInput;
class PillLabel;
class StatusDot;

class ConnectionPage final : public BackdropWidget {
    Q_OBJECT

public:
    explicit ConnectionPage(QWidget *parent = nullptr);

    void setCode(const QString &code);
    void setServer(const QString &server);
    void setToken(const QString &token);
    void setBusy(bool busy);
    void setConnected(bool connected, const QString &peerName = {});
    void setStatus(
        const QString &text,
        PillLabel::Tone tone = PillLabel::Tone::Neutral);
    void setError(const QString &message);
    void focusCode();
    void setRecentConnections(
        const QList<RecentConnection> &connections);
    void setFavoriteConnections(
        const QList<FavoriteConnection> &connections);

    [[nodiscard]] QString code() const;
    [[nodiscard]] QString server() const;
    [[nodiscard]] QString token() const;
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] QWidget *recentConnectionsPanel() const
    {
        return recentPanel_;
    }
    [[nodiscard]] QPushButton *clearRecentConnectionsButton() const
    {
        return clearRecentButton_;
    }
    [[nodiscard]] QWidget *favoriteDevicesPanel() const
    {
        return favoritePanel_;
    }
    [[nodiscard]] QList<QPushButton *> recentConnectionButtons() const;
    [[nodiscard]] QList<QPushButton *> recentFavoriteButtons() const;
    [[nodiscard]] QList<QPushButton *> favoriteConnectionButtons() const;
    [[nodiscard]] QList<QPushButton *> favoriteRemoveButtons() const;

    [[nodiscard]] CodeInput *codeInput() const { return codeInput_; }
    [[nodiscard]] QLineEdit *serverInput() const { return serverInput_; }
    [[nodiscard]] QPushButton *connectButton() const { return connectButton_; }

public slots:
    void requestConnect();

signals:
    void connectRequested(
        const QString &code,
        const QString &server,
        const QString &token);
    void disconnectRequested();
    void hostModeRequested();
    void recentConnectionsCleared();
    void favoriteToggleRequested(
        const QString &code,
        const QString &name,
        const QString &server,
        bool favorite);

private:
    struct RecentTile {
        QPushButton *button = nullptr;
        QPushButton *favorite = nullptr;
        QLabel *name = nullptr;
        QLabel *code = nullptr;
        QLabel *server = nullptr;
    };

    struct FavoriteTile {
        QPushButton *button = nullptr;
        QPushButton *remove = nullptr;
        QLabel *name = nullptr;
        QLabel *code = nullptr;
        QLabel *server = nullptr;
    };

    void buildUi();
    void chooseRecentConnection(int index);
    void chooseFavoriteConnection(int index);
    void updateRecentControls();
    void updateFavoriteControls();
    [[nodiscard]] bool isFavorite(
        const QString &code,
        const QString &server) const;

    CodeInput *codeInput_ = nullptr;
    QLineEdit *serverInput_ = nullptr;
    QLineEdit *tokenInput_ = nullptr;
    QPushButton *connectButton_ = nullptr;
    QLabel *statusText_ = nullptr;
    QLabel *errorText_ = nullptr;
    StatusDot *statusDot_ = nullptr;
    QWidget *recentPanel_ = nullptr;
    QPushButton *clearRecentButton_ = nullptr;
    QWidget *favoritePanel_ = nullptr;
    QList<RecentConnection> recentConnections_;
    QList<FavoriteConnection> favoriteConnections_;
    QList<RecentTile> recentTiles_;
    QList<FavoriteTile> favoriteTiles_;
    bool busy_ = false;
    bool connected_ = false;
};

} // namespace pd::ui
