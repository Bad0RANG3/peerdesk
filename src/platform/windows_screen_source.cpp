#include "platform/windows_screen_source.h"

#include "session/cursor_codec.h"

#include <Windows.h>

#include <QBitArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace pd {

namespace {

constexpr int kCursorImageCacheLimit = 16;

QString win32Error(DWORD code)
{
    wchar_t *text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t *>(&text),
        0,
        nullptr);
    QString result;
    if (length > 0 && text != nullptr) {
        result = QString::fromWCharArray(text, static_cast<int>(length))
                     .trimmed();
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    if (result.isEmpty()) {
        result = QStringLiteral("Win32 错误 %1").arg(code);
    }
    return result;
}

BOOL CALLBACK collectMonitor(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM context)
{
    auto *found = reinterpret_cast<QList<MonitorInfo> *>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    MonitorInfo item;
    item.left = info.rcMonitor.left;
    item.top = info.rcMonitor.top;
    item.width = info.rcMonitor.right - info.rcMonitor.left;
    item.height = info.rcMonitor.bottom - info.rcMonitor.top;
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    item.name = QString::fromWCharArray(info.szDevice);
    found->append(item);
    return TRUE;
}

std::optional<CursorShape> standardCursorShape(HCURSOR cursor)
{
    struct ShapeMap {
        LPCWSTR id;
        CursorShape shape;
    };
    static const ShapeMap shapes[] = {
        {IDC_ARROW, CursorShape::Arrow},
        {IDC_IBEAM, CursorShape::IBeam},
        {IDC_HAND, CursorShape::Hand},
        {IDC_CROSS, CursorShape::Crosshair},
        {IDC_WAIT, CursorShape::Wait},
        {IDC_APPSTARTING, CursorShape::Wait},
        {IDC_SIZEALL, CursorShape::SizeAll},
        {IDC_SIZENWSE, CursorShape::SizeNWSE},
        {IDC_SIZENESW, CursorShape::SizeNESW},
        {IDC_SIZEWE, CursorShape::SizeWE},
        {IDC_SIZENS, CursorShape::SizeNS},
        {IDC_NO, CursorShape::NotAllowed},
    };
    for (const ShapeMap &entry : shapes) {
        if (LoadCursorW(nullptr, entry.id) == cursor) {
            return entry.shape;
        }
    }
    return std::nullopt;
}

struct IconInfoData {
    ICONINFO value{};

    ~IconInfoData()
    {
        if (value.hbmMask != nullptr) {
            DeleteObject(value.hbmMask);
        }
        if (value.hbmColor != nullptr) {
            DeleteObject(value.hbmColor);
        }
    }
};

bool bitmapSize(HBITMAP bitmap, QSize *size)
{
    if (bitmap == nullptr || size == nullptr) {
        return false;
    }
    BITMAP description{};
    if (GetObjectW(bitmap, sizeof(description), &description)
        != sizeof(description)) {
        return false;
    }
    if (description.bmWidth <= 0 || description.bmHeight <= 0) {
        return false;
    }
    *size = QSize(description.bmWidth, description.bmHeight);
    return true;
}

QImage colorBitmapImage(HBITMAP bitmap, const QSize &size)
{
    if (bitmap == nullptr || size.isEmpty()) {
        return {};
    }
    QImage image(size, QImage::Format_ARGB32);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.width();
    info.bmiHeader.biHeight = -size.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage =
        static_cast<DWORD>(size.width() * size.height() * 4);

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        return {};
    }
    const int rows = GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(size.height()),
        image.bits(),
        &info,
        DIB_RGB_COLORS);
    DeleteDC(dc);
    return rows == size.height() ? image : QImage{};
}

QBitArray monochromeBitmapBits(
    HBITMAP bitmap,
    int width,
    int rows)
{
    if (bitmap == nullptr || width <= 0 || rows <= 0) {
        return {};
    }
    const int stride = ((width + 31) / 32) * 4;
    QByteArray bits(stride * rows, '\0');

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 1;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage =
        static_cast<DWORD>(stride * rows);

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        return {};
    }
    const int copied = GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(rows),
        bits.data(),
        &info,
        DIB_RGB_COLORS);
    DeleteDC(dc);
    if (copied != rows) {
        return {};
    }

    QBitArray result(width * rows);
    for (int y = 0; y < rows; ++y) {
        const uchar *row = reinterpret_cast<const uchar *>(
            bits.constData() + y * stride);
        for (int x = 0; x < width; ++x) {
            result.setBit(
                y * width + x,
                (row[x / 8] & (0x80U >> (x % 8))) != 0);
        }
    }
    return result;
}

