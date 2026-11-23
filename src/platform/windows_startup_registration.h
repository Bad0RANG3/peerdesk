#pragma once

#include "platform/startup_registration.h"

#include <QSettings>

namespace pd {

class WindowsStartupRegistration final : public StartupRegistration {
public:
    explicit WindowsStartupRegistration(
        QString executablePath = defaultExecutablePath(),
        QString runKey = defaultRunKey(),
        QString valueName = QStringLiteral("PeerDesk"));

    [[nodiscard]] bool isEnabled() const override;
    [[nodiscard]] QString registeredCommand() const override;
    bool setEnabled(bool enabled, QString *error = nullptr) override;

    [[nodiscard]] static QString commandForExecutable(
        const QString &executablePath);
    [[nodiscard]] static QString defaultExecutablePath();
    [[nodiscard]] static QString defaultRunKey();

private:
    QString executablePath_;
    QString valueName_;
    QSettings settings_;
};

} // namespace pd
