#pragma once

#include <QList>
#include <QString>

namespace pd {

struct QualityPreset {
    QString id;
    QString name;
    int maxWidth = 1600;
    int quality = 62;
    int fps = 30;
};

[[nodiscard]] const QList<QualityPreset> &qualityPresets();
[[nodiscard]] const QualityPreset &qualityPreset(const QString &id);
[[nodiscard]] QString qualityIdFor(int maxWidth, int fps, int quality);

} // namespace pd
