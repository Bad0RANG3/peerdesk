#include "diagnostics/logging.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace pd::diagnostics {
namespace {

QtMessageHandler g_previousHandler = nullptr;
bool g_handlerInstalled = false;

QtMsgType toQtMsgType(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QtDebugMsg;
    case LogLevel::Info:
        return QtInfoMsg;
    case LogLevel::Warning:
        return QtWarningMsg;
    case LogLevel::Critical:
        return QtCriticalMsg;
    }
    return QtInfoMsg;
}

const char *consoleLevel(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Critical:
        return "CRIT";
    }
    return "INFO";
}

void forwardToConsole(
    LogLevel level,
    const QMessageLogContext &context,
    const QString &message)
{
    if (g_previousHandler) {
        g_previousHandler(
            toQtMsgType(level),
            context,
            message.toUtf8().constData());
        return;
    }
    std::fprintf(
        stderr,
        "[%s] %s\n",
        consoleLevel(level),
        message.toUtf8().constData());
    std::fflush(stderr);
}

void messageHandler(
    QtMsgType type,
    const QMessageLogContext &context,
    const QString &message)
{
    LogLevel level = LogLevel::Info;
    switch (type) {
    case QtDebugMsg:
        level = LogLevel::Debug;
        break;
    case QtInfoMsg:
        level = LogLevel::Info;
        break;
    case QtWarningMsg:
        level = LogLevel::Warning;
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        level = LogLevel::Critical;
        break;
    }

    LogManager::instance().write(level, message);
    forwardToConsole(level, context, message);
    if (type == QtFatalMsg) {
        std::abort();
    }
}

QString normalizedLogFileBase(const QString &applicationId)
{
    QString base = applicationId.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("peerdesk");
    }
    static const QRegularExpression unsafe(
        QStringLiteral(R"([^A-Za-z0-9._-]+)"));
    base.replace(unsafe, QStringLiteral("-"));
    return base;
}

QString rotatedPath(
    const QString &directory,
    const QString &base,
    int index)
{
    return QDir(directory).filePath(
        QStringLiteral("%1.%2.log").arg(base).arg(index));
}

} // namespace

LogManager &LogManager::instance()
{
    static LogManager manager;
    return manager;
}

LogManager::LogManager()
    : mutex_(std::make_unique<QMutex>())
{
}

LogManager::~LogManager()
{
    stop();
}

bool LogManager::start(
    const LoggingOptions &options,
    QString *error)
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)

    if (active_) {
        closeLocked();
    }

    QString directory = options.directory.trimmed();
    if (directory.isEmpty()) {
        directory = defaultLogDirectory();
    }
    directory = QDir::cleanPath(QDir(directory).absolutePath());
    if (!QDir().mkpath(directory)) {
        if (error) {
            *error = QStringLiteral("无法创建日志目录");
        }
        return false;
    }

    baseName_ = normalizedLogFileBase(options.applicationId);
    directory_ = directory;
    maxFileBytes_ = std::max<qint64>(1024, options.maxFileBytes);
    maxFiles_ = std::max(2, options.maxFiles);
    trackSession_ = options.trackSession;
    markerPath_ = QDir(directory_).filePath(baseName_ + QStringLiteral(".running"));
    previousSessionCrashed_ = false;

    if (trackSession_) {
        previousSessionCrashed_ = QFileInfo::exists(markerPath_);
        QFile::remove(markerPath_);
        QFile marker(markerPath_);
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) {
                *error = QStringLiteral("无法创建运行标记");
            }
            directory_.clear();
            markerPath_.clear();
            return false;
        }
        marker.write(
            QDateTime::currentDateTimeUtc()
                .toString(Qt::ISODateWithMs)
                .toUtf8());
        marker.write("\n");
        marker.close();
    }

    file_ = std::make_unique<QFile>(
        QDir(directory_).filePath(baseName_ + QStringLiteral(".log")));
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) {
            *error = QStringLiteral("无法打开日志文件");
        }
        file_.reset();
        if (trackSession_) {
            QFile::remove(markerPath_);
        }
        directory_.clear();
        markerPath_.clear();
        return false;
    }
    currentBytes_ = file_->size();
    active_ = true;

    if (options.installMessageHandler && !g_handlerInstalled) {
        g_previousHandler = qInstallMessageHandler(&messageHandler);
        g_handlerInstalled = true;
    }

    QString crashLine;
    if (previousSessionCrashed_) {
        crashLine = QStringLiteral("检测到上次运行未正常退出；");
    }
    appendLineLocked(
        LogLevel::Info,
        QStringLiteral("%1日志会话开始，版本=%2，Qt=%3，系统=%4")
            .arg(crashLine, QCoreApplication::applicationVersion(),
                 QString::fromLatin1(qVersion()),
                 QSysInfo::prettyProductName()));
    return true;
}

