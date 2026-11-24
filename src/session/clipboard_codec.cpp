#include "session/clipboard_codec.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QImageReader>
#include <QImageWriter>

#include <utility>

namespace pd {

namespace {

constexpr int kBase64EncodedMax =
    static_cast<int>(((kClipboardPngMaxBytes + 2) / 3) * 4);

bool imageSizeAllowed(const QSize &size)
{
    if (size.width() <= 0 || size.height() <= 0
        || size.width() > kClipboardImageMaxDimension
        || size.height() > kClipboardImageMaxDimension) {
        return false;
    }
    const qint64 pixels =
        static_cast<qint64>(size.width()) * size.height();
    return pixels <= kClipboardImageMaxPixels;
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

QByteArray encodeClipboardImage(const QImage &image, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return QByteArray{};
    };
    if (image.isNull() || !imageSizeAllowed(image.size())) {
        return fail(QStringLiteral("invalid image size"));
    }

    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("cannot open image buffer"));
    }
    QImageWriter writer(&buffer, QByteArrayLiteral("png"));
    writer.setQuality(100);
    if (!writer.write(image.convertToFormat(QImage::Format_ARGB32))) {
        return fail(writer.errorString());
    }
    if (encoded.size() > kClipboardPngMaxBytes) {
        return fail(QStringLiteral("image exceeds PNG size limit"));
    }
    if (error) {
        error->clear();
    }
    return encoded;
}

ClipboardImageDecodeResult decodeClipboardImage(const QString &base64)
{
    if (!strictlyBase64(base64)) {
        return {{}, QStringLiteral("invalid base64")};
    }

    const QByteArray decoded = QByteArray::fromBase64(
        base64.toLatin1(),
        QByteArray::Base64Encoding);
    if (decoded.isEmpty() || decoded.size() > kClipboardPngMaxBytes) {
        return {{}, QStringLiteral("invalid PNG size")};
    }
    static const QByteArray signature =
        QByteArray::fromHex("89504e470d0a1a0a");
    if (!decoded.startsWith(signature)) {
        return {{}, QStringLiteral("not a PNG image")};
    }

    QBuffer buffer;
    buffer.setData(decoded);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {{}, QStringLiteral("cannot open image buffer")};
    }
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    const QSize declaredSize = reader.size();
    if (!imageSizeAllowed(declaredSize)) {
        return {{}, QStringLiteral("invalid PNG dimensions")};
    }

    QImage image = reader.read();
    if (image.isNull() || !imageSizeAllowed(image.size())) {
        return {{}, reader.errorString()};
    }
    return {std::move(image), {}};
}

bool clipboardImageMatchesDimensions(
    const QImage &image,
    int width,
    int height)
{
    return !image.isNull()
        && width == image.width()
        && height == image.height();
}

QByteArray clipboardImageFingerprint(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }
    const QImage canonical =
        image.convertToFormat(QImage::Format_RGBA8888);
    if (canonical.isNull()) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray dimensions =
        QByteArray::number(canonical.width())
        + ':'
        + QByteArray::number(canonical.height());
    hash.addData(dimensions);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char *>(canonical.constBits()),
        canonical.sizeInBytes()));
    return hash.result().toHex();
}

} // namespace pd
