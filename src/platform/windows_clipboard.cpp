#include "platform/windows_clipboard.h"

#include <Windows.h>

#include <QString>

#include <algorithm>
#include <limits>

namespace pd {

namespace {

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

} // namespace pd
