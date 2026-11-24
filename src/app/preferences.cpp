#include "app/preferences.h"

#include "app/quality_presets.h"
#include "core/util.h"

#include <QTemporaryDir>

#include <algorithm>

namespace pd {
namespace {

QString readString(
    const QSettings &settings,
    const QString &key,
    const QString &fallback = {})
{
    const QString value = settings.value(key, fallback).toString().trimmed();
    return value;
}

int readInt(const QSettings &settings, const QString &key, int fallback)
{
    return settings.value(key, fallback).toInt();
}

bool readBool(const QSettings &settings, const QString &key, bool fallback)
{
    return settings.value(key, fallback).toBool();
}

QString cleanPeerName(const QString &name)
{
    const QString cleaned = name.simplified();
    if (cleaned.isEmpty()) {
        return QStringLiteral("远端设备");
    }
    return cleaned.left(64);
}

QString normalizedServer(const QString &server)
{
    const QString trimmed = server.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const auto parsed = parseServerAddress(trimmed);
    return parsed ? formatAddress(parsed->host, parsed->port) : QString();
}

QDateTime readDateTime(const QVariant &value)
{
    const QString encoded = value.toString().trimmed();
    if (encoded.isEmpty()) {
        return {};
    }
    QDateTime result = QDateTime::fromString(encoded, Qt::ISODateWithMs);
    if (!result.isValid()) {
        result = QDateTime::fromString(encoded, Qt::ISODate);
    }
    return result;
}

bool sameEndpoint(
    const QString &code,
    const QString &server,
    const QString &otherCode,
    const QString &otherServer)
{
    return code == otherCode
        && server.compare(otherServer, Qt::CaseInsensitive) == 0;
}

} // namespace

Preferences::Preferences()
    : Preferences(Mode::Persistent)
{
}

Preferences::Preferences(Mode mode)
{
    if (mode == Mode::Ephemeral) {
        ephemeralDirectory_ = std::make_unique<QTemporaryDir>();
        if (ephemeralDirectory_->isValid()) {
            settings_ = std::make_unique<QSettings>(
                ephemeralDirectory_->filePath(
                    QStringLiteral("settings.ini")),
                QSettings::IniFormat);
        }
    }
    if (!settings_) {
        openPersistent();
    }
}

Preferences::Preferences(const QString &iniPath)
{
    if (iniPath.trimmed().isEmpty()) {
        openPersistent();
        return;
    }
    settings_ = std::make_unique<QSettings>(
        iniPath,
        QSettings::IniFormat);
}

Preferences::~Preferences() = default;

void Preferences::openPersistent()
{
    settings_ = std::make_unique<QSettings>(
        QSettings::IniFormat,
        QSettings::UserScope,
        QStringLiteral("PeerDesk"),
        QStringLiteral("PeerDesk"));
}

HostPreferences Preferences::host() const
{
    HostPreferences preferences;
    if (!settings_) {
        return preferences;
    }
    const QSettings &settings = *settings_;
    preferences.server = readString(
        settings,
        QStringLiteral("host/server"),
        QStringLiteral("127.0.0.1:7900"));
    preferences.token = readString(settings, QStringLiteral("host/token"));
    const QString code = readString(settings, QStringLiteral("host/code"));
    preferences.code = codeIsValid(code) ? normalizeCode(code) : QString();
    preferences.video.fps = readInt(settings, QStringLiteral("host/fps"), 30);
    preferences.video.maxWidth =
        readInt(settings, QStringLiteral("host/maxWidth"), 1920);
    preferences.video.quality =
        readInt(settings, QStringLiteral("host/quality"), 62);
    preferences.video.monitor =
        readInt(settings, QStringLiteral("host/monitor"), 0);
    preferences.video.skipStatic =
        readBool(settings, QStringLiteral("host/skipStatic"), true);
    preferences.video.clipboard =
        readBool(settings, QStringLiteral("host/clipboard"), true);
    preferences.video.audio =
        readBool(settings, QStringLiteral("host/audio"), true);
    preferences.allowInput =
        readBool(settings, QStringLiteral("host/allowInput"), true);
    preferences.forceRelay =
        readBool(settings, QStringLiteral("host/forceRelay"), false);
    preferences.receiveDirectory = readString(
        settings,
        QStringLiteral("host/receiveDirectory"),
        defaultReceiveDirectory());
    preferences.video.clamp();
    return preferences;
}

void Preferences::setHost(const HostPreferences &preferences)
{
    if (!settings_) {
        return;
    }
    settings_->setValue(
        QStringLiteral("host/server"),
        preferences.server.trimmed());
    settings_->setValue(
        QStringLiteral("host/token"),
        preferences.token.trimmed());
    settings_->setValue(
        QStringLiteral("host/code"),
        normalizeCode(preferences.code));
    settings_->setValue(
        QStringLiteral("host/fps"),
        preferences.video.fps);
    settings_->setValue(
        QStringLiteral("host/maxWidth"),
        preferences.video.maxWidth);
    settings_->setValue(
        QStringLiteral("host/quality"),
        preferences.video.quality);
    settings_->setValue(
        QStringLiteral("host/monitor"),
        preferences.video.monitor);
    settings_->setValue(
        QStringLiteral("host/skipStatic"),
        preferences.video.skipStatic);
    settings_->setValue(
        QStringLiteral("host/clipboard"),
        preferences.video.clipboard);
    settings_->setValue(
        QStringLiteral("host/audio"),
        preferences.video.audio);
    settings_->setValue(
        QStringLiteral("host/allowInput"),
        preferences.allowInput);
    settings_->setValue(
        QStringLiteral("host/forceRelay"),
        preferences.forceRelay);
    settings_->setValue(
        QStringLiteral("host/receiveDirectory"),
        preferences.receiveDirectory);
    settings_->sync();
}

ViewerPreferences Preferences::viewer() const
{
    ViewerPreferences preferences;
    if (!settings_) {
        return preferences;
    }
    const QSettings &settings = *settings_;
    preferences.server = readString(
        settings,
        QStringLiteral("viewer/server"),
        QStringLiteral("127.0.0.1:7900"));
    preferences.token = readString(settings, QStringLiteral("viewer/token"));
    const QString code = readString(settings, QStringLiteral("viewer/code"));
    preferences.code = codeIsValid(code) ? normalizeCode(code) : QString();
    preferences.qualityId = readString(
        settings,
        QStringLiteral("viewer/quality"),
        QStringLiteral("balanced"));
    preferences.qualityId = qualityPreset(preferences.qualityId).id;
    preferences.audio =
        settings.value(QStringLiteral("viewer/audio"), true).toBool();
    preferences.receiveDirectory = readString(
        settings,
        QStringLiteral("viewer/receiveDirectory"),
        defaultReceiveDirectory());
    return preferences;
}

void Preferences::setViewer(const ViewerPreferences &preferences)
{
    if (!settings_) {
        return;
    }
    settings_->setValue(
        QStringLiteral("viewer/server"),
        preferences.server.trimmed());
    settings_->setValue(
        QStringLiteral("viewer/token"),
        preferences.token.trimmed());
    settings_->setValue(
        QStringLiteral("viewer/code"),
        normalizeCode(preferences.code));
    settings_->setValue(
        QStringLiteral("viewer/quality"),
        qualityPreset(preferences.qualityId).id);
    settings_->setValue(
        QStringLiteral("viewer/audio"),
        preferences.audio);
    settings_->setValue(
        QStringLiteral("viewer/receiveDirectory"),
        preferences.receiveDirectory);
    settings_->sync();
}

WindowPreferences Preferences::window() const
{
    WindowPreferences preferences;
    if (!settings_) {
        return preferences;
    }
    const QSettings &settings = *settings_;
    preferences.geometry = settings
        .value(QStringLiteral("window/geometry"))
        .toByteArray();
    preferences.maximized =
        settings.value(QStringLiteral("window/maximized"), false).toBool();
    preferences.closeToTray =
        settings.value(QStringLiteral("window/closeToTray"), true).toBool();
    return preferences;
}

void Preferences::setWindow(const WindowPreferences &preferences)
{
    if (!settings_) {
        return;
    }
    settings_->setValue(
        QStringLiteral("window/geometry"),
        preferences.geometry);
    settings_->setValue(
        QStringLiteral("window/maximized"),
        preferences.maximized);
    settings_->setValue(
        QStringLiteral("window/closeToTray"),
        preferences.closeToTray);
    settings_->sync();
}

QList<RecentConnection> Preferences::recentConnections() const
{
    QList<RecentConnection> result;
    if (!settings_) {
        return result;
    }

    const int storedCount =
        settings_->beginReadArray(QStringLiteral("viewer/recentConnections"));
    const int count = std::min(storedCount, kRecentConnectionLimit);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        settings_->setArrayIndex(index);
        RecentConnection entry;
        const QString code = settings_
            ->value(QStringLiteral("code"))
            .toString()
            .trimmed();
        if (!codeIsValid(code)) {
            continue;
        }
        entry.code = normalizeCode(code);
        entry.name = cleanPeerName(
            settings_->value(QStringLiteral("name")).toString());
        entry.server = normalizedServer(
            settings_->value(QStringLiteral("server")).toString());
        if (entry.server.isEmpty()) {
            continue;
        }
        entry.connectedAt = readDateTime(
            settings_->value(QStringLiteral("connectedAt")));

        const bool duplicate = std::any_of(
            result.cbegin(),
            result.cend(),
            [&entry](const RecentConnection &existing) {
                return existing.code == entry.code
                    && existing.server.compare(
                           entry.server,
                           Qt::CaseInsensitive)
                        == 0;
            });
        if (!duplicate) {
            result.append(entry);
        }
    }
    settings_->endArray();
    return result;
}

