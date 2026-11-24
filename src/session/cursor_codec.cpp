#include "session/cursor_codec.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QImageReader>
#include <QImageWriter>

#include <utility>

namespace pd {

namespace {

constexpr int kBase64EncodedMax =
    static_cast<int>(((kCursorImageMaxPngBytes + 2) / 3) * 4);

bool imageSizeAllowed(const QSize &size)
{
    return size.width() > 0
        && size.height() > 0
        && size.width() <= kCursorImageMaxDimension
        && size.height() <= kCursorImageMaxDimension;
}

bool strictlyBase64(const QString &text)
{
    if (text.isEmpty() || text.size() % 4 != 0
        || text.size() > kBase64EncodedMax) {
        return false;
    }

    qsizetype padding = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const ushort value = text.at(index).unicode();
        if (value == '=') {
            ++padding;
            if (padding > 2) {
                return false;
            }
            continue;
        }
        if (padding > 0) {
            return false;
        }
        const bool valid =
            (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')
            || value == '+' || value == '/';
        if (!valid) {
            return false;
        }
    }
    return true;
}

} // namespace

QByteArray encodeCursorImage(const QImage &image, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return QByteArray{};
    };
    if (image.isNull() || !imageSizeAllowed(image.size())) {
        return fail(QStringLiteral("invalid cursor image size"));
    }

    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("cannot open cursor image buffer"));
    }
    QImageWriter writer(&buffer, QByteArrayLiteral("png"));
    writer.setQuality(100);
    if (!writer.write(image.convertToFormat(QImage::Format_ARGB32))) {
        return fail(writer.errorString());
    }
    if (encoded.size() > kCursorImageMaxPngBytes) {
        return fail(QStringLiteral("cursor image exceeds PNG size limit"));
    }
    if (error) {
        error->clear();
    }
    return encoded;
}

CursorImageDecodeResult decodeCursorImage(
    const QString &base64,
    int width,
    int height)
{
    if (!imageSizeAllowed(QSize(width, height))) {
        return {{}, QStringLiteral("invalid cursor image dimensions")};
    }
    if (!strictlyBase64(base64)) {
        return {{}, QStringLiteral("invalid cursor image base64")};
    }

    const QByteArray decoded = QByteArray::fromBase64(
        base64.toLatin1(),
        QByteArray::Base64Encoding);
    if (decoded.isEmpty() || decoded.size() > kCursorImageMaxPngBytes) {
        return {{}, QStringLiteral("invalid cursor PNG size")};
    }
    static const QByteArray signature =
        QByteArray::fromHex("89504e470d0a1a0a");
    if (!decoded.startsWith(signature)) {
        return {{}, QStringLiteral("not a PNG image")};
    }

    QBuffer buffer;
    buffer.setData(decoded);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {{}, QStringLiteral("cannot open cursor image buffer")};
    }
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    if (reader.size() != QSize(width, height)
        || !imageSizeAllowed(reader.size())) {
        return {{}, QStringLiteral("cursor image dimensions mismatch")};
    }

    QImage image = reader.read();
    if (image.isNull() || image.size() != QSize(width, height)
        || !imageSizeAllowed(image.size())) {
        return {{}, reader.errorString()};
    }
    return {std::move(image), {}};
}

quint32 cursorImageId(const QImage &image)
{
    if (image.isNull() || !imageSizeAllowed(image.size())) {
        return 0;
    }
    const QImage canonical =
        image.convertToFormat(QImage::Format_RGBA8888);
    if (canonical.isNull()) {
        return 0;
    }

    const auto appendU32 = [](QByteArray *bytes, quint32 value) {
        bytes->append(static_cast<char>((value >> 24) & 0xFF));
        bytes->append(static_cast<char>((value >> 16) & 0xFF));
        bytes->append(static_cast<char>((value >> 8) & 0xFF));
        bytes->append(static_cast<char>(value & 0xFF));
    };
    QByteArray dimensions;
    dimensions.reserve(8);
    appendU32(
        &dimensions,
        static_cast<quint32>(canonical.width()));
    appendU32(
        &dimensions,
        static_cast<quint32>(canonical.height()));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(dimensions);
    for (int y = 0; y < canonical.height(); ++y) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char *>(canonical.constScanLine(y)),
            canonical.width() * 4));
    }
    const QByteArray digest = hash.result();
    quint32 identifier =
        (static_cast<quint32>(
             static_cast<quint8>(digest.at(0)))
         << 24)
        | (static_cast<quint32>(
               static_cast<quint8>(digest.at(1)))
           << 16)
        | (static_cast<quint32>(
               static_cast<quint8>(digest.at(2)))
           << 8)
        | static_cast<quint32>(
            static_cast<quint8>(digest.at(3)));
    if (identifier == 0) {
        identifier = 1;
    }
    return identifier;
}

} // namespace pd
