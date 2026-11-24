#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace pd {

inline constexpr int kCursorImageMaxDimension = 256;
inline constexpr qint64 kCursorImageMaxPngBytes = 256 * 1024;

struct CursorImageDecodeResult {
    QImage image;
    QString error;

    [[nodiscard]] bool ok() const { return !image.isNull(); }
};

[[nodiscard]] QByteArray encodeCursorImage(
    const QImage &image,
    QString *error = nullptr);
[[nodiscard]] CursorImageDecodeResult decodeCursorImage(
    const QString &base64,
    int width,
    int height);
[[nodiscard]] quint32 cursorImageId(const QImage &image);

} // namespace pd