void LogManager::stop()
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)
    closeLocked();
}

bool LogManager::isActive() const
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)
    return active_;
}

QString LogManager::logDirectory() const
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)
    return directory_;
}

QString LogManager::currentLogPath() const
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)
    return file_ ? file_->fileName() : QString();
}

bool LogManager::previousSessionCrashed() const
{
    QMutexLocker locker(mutex_.get());
    Q_UNUSED(locker)
    return previousSessionCrashed_;
}

void LogManager::addSecret(const QString &secret)
{
    const QString cleaned = secret.trimmed();
    if (cleaned.size() < 3) {
        return;
    }
    QMutexLocker locker(mutex_.get());
    if (secrets_.contains(cleaned)) {
        return;
    }
    secrets_.append(cleaned);
    std::sort(
        secrets_.begin(),
        secrets_.end(),
        [](const QString &left, const QString &right) {
            return left.size() > right.size();
        });
}

void LogManager::clearSecrets()
{
    QMutexLocker locker(mutex_.get());
    secrets_.clear();
}

void LogManager::write(LogLevel level, const QString &message)
{
    QMutexLocker locker(mutex_.get());
    if (!active_ || !file_) {
        return;
    }

    QString cleaned = redact(message, secrets_);
    cleaned.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    cleaned.replace(u'\r', u'\n');
    const QStringList lines = cleaned.split(u'\n');
    for (const QString &line : lines) {
        appendLineLocked(level, line);
    }
}

QString LogManager::redactCurrent(const QString &text) const
{
    QMutexLocker locker(mutex_.get());
    return redact(text, secrets_);
}

