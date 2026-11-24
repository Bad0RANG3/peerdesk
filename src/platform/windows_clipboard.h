#pragma once

#include "session/media_sources.h"

namespace pd {

class WindowsClipboard final : public IClipboard {
public:
    [[nodiscard]] QString text() const override;
    [[nodiscard]] QImage image() const override;
    bool setText(const QString &text) override;
    bool setImage(const QImage &image) override;
};

} // namespace pd
