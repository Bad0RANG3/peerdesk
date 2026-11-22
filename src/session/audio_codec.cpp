#include "session/audio_codec.h"

#include <algorithm>
#include <cmath>

namespace pd {

namespace {

qint16 readPcm16(const char *data)
{
    const auto low = static_cast<quint8>(data[0]);
    const auto high = static_cast<quint8>(data[1]);
    return static_cast<qint16>(
        static_cast<quint16>(low)
        | (static_cast<quint16>(high) << 8));
}

quint8 encodeSample(qint16 value)
{
    int sample = value;
    const quint8 sign = sample < 0 ? 0x80 : 0x00;
    if (sample < 0) {
        sample = -sample;
    }
    sample = std::min(sample, 32635) + 0x84;
    int exponent = 7;
    int mask = 0x4000;
    while (exponent > 0 && (sample & mask) == 0) {
        --exponent;
        mask >>= 1;
    }
    const quint8 mantissa =
        static_cast<quint8>((sample >> (exponent + 3)) & 0x0F);
    return static_cast<quint8>(
        ~(sign | static_cast<quint8>(exponent << 4) | mantissa)
        & 0xFF);
}

qint16 decodeSample(quint8 byte)
{
    const quint8 value = static_cast<quint8>(~byte & 0xFF);
    int decoded = (static_cast<int>(value & 0x0F) << 3) + 0x84;
    decoded <<= (value & 0x70) >> 4;
    const int sample = (value & 0x80) != 0
        ? 0x84 - decoded
        : decoded - 0x84;
    return static_cast<qint16>(
        std::clamp(sample, -32768, 32767));
}

} // namespace

QByteArray ulawEncode(const QByteArray &pcm16)
{
    const qsizetype samples = pcm16.size() / 2;
    QByteArray encoded(samples, '\0');
    const char *source = pcm16.constData();
    char *target = encoded.data();
    for (qsizetype index = 0; index < samples; ++index) {
        target[index] = static_cast<char>(
            encodeSample(readPcm16(source + index * 2)));
    }
    return encoded;
}

QByteArray ulawDecode(const QByteArray &ulaw)
{
    QByteArray pcm(ulaw.size() * 2, '\0');
    char *target = pcm.data();
    for (qsizetype index = 0; index < ulaw.size(); ++index) {
        const qint16 sample = decodeSample(
            static_cast<quint8>(ulaw.at(index)));
        target[index * 2] = static_cast<char>(sample & 0xFF);
        target[index * 2 + 1] =
            static_cast<char>((sample >> 8) & 0xFF);
    }
    return pcm;
}

QByteArray silentUlawFrame()
{
    return QByteArray(kAudioSamples, static_cast<char>(0xFF));
}

double pcm16Rms(const QByteArray &pcm16)
{
    const qsizetype samples = pcm16.size() / 2;
    if (samples == 0) {
        return 0.0;
    }
    long double sum = 0.0;
    const char *source = pcm16.constData();
    for (qsizetype index = 0; index < samples; ++index) {
        const double value = readPcm16(source + index * 2);
        sum += value * value;
    }
    return std::sqrt(
        static_cast<double>(sum / static_cast<long double>(samples)));
}

AudioEncoder::AudioEncoder()
{
    clock_.start();
}

std::optional<AudioPacket> AudioEncoder::next(const QByteArray &pcm16)
{
    if (pcm16.size() != kAudioPcmBytes) {
        return std::nullopt;
    }
    const QByteArray current = ulawEncode(pcm16);
    if (current == silentUlawFrame()) {
        ++silentRun_;
    } else {
        silentRun_ = 0;
    }
    if (silentRun_ > kSilenceHoldFrames
        && silentRun_ % kSilenceKeepFrames != 0) {
        return std::nullopt;
    }

    AudioPacket packet;
    packet.sequence = sequence_++;
    packet.timestampMs = static_cast<quint32>(
        std::max<qint64>(0, clock_.elapsed()) & 0xFFFFFFFF);
    packet.samples = static_cast<quint16>(pcm16.size() / 2);
    packet.previous = previous_;
    packet.current = current;
    previous_ = current;
    return packet;
}

void AudioEncoder::reset()
{
    clock_.restart();
    sequence_ = 0;
    previous_.clear();
    silentRun_ = 0;
}

} // namespace pd