void Preferences::setRecentConnections(
    const QList<RecentConnection> &connections)
{
    if (!settings_) {
        return;
    }

    QList<RecentConnection> normalized;
    normalized.reserve(
        std::min(
            static_cast<qsizetype>(kRecentConnectionLimit),
            connections.size()));
    for (const RecentConnection &connection : connections) {
        if (!codeIsValid(connection.code)) {
            continue;
        }
        RecentConnection entry;
        entry.code = normalizeCode(connection.code);
        entry.name = cleanPeerName(connection.name);
        entry.server = normalizedServer(connection.server);
        if (entry.server.isEmpty()) {
            continue;
        }
        entry.connectedAt = connection.connectedAt;
        if (!entry.connectedAt.isValid()) {
            entry.connectedAt = QDateTime::currentDateTimeUtc();
        }
        const bool duplicate = std::any_of(
            normalized.cbegin(),
            normalized.cend(),
            [&entry](const RecentConnection &existing) {
                return existing.code == entry.code
                    && existing.server.compare(
                           entry.server,
                           Qt::CaseInsensitive)
                        == 0;
            });
        if (!duplicate) {
            normalized.append(entry);
        }
        if (normalized.size() == kRecentConnectionLimit) {
            break;
        }
    }

    settings_->remove(QStringLiteral("viewer/recentConnections"));
    settings_->beginWriteArray(
        QStringLiteral("viewer/recentConnections"),
        normalized.size());
    for (int index = 0; index < normalized.size(); ++index) {
        settings_->setArrayIndex(index);
        const RecentConnection &entry = normalized.at(index);
        settings_->setValue(QStringLiteral("code"), entry.code);
        settings_->setValue(QStringLiteral("name"), entry.name);
        settings_->setValue(QStringLiteral("server"), entry.server);
        settings_->setValue(
            QStringLiteral("connectedAt"),
            entry.connectedAt.toUTC().toString(Qt::ISODateWithMs));
    }
    settings_->endArray();
    settings_->sync();
}

