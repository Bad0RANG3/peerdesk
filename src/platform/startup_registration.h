#pragma once

#include <QString>

namespace pd {

class StartupRegistration {
public:
    virtual ~StartupRegistration() = default;

    [[nodiscard]] virtual bool isSupported() const
    {
        return true;
    }
    [[nodiscard]] virtual bool isEnabled() const = 0;
    [[nodiscard]] virtual QString registeredCommand() const = 0;
    virtual bool setEnabled(bool enabled, QString *error = nullptr) = 0;
};

} // namespace pd
