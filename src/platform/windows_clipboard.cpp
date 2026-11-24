#include "platform/windows_clipboard.h"

#include <Windows.h>

#include <QBuffer>
#include <QImageWriter>
#include <QString>

#include <algorithm>
#include <cstring>
#include <limits>

namespace pd {

namespace {

constexpr quint32 kBiAlphaBitfields = 6;

class ClipboardSession {
public:
    ClipboardSession()
    {
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (OpenClipboard(nullptr)) {
                opened_ = true;
                break;
            }
            Sleep(5);
        }
    }

    ~ClipboardSession()
    {
        if (opened_) {
            CloseClipboard();
        }
    }

    [[nodiscard]] bool opened() const { return opened_; }

private:
    bool opened_ = false;
};

QByteArray dibToBmp(const QByteArray &dib)
{
    if (dib.size() < 40) {
        return {};
    }
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(dib.constData());
    quint32 headerSize = 0;
    quint16 bitCount = 0;
    quint32 compression = 0;
    quint32 colorsUsed = 0;
    std::memcpy(&headerSize, bytes, sizeof(headerSize));
    std::memcpy(&bitCount, bytes + 14, sizeof(bitCount));
    std::memcpy(&compression, bytes + 16, sizeof(compression));
    std::memcpy(&colorsUsed, bytes + 32, sizeof(colorsUsed));
    if (headerSize < 40
        || headerSize > static_cast<quint32>(dib.size())) {
        return {};
    }

    quint64 paletteEntries = 0;
    if (bitCount > 0 && bitCount <= 8) {
        paletteEntries = colorsUsed > 0
            ? colorsUsed
            : (quint64{1} << bitCount);
    }
    quint64 maskBytes = 0;
    if (headerSize == 40) {
        if (compression == BI_BITFIELDS) {
            maskBytes = 12;
        } else if (compression == kBiAlphaBitfields) {
            maskBytes = 16;
        }
    }
    const quint64 pixelOffset =
        14 + static_cast<quint64>(headerSize)
        + paletteEntries * 4 + maskBytes;
    if (pixelOffset > 14 + static_cast<quint64>(dib.size())
        || pixelOffset > std::numeric_limits<quint32>::max()) {
        return {};
    }

    QByteArray bitmap;
    bitmap.resize(14);
    auto *header =
        reinterpret_cast<unsigned char *>(bitmap.data());
    header[0] = 'B';
    header[1] = 'M';
    const quint32 fileSize =
        static_cast<quint32>(14 + static_cast<quint64>(dib.size()));
    const quint32 offset = static_cast<quint32>(pixelOffset);
    std::memcpy(header + 2, &fileSize, sizeof(fileSize));
    std::memcpy(header + 10, &offset, sizeof(offset));
    bitmap.append(dib);
    return bitmap;
}

QImage imageFromClipboardFormat(UINT format)
{
    const HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        return {};
    }
    const auto *data =
        static_cast<const unsigned char *>(GlobalLock(handle));
    if (data == nullptr) {
        return {};
    }
    const SIZE_T bytes = GlobalSize(handle);
    const qsizetype size = static_cast<qsizetype>(
        std::min<SIZE_T>(
            bytes,
            static_cast<SIZE_T>(
                std::numeric_limits<int>::max())));
    const QByteArray dib(
        reinterpret_cast<const char *>(data),
        size);
    GlobalUnlock(handle);
    return QImage::fromData(dibToBmp(dib), "BMP");
}

} // namespace

QString WindowsClipboard::text() const
{
    ClipboardSession clipboard;
    if (!clipboard.opened()) {
        return {};
    }
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        return {};
    }
    const auto *data =
        static_cast<const wchar_t *>(GlobalLock(handle));
    if (data == nullptr) {
        return {};
    }
    const SIZE_T bytes = GlobalSize(handle);
    const int capacity = static_cast<int>(
        std::min<SIZE_T>(
            bytes / sizeof(wchar_t),
            static_cast<SIZE_T>(std::numeric_limits<int>::max())));
    int length = 0;
    while (length < capacity && data[length] != L'\0') {
        ++length;
    }
    const QString result = QString::fromWCharArray(data, length);
    GlobalUnlock(handle);
    return result;
}

QImage WindowsClipboard::image() const
{
    ClipboardSession clipboard;
    if (!clipboard.opened()) {
        return {};
    }
    QImage image = imageFromClipboardFormat(CF_DIBV5);
    if (image.isNull()) {
        image = imageFromClipboardFormat(CF_DIB);
    }
    return image;
}

bool WindowsClipboard::setText(const QString &text)
{
    ClipboardSession clipboard;
    if (!clipboard.opened()) {
        return false;
    }
    if (!EmptyClipboard()) {
        return false;
    }

    const qsizetype units =
        std::min<qsizetype>(text.size(), 1 << 20);
    const SIZE_T bytes =
        static_cast<SIZE_T>(units + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    auto *destination =
        static_cast<wchar_t *>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    for (qsizetype index = 0; index < units; ++index) {
        destination[index] = text.at(index).unicode();
    }
    destination[units] = L'\0';
    GlobalUnlock(memory);

    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

bool WindowsClipboard::setImage(const QImage &image)
{
    if (image.isNull()) {
        return false;
    }

    QByteArray bitmap;
    QBuffer buffer(&bitmap);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return false;
    }
    QImageWriter writer(&buffer, QByteArrayLiteral("BMP"));
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull() || !writer.write(rgb)
        || bitmap.size() <= 14
        || !bitmap.startsWith(QByteArrayLiteral("BM"))) {
        return false;
    }
    const QByteArray dib = bitmap.mid(14);

    ClipboardSession clipboard;
    if (!clipboard.opened() || !EmptyClipboard()) {
        return false;
    }
    HGLOBAL memory = GlobalAlloc(
        GMEM_MOVEABLE,
        static_cast<SIZE_T>(dib.size()));
    if (memory == nullptr) {
        return false;
    }
    void *destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, dib.constData(), dib.size());
    GlobalUnlock(memory);

    if (SetClipboardData(CF_DIB, memory) == nullptr) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

} // namespace pd
