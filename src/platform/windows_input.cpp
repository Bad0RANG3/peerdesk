#include "platform/windows_input.h"

#include <Windows.h>

#include <QHash>
#include <QRect>

#include <algorithm>
#include <array>
#include <cmath>

namespace pd {

namespace {

constexpr DWORD kMouseMove = 0x0001;
constexpr DWORD kMouseLeftDown = 0x0002;
constexpr DWORD kMouseLeftUp = 0x0004;
constexpr DWORD kMouseRightDown = 0x0008;
constexpr DWORD kMouseRightUp = 0x0010;
constexpr DWORD kMouseMiddleDown = 0x0020;
constexpr DWORD kMouseMiddleUp = 0x0040;
constexpr DWORD kMouseXDown = 0x0080;
constexpr DWORD kMouseXUp = 0x0100;
constexpr DWORD kMouseWheel = 0x0800;
constexpr DWORD kMouseHWheel = 0x1000;
constexpr DWORD kMouseAbsolute = 0x8000;
constexpr DWORD kMouseVirtualDesk = 0x4000;
constexpr DWORD kKeyExtended = 0x0001;
constexpr DWORD kKeyUp = 0x0002;
constexpr DWORD kKeyUnicode = 0x0004;
constexpr DWORD kXButton1 = 0x0001;
constexpr DWORD kXButton2 = 0x0002;

const QHash<QString, quint16> &keyMap()
{
    static const QHash<QString, quint16> map = [] {
        QHash<QString, quint16> keys{
            {QStringLiteral("Backspace"), 0x08},
            {QStringLiteral("Tab"), 0x09},
            {QStringLiteral("Enter"), 0x0D},
            {QStringLiteral("NumpadEnter"), 0x0D},
            {QStringLiteral("ShiftLeft"), 0xA0},
            {QStringLiteral("ShiftRight"), 0xA1},
            {QStringLiteral("ControlLeft"), 0xA2},
            {QStringLiteral("ControlRight"), 0xA3},
            {QStringLiteral("AltLeft"), 0xA4},
            {QStringLiteral("AltRight"), 0xA5},
            {QStringLiteral("MetaLeft"), 0x5B},
            {QStringLiteral("MetaRight"), 0x5C},
            {QStringLiteral("CapsLock"), 0x14},
            {QStringLiteral("Escape"), 0x1B},
            {QStringLiteral("Space"), 0x20},
            {QStringLiteral("PageUp"), 0x21},
            {QStringLiteral("PageDown"), 0x22},
            {QStringLiteral("End"), 0x23},
            {QStringLiteral("Home"), 0x24},
            {QStringLiteral("ArrowLeft"), 0x25},
            {QStringLiteral("ArrowUp"), 0x26},
            {QStringLiteral("ArrowRight"), 0x27},
            {QStringLiteral("ArrowDown"), 0x28},
            {QStringLiteral("PrintScreen"), 0x2C},
            {QStringLiteral("Insert"), 0x2D},
            {QStringLiteral("Delete"), 0x2E},
            {QStringLiteral("ContextMenu"), 0x5D},
            {QStringLiteral("NumLock"), 0x90},
            {QStringLiteral("ScrollLock"), 0x91},
            {QStringLiteral("Pause"), 0x13},
            {QStringLiteral("Semicolon"), 0xBA},
            {QStringLiteral("Equal"), 0xBB},
            {QStringLiteral("Comma"), 0xBC},
            {QStringLiteral("Minus"), 0xBD},
            {QStringLiteral("Period"), 0xBE},
            {QStringLiteral("Slash"), 0xBF},
            {QStringLiteral("Backquote"), 0xC0},
            {QStringLiteral("BracketLeft"), 0xDB},
            {QStringLiteral("Backslash"), 0xDC},
            {QStringLiteral("BracketRight"), 0xDD},
            {QStringLiteral("Quote"), 0xDE},
            {QStringLiteral("NumpadMultiply"), 0x6A},
            {QStringLiteral("NumpadAdd"), 0x6B},
            {QStringLiteral("NumpadSubtract"), 0x6D},
            {QStringLiteral("NumpadDecimal"), 0x6E},
            {QStringLiteral("NumpadDivide"), 0x6F},
        };
        for (int index = 0; index < 10; ++index) {
            keys.insert(
                QStringLiteral("Digit%1").arg(index),
                static_cast<quint16>(0x30 + index));
            keys.insert(
                QStringLiteral("Numpad%1").arg(index),
                static_cast<quint16>(0x60 + index));
        }
        for (int index = 0; index < 26; ++index) {
            keys.insert(
                QStringLiteral("Key%1").arg(QChar('A' + index)),
                static_cast<quint16>(0x41 + index));
        }
        for (int index = 1; index <= 24; ++index) {
            keys.insert(
                QStringLiteral("F%1").arg(index),
                static_cast<quint16>(0x6F + index));
        }
        return keys;
    }();
    return map;
}

const QSet<quint16> &extendedKeys()
{
    static const QSet<quint16> keys{
        0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x23, 0x24,
        0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6A, 0x6F, 0x2C,
        0x90, 0xA3, 0xA5,
    };
    return keys;
}

INPUT mouseInput(DWORD x, DWORD y, DWORD data, DWORD flags)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(x);
    input.mi.dy = static_cast<LONG>(y);
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    return input;
}

INPUT keyInput(
    quint16 virtualKey,
    quint16 scanCode,
    DWORD flags,
    bool extended)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = flags | (extended ? kKeyExtended : 0);
    return input;
}

void send(std::array<INPUT, 1> inputs)
{
    SendInput(
        static_cast<UINT>(inputs.size()),
        inputs.data(),
        sizeof(INPUT));
}

