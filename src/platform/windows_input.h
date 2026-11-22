#pragma once

#include "session/media_sources.h"

#include <QSet>
#include <QString>

#include <optional>

namespace pd {

struct WindowsKeyStroke {
    quint16 virtualKey = 0;
    quint16 scanCode = 0;
    bool extended = false;
};

[[nodiscard]] std::optional<quint16> windowsVirtualKeyForCode(
    const QString &code);
[[nodiscard]] bool windowsIsExtendedKey(quint16 virtualKey);
[[nodiscard]] std::optional<WindowsKeyStroke> windowsKeyStrokeForCode(
    const QString &code);

class WindowsInputSink final : public IInputSink {
public:
    WindowsInputSink() = default;

    [[nodiscard]] QRect virtualDesktop() const override;
    void moveAbsolute(double x, double y) override;
    void moveRelative(int dx, int dy) override;
    void mouseButton(const QString &button, bool down) override;
    void click(const QString &button) override;
    void wheel(int delta, bool horizontal) override;
    void key(const QString &code, bool down) override;
    void typeText(const QString &text) override;
    void ctrlAltDelete() override;
    void releaseAll() override;

private:
    QSet<QString> pressedKeyCodes_;
    QSet<QString> pressedButtons_;
};

} // namespace pd