QImage cursorImageFromIcon(const ICONINFO &icon)
{
    QSize colorSize;
    QSize maskSize;
    const bool hasColor =
        bitmapSize(icon.hbmColor, &colorSize);
    const bool hasMask = bitmapSize(icon.hbmMask, &maskSize);
    if (!hasColor && !hasMask) {
        return {};
    }

    QSize size = hasColor ? colorSize : maskSize;
    if (!hasColor) {
        size.setHeight(size.height() / 2);
    }
    if (size.width() <= 0 || size.height() <= 0
        || size.width() > kCursorImageMaxDimension
        || size.height() > kCursorImageMaxDimension) {
        return {};
    }

    QBitArray maskBits;
    if (hasMask) {
        maskBits = monochromeBitmapBits(
            icon.hbmMask,
            size.width(),
            size.height());
    }

    QImage image;
    if (hasColor && colorSize == size) {
        image = colorBitmapImage(icon.hbmColor, size);
        if (image.isNull()) {
            return {};
        }

        bool hasAlpha = false;
        for (int y = 0; y < size.height() && !hasAlpha; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(
                image.constScanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                if (qAlpha(row[x]) != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }
        if (hasAlpha) {
            return image;
        }
        if (maskBits.isEmpty()) {
            return {};
        }
        for (int y = 0; y < size.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                if (!maskBits.testBit(y * size.width() + x)) {
                    row[x] |= 0xFF000000U;
                }
            }
        }
        return image;
    }

    if (maskBits.isEmpty() || maskSize.width() != size.width()
        || maskSize.height() < size.height() * 2) {
        return {};
    }
    const QBitArray xorBits = monochromeBitmapBits(
        icon.hbmMask,
        size.width(),
        size.height() * 2);
    if (xorBits.isEmpty()) {
        return {};
    }

    image = QImage(size, QImage::Format_ARGB32);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);
    for (int y = 0; y < size.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int index = y * size.width() + x;
            const bool andBit = maskBits.testBit(index);
            const bool xorBit = xorBits.testBit(
                (size.height() + y) * size.width() + x);
            if (andBit && !xorBit) {
                continue;
            }
            row[x] = xorBit ? qRgba(255, 255, 255, 255)
                            : qRgba(0, 0, 0, 255);
        }
    }
    return image;
}

} // namespace

struct WindowsScreenSource::Impl {
    HDC screenDc = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void *bits = nullptr;
    int width = 0;
    int height = 0;
    mutable QList<MonitorInfo> monitors;
    QHash<quintptr, QImage> cursorImages;
    QList<quintptr> cursorImageOrder;

    ~Impl() { close(); }

    void close()
    {
        if (memoryDc != nullptr && oldBitmap != nullptr) {
            SelectObject(memoryDc, oldBitmap);
        }
        oldBitmap = nullptr;
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        bits = nullptr;
        width = 0;
        height = 0;
        if (memoryDc != nullptr) {
            DeleteDC(memoryDc);
            memoryDc = nullptr;
        }
        if (screenDc != nullptr) {
            ReleaseDC(nullptr, screenDc);
            screenDc = nullptr;
        }
        cursorImages.clear();
        cursorImageOrder.clear();
    }

    bool ensureDevice(QString *error)
    {
        if (screenDc != nullptr && memoryDc != nullptr) {
            return true;
        }
        screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            if (error) {
                *error = QStringLiteral("GetDC 失败：%1")
                             .arg(win32Error(GetLastError()));
            }
            return false;
        }
        memoryDc = CreateCompatibleDC(screenDc);
        if (memoryDc == nullptr) {
            if (error) {
                *error = QStringLiteral("CreateCompatibleDC 失败：%1")
                             .arg(win32Error(GetLastError()));
            }
            close();
            return false;
        }
        return true;
    }

    bool ensureBitmap(int requestedWidth, int requestedHeight, QString *error)
    {
        if (bitmap != nullptr && width == requestedWidth
            && height == requestedHeight) {
            return true;
        }
        if (memoryDc != nullptr && oldBitmap != nullptr) {
            SelectObject(memoryDc, oldBitmap);
            oldBitmap = nullptr;
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        bits = nullptr;
        width = 0;
        height = 0;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requestedWidth;
        info.bmiHeader.biHeight = -requestedHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        bitmap = CreateDIBSection(
            screenDc,
            &info,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0);
        if (bitmap == nullptr || bits == nullptr) {
            if (error) {
                *error = QStringLiteral("CreateDIBSection 失败：%1")
                             .arg(win32Error(GetLastError()));
            }
            close();
            return false;
        }
        oldBitmap = SelectObject(memoryDc, bitmap);
        if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR) {
            oldBitmap = nullptr;
            if (error) {
                *error = QStringLiteral("SelectObject 失败：%1")
                             .arg(win32Error(GetLastError()));
            }
            close();
            return false;
        }
        width = requestedWidth;
        height = requestedHeight;
        return true;
    }
};

WindowsScreenSource::WindowsScreenSource()
    : impl_(std::make_unique<Impl>())
{
}

WindowsScreenSource::~WindowsScreenSource() = default;

QString WindowsScreenSource::backend() const
{
    return QStringLiteral("gdi");
}

