#include "diagnostics/logging.h"
#include "server/server.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>

#include <cstdio>

namespace {

QString environmentValue(
    const char *name, const QString &fallback)
{
    return qEnvironmentVariable(name, fallback);
}

void printError(const QString &text)
{
    std::fprintf(stderr, "%s\n", text.toUtf8().constData());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("peerdesk-server"));
    QCoreApplication::setOrganizationName(QStringLiteral("PeerDesk"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PEERDESK_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PeerDesk signaling + relay + STUN server"));
    parser.addHelpOption();
    const QCommandLineOption versionOption(
        QStringLiteral("version"),
        QStringLiteral("显示版本信息"));
    parser.addOption(versionOption);
    parser.addOptions({
        {QStringList{QStringLiteral("host")},
         QStringLiteral("监听地址（默认 0.0.0.0）"),
         QStringLiteral("host"),
         environmentValue("PEERDESK_BIND", QStringLiteral("0.0.0.0"))},
        {QStringList{QStringLiteral("port")},
         QStringLiteral("TCP 信令端口（0 = 随机）"),
         QStringLiteral("port"),
         environmentValue("PEERDESK_PORT", QStringLiteral("7900"))},
        {QStringList{QStringLiteral("udp-port")},
         QStringLiteral("UDP 中继/STUN 端口（0 = 随机）"),
         QStringLiteral("port"),
         environmentValue("PEERDESK_UDP_PORT", QStringLiteral("7901"))},
        {QStringList{QStringLiteral("token")},
         QStringLiteral("握手令牌；两端与服务器必须一致"),
         QStringLiteral("token"),
         environmentValue("PEERDESK_TOKEN", QString())},
        {QStringList{QStringLiteral("relay-mbps")},
         QStringLiteral("每会话中继带宽上限，0 = 不限"),
         QStringLiteral("mbps"),
         environmentValue("PEERDESK_RELAY_MBPS", QStringLiteral("0"))},
        {QStringList{QStringLiteral("connect-max")},
         QStringLiteral("窗口内每 IP 允许的连接尝试次数"),
         QStringLiteral("count"),
         QStringLiteral("10")},
        {QStringList{QStringLiteral("connect-window")},
         QStringLiteral("连接尝试计数窗口（秒）"),
         QStringLiteral("seconds"),
         QStringLiteral("60")},
        {QStringList{QStringLiteral("v"), QStringLiteral("verbose")},
         QStringLiteral("输出更详细的日志")},
        {QStringList{QStringLiteral("log-dir")},
         QStringLiteral("日志目录（默认 %APPDATA%\\PeerDesk\\logs）"),
         QStringLiteral("path"),
         environmentValue("PEERDESK_LOG_DIR", QString())},
    });
    parser.process(app);

    if (parser.isSet(versionOption)) {
        std::printf(
            "peerdesk-server %s\n",
            QCoreApplication::applicationVersion()
                .toUtf8()
                .constData());
        return 0;
    }

    bool ok = true;
    const uint port = parser.value(QStringLiteral("port")).toUInt(&ok);
    if (!ok || port > 65535) {
        printError(QStringLiteral("无效的 TCP 端口"));
        return 2;
    }
    const uint udpPort =
        parser.value(QStringLiteral("udp-port")).toUInt(&ok);
    if (!ok || udpPort > 65535) {
        printError(QStringLiteral("无效的 UDP 端口"));
        return 2;
    }
    const double relayMbps =
        parser.value(QStringLiteral("relay-mbps")).toDouble(&ok);
    if (!ok || relayMbps < 0.0) {
        printError(QStringLiteral("无效的中继带宽上限"));
        return 2;
    }
    const int connectMax =
        parser.value(QStringLiteral("connect-max")).toInt(&ok);
    if (!ok || connectMax < 1) {
        printError(QStringLiteral("无效的连接尝试次数"));
        return 2;
    }
    const double connectWindow =
        parser.value(QStringLiteral("connect-window")).toDouble(&ok);
    if (!ok || connectWindow < 1.0) {
        printError(QStringLiteral("无效的连接计数窗口"));
        return 2;
    }

    if (parser.isSet(QStringLiteral("verbose"))) {
        QLoggingCategory::setFilterRules(
            QStringLiteral("*.debug=true"));
    }

    pd::diagnostics::LoggingOptions loggingOptions;
    loggingOptions.directory = parser
        .value(QStringLiteral("log-dir"))
        .trimmed();
    if (!loggingOptions.directory.isEmpty()) {
        loggingOptions.directory = QDir::current().absoluteFilePath(
            loggingOptions.directory);
    }
    loggingOptions.applicationId = QStringLiteral("peerdesk-server");
    loggingOptions.trackSession = false;
    QString loggingError;
    if (!pd::diagnostics::LogManager::instance().start(
            loggingOptions,
            &loggingError)) {
        printError(QStringLiteral("无法启用文件日志：%1")
                       .arg(loggingError));
    }

    pd::SignalingServer::Options options;
    options.host = parser.value(QStringLiteral("host"));
    options.port = static_cast<quint16>(port);
    options.udpPort = static_cast<quint16>(udpPort);
    options.token = parser.value(QStringLiteral("token"));
    options.relayMbps = relayMbps;
    options.connectMax = connectMax;
    options.connectWindow = connectWindow;
    pd::diagnostics::LogManager::instance().addSecret(options.token);

    pd::SignalingServer server(options);
    QObject::connect(
        &server, &pd::SignalingServer::logMessage,
        [](const QString &message) {
            qInfo().noquote() << message;
        });

    QString error;
    if (!server.start(&error)) {
        printError(error);
        pd::diagnostics::LogManager::instance().stop();
        return 1;
    }
    QObject::connect(
        &app,
        &QCoreApplication::aboutToQuit,
        &app,
        []() {
            pd::diagnostics::LogManager::instance().stop();
        });
    qInfo().noquote()
        << QStringLiteral("PeerDesk 服务端已启动：%1:%2，UDP %3")
               .arg(options.host)
               .arg(options.port)
               .arg(options.udpPort);
    return app.exec();
}
