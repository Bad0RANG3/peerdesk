#pragma once

#include <QByteArray>
#include <QList>

namespace pd {

class PcmMonoResampler {
public:
    enum class SampleFormat {
        Float32,
        Int16,
    };

    PcmMonoResampler(
        int inputRate,
        int channels,
        SampleFormat format,
        int outputRate = 16000);

    [[nodiscard]] QByteArray feed(const void *data, qsizetype bytes);
    void reset();

    [[nodiscard]] int inputRate() const { return inputRate_; }
    [[nodiscard]] int outputRate() const { return outputRate_; }
    [[nodiscard]] int channels() const { return channels_; }
    [[nodiscard]] SampleFormat format() const { return format_; }

private:
    [[nodiscard]] float sampleAt(const char *data, qsizetype index) const;

    int inputRate_;
    int outputRate_;
    int channels_;
    SampleFormat format_;
    double step_;
    double position_ = 0.0;
    QList<float> input_;
};

} // namespace pd
