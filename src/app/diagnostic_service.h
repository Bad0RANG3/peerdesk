#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace pd {

class Preferences;

class DiagnosticService final {
public:
    [[nodiscard]] static QString defaultFileName();
    [[nodiscard]] static QString logDirectory();
    [[nodiscard]] static QStringList redactionSecrets(
        const Preferences &preferences);
    [[nodiscard]] static QJsonObject settingsSummary(
        const Preferences &preferences);

    static bool exportBundle(
        const Preferences &preferences,
        const QString &outputPath,
        QString *error = nullptr,
        const QString &logDirectory = {});
};

} // namespace pd