void sendKeyEvent(quint16 virtualKey, bool down)
{
    const UINT scan = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC) & 0xFFU;
    send({keyInput(
        virtualKey,
        static_cast<quint16>(scan),
        down ? 0U : kKeyUp,
        windowsIsExtendedKey(virtualKey))});
}

struct ButtonFlags {
    DWORD down = kMouseLeftDown;
    DWORD up = kMouseLeftUp;
    DWORD data = 0;
};

ButtonFlags buttonFlags(const QString &button)
{
    if (button == QStringLiteral("right")) {
        return {kMouseRightDown, kMouseRightUp, 0};
    }
    if (button == QStringLiteral("middle")) {
        return {kMouseMiddleDown, kMouseMiddleUp, 0};
    }
    if (button == QStringLiteral("back")) {
        return {kMouseXDown, kMouseXUp, kXButton1};
    }
    if (button == QStringLiteral("forward")) {
        return {kMouseXDown, kMouseXUp, kXButton2};
    }
    return {};
}

} // namespace

std::optional<quint16> windowsVirtualKeyForCode(const QString &code)
{
    const auto found = keyMap().constFind(code);
    if (found != keyMap().cend()) {
        return found.value();
    }
    if (code.size() == 1) {
        const SHORT translated = VkKeyScanW(code.at(0).unicode());
        if (translated != -1) {
            return static_cast<quint16>(translated & 0xFF);
        }
    }
    return std::nullopt;
}

bool windowsIsExtendedKey(quint16 virtualKey)
{
    return extendedKeys().contains(virtualKey);
}

std::optional<WindowsKeyStroke> windowsKeyStrokeForCode(
    const QString &code)
{
    const std::optional<quint16> virtualKey =
        windowsVirtualKeyForCode(code);
    if (!virtualKey.has_value()) {
        return std::nullopt;
    }
    WindowsKeyStroke stroke;
    stroke.virtualKey = *virtualKey;
    stroke.scanCode = static_cast<quint16>(
        MapVirtualKeyW(stroke.virtualKey, MAPVK_VK_TO_VSC) & 0xFFU);
    stroke.extended = code == QStringLiteral("NumpadEnter")
        || windowsIsExtendedKey(stroke.virtualKey);
    return stroke;
}

QRect WindowsInputSink::virtualDesktop() const
{
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
        std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)),
    };
}

void WindowsInputSink::moveAbsolute(double x, double y)
{
    const DWORD absoluteX = static_cast<DWORD>(std::lround(
        std::clamp(x, 0.0, 1.0) * 65535.0));
    const DWORD absoluteY = static_cast<DWORD>(std::lround(
        std::clamp(y, 0.0, 1.0) * 65535.0));
    send({mouseInput(
        absoluteX,
        absoluteY,
        0,
        kMouseMove | kMouseAbsolute | kMouseVirtualDesk)});
}

void WindowsInputSink::moveRelative(int dx, int dy)
{
    send({mouseInput(
        static_cast<DWORD>(dx),
        static_cast<DWORD>(dy),
        0,
        kMouseMove)});
}

void WindowsInputSink::mouseButton(const QString &button, bool down)
{
    const ButtonFlags flags = buttonFlags(button);
    if (down) {
        pressedButtons_.insert(button);
    } else {
        pressedButtons_.remove(button);
    }
    send({mouseInput(0, 0, flags.data, down ? flags.down : flags.up)});
}

void WindowsInputSink::click(const QString &button)
{
    mouseButton(button, true);
    mouseButton(button, false);
}

void WindowsInputSink::wheel(int delta, bool horizontal)
{
    send({mouseInput(
        0,
        0,
        static_cast<DWORD>(static_cast<qint32>(delta)),
        horizontal ? kMouseHWheel : kMouseWheel)});
}

void WindowsInputSink::key(const QString &code, bool down)
{
    const std::optional<WindowsKeyStroke> stroke =
        windowsKeyStrokeForCode(code);
    if (!stroke.has_value()) {
        return;
    }
    if (down) {
        pressedKeyCodes_.insert(code);
    } else {
        pressedKeyCodes_.remove(code);
    }
    send({
        keyInput(
            stroke->virtualKey,
            stroke->scanCode,
            down ? 0U : kKeyUp,
            stroke->extended),
    });
}

void WindowsInputSink::typeText(const QString &text)
{
    for (const QChar character : text) {
        const quint16 codeUnit = character.unicode();
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = codeUnit;
        down.ki.dwFlags = kKeyUnicode;

        INPUT up = down;
        up.ki.dwFlags = kKeyUnicode | kKeyUp;
        send({down});
        send({up});
    }
}

void WindowsInputSink::ctrlAltDelete()
{
    sendKeyEvent(0xA2, true);
    sendKeyEvent(0xA4, true);
    sendKeyEvent(0x2E, true);
    sendKeyEvent(0x2E, false);
    sendKeyEvent(0xA4, false);
    sendKeyEvent(0xA2, false);
}

void WindowsInputSink::releaseAll()
{
    const QSet<QString> buttons = pressedButtons_;
    for (const QString &button : buttons) {
        mouseButton(button, false);
    }
    pressedButtons_.clear();

    const QSet<QString> keyCodes = pressedKeyCodes_;
    for (const QString &code : keyCodes) {
        const std::optional<WindowsKeyStroke> stroke =
            windowsKeyStrokeForCode(code);
        if (!stroke.has_value()) {
            continue;
        }
        send({
            keyInput(
                stroke->virtualKey,
                stroke->scanCode,
                kKeyUp,
                stroke->extended),
        });
    }
    pressedKeyCodes_.clear();
}

} // namespace pd
