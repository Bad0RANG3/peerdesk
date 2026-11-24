#include "platform/windows_screen_source.h"

#include <Windows.h>

#include <QImage>
#include <QString>

#include <algorithm>
#include <cmath>

namespace pd {

namespace {

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

CursorShape standardCursorShape(HCURSOR cursor)
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
    return CursorShape::Arrow;
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
    sample.shape = standardCursorShape(info.hCursor);

    ICONINFO icon{};
    if (GetIconInfo(info.hCursor, &icon)) {
        sample.hotspot = QPoint(
            static_cast<int>(icon.xHotspot),
            static_cast<int>(icon.yHotspot));
        if (icon.hbmMask != nullptr) {
            DeleteObject(icon.hbmMask);
        }
        if (icon.hbmColor != nullptr) {
            DeleteObject(icon.hbmColor);
        }
    }
    return sample;
}

void WindowsScreenSource::close()
{
    impl_->close();
}

} // namespace pd
