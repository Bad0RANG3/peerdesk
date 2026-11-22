#include "transport/peer_link.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

namespace {

void emitJson(QJsonObject object)
{
    QTextStream out(stdout);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

QString hashText(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool parseEndpoint(const QString &text, QString *host, quint16 *port)
{
    const qsizetype separator = text.lastIndexOf(QLatin1Char(':'));
    if (separator <= 0 || separator == text.size() - 1) {
        return false;
    }
    QString parsedHost = text.left(separator);
    if (parsedHost.startsWith(QLatin1Char('['))
        && parsedHost.endsWith(QLatin1Char(']'))) {
        parsedHost = parsedHost.mid(1, parsedHost.size() - 2);
    }
    bool ok = false;
    const uint parsedPort = text.mid(separator + 1).toUInt(&ok);
    if (!ok || parsedPort == 0 || parsedPort > 65535) {
        return false;
    }
    *host = parsedHost;
    *port = static_cast<quint16>(parsedPort);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("peerdesk-link-harness"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PeerDesk transport interoperability harness"));
    parser.addHelpOption();
    parser.addOptions({
        {QStringList{QStringLiteral("role")},
         QStringLiteral("host or client"),
         QStringLiteral("role"),
         QStringLiteral("client")},
        {QStringList{QStringLiteral("bind")},
         QStringLiteral("UDP bind address"),
         QStringLiteral("address"),
         QStringLiteral("127.0.0.1")},
        {QStringList{QStringLiteral("port")},
         QStringLiteral("UDP bind port (0 = random)"),
         QStringLiteral("port"),
         QStringLiteral("0")},
        {QStringList{QStringLiteral("candidate")},
         QStringLiteral("Peer candidate host:port (repeatable)"),
         QStringLiteral("endpoint")},
        {QStringList{QStringLiteral("relay")},
         QStringLiteral("Relay host:port"),
         QStringLiteral("endpoint")},
        {QStringList{QStringLiteral("relay-tag")},
         QStringLiteral("Relay outbound tag"),
         QStringLiteral("number")},
        {QStringList{QStringLiteral("psk")},
         QStringLiteral("Shared secret"),
         QStringLiteral("text"),
         QStringLiteral("interop-secret")},
        {QStringList{QStringLiteral("device-id")},
         QStringLiteral("Device id"),
         QStringLiteral("id"),
         QStringLiteral("cpp-harness")},
        {QStringList{QStringLiteral("duration")},
         QStringLiteral("Run duration in milliseconds"),
         QStringLiteral("ms"),
         QStringLiteral("8000")},
        {QStringList{QStringLiteral("force-relay")},
         QStringLiteral("Skip direct candidates and use the relay")},
    });
    parser.process(app);

    const QString role = parser.value(QStringLiteral("role")).toLower();
    const bool isHost = role == QStringLiteral("host");
    if (!isHost && role != QStringLiteral("client")) {
        std::fprintf(stderr, "invalid --role\n");
        return 2;
    }

    bool ok = false;
    const uint bindPort = parser.value(QStringLiteral("port")).toUInt(&ok);
    if (!ok || bindPort > 65535) {
        std::fprintf(stderr, "invalid --port\n");
        return 2;
    }
    const int duration = parser.value(QStringLiteral("duration")).toInt(&ok);
    if (!ok || duration < 500) {
        std::fprintf(stderr, "invalid --duration\n");
        return 2;
    }
    quint32 relayTag = 0;
    if (parser.isSet(QStringLiteral("relay-tag"))) {
        bool relayTagOk = false;
        relayTag = parser.value(QStringLiteral("relay-tag"))
                       .toUInt(&relayTagOk);
        if (!relayTagOk) {
            std::fprintf(stderr, "invalid --relay-tag\n");
            return 2;
        }
    }

    pd::PeerLink link(
        isHost,
        parser.value(QStringLiteral("psk")).toUtf8());
    link.setLabel(QStringLiteral("cpp-harness"));
    link.setDeviceId(parser.value(QStringLiteral("device-id")));
    link.setForceRelay(parser.isSet(QStringLiteral("force-relay")));

    QList<pd::Candidate> candidates;
    for (const QString &endpoint : parser.values(QStringLiteral("candidate"))) {
        QString host;
        quint16 port = 0;
        if (!parseEndpoint(endpoint, &host, &port)) {
            std::fprintf(stderr, "invalid --candidate %s\n", endpoint.toUtf8().constData());
            return 2;
        }
        candidates.append(pd::Candidate{
            host, port, QStringLiteral("host")});
    }
    link.addCandidates(candidates);

    const QString relayEndpoint = parser.value(QStringLiteral("relay"));
    if (!relayEndpoint.isEmpty()) {
        QString host;
        quint16 port = 0;
        if (!parseEndpoint(relayEndpoint, &host, &port)) {
            std::fprintf(stderr, "invalid --relay %s\n", relayEndpoint.toUtf8().constData());
            return 2;
        }
        link.offerRelay(host, port);
        link.setRelayOutTag(relayTag);
    }

    bool sent = false;
    QObject::connect(
        &link,
        &pd::PeerLink::stateChanged,
        [&link, &sent](const QString &state) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("state")},
                {QStringLiteral("state"), state},
                {QStringLiteral("established"), link.established()},
            });
            if (!sent && link.established()) {
                sent = true;
                QTimer::singleShot(
                    250,
                    &link,
                    [&link]() {
                        QByteArray video(12000, '\0');
                        for (qsizetype index = 0; index < video.size(); ++index) {
                            video[index] = static_cast<char>((index * 17) & 0xFF);
                        }
                        link.sendControl(QJsonObject{
                            {QStringLiteral("t"), QStringLiteral("cpp-control")},
                            {QStringLiteral("body"), QString(2400, QChar(u'c'))},
                        });
                        link.sendVideo(video, true);
                        link.sendAudio(QByteArrayLiteral("cpp-audio-frame"));
                    });
            }
        });
    QObject::connect(
        &link,
        &pd::PeerLink::controlMessageReceived,
        [](const QJsonObject &message) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("control")},
                {QStringLiteral("message"), message},
            });
        });
    QObject::connect(
        &link,
        &pd::PeerLink::videoFrameReceived,
        [](const QByteArray &frame, bool keyframe) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("video")},
                {QStringLiteral("size"), frame.size()},
                {QStringLiteral("keyframe"), keyframe},
                {QStringLiteral("sha256"), hashText(frame)},
            });
        });
    QObject::connect(
        &link,
        &pd::PeerLink::audioReceived,
        [](const QByteArray &payload) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("audio")},
                {QStringLiteral("size"), payload.size()},
                {QStringLiteral("sha256"), hashText(payload)},
            });
        });
    QObject::connect(
        &link,
        &pd::PeerLink::statsChanged,
        [](const pd::LinkStats &stats) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("stats")},
                {QStringLiteral("rtt_ms"), stats.rttMs},
                {QStringLiteral("mode"), stats.mode},
                {QStringLiteral("path"), stats.path},
                {QStringLiteral("relay"), stats.usingRelay},
            });
        });
    QObject::connect(
        &link,
        &pd::PeerLink::logMessage,
        [](const QString &message) {
            emitJson(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("log")},
                {QStringLiteral("message"), message},
            });
        });

    QString error;
    if (!link.start(
            parser.value(QStringLiteral("bind")),
            static_cast<quint16>(bindPort),
            &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    emitJson(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("ready")},
        {QStringLiteral("port"), link.localPort()},
        {QStringLiteral("role"), isHost ? QStringLiteral("host") : QStringLiteral("client")},
    });
    link.beginHandshake();

    QTimer::singleShot(duration, &app, [&link]() {
        link.close();
        QCoreApplication::quit();
    });
    return app.exec();
}