QString LogManager::redact(
    const QString &text,
    const QStringList &secrets)
{
    QString result = text;

    static const QRegularExpression bearer(
        QStringLiteral(R"((?i)\bBearer\s+[A-Za-z0-9._~+/=-]+)"));
    result.replace(bearer, QStringLiteral("Bearer [REDACTED]"));

    static const QRegularExpression keyed(
        QStringLiteral(
            R"((?i)\b(token|authorization|password|passwd|secret|psk|code)\b(\s*[:=]\s*)(\"[^\"]*\"|'[^']*'|[^\s,;]+))"));
    result.replace(
        keyed,
        QStringLiteral("\\1\\2[REDACTED]"));

    static const QRegularExpression urlEndpoint(
        QStringLiteral(
            R"((?i)\b(?:wss?|https?|tcp|udp)://[^\s<>"',;]+)"));
    result.replace(urlEndpoint, QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression ipv4Endpoint(
        QStringLiteral(
            R"((?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?::\d{1,5})?(?![\d.]))"));
    result.replace(ipv4Endpoint, QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression bracketedIpv6Endpoint(
        QStringLiteral(
            R"((?i)\[[0-9a-f:.]+\](?::\d{1,5})?)"));
    result.replace(
        bracketedIpv6Endpoint,
        QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression compressedIpv6Endpoint(
        QStringLiteral(
            R"((?i)(?<![0-9a-f:.])(?=[0-9a-f:]{2,39}(?![0-9a-f:.]))(?=[0-9a-f:]*::)[0-9a-f:]+(?![0-9a-f:.]))"));
    result.replace(
        compressedIpv6Endpoint,
        QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression fullIpv6Endpoint(
        QStringLiteral(
            R"((?i)(?<![0-9a-f:.])(?:[0-9a-f]{1,4}:){7}[0-9a-f]{1,4}(?![0-9a-f:.]))"));
    result.replace(
        fullIpv6Endpoint,
        QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression hostPortEndpoint(
        QStringLiteral(
            R"((?i)(?<![A-Za-z0-9_.-])(?:[A-Za-z](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?):\d{1,5}(?![A-Za-z0-9_]))"));
    result.replace(
        hostPortEndpoint,
        QStringLiteral("[ENDPOINT]"));

    static const QRegularExpression connectionCode(
        QStringLiteral(R"((?<!\d)(?:\d{3}[ -]?){2}\d{3}(?!\d))"));
    result.replace(connectionCode, QStringLiteral("[REDACTED]"));

    static const QRegularExpression windowsPath(
        QStringLiteral(
            R"((?i)\b[A-Z]:\\(?:[^\\\r\n"<>|]*\\)*[^\\\r\n"<>|]*)"));
    result.replace(windowsPath, QStringLiteral("[PATH]"));

    static const QRegularExpression fileName(
        QStringLiteral(
            R"((?i)(?<![A-Za-z0-9_.-])[A-Za-z0-9][A-Za-z0-9_.() -]{0,127}\.(?:docx?|xlsx?|pptx?|pdf|zip|7z|rar|txt|csv|tsv|json|xml|png|jpe?g|gif|bmp|webp|svg|mp4|mkv|avi|mov|mp3|wav|flac|exe|msi|dll|log)(?![A-Za-z0-9_]))"));
    result.replace(fileName, QStringLiteral("[FILE]"));

    QStringList ordered = secrets;
    ordered.removeAll(QString());
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const QString &left, const QString &right) {
            return left.size() > right.size();
        });
    for (const QString &secret : ordered) {
        if (secret.size() >= 3) {
            result.replace(
                secret,
                QStringLiteral("[REDACTED]"),
                Qt::CaseInsensitive);
        }
    }
    return result;
}

QString LogManager::levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("DEBUG");
    case LogLevel::Info:
        return QStringLiteral("INFO");
    case LogLevel::Warning:
        return QStringLiteral("WARN");
    case LogLevel::Critical:
        return QStringLiteral("CRIT");
    }
    return QStringLiteral("INFO");
}

QString LogManager::defaultLogDirectory()
{
#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA").trimmed();
    if (!appData.isEmpty()) {
        return QDir(appData).filePath(QStringLiteral("PeerDesk/logs"));
    }
#endif
    const QString generic = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    return QDir(generic).filePath(QStringLiteral("PeerDesk/logs"));
}

bool LogManager::rotateIfNeededLocked(qint64 incomingBytes)
{
    if (!file_ || currentBytes_ + incomingBytes <= maxFileBytes_) {
        return true;
    }
    file_->close();
    file_.reset();

    for (int index = maxFiles_ - 1; index >= 1; --index) {
        const QString destination = rotatedPath(
            directory_, baseName_, index);
        const QString source = index == 1
            ? QDir(directory_).filePath(baseName_ + QStringLiteral(".log"))
            : rotatedPath(directory_, baseName_, index - 1);
        QFile::remove(destination);
        if (QFileInfo::exists(source)) {
            if (!QFile::rename(source, destination)) {
                QFile::remove(destination);
            }
        }
    }

    file_ = std::make_unique<QFile>(
        QDir(directory_).filePath(baseName_ + QStringLiteral(".log")));
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file_.reset();
        currentBytes_ = 0;
        return false;
    }
    currentBytes_ = 0;
    return true;
}

void LogManager::appendLineLocked(
    LogLevel level,
    const QString &line)
{
    if (!file_) {
        return;
    }
    const QString output =
        QStringLiteral("%1 [%2] pid=%3 tid=0x%4 %5\n")
            .arg(
                QDateTime::currentDateTimeUtc()
                    .toString(Qt::ISODateWithMs),
                levelName(level),
                QString::number(QCoreApplication::applicationPid()),
                QString::number(
                    reinterpret_cast<quintptr>(QThread::currentThreadId()),
                    16),
                line);
    const QByteArray bytes = output.toUtf8();
    if (!rotateIfNeededLocked(bytes.size())) {
        active_ = false;
        return;
    }
    const qint64 written = file_->write(bytes);
    if (written != bytes.size()) {
        active_ = false;
        return;
    }
    file_->flush();
    currentBytes_ += written;
}

void LogManager::closeLocked()
{
    if (file_) {
        file_->flush();
        file_->close();
        file_.reset();
    }
    if (trackSession_ && !markerPath_.isEmpty()) {
        QFile::remove(markerPath_);
    }
    if (g_handlerInstalled) {
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        g_handlerInstalled = false;
    }
    active_ = false;
    trackSession_ = true;
    currentBytes_ = 0;
    directory_.clear();
    markerPath_.clear();
}

} // namespace pd::diagnostics
