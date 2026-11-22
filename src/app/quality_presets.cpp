#include "app/quality_presets.h"

#include <algorithm>
#include <limits>

namespace pd {
namespace {

const QList<QualityPreset> kPresets{
    {
        QStringLiteral("fluent"),
        QStringLiteral("流畅"),
        1280,
        45,
        24,
    },
    {
        QStringLiteral("balanced"),
        QStringLiteral("均衡"),
        1600,
        62,
        30,
    },
    {
        QStringLiteral("hd"),
        QStringLiteral("高清"),
        1920,
        72,
        40,
    },
    {
        QStringLiteral("uhd"),
        QStringLiteral("超清"),
        2560,
        80,
        50,
    },
    {
        QStringLiteral("native"),
        QStringLiteral("原画"),
        3840,
        88,
        60,
    },
};

} // namespace

const QList<QualityPreset> &qualityPresets()
{
    return kPresets;
}

const QualityPreset &qualityPreset(const QString &id)
{
    for (const QualityPreset &preset : kPresets) {
        if (preset.id == id) {
            return preset;
        }
    }
    return kPresets.at(1);
}

QString qualityIdFor(int maxWidth, int fps, int quality)
{
    const QualityPreset *best = &kPresets.at(1);
    int bestDistance = std::numeric_limits<int>::max();
    for (const QualityPreset &preset : kPresets) {
        const int distance =
            std::abs(preset.maxWidth - maxWidth) * 4
            + std::abs(preset.fps - fps) * 40
            + std::abs(preset.quality - quality) * 30;
        if (distance < bestDistance) {
            best = &preset;
            bestDistance = distance;
        }
    }
    return best->id;
}

} // namespace pd
