#pragma once

namespace pd {

inline constexpr int kAdaptiveMaxQuality = 92;
inline constexpr int kAdaptiveMinQuality = 32;
inline constexpr int kAdaptiveMaxFps = 120;
inline constexpr int kAdaptiveMinFps = 6;

enum class AdaptiveLinkHealth {
    Unknown,
    Healthy,
    Guarded,
    Congested,
};

struct AdaptiveVideoFeedback {
    double timestampSeconds = 0.0;
    double rttMs = 0.0;
    int nackFrames = 0;
    int nackShards = 0;
    int lossKeyframeRequests = 0;
};

struct AdaptiveVideoLimits {
    int quality = kAdaptiveMaxQuality;
    int fps = kAdaptiveMaxFps;
    AdaptiveLinkHealth health = AdaptiveLinkHealth::Unknown;
};

class AdaptiveVideoRateController {
public:
    void reset(
        int qualityCeiling = kAdaptiveMaxQuality,
        int fpsCeiling = kAdaptiveMaxFps);
    void setCeilings(int qualityCeiling, int fpsCeiling);

    AdaptiveVideoLimits observe(const AdaptiveVideoFeedback &feedback);

    [[nodiscard]] int qualityLimit() const { return qualityLimit_; }
    [[nodiscard]] int fpsLimit() const { return fpsLimit_; }
    [[nodiscard]] int qualityCeiling() const { return qualityCeiling_; }
    [[nodiscard]] int fpsCeiling() const { return fpsCeiling_; }
    [[nodiscard]] AdaptiveLinkHealth health() const { return health_; }

private:
    [[nodiscard]] int fpsFloor() const;
    void clampToCeilings();

    int qualityCeiling_ = kAdaptiveMaxQuality;
    int fpsCeiling_ = kAdaptiveMaxFps;
    int qualityLimit_ = kAdaptiveMaxQuality;
    int fpsLimit_ = kAdaptiveMaxFps;
    AdaptiveLinkHealth health_ = AdaptiveLinkHealth::Unknown;
    double lastAdjustmentAt_ = -1.0e12;
    double healthySince_ = -1.0;
};

[[nodiscard]] const char *adaptiveLinkHealthKey(AdaptiveLinkHealth health);

} // namespace pd
