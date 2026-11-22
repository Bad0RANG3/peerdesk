#pragma once

#include "core/protocol.h"

#include <QByteArray>
#include <QElapsedTimer>

#include <optional>

namespace pd {

inline constexpr int kAudioRate = 16000;
inline constexpr int kAudioFrameMs = 20;
inline constexpr int kAudioSamples = kAudioRate * kAudioFrameMs / 1000;
inline constexpr int kAudioPcmBytes = kAudioSamples * 2;
inline constexpr int kSilenceHoldFrames = 25;
inline constexpr int kSilenceKeepFrames = 25;

[[nodiscard]] QByteArray ulawEncode(const QByteArray &pcm16);
[[nodiscard]] QByteArray ulawDecode(const QByteArray &ulaw);
[[nodiscard]] QByteArray silentUlawFrame();
[[nodiscard]] double pcm16Rms(const QByteArray &pcm16);

class AudioEncoder {
public:
    AudioEncoder();

    // pcm16 must be exactly one 20 ms frame (640 bytes) of 16 kHz mono PCM.
    [[nodiscard]] std::optional<AudioPacket> next(const QByteArray &pcm16);
    void reset();

    [[nodiscard]] quint32 sequence() const { return sequence_; }

private:
    QElapsedTimer clock_;
    quint32 sequence_ = 0;
    QByteArray previous_;
    int silentRun_ = 0;
};

} // namespace pd
