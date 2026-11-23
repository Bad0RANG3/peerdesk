#include "app/diagnostic_service.h"
#include "app/preferences.h"
#include "diagnostics/diagnostic_bundle.h"
#include "diagnostics/logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

namespace {

quint16 readU16(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) {
        return 0;
    }
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(data.constData() + offset));
}

quint32 readU32(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) {
        return 0;
    }
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(data.constData() + offset));
}

QHash<QString, QByteArray> readStoredZip(const QByteArray &data)
{
    QHash<QString, QByteArray> entries;
    qsizetype offset = 0;
    while (offset + 30 <= data.size()
           && readU32(data, offset) == 0x04034b50U) {
        const quint16 flags = readU16(data, offset + 6);
        const quint16 method = readU16(data, offset + 8);
        const quint32 compressed = readU32(data, offset + 18);
        const quint32 uncompressed = readU32(data, offset + 22);
        const quint16 nameBytes = readU16(data, offset + 26);
        const quint16 extraBytes = readU16(data, offset + 28);
        const qsizetype nameOffset = offset + 30;
        const qsizetype dataOffset =
            nameOffset + nameBytes + extraBytes;
        if ((flags & 0x0008U) != 0
            || method != 0
            || compressed != uncompressed
            || dataOffset < 0
            || dataOffset + compressed > data.size()) {
            return {};
        }
        const QString name = QString::fromUtf8(
            data.constData() + nameOffset,
            nameBytes);
        entries.insert(name, data.mid(dataOffset, compressed));
        offset = dataOffset + compressed;
    }
    return entries;
}

QString firstLogEntry(const QHash<QString, QByteArray> &entries)
{
    QString text;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (it.key().startsWith(QStringLiteral("logs/"))) {
            text += QString::fromUtf8(it.value());
        }
    }
    return text;
}

} // namespace

class LoggingTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void redactsSensitiveValues();
    void writesRotatesAndCleansSessionMarker();
    void detectsPreviousSessionCrash();
    void diagnosticBundleRedactsSecretsAndSettings();
    void diagnosticBundleRejectsOversizedSummary();
};

void LoggingTest::init()
{
    auto &logging = pd::diagnostics::LogManager::instance();
    logging.stop();
    logging.clearSecrets();
}

void LoggingTest::cleanup()
{
    auto &logging = pd::diagnostics::LogManager::instance();
    logging.stop();
    logging.clearSecrets();
}

void LoggingTest::redactsSensitiveValues()
{
    const QString token = QStringLiteral("unit-test-token-alpha");
    const QString dynamicSecret =
        QStringLiteral("unit-test-dynamic-beta");
    const QString bearerSecret =
        QStringLiteral("unit-test-bearer-gamma");
    const QString password =
        QStringLiteral("unit-test-password-delta");
    const QString redacted = pd::diagnostics::LogManager::redact(
        QStringLiteral(
            "token=%1 code=135792468 "
            "Authorization: Bearer %3 password=\"%4\" %2 "
            "server=wss://signal.example:9443 "
            "peer=10.20.30.40:7901 [2001:db8::7]:7902 "
            "reflexive=2001:db8::9 "
            "file=finance-report.docx "
            "C:\\Users\\Alice\\Documents\\finance-report.docx")
            .arg(token, dynamicSecret, bearerSecret, password),
        {dynamicSecret});

    QVERIFY(!redacted.contains(token));
    QVERIFY(!redacted.contains(QStringLiteral("135792468")));
    QVERIFY(!redacted.contains(bearerSecret));
    QVERIFY(!redacted.contains(password));
    QVERIFY(!redacted.contains(dynamicSecret));
    QVERIFY(!redacted.contains(QStringLiteral("signal.example")));
    QVERIFY(!redacted.contains(QStringLiteral("10.20.30.40")));
    QVERIFY(!redacted.contains(QStringLiteral("2001:db8")));
    QVERIFY(!redacted.contains(QStringLiteral("Users")));
    QVERIFY(!redacted.contains(QStringLiteral("finance-report.docx")));
    QVERIFY(redacted.contains(QStringLiteral("[REDACTED]")));
    QVERIFY(redacted.contains(QStringLiteral("[ENDPOINT]")));
    QVERIFY(redacted.contains(QStringLiteral("[PATH]")));
    QVERIFY(redacted.contains(QStringLiteral("[FILE]")));
}