void Preferences::recordRecentConnection(
    const QString &code,
    const QString &name,
    const QString &server,
    const QDateTime &connectedAt)
{
    RecentConnection entry;
    entry.code = normalizeCode(code);
    entry.name = cleanPeerName(name);
    entry.server = normalizedServer(server);
    entry.connectedAt = connectedAt.isValid()
        ? connectedAt.toUTC()
        : QDateTime::currentDateTimeUtc();
    if (!codeIsValid(entry.code) || entry.server.isEmpty()) {
        return;
    }

    QList<RecentConnection> connections = recentConnections();
    connections.erase(
        std::remove_if(
            connections.begin(),
            connections.end(),
            [&entry](const RecentConnection &existing) {
                return existing.code == entry.code
                    && existing.server.compare(
                           entry.server,
                           Qt::CaseInsensitive)
                        == 0;
            }),
        connections.end());
    connections.prepend(entry);
    if (connections.size() > kRecentConnectionLimit) {
        connections.resize(kRecentConnectionLimit);
    }
    setRecentConnections(connections);
}

void Preferences::clearRecentConnections()
{
    setRecentConnections({});
}

QList<FavoriteConnection> Preferences::favoriteConnections() const
{
    QList<FavoriteConnection> result;
    if (!settings_) {
        return result;
    }

    const int storedCount =
        settings_->beginReadArray(QStringLiteral("viewer/favorites"));
    const int count = std::min(storedCount, kFavoriteConnectionLimit);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        settings_->setArrayIndex(index);
        FavoriteConnection entry;
        const QString code = settings_
            ->value(QStringLiteral("code"))
            .toString()
            .trimmed();
        if (!codeIsValid(code)) {
            continue;
        }
        entry.code = normalizeCode(code);
        entry.name = cleanPeerName(
            settings_->value(QStringLiteral("name")).toString());
        entry.server = normalizedServer(
            settings_->value(QStringLiteral("server")).toString());
        if (entry.server.isEmpty()) {
            continue;
        }

        const bool duplicate = std::any_of(
            result.cbegin(),
            result.cend(),
            [&entry](const FavoriteConnection &existing) {
                return sameEndpoint(
                    existing.code,
                    existing.server,
                    entry.code,
                    entry.server);
            });
        if (!duplicate) {
            result.append(entry);
        }
    }
    settings_->endArray();
    return result;
}

