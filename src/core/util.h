#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include <optional>

namespace pd {

[[nodiscard]] QString normalizeCode(const QString &text);
[[nodiscard]] QString formatCode(const QString &text);
[[nodiscard]] bool codeIsValid(const QString &text);
[[nodiscard]] QString generateCode();
[[nodiscard]] QByteArray pskFromCode(const QString &code, const QByteArray &salt = {});
[[nodiscard]] QString codeId(const QString &code);

struct ServerAddress {
    QString host;
    quint16 port = 0;
};

[[nodiscard]] std::optional<ServerAddress> parseServerAddress(
    const QString &text, quint16 defaultPort = 7900);
[[nodiscard]] QString formatAddress(const QString &host, quint16 port);

} // namespace pd