void LoggingTest::writesRotatesAndCleansSessionMarker()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pd::diagnostics::LoggingOptions options;
    options.directory = directory.path();
    options.applicationId = QStringLiteral("rotate-test");
    options.maxFileBytes = 1024;
    options.maxFiles = 3;
    options.installMessageHandler = false;
    options.trackSession = true;

    auto &logging = pd::diagnostics::LogManager::instance();
    QString error;
    QVERIFY2(logging.start(options, &error), qPrintable(error));
    QVERIFY(logging.isActive());
    const QString marker = QDir(directory.path())
                               .filePath(
                                   QStringLiteral("rotate-test.running"));
    QVERIFY(QFileInfo::exists(marker));
    QVERIFY(!logging.previousSessionCrashed());

    for (int index = 0; index < 24; ++index) {
        logging.write(
            pd::diagnostics::LogLevel::Info,
            QStringLiteral("%1 %2")
                .arg(index)
                .arg(QString(360, QLatin1Char('x'))));
    }

    const QFileInfoList files = QDir(directory.path()).entryInfoList(
        QStringList{QStringLiteral("*.log")},
        QDir::Files,
        QDir::Name);
    QVERIFY(files.size() >= 2);
    QVERIFY(files.size() <= 3);
    QVERIFY(logging.currentLogPath().endsWith(
        QStringLiteral("rotate-test.log")));

    logging.stop();
    QVERIFY(!logging.isActive());
    QVERIFY(!QFileInfo::exists(marker));
}

void LoggingTest::detectsPreviousSessionCrash()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString marker = QDir(directory.path())
                               .filePath(
                                   QStringLiteral("crash-test.running"));
    QFile stale(marker);
    QVERIFY(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(stale.write(QByteArrayLiteral("stale\n")), qint64(6));
    stale.close();

    pd::diagnostics::LoggingOptions options;
    options.directory = directory.path();
    options.applicationId = QStringLiteral("crash-test");
    options.installMessageHandler = false;
    options.trackSession = true;

    auto &logging = pd::diagnostics::LogManager::instance();
    QString error;
    QVERIFY2(logging.start(options, &error), qPrintable(error));
    QVERIFY(logging.previousSessionCrashed());
    QVERIFY(QFileInfo::exists(marker));

    logging.stop();
    QVERIFY(!QFileInfo::exists(marker));
}

