#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace pd::diagnostics {

struct DiagnosticBundleOptions {
    QString outputPath;
    QString logDirectory;
    QString applicationId = QStringLiteral("peerdesk-client");
    QString applicationName = QStringLiteral("PeerDesk");
    QString applicationVersion;
    QString processName = QStringLiteral("client");
    QString logFilePrefix;
    QJsonObject settingsSummary;
    QStringList redactionSecrets;
    qint64 maxBytes = 20 * 1024 * 1024;
    bool previousSessionCrashed = false;
};

bool writeDiagnosticBundle(
    const DiagnosticBundleOptions &options,
    QString *error = nullptr);

} // namespace pd::diagnostics
