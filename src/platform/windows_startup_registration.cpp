#include "platform/windows_startup_registration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <utility>

namespace pd {

WindowsStartupRegistration::WindowsStartupRegistration(
    QString executablePath,
    QString runKey,
    QString valueName)
    : executablePath_(std::move(executablePath))
    , valueName_(std::move(valueName))
    , settings_(std::move(runKey), QSettings::NativeFormat)
{
}

bool WindowsStartupRegistration::isEnabled() const
{
    return settings_.contains(valueName_)
        && !settings_.value(valueName_).toString().trimmed().isEmpty();
}

QString WindowsStartupRegistration::registeredCommand() const
{
    return settings_.value(valueName_).toString().trimmed();
}

bool WindowsStartupRegistration::setEnabled(
    bool enabled,
    QString *error)
{
    if (enabled) {
        const QString command =
            commandForExecutable(executablePath_);
        if (command.isEmpty()) {
            if (error) {
                *error = QStringLiteral("无法生成开机启动命令");
            }
            return false;
        }
        settings_.setValue(valueName_, command);
    } else {
        settings_.remove(valueName_);
    }
    settings_.sync();

    if (settings_.status() != QSettings::NoError) {
        if (error) {
            *error = QStringLiteral("无法更新当前用户的 Windows 启动项");
        }
        return false;
    }
    if (isEnabled() != enabled) {
        if (error) {
            *error = enabled
                ? QStringLiteral("Windows 启动项写入后未生效")
                : QStringLiteral("Windows 启动项移除后仍然存在");
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QString WindowsStartupRegistration::commandForExecutable(
    const QString &executablePath)
{
    const QString trimmed = executablePath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QFileInfo executable(trimmed);
    const QString absolute = QDir::toNativeSeparators(
        executable.absoluteFilePath());
    if (absolute.isEmpty() || absolute.contains(QLatin1Char('"'))) {
        return {};
    }
    return QStringLiteral("\"%1\" --background").arg(absolute);
}

QString WindowsStartupRegistration::defaultExecutablePath()
{
    return QCoreApplication::applicationFilePath();
}

QString WindowsStartupRegistration::defaultRunKey()
{
    return QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

} // namespace pd
