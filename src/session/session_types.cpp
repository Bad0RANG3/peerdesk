#include "session/session_types.h"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace pd {

QJsonObject MonitorInfo::toJson() const
{
    return {
        {QStringLiteral("index"), index},
        {QStringLiteral("x"), left},
        {QStringLiteral("y"), top},
        {QStringLiteral("w"), width},
        {QStringLiteral("h"), height},
        {QStringLiteral("primary"), primary},
        {QStringLiteral("name"),
         name.isEmpty()
             ? QStringLiteral("显示器 %1").arg(index + 1)
             : name},
    };
}

QJsonObject VideoSettings::toJson() const
{
    return {
        {QStringLiteral("fps"), fps},
        {QStringLiteral("max_width"), maxWidth},
        {QStringLiteral("quality"), quality},
        {QStringLiteral("monitor"), monitor},
        {QStringLiteral("skip_static"), skipStatic},
        {QStringLiteral("clipboard"), clipboard},
        {QStringLiteral("audio"), audio},
        {QStringLiteral("pixel_format"), pixelFormat},
    };
}

void VideoSettings::update(const QJsonObject &object)
{
    const auto readInt = [&object](const char *key, int fallback) {
        const auto value = object.value(QLatin1String(key));
        if (!value.isDouble()) {
            return fallback;
        }
        bool ok = false;
        const int parsed = value.toVariant().toInt(&ok);
        return ok ? parsed : fallback;
    };
    const auto readBool = [&object](const char *key, bool fallback) {
        const auto value = object.value(QLatin1String(key));
        return value.isBool() ? value.toBool() : fallback;
    };

    fps = readInt("fps", fps);
    maxWidth = readInt("max_width", maxWidth);
    quality = readInt("quality", quality);
    monitor = readInt("monitor", monitor);
    skipStatic = readBool("skip_static", skipStatic);
    clipboard = readBool("clipboard", clipboard);
    audio = readBool("audio", audio);
    const QString format =
        object.value(QStringLiteral("pixel_format")).toString();
    if (!format.isEmpty()) {
        pixelFormat = format;
    }
    clamp();
}

void VideoSettings::clamp()
{
    fps = std::clamp(fps, 1, 120);
    maxWidth = std::clamp(maxWidth, 320, 3840);
    quality = std::clamp(quality, 20, 95);
    monitor = std::max(0, monitor);
}

FramePacer::FramePacer(
    int quality,
    int targetKb,
    int minQuality,
    int maxQuality)
    : quality_(quality)
    , targetKb_(targetKb)
    , minQuality_(minQuality)
    , maxQuality_(maxQuality)
{
    quality_ = std::clamp(quality_, minQuality_, maxQuality_);
    targetKb_ = std::clamp(targetKb_, 4, 600);
}

void FramePacer::observe(qint64 size, double elapsedMs)
{
    lastSize_ = size;
    lastMs_ = elapsedMs;
    const double target = static_cast<double>(targetKb_) * 1024.0;
    if (static_cast<double>(size) > target * 1.25) {
        quality_ = std::max(minQuality_, quality_ - 3);
    } else if (static_cast<double>(size) < target * 0.6) {
        quality_ = std::min(maxQuality_, quality_ + 1);
    }
}

void FramePacer::setBudgetKb(double kb)
{
    targetKb_ = kb <= 0.0
        ? 140
        : static_cast<int>(std::clamp(kb, 4.0, 600.0));
}

void FramePacer::setQuality(int quality)
{
    quality_ = std::clamp(quality, minQuality_, maxQuality_);
}

QString defaultReceiveDirectory()
{
    QString root =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (root.isEmpty()) {
        root = QDir::homePath();
    }
    return QDir(root).filePath(QStringLiteral("PeerDesk"));
}

} // namespace pd
