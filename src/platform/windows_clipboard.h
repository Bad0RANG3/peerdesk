#pragma once

#include "session/media_sources.h"

namespace pd {

class WindowsClipboard final : public IClipboard {
public:
    [[nodiscard]] QString text() const override;
    bool setText(const QString &text) override;
};

} // namespace pd
