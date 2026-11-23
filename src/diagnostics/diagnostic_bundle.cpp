#include "diagnostics/diagnostic_bundle.h"

#include "diagnostics/logging.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <QTime>
#include <QVector>

#include <algorithm>

namespace pd::diagnostics {
namespace {

struct ZipEntry {
    QByteArray name;
    QByteArray data;
    quint32 crc = 0;
    quint32 offset = 0;
    quint16 dosTime = 0;
    quint16 dosDate = 0;
};

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffU;
    for (const char raw : data) {
        crc ^= static_cast<quint8>(raw);
        for (int bit = 0; bit < 8; ++bit) {
            const quint32 mask =
                0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

quint16 dosTime(const QDateTime &time)
{
    const QTime local = time.toLocalTime().time();
    return static_cast<quint16>(
        (local.hour() << 11)
        | (local.minute() << 5)
        | (local.second() / 2));
}

quint16 dosDate(const QDateTime &time)
{
    const QDate local = time.toLocalTime().date();
    const int year = std::clamp(local.year(), 1980, 2107);
    return static_cast<quint16>(
        ((year - 1980) << 9)
        | (local.month() << 5)
        | local.day());
}

void writeU16(QByteArray *buffer, quint16 value)
{
    buffer->append(static_cast<char>(value & 0xff));
    buffer->append(static_cast<char>((value >> 8) & 0xff));
}

void writeU32(QByteArray *buffer, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        buffer->append(
            static_cast<char>((value >> shift) & 0xff));
    }
}

void writeLocalHeader(QByteArray *buffer, const ZipEntry &entry)
{
    writeU32(buffer, 0x04034b50U);
    writeU16(buffer, 20);
    writeU16(buffer, 0x0800);
    writeU16(buffer, 0);
    writeU16(buffer, entry.dosTime);
    writeU16(buffer, entry.dosDate);
    writeU32(buffer, entry.crc);
    writeU32(buffer, static_cast<quint32>(entry.data.size()));
    writeU32(buffer, static_cast<quint32>(entry.data.size()));
    writeU16(buffer, static_cast<quint16>(entry.name.size()));
    writeU16(buffer, 0);
    buffer->append(entry.name);
    buffer->append(entry.data);
}

void writeCentralHeader(QByteArray *buffer, const ZipEntry &entry)
{
    writeU32(buffer, 0x02014b50U);
    writeU16(buffer, 20);
    writeU16(buffer, 20);
    writeU16(buffer, 0x0800);
    writeU16(buffer, 0);
    writeU16(buffer, entry.dosTime);
    writeU16(buffer, entry.dosDate);
    writeU32(buffer, entry.crc);
    writeU32(buffer, static_cast<quint32>(entry.data.size()));
    writeU32(buffer, static_cast<quint32>(entry.data.size()));
    writeU16(buffer, static_cast<quint16>(entry.name.size()));
    writeU16(buffer, 0);
    writeU16(buffer, 0);
    writeU16(buffer, 0);
    writeU16(buffer, 0);
    writeU32(buffer, 0);
    writeU32(buffer, entry.offset);
    buffer->append(entry.name);
}

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

QJsonObject manifestObject(
    const DiagnosticBundleOptions &options,
    const QStringList &logs,
    bool truncated)
{
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("peerdesk-diagnostics")},
        {QStringLiteral("formatVersion"), 1},
        {
            QStringLiteral("createdUtc"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
        },
        {
            QStringLiteral("application"),
            QJsonObject{
                {QStringLiteral("id"), options.applicationId},
                {QStringLiteral("name"), options.applicationName},
                {
                    QStringLiteral("version"),
                    options.applicationVersion.isEmpty()
                        ? QCoreApplication::applicationVersion()
                        : options.applicationVersion,
                },
                {QStringLiteral("process"), options.processName},
            },
        },
        {
            QStringLiteral("runtime"),
            QJsonObject{
                {
                    QStringLiteral("os"),
                    QSysInfo::prettyProductName(),
                },
                {
                    QStringLiteral("kernel"),
                    QSysInfo::kernelType()
                        + u' '
                        + QSysInfo::kernelVersion(),
                },
                {
                    QStringLiteral("architecture"),
                    QSysInfo::currentCpuArchitecture(),
                },
                {
                    QStringLiteral("qt"),
                    QString::fromLatin1(qVersion()),
                },
                {
                    QStringLiteral("previousSessionCrashed"),
                    options.previousSessionCrashed,
                },
            },
        },
        {
            QStringLiteral("contents"),
            QJsonObject{
                {
                    QStringLiteral("settingsSummary"),
                    QStringLiteral("settings-summary.json"),
                },
                {
                    QStringLiteral("logs"),
                    QJsonArray::fromStringList(logs),
                },
            },
        },
        {
            QStringLiteral("privacy"),
            QJsonObject{
                {QStringLiteral("redacted"), true},
                {
                    QStringLiteral("note"),
                    QStringLiteral(
                        "日志在写入和打包时会脱敏，服务器地址和网络端点替换为占位符；"
                        "设置摘要不包含地址、令牌、连接码或路径。"),
                },
            },
        },
        {QStringLiteral("truncated"), truncated},
    };
}

QByteArray safeLogText(
    const QByteArray &raw,
    const QStringList &secrets)
{
    return LogManager::redact(
               QString::fromUtf8(raw),
               secrets)
        .toUtf8();
}

bool belongsToApplication(const QFileInfo &info, const QString &prefix)
{
    const QString cleaned = prefix.trimmed();
    if (cleaned.isEmpty()) {
        return true;
    }
    const QString name = info.fileName();
    return name == cleaned + QStringLiteral(".log")
        || (name.startsWith(cleaned + u'.')
            && name.endsWith(QStringLiteral(".log")));
}

} // namespace

bool writeDiagnosticBundle(
    const DiagnosticBundleOptions &options,
    QString *error)
{
    if (options.outputPath.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("诊断包路径为空");
        }
        return false;
    }
    const QFileInfo outputInfo(options.outputPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        if (error) {
            *error = QStringLiteral("无法创建诊断包目录");
        }
        return false;
    }

