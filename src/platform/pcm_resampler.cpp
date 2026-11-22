#include "platform/pcm_resampler.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pd {

namespace {

constexpr double kFloatScale = 32767.0;

} // namespace

PcmMonoResampler::PcmMonoResampler(
    int inputRate,
    int channels,
    SampleFormat format,
    int outputRate)
    : inputRate_(std::max(1, inputRate))
    , outputRate_(std::max(1, outputRate))
    , channels_(std::max(1, channels))
    , format_(format)
    , step_(static_cast<double>(inputRate_) / outputRate_)
{
}

void PcmMonoResampler::reset()
{
    input_.clear();
    position_ = 0.0;
}

float PcmMonoResampler::sampleAt(const char *data, qsizetype index) const
{
    switch (format_) {
    case SampleFormat::Float32: {
        quint32 bits = 0;
        std::memcpy(&bits, data + index * 4, sizeof(bits));
        const quint32 hostBits = qFromLittleEndian<quint32>(bits);
        float value = 0.0F;
        std::memcpy(&value, &hostBits, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    case SampleFormat::Int16: {
        qint16 value = 0;
        std::memcpy(&value, data + index * 2, sizeof(value));
        return static_cast<float>(qFromLittleEndian<qint16>(value));
    }
    }
    return 0.0F;
}

QByteArray PcmMonoResampler::feed(const void *data, qsizetype bytes)
{
    if (data == nullptr || bytes <= 0) {
        return {};
    }

    const int bytesPerSample =
        format_ == SampleFormat::Float32 ? 4 : 2;
    const qsizetype values =
        bytes / (bytesPerSample * channels_);
    if (values <= 0) {
        return {};
    }

    const char *bytesData = static_cast<const char *>(data);
    input_.reserve(input_.size() + values);
    for (qsizetype frame = 0; frame < values; ++frame) {
        double total = 0.0;
        const qsizetype base = frame * channels_;
        for (int channel = 0; channel < channels_; ++channel) {
            total += sampleAt(bytesData, base + channel);
        }
        input_.append(static_cast<float>(total / channels_));
    }

    QByteArray output;
    output.reserve(
        static_cast<qsizetype>(std::ceil(values / step_)) * 2 + 4);

    double position = position_;
    const qsizetype limit = input_.size() - 2;
    const bool boxFilter = step_ >= 2.0;
    while (position <= static_cast<double>(limit)) {
        const qsizetype index = static_cast<qsizetype>(position);
        float value = 0.0F;
        if (boxFilter) {
            const float left =
                index > 0 ? input_.at(index - 1) : input_.at(index);
            value =
                (left + input_.at(index) + input_.at(index + 1)) / 3.0F;
        } else {
            const float fraction =
                static_cast<float>(position - index);
            value = input_.at(index) * (1.0F - fraction)
                + input_.at(index + 1) * fraction;
        }

        const double scaled =
            format_ == SampleFormat::Float32
            ? static_cast<double>(value) * kFloatScale
            : static_cast<double>(value);
        const qint16 pcm = static_cast<qint16>(std::clamp(
            std::lround(scaled),
            -32768L,
            32767L));
        const quint16 little = qToLittleEndian<quint16>(
            static_cast<quint16>(pcm));
        output.append(
            reinterpret_cast<const char *>(&little),
            sizeof(little));
        position += step_;
    }

    const qsizetype consumed =
        std::min(input_.size(), static_cast<qsizetype>(position));
    if (consumed > 0) {
        input_.remove(0, consumed);
        position -= consumed;
    }
    if (input_.size() > 8192) {
        input_ = input_.mid(input_.size() - 4);
        position = 0.0;
    }
    position_ = position;
    return output;
}

} // namespace pd
