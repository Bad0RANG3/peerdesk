#pragma once

#include "session/media_sources.h"

#include <memory>

namespace pd {

class WindowsScreenSource final : public IScreenSource {
public:
    WindowsScreenSource();
    ~WindowsScreenSource() override;

    [[nodiscard]] QString backend() const override;
    [[nodiscard]] QList<MonitorInfo> monitors() const override;
    CaptureResult capture(const CaptureRequest &request) override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pd
