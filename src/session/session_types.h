#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace pd {

struct MonitorInfo {
    int index = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
    QString name;

    [[nodiscard]] QJsonObject toJson() const;
};

struct VideoSettings {
    int fps = 30;
    int maxWidth = 1920;
    int quality = 65;
    int monitor = 0;
    bool skipStatic = true;
    bool clipboard = true;
    bool audio = true;
    QString pixelFormat = QStringLiteral("jpeg");

    [[nodiscard]] QJsonObject toJson() const;
    void update(const QJsonObject &object);
    void clamp();
};

class FramePacer {
public:
    explicit FramePacer(
        int quality = 65,
        int targetKb = 140,
        int minQuality = 32,
        int maxQuality = 92);

    void observe(qint64 size, double elapsedMs);
    void setBudgetKb(double kb);
    void setQuality(int quality);

    [[nodiscard]] int quality() const { return quality_; }
    [[nodiscard]] int targetKb() const { return targetKb_; }
    [[nodiscard]] qint64 lastSize() const { return lastSize_; }
    [[nodiscard]] double lastMs() const { return lastMs_; }

private:
    int quality_;
    int targetKb_;
    int minQuality_;
    int maxQuality_;
    qint64 lastSize_ = 0;
    double lastMs_ = 0.0;
};

[[nodiscard]] QString defaultReceiveDirectory();

} // namespace pd
