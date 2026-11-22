#include "core/util.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStringList>

namespace pd {

QString normalizeCode(const QString &text)
{
    QString digits;
    digits.reserve(9);
    for (const QChar ch : text) {
        if (ch >= u'0' && ch <= u'9') {
            digits.append(ch);
        }
    }
    return digits;
}

QString formatCode(const QString &text)
{
    const QString code = normalizeCode(text);
    QStringList groups;
    for (qsizetype offset = 0; offset < code.size(); offset += 3) {
        groups.append(code.mid(offset, 3));
    }
    return groups.join(u' ');
}

bool codeIsValid(const QString &text)
{
    const QString code = normalizeCode(text);
    return code.size() == 9 && code.front() != u'0';
}

QString generateCode()
{
    auto *random = QRandomGenerator::system();
    QString code;
    code.reserve(9);
    code.append(QChar(u'1' + static_cast<char16_t>(random->bounded(9))));
    for (int index = 1; index < 9; ++index) {
        code.append(QChar(u'0' + static_cast<char16_t>(random->bounded(10))));
    }
    return code;
}

QByteArray pskFromCode(const QString &code, const QByteArray &salt)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("peerdesk-psk"));
    hash.addData(salt);
    hash.addData(QByteArray(1, '\0'));
    hash.addData(normalizeCode(code).toUtf8());
    return hash.result();
}

QString codeId(const QString &code)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("peerdesk-id"));
    hash.addData(QByteArray(1, '\0'));
    hash.addData(normalizeCode(code).toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

std::optional<ServerAddress> parseServerAddress(const QString &text, quint16 defaultPort)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return ServerAddress{QStringLiteral("127.0.0.1"), defaultPort};
    }

    QString host;
    QString portText;
    if (trimmed.startsWith(u'[')) {
        const qsizetype close = trimmed.indexOf(u']');
        if (close < 0) {
            return std::nullopt;
        }
        host = trimmed.mid(1, close - 1);
        if (close + 1 < trimmed.size()) {
            if (trimmed.at(close + 1) != u':') {
                return std::nullopt;
            }
            portText = trimmed.mid(close + 2);
        }
    } else if (trimmed.count(u':') == 1) {
        const qsizetype separator = trimmed.indexOf(u':');
        host = trimmed.left(separator);
        portText = trimmed.mid(separator + 1);
    } else {
        host = trimmed;
    }

    if (host.trimmed().isEmpty()) {
        return std::nullopt;
    }
    quint16 port = defaultPort;
    if (!portText.isEmpty()) {
        bool ok = false;
        const uint parsed = portText.toUInt(&ok);
        if (!ok || parsed == 0 || parsed > 65535) {
            return std::nullopt;
        }
        port = static_cast<quint16>(parsed);
    }
    return ServerAddress{host.trimmed(), port};
}

QString formatAddress(const QString &host, quint16 port)
{
    if (host.contains(u':') && !host.startsWith(u'[')) {
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    }
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

} // namespace pd
