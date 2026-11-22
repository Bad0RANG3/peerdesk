#include "app/preferences.h"

#include "app/quality_presets.h"
#include "core/util.h"

#include <QTemporaryDir>

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

void Preferences::sync()
{
    if (settings_) {
        settings_->sync();
    }
}

} // namespace pd