void LoggingTest::diagnosticBundleRedactsSecretsAndSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logsDirectory = QDir(directory.path())
                                      .filePath(QStringLiteral("logs"));

    pd::diagnostics::LoggingOptions loggingOptions;
    loggingOptions.directory = logsDirectory;
    loggingOptions.applicationId = QStringLiteral("peerdesk-client");
    loggingOptions.maxFileBytes = 256 * 1024;
    loggingOptions.installMessageHandler = false;
    loggingOptions.trackSession = false;
    auto &logging = pd::diagnostics::LogManager::instance();
    QString error;
    QVERIFY2(logging.start(loggingOptions, &error), qPrintable(error));

    const QString token = QStringLiteral("unit-test-token-alpha");
    const QString code = QStringLiteral("246813579");
    const QString server = QStringLiteral("wss://private.example:9443");
    const QString receiveDirectory =
        QStringLiteral("C:\\Users\\Alice\\Desktop\\Inbox");
    const QString fileName = QStringLiteral("finance-report.docx");
    const QString filePath =
        QStringLiteral("C:\\Users\\Alice\\Documents\\finance-report.docx");
    const QString dynamicSecret =
        QStringLiteral("unit-test-dynamic-beta");
    const QString endpoint = QStringLiteral("10.20.30.40:7901");
    const QString ipv6Endpoint =
        QStringLiteral("[2001:db8::42]:7902");
    const QString serverOnlyMarker =
        QStringLiteral("server-only-log-marker");

    logging.addSecret(dynamicSecret);
    logging.write(
        pd::diagnostics::LogLevel::Info,
        QStringLiteral(
            "token=%1 code=%2 Authorization: Bearer %3 "
            "file=%5 path=%4 secret=%6")
            .arg(
                token,
                code,
                token,
                filePath,
                fileName,
                dynamicSecret));
    logging.write(
        pd::diagnostics::LogLevel::Info,
        QStringLiteral("server=%1 peer=%2 ipv6=%3")
            .arg(server, endpoint, ipv6Endpoint));
    logging.write(
        pd::diagnostics::LogLevel::Warning,
        QStringLiteral("network retry scheduled"));
    QFile serverLog(
        QDir(logsDirectory).filePath(
            QStringLiteral("peerdesk-server.log")));
    QVERIFY(serverLog.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(
        serverLog.write(
            QStringLiteral("%1 token=%2 code=%3 peer=%4")
                    .arg(serverOnlyMarker, token, code, endpoint)
                    .toUtf8())
        > 0);
    serverLog.close();

    pd::Preferences preferences(pd::Preferences::Mode::Ephemeral);
    pd::HostPreferences host;
    host.server = server;
    host.token = token;
    host.code = code;
    host.receiveDirectory = receiveDirectory;
    host.video.fps = 45;
    host.video.maxWidth = 1920;
    host.allowInput = false;
    host.forceRelay = true;
    preferences.setHost(host);

    pd::ViewerPreferences viewer;
    viewer.server = server;
    viewer.token = token;
    viewer.code = code;
    viewer.receiveDirectory = receiveDirectory;
    viewer.qualityId = QStringLiteral("hd");
    viewer.audio = false;
    preferences.setViewer(viewer);

    const QString bundlePath =
        QDir(directory.path()).filePath(QStringLiteral("bundle.zip"));
    QVERIFY2(
        pd::DiagnosticService::exportBundle(
            preferences,
            bundlePath,
            &error,
            logsDirectory),
        qPrintable(error));
    logging.stop();

    QFile bundleFile(bundlePath);
    QVERIFY(bundleFile.open(QIODevice::ReadOnly));
    const QByteArray bundle = bundleFile.readAll();
    QVERIFY(!bundle.isEmpty());

    for (const QString &secret : {
             token,
             code,
             server,
             receiveDirectory,
             filePath,
             fileName,
             dynamicSecret,
             endpoint,
             ipv6Endpoint,
             serverOnlyMarker,
         }) {
        QVERIFY2(
            !bundle.contains(secret.toUtf8()),
            qPrintable(QStringLiteral("泄漏敏感值：%1").arg(secret)));
    }
    QVERIFY(!bundle.contains("Bearer " + token.toUtf8()));

    const QHash<QString, QByteArray> entries =
        readStoredZip(bundle);
    QVERIFY2(entries.contains(QStringLiteral("manifest.json")), "缺少 manifest");
    QVERIFY2(
        entries.contains(QStringLiteral("settings-summary.json")),
        "缺少设置摘要");

    const QJsonObject manifest = QJsonDocument::fromJson(
                                     entries.value(
                                         QStringLiteral("manifest.json")))
                                     .object();
    QCOMPARE(
        manifest.value(QStringLiteral("format")).toString(),
        QStringLiteral("peerdesk-diagnostics"));
    QCOMPARE(manifest.value(QStringLiteral("formatVersion")).toInt(), 1);
    QVERIFY(manifest.value(QStringLiteral("privacy"))
                .toObject()
                .value(QStringLiteral("redacted"))
                .toBool());
    const QJsonArray includedLogs = manifest
                                        .value(QStringLiteral("contents"))
                                        .toObject()
                                        .value(QStringLiteral("logs"))
                                        .toArray();
    QVERIFY(!includedLogs.isEmpty());
    bool includedServerLog = false;
    for (const QJsonValue &value : includedLogs) {
        if (value.toString()
            == QStringLiteral("logs/peerdesk-server.log")) {
            includedServerLog = true;
            break;
        }
    }
    QVERIFY(!includedServerLog);

    const QJsonObject settings = QJsonDocument::fromJson(
                                     entries.value(
                                         QStringLiteral("settings-summary.json")))
                                     .object();
    const QJsonObject connection =
        settings.value(QStringLiteral("connection")).toObject();
    QVERIFY(connection.value(
                        QStringLiteral("serverConfigured"))
                .toBool());
    QVERIFY(connection.value(
                        QStringLiteral("tokenConfigured"))
                .toBool());
    QVERIFY(connection.value(
                        QStringLiteral("hostCodeConfigured"))
                .toBool());
    QVERIFY(!settings.contains(QStringLiteral("server")));
    QVERIFY(!settings.contains(QStringLiteral("token")));
    QVERIFY(!settings.contains(QStringLiteral("code")));
    QVERIFY(!settings.contains(QStringLiteral("receiveDirectory")));
    QVERIFY(!settings.contains(QStringLiteral("path")));
    QCOMPARE(
        settings.value(QStringLiteral("host"))
            .toObject()
            .value(QStringLiteral("fps"))
            .toInt(),
        45);

    const QString logText = firstLogEntry(entries);
    QVERIFY(!logText.isEmpty());
    QVERIFY(logText.contains(QStringLiteral("[REDACTED]")));
    QVERIFY(logText.contains(QStringLiteral("[ENDPOINT]")));
    QVERIFY(logText.contains(QStringLiteral("[PATH]")));
    QVERIFY(logText.contains(QStringLiteral("[FILE]")));
    QVERIFY(logText.contains(QStringLiteral("network retry scheduled")));

    const QString fileNamePattern =
        QStringLiteral("^PeerDesk-Diagnostics-\\d{8}-\\d{6}\\.zip$");
    QVERIFY(QRegularExpression(fileNamePattern)
                .match(pd::DiagnosticService::defaultFileName())
                .hasMatch());
}

void LoggingTest::diagnosticBundleRejectsOversizedSummary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pd::diagnostics::DiagnosticBundleOptions options;
    options.outputPath =
        QDir(directory.path()).filePath(QStringLiteral("too-large.zip"));
    options.logDirectory = directory.path();
    options.maxBytes = 64 * 1024;
    options.settingsSummary = QJsonObject{
        {
            QStringLiteral("padding"),
            QString(80 * 1024, QLatin1Char('x')),
        },
    };

    QString error;
    QVERIFY(!pd::diagnostics::writeDiagnosticBundle(options, &error));
    QCOMPARE(error, QStringLiteral("诊断包超过大小上限"));
    QVERIFY(!QFileInfo::exists(options.outputPath));
}

QTEST_GUILESS_MAIN(LoggingTest)

#include "logging_test.moc"