    const QDateTime modified = QDateTime::currentDateTime();
    QVector<ZipEntry> entries;
    QVector<ZipEntry> logs;
    QStringList includedLogs;
    bool truncated = false;
    qint64 estimatedBytes = 22;
    const qint64 maxBytes = std::max<qint64>(
        64 * 1024,
        options.maxBytes);

    const QString summaryName =
        QStringLiteral("settings-summary.json");
    ZipEntry summary;
    summary.name = summaryName.toUtf8();
    summary.data = jsonBytes(options.settingsSummary);
    summary.crc = crc32(summary.data);
    summary.dosTime = dosTime(modified);
    summary.dosDate = dosDate(modified);
    estimatedBytes +=
        30 + summary.name.size() + summary.data.size()
        + 46 + summary.name.size();

    const QDir logDir(options.logDirectory);
    const QFileInfoList files = logDir.entryInfoList(
        QStringList{QStringLiteral("*.log")},
        QDir::Files | QDir::Readable,
        QDir::Time);
    for (const QFileInfo &info : files) {
        if (!info.isFile() || info.size() <= 0) {
            continue;
        }
        if (!belongsToApplication(info, options.logFilePrefix)) {
            continue;
        }
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) {
            continue;
        }
        QByteArray data = safeLogText(
            input.readAll(),
            options.redactionSecrets);
        const QByteArray name =
            QByteArrayLiteral("logs/")
            + info.fileName().toUtf8();
        const qint64 cost =
            30 + name.size() + data.size()
            + 46 + name.size();
        if (estimatedBytes + cost > maxBytes - 2048) {
            truncated = true;
            continue;
        }
        ZipEntry entry;
        entry.name = name;
        entry.data = std::move(data);
        entry.crc = crc32(entry.data);
        entry.dosTime = dosTime(info.lastModified());
        entry.dosDate = dosDate(info.lastModified());
        estimatedBytes += cost;
        includedLogs.append(QString::fromUtf8(name));
        logs.append(std::move(entry));
    }

    ZipEntry manifest;
    manifest.name = QByteArrayLiteral("manifest.json");
    manifest.data = jsonBytes(manifestObject(
        options,
        includedLogs,
        truncated));
    manifest.crc = crc32(manifest.data);
    manifest.dosTime = dosTime(modified);
    manifest.dosDate = dosDate(modified);
    estimatedBytes +=
        30 + manifest.name.size() + manifest.data.size()
        + 46 + manifest.name.size();
    if (estimatedBytes > maxBytes) {
        if (error) {
            *error = QStringLiteral("诊断包超过大小上限");
        }
        return false;
    }

    entries.append(std::move(manifest));
    entries.append(std::move(summary));
    for (ZipEntry &entry : logs) {
        entries.append(std::move(entry));
    }

    QSaveFile output(options.outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("无法写入诊断包");
        }
        return false;
    }

    quint32 offset = 0;
    for (ZipEntry &entry : entries) {
        entry.offset = offset;
        QByteArray local;
        local.reserve(
            30 + entry.name.size() + entry.data.size());
        writeLocalHeader(&local, entry);
        if (output.write(local) != local.size()) {
            output.cancelWriting();
            if (error) {
                *error = QStringLiteral("写入诊断包失败");
            }
            return false;
        }
        offset += static_cast<quint32>(local.size());
    }

    QByteArray central;
    for (const ZipEntry &entry : entries) {
        writeCentralHeader(&central, entry);
    }
    if (output.write(central) != central.size()) {
        output.cancelWriting();
        if (error) {
            *error = QStringLiteral("写入诊断包索引失败");
        }
        return false;
    }
    const quint32 centralOffset = offset;
    const quint32 centralSize =
        static_cast<quint32>(central.size());
    offset += centralSize;

    QByteArray end;
    writeU32(&end, 0x06054b50U);
    writeU16(&end, 0);
    writeU16(&end, 0);
    writeU16(
        &end,
        static_cast<quint16>(entries.size()));
    writeU16(
        &end,
        static_cast<quint16>(entries.size()));
    writeU32(&end, centralSize);
    writeU32(&end, centralOffset);
    writeU16(&end, 0);
    if (output.write(end) != end.size()) {
        output.cancelWriting();
        if (error) {
            *error = QStringLiteral("写入诊断包结束标记失败");
        }
        return false;
    }
    if (offset + end.size() > maxBytes) {
        output.cancelWriting();
        if (error) {
            *error = QStringLiteral("诊断包超过大小上限");
        }
        return false;
    }
    if (!output.commit()) {
        if (error) {
            *error = QStringLiteral("无法保存诊断包");
        }
        return false;
    }
    return true;
}

} // namespace pd::diagnostics
