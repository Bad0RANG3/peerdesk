#include "session/adaptive_video_rate_controller.h"

#include <algorithm>

namespace pd {
namespace {

inline constexpr double kMinimumAdjustmentIntervalSeconds = 0.9;
inline constexpr double kHealthyRecoveryDelaySeconds = 4.0;
inline constexpr int kModerateQualityStep = 4;
inline constexpr int kModerateFpsStep = 3;
inline constexpr int kSevereQualityStep = 8;
inline constexpr int kSevereFpsStep = 6;
inline constexpr int kRecoveryQualityStep = 2;
inline constexpr int kRecoveryFpsStep = 2;

enum class FeedbackSeverity {
    Healthy,
    Guarded,
    Moderate,
    Severe,
    Unknown,
};

FeedbackSeverity classify(const AdaptiveVideoFeedback &feedback)
{
    const int nackFrames = std::max(0, feedback.nackFrames);
    const int nackShards = std::max(0, feedback.nackShards);
    const int keyframeRequests =
        std::max(0, feedback.lossKeyframeRequests);
    const double rttMs = std::max(0.0, feedback.rttMs);

    if (nackFrames >= 2 || nackShards >= 10
        || keyframeRequests >= 2 || rttMs >= 240.0) {
        return FeedbackSeverity::Severe;
    }
    if (nackFrames >= 1 || nackShards >= 4
        || keyframeRequests >= 1 || rttMs >= 150.0) {
        return FeedbackSeverity::Moderate;
    }
    if (nackShards > 0 || rttMs >= 100.0) {
        return FeedbackSeverity::Guarded;
    }
    if (rttMs > 0.0) {
        return FeedbackSeverity::Healthy;
    }
    return FeedbackSeverity::Unknown;
}

AdaptiveLinkHealth healthFor(FeedbackSeverity severity)
{
    switch (severity) {
    case FeedbackSeverity::Healthy:
        return AdaptiveLinkHealth::Healthy;
    case FeedbackSeverity::Guarded:
        return AdaptiveLinkHealth::Guarded;
    case FeedbackSeverity::Moderate:
    case FeedbackSeverity::Severe:
        return AdaptiveLinkHealth::Congested;
    case FeedbackSeverity::Unknown:
        break;
    }
    return AdaptiveLinkHealth::Unknown;
}

} // namespace

void AdaptiveVideoRateController::reset(
    int qualityCeiling,
    int fpsCeiling)
{
    qualityCeiling_ = std::clamp(
        qualityCeiling,
        kAdaptiveMinQuality,
        kAdaptiveMaxQuality);
    fpsCeiling_ = std::clamp(
        fpsCeiling,
        1,
        kAdaptiveMaxFps);
    qualityLimit_ = qualityCeiling_;
    fpsLimit_ = fpsCeiling_;
    health_ = AdaptiveLinkHealth::Unknown;
    lastAdjustmentAt_ = -1.0e12;
    healthySince_ = -1.0;
}

void AdaptiveVideoRateController::setCeilings(
    int qualityCeiling,
    int fpsCeiling)
{
    qualityCeiling_ = std::clamp(
        qualityCeiling,
        kAdaptiveMinQuality,
        kAdaptiveMaxQuality);
    fpsCeiling_ = std::clamp(
        fpsCeiling,
        1,
        kAdaptiveMaxFps);
    clampToCeilings();
}

AdaptiveVideoLimits AdaptiveVideoRateController::observe(
    const AdaptiveVideoFeedback &feedback)
{
    const FeedbackSeverity severity = classify(feedback);
    health_ = healthFor(severity);
    const double now = feedback.timestampSeconds;
    const bool adjustmentAllowed =
        now + 1.0e-9
        >= lastAdjustmentAt_ + kMinimumAdjustmentIntervalSeconds;

    if ((severity == FeedbackSeverity::Moderate
         || severity == FeedbackSeverity::Severe)
        && adjustmentAllowed) {
        const bool severe = severity == FeedbackSeverity::Severe;
        const int previousQuality = qualityLimit_;
        const int previousFps = fpsLimit_;
        qualityLimit_ = std::max(
            kAdaptiveMinQuality,
            qualityLimit_
                - (severe ? kSevereQualityStep
                          : kModerateQualityStep));
        fpsLimit_ = std::max(
            fpsFloor(),
            fpsLimit_
                - (severe ? kSevereFpsStep
                          : kModerateFpsStep));
        clampToCeilings();
        if (qualityLimit_ != previousQuality
            || fpsLimit_ != previousFps) {
            lastAdjustmentAt_ = now;
            healthySince_ = -1.0;
        }
    } else if (severity == FeedbackSeverity::Healthy) {
        if (healthySince_ < 0.0) {
            healthySince_ = now;
        }
        if (now - healthySince_ >= kHealthyRecoveryDelaySeconds
            && now + 1.0e-9
                >= lastAdjustmentAt_
                    + kHealthyRecoveryDelaySeconds) {
            const int previousQuality = qualityLimit_;
            const int previousFps = fpsLimit_;
            qualityLimit_ = std::min(
                qualityCeiling_,
                qualityLimit_ + kRecoveryQualityStep);
            fpsLimit_ = std::min(
                fpsCeiling_,
                fpsLimit_ + kRecoveryFpsStep);
            if (qualityLimit_ != previousQuality
                || fpsLimit_ != previousFps) {
                lastAdjustmentAt_ = now;
                healthySince_ = now;
            }
        }
    } else {
        healthySince_ = -1.0;
    }

    return AdaptiveVideoLimits{
        qualityLimit_,
        fpsLimit_,
        health_,
    };
}

int AdaptiveVideoRateController::fpsFloor() const
{
    return std::min(kAdaptiveMinFps, fpsCeiling_);
}

void AdaptiveVideoRateController::clampToCeilings()
{
    qualityLimit_ = std::clamp(
        qualityLimit_,
        kAdaptiveMinQuality,
        qualityCeiling_);
    fpsLimit_ = std::clamp(
        fpsLimit_,
        1,
        fpsCeiling_);
}

const char *adaptiveLinkHealthKey(AdaptiveLinkHealth health)
{
    switch (health) {
    case AdaptiveLinkHealth::Healthy:
        return "healthy";
    case AdaptiveLinkHealth::Guarded:
        return "guarded";
    case AdaptiveLinkHealth::Congested:
        return "congested";
    case AdaptiveLinkHealth::Unknown:
        break;
    }
    return "unknown";
}

} // namespace pd
