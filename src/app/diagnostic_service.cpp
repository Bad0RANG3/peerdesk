#include "app/diagnostic_service.h"

#include "app/preferences.h"
#include "diagnostics/diagnostic_bundle.h"
#include "diagnostics/logging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>

namespace pd {

QString DiagnosticService::defaultFileName()
{
    return QStringLiteral("PeerDesk-Diagnostics-%1.zip")
        .arg(
            QDateTime::currentDateTime()
                .toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QString DiagnosticService::logDirectory()
{
    const diagnostics::LogManager &logging =
        diagnostics::LogManager::instance();
    const QString active = logging.logDirectory();
    return active.isEmpty()
        ? diagnostics::LogManager::defaultLogDirectory()
        : active;
}

QStringList DiagnosticService::redactionSecrets(
    const Preferences &preferences)
{
    const HostPreferences host = preferences.host();
    const ViewerPreferences viewer = preferences.viewer();
    QStringList secrets{
        host.token,
        viewer.token,
        host.code,
        viewer.code,
        host.server,
        viewer.server,
    };
    secrets.removeAll(QString());
    secrets.removeDuplicates();
    return secrets;
}

QJsonObject DiagnosticService::settingsSummary(
    const Preferences &preferences)
{
    const HostPreferences host = preferences.host();
    const ViewerPreferences viewer = preferences.viewer();
    const WindowPreferences window = preferences.window();

    return QJsonObject{
        {QStringLiteral("formatVersion"), 1},
        {
            QStringLiteral("connection"),
            QJsonObject{
                {
                    QStringLiteral("serverConfigured"),
                    !host.server.trimmed().isEmpty(),
                },
                {
                    QStringLiteral("tokenConfigured"),
                    !host.token.trimmed().isEmpty(),
                },
                {
                    QStringLiteral("hostCodeConfigured"),
                    !host.code.trimmed().isEmpty(),
                },
                {
                    QStringLiteral("viewerCodeConfigured"),
                    !viewer.code.trimmed().isEmpty(),
                },
            },
        },
        {
            QStringLiteral("host"),
            QJsonObject{
                {QStringLiteral("fps"), host.video.fps},
                {QStringLiteral("maxWidth"), host.video.maxWidth},
                {QStringLiteral("quality"), host.video.quality},
                {QStringLiteral("monitor"), host.video.monitor},
                {
                    QStringLiteral("skipStatic"),
                    host.video.skipStatic,
                },
                {
                    QStringLiteral("clipboard"),
                    host.video.clipboard,
                },
                {QStringLiteral("audio"), host.video.audio},
                {QStringLiteral("allowInput"), host.allowInput},
                {QStringLiteral("forceRelay"), host.forceRelay},
                {
                    QStringLiteral("receiveDirectoryConfigured"),
                    !host.receiveDirectory.trimmed().isEmpty(),
                },
            },
        },
        {
            QStringLiteral("viewer"),
            QJsonObject{
                {QStringLiteral("quality"), viewer.qualityId},
                {QStringLiteral("audio"), viewer.audio},
                {
                    QStringLiteral("receiveDirectoryConfigured"),
                    !viewer.receiveDirectory.trimmed().isEmpty(),
                },
            },
        },
        {
            QStringLiteral("window"),
            QJsonObject{
                {QStringLiteral("maximized"), window.maximized},
                {
                    QStringLiteral("closeToTray"),
                    window.closeToTray,
                },
            },
        },
    };
}

bool DiagnosticService::exportBundle(
    const Preferences &preferences,
    const QString &outputPath,
    QString *error,
    const QString &logDirectory)
{
    diagnostics::DiagnosticBundleOptions options;
    options.outputPath = outputPath;
    options.logDirectory = logDirectory.isEmpty()
        ? DiagnosticService::logDirectory()
        : logDirectory;
    options.applicationId = QStringLiteral("peerdesk-client");
    options.applicationName = QStringLiteral("PeerDesk");
    options.applicationVersion =
        QCoreApplication::applicationVersion();
    options.processName = QStringLiteral("client");
    options.logFilePrefix = QStringLiteral("peerdesk-client");
    options.settingsSummary = settingsSummary(preferences);
    options.redactionSecrets = redactionSecrets(preferences);
    options.previousSessionCrashed =
        diagnostics::LogManager::instance().previousSessionCrashed();
    return diagnostics::writeDiagnosticBundle(options, error);
}

} // namespace pd
