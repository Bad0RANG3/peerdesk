#pragma once

#include "session/session_types.h"

#include <QDateTime>
#include <QList>
#include <QSettings>
#include <QString>

#include <memory>

class QTemporaryDir;

namespace pd {

struct HostPreferences {
    QString server = QStringLiteral("127.0.0.1:7900");
    QString token;
    QString code;
    VideoSettings video;
    bool allowInput = true;
    bool forceRelay = false;
    QString receiveDirectory;
};

struct ViewerPreferences {
    QString server = QStringLiteral("127.0.0.1:7900");
    QString token;
    QString code;
    QString qualityId = QStringLiteral("balanced");
    bool audio = true;
    QString receiveDirectory;
};

struct WindowPreferences {
    QByteArray geometry;
    bool maximized = false;
    bool closeToTray = true;
};

struct RecentConnection {
    QString code;
    QString name;
    QString server;
    QDateTime connectedAt;
};

class Preferences final {
public:
    static constexpr int kRecentConnectionLimit = 4;

    enum class Mode {
        Persistent,
        Ephemeral,
    };

    Preferences();
    explicit Preferences(Mode mode);
    explicit Preferences(const QString &iniPath);
    ~Preferences();

    Preferences(const Preferences &) = delete;
    Preferences &operator=(const Preferences &) = delete;

    [[nodiscard]] HostPreferences host() const;
    void setHost(const HostPreferences &preferences);
    [[nodiscard]] ViewerPreferences viewer() const;
    void setViewer(const ViewerPreferences &preferences);
    [[nodiscard]] WindowPreferences window() const;
    void setWindow(const WindowPreferences &preferences);
    [[nodiscard]] QList<RecentConnection> recentConnections() const;
    void setRecentConnections(
        const QList<RecentConnection> &connections);
    void recordRecentConnection(
        const QString &code,
        const QString &name,
        const QString &server,
        const QDateTime &connectedAt = {});
    void clearRecentConnections();
    void sync();

private:
    void openPersistent();

    std::unique_ptr<QTemporaryDir> ephemeralDirectory_;
    std::unique_ptr<QSettings> settings_;
};

} // namespace pd