void Preferences::setFavoriteConnections(
    const QList<FavoriteConnection> &connections)
{
    if (!settings_) {
        return;
    }

    QList<FavoriteConnection> normalized;
    normalized.reserve(
        std::min(
            static_cast<qsizetype>(kFavoriteConnectionLimit),
            connections.size()));
    for (const FavoriteConnection &connection : connections) {
        if (!codeIsValid(connection.code)) {
            continue;
        }
        FavoriteConnection entry;
        entry.code = normalizeCode(connection.code);
        entry.name = cleanPeerName(connection.name);
        entry.server = normalizedServer(connection.server);
        if (entry.server.isEmpty()) {
            continue;
        }
        const bool duplicate = std::any_of(
            normalized.cbegin(),
            normalized.cend(),
            [&entry](const FavoriteConnection &existing) {
                return sameEndpoint(
                    existing.code,
                    existing.server,
                    entry.code,
                    entry.server);
            });
        if (!duplicate) {
            normalized.append(entry);
        }
        if (normalized.size() == kFavoriteConnectionLimit) {
            break;
        }
    }

    settings_->remove(QStringLiteral("viewer/favorites"));
    settings_->beginWriteArray(
        QStringLiteral("viewer/favorites"),
        normalized.size());
    for (int index = 0; index < normalized.size(); ++index) {
        settings_->setArrayIndex(index);
        const FavoriteConnection &entry = normalized.at(index);
        settings_->setValue(QStringLiteral("code"), entry.code);
        settings_->setValue(QStringLiteral("name"), entry.name);
        settings_->setValue(QStringLiteral("server"), entry.server);
    }
    settings_->endArray();
    settings_->sync();
}

void Preferences::recordFavoriteConnection(
    const QString &code,
    const QString &name,
    const QString &server)
{
    FavoriteConnection entry;
    entry.code = normalizeCode(code);
    entry.name = cleanPeerName(name);
    entry.server = normalizedServer(server);
    if (!codeIsValid(entry.code) || entry.server.isEmpty()) {
        return;
    }

    QList<FavoriteConnection> connections = favoriteConnections();
    connections.erase(
        std::remove_if(
            connections.begin(),
            connections.end(),
            [&entry](const FavoriteConnection &existing) {
                return sameEndpoint(
                    existing.code,
                    existing.server,
                    entry.code,
                    entry.server);
            }),
        connections.end());
    connections.prepend(entry);
    if (connections.size() > kFavoriteConnectionLimit) {
        connections.resize(kFavoriteConnectionLimit);
    }
    setFavoriteConnections(connections);
}

void Preferences::removeFavoriteConnection(
    const QString &code,
    const QString &server)
{
    const QString normalizedCode = normalizeCode(code);
    const QString normalized = normalizedServer(server);
    if (!codeIsValid(normalizedCode) || normalized.isEmpty()) {
        return;
    }

    QList<FavoriteConnection> connections = favoriteConnections();
    connections.erase(
        std::remove_if(
            connections.begin(),
            connections.end(),
            [&normalizedCode, &normalized](
                const FavoriteConnection &existing) {
                return sameEndpoint(
                    existing.code,
                    existing.server,
                    normalizedCode,
                    normalized);
            }),
        connections.end());
    setFavoriteConnections(connections);
}

bool Preferences::isFavoriteConnection(
    const QString &code,
    const QString &server) const
{
    const QString normalizedCode = normalizeCode(code);
    const QString normalized = normalizedServer(server);
    if (!codeIsValid(normalizedCode) || normalized.isEmpty()) {
        return false;
    }
    const QList<FavoriteConnection> connections = favoriteConnections();
    return std::any_of(
        connections.cbegin(),
        connections.cend(),
        [&normalizedCode, &normalized](
            const FavoriteConnection &existing) {
            return sameEndpoint(
                existing.code,
                existing.server,
                normalizedCode,
                normalized);
        });
}

void Preferences::clearFavoriteConnections()
{
    setFavoriteConnections({});
}

void Preferences::sync()
{
    if (settings_) {
        settings_->sync();
    }
}

} // namespace pd
