#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace pd {

inline constexpr qint64 kClipboardPngMaxBytes = 4 * 1024 * 1024;
inline constexpr int kClipboardImageMaxDimension = 8192;
inline constexpr qint64 kClipboardImageMaxPixels = 16000000;

struct ClipboardImageDecodeResult {
    QImage image;
    QString error;

    [[nodiscard]] bool ok() const { return !image.isNull(); }
};

[[nodiscard]] QByteArray encodeClipboardImage(
    const QImage &image,
    QString *error = nullptr);
[[nodiscard]] ClipboardImageDecodeResult decodeClipboardImage(
    const QString &base64);
[[nodiscard]] bool clipboardImageMatchesDimensions(
    const QImage &image,
    int width,
    int height);
[[nodiscard]] QByteArray clipboardImageFingerprint(const QImage &image);

} // namespace pd