QList<MonitorInfo> WindowsScreenSource::monitors() const
{
    QList<MonitorInfo> found;
    if (!EnumDisplayMonitors(
            nullptr,
            nullptr,
            collectMonitor,
            reinterpret_cast<LPARAM>(&found))) {
        return impl_->monitors;
    }
    std::sort(
        found.begin(),
        found.end(),
        [](const MonitorInfo &left, const MonitorInfo &right) {
            if (left.left != right.left) {
                return left.left < right.left;
            }
            return left.top < right.top;
        });
    for (int index = 0; index < found.size(); ++index) {
        found[index].index = index;
    }
    impl_->monitors = found;
    return found;
}

CaptureResult WindowsScreenSource::capture(const CaptureRequest &request)
{
    CaptureResult result;
    if (impl_->monitors.isEmpty()) {
        (void)monitors();
    }
    if (impl_->monitors.isEmpty()) {
        result.error = QStringLiteral("没有检测到显示器");
        return result;
    }

    const int monitorIndex = std::clamp(
        request.monitor,
        0,
        static_cast<int>(impl_->monitors.size()) - 1);
    const MonitorInfo monitor = impl_->monitors.at(monitorIndex);
    if (monitor.width <= 0 || monitor.height <= 0) {
        result.error = QStringLiteral("显示器尺寸无效");
        return result;
    }

    int outputWidth = monitor.width;
    if (request.maxWidth > 0 && outputWidth > request.maxWidth) {
        outputWidth = std::max(2, request.maxWidth);
    }
    const int outputHeight = outputWidth == monitor.width
        ? monitor.height
        : std::max(
              2,
              static_cast<int>(std::lround(
                  static_cast<double>(monitor.height) * outputWidth
                  / monitor.width)));

    if (!impl_->ensureDevice(&result.error)
        || !impl_->ensureBitmap(outputWidth, outputHeight, &result.error)) {
        return result;
    }

    BOOL copied = FALSE;
    if (outputWidth == monitor.width && outputHeight == monitor.height) {
        copied = BitBlt(
            impl_->memoryDc,
            0,
            0,
            outputWidth,
            outputHeight,
            impl_->screenDc,
            monitor.left,
            monitor.top,
            SRCCOPY | CAPTUREBLT);
        if (!copied) {
            copied = BitBlt(
                impl_->memoryDc,
                0,
                0,
                outputWidth,
                outputHeight,
                impl_->screenDc,
                monitor.left,
                monitor.top,
                SRCCOPY);
        }
    } else {
        SetStretchBltMode(impl_->memoryDc, HALFTONE);
        SetBrushOrgEx(impl_->memoryDc, 0, 0, nullptr);
        copied = StretchBlt(
            impl_->memoryDc,
            0,
            0,
            outputWidth,
            outputHeight,
            impl_->screenDc,
            monitor.left,
            monitor.top,
            monitor.width,
            monitor.height,
            SRCCOPY | CAPTUREBLT);
    }
    if (!copied) {
        result.error = QStringLiteral("抓屏复制失败：%1")
                           .arg(win32Error(GetLastError()));
        return result;
    }
    GdiFlush();

    result.image = QImage(
                       static_cast<const uchar *>(impl_->bits),
                       outputWidth,
                       outputHeight,
                       outputWidth * 4,
                       QImage::Format_RGB32)
                       .copy();
    if (result.image.isNull()) {
        result.error = QStringLiteral("无法创建抓屏图像");
    }
    return result;
}

CursorSample WindowsScreenSource::cursorSample() const
{
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || info.hCursor == nullptr
        || (info.flags & CURSOR_SHOWING) == 0) {
        return {};
    }

    CursorSample sample;
    sample.visible = true;
    sample.screenPosition = QPoint(
        info.ptScreenPos.x,
        info.ptScreenPos.y);

    IconInfoData icon;
    const bool hasIcon = GetIconInfo(info.hCursor, &icon.value);
    if (hasIcon) {
        sample.hotspot = QPoint(
            static_cast<int>(icon.value.xHotspot),
            static_cast<int>(icon.value.yHotspot));
    }

    const std::optional<CursorShape> standard =
        standardCursorShape(info.hCursor);
    if (standard.has_value()) {
        sample.shape = *standard;
        return sample;
    }

    sample.shape = CursorShape::Arrow;
    if (!hasIcon) {
        return sample;
    }

    const quintptr key =
        reinterpret_cast<quintptr>(info.hCursor);
    QImage image = impl_->cursorImages.value(key);
    if (!image.isNull()) {
        impl_->cursorImageOrder.removeOne(key);
    } else {
        image = cursorImageFromIcon(icon.value);
        if (image.isNull()) {
            return sample;
        }
        if (impl_->cursorImageOrder.size() >= kCursorImageCacheLimit) {
            const quintptr oldest = impl_->cursorImageOrder.takeFirst();
            impl_->cursorImages.remove(oldest);
        }
        impl_->cursorImages.insert(key, image);
    }
    impl_->cursorImageOrder.append(key);
    sample.shape = CursorShape::Bitmap;
    sample.image = std::move(image);
    return sample;
}

void WindowsScreenSource::close()
{
    impl_->close();
}

} // namespace pd
