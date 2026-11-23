#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

class QFile;
class QMutex;

namespace pd::diagnostics {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Critical,
};

struct LoggingOptions {
    QString directory;
    QString applicationId = QStringLiteral("peerdesk");
    qint64 maxFileBytes = 4 * 1024 * 1024;
    int maxFiles = 5;
    bool installMessageHandler = true;
    bool trackSession = true;
};

class LogManager final {
public:
    static LogManager &instance();

    ~LogManager();

    LogManager(const LogManager &) = delete;
    LogManager &operator=(const LogManager &) = delete;

    bool start(
        const LoggingOptions &options,
        QString *error = nullptr);
    void stop();

    [[nodiscard]] bool isActive() const;
    [[nodiscard]] QString logDirectory() const;
    [[nodiscard]] QString currentLogPath() const;
    [[nodiscard]] bool previousSessionCrashed() const;

    void addSecret(const QString &secret);
    void clearSecrets();
    void write(LogLevel level, const QString &message);

    [[nodiscard]] QString redactCurrent(const QString &text) const;
    [[nodiscard]] static QString redact(
        const QString &text,
        const QStringList &secrets = {});
    [[nodiscard]] static QString levelName(LogLevel level);
    [[nodiscard]] static QString defaultLogDirectory();

private:
    LogManager();

    bool rotateIfNeededLocked(qint64 incomingBytes);
    void appendLineLocked(LogLevel level, const QString &line);
    void closeLocked();

    std::unique_ptr<QMutex> mutex_;
    std::unique_ptr<QFile> file_;
    QString directory_;
    QString baseName_;
    QString markerPath_;
    qint64 maxFileBytes_ = 4 * 1024 * 1024;
    int maxFiles_ = 5;
    qint64 currentBytes_ = 0;
    bool active_ = false;
    bool trackSession_ = true;
    bool previousSessionCrashed_ = false;
    QStringList secrets_;
};

} // namespace pd::diagnostics
