#include "app/audio_output.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>

#include <algorithm>

namespace pd {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kMaxPendingMs = 360;

qint64 byteLimit()
{
    return static_cast<qint64>(kSampleRate)
        * kChannels
        * static_cast<int>(sizeof(qint16))
        * kMaxPendingMs
        / 1000;
}

} // namespace

PcmAudioPlayer::PcmAudioPlayer(QObject *parent)
    : QObject(parent)
    , format_()
    , pumpTimer_(new QTimer(this))
{
    format_.setSampleRate(kSampleRate);
    format_.setChannelCount(kChannels);
    format_.setSampleFormat(QAudioFormat::Int16);

    pumpTimer_->setInterval(10);
    pumpTimer_->setTimerType(Qt::PreciseTimer);
    connect(pumpTimer_, &QTimer::timeout, this, &PcmAudioPlayer::pump);
}

PcmAudioPlayer::~PcmAudioPlayer()
{
    reset();
}

void PcmAudioPlayer::setEnabled(bool enabled)
{
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        reset();
        failed_ = false;
        error_.clear();
    }
}

void PcmAudioPlayer::pushPcm16(const QByteArray &pcm)
{
    if (!enabled_ || pcm.isEmpty()) {
        return;
    }
    if (failed_) {
        return;
    }

    ensureSink();
    if (!sink_ || !device_) {
        return;
    }

    pending_.append(pcm);
    const qint64 limit = byteLimit();
    if (pending_.size() > limit) {
        pending_.remove(0, pending_.size() - static_cast<qsizetype>(limit));
    }
    pump();
}

void PcmAudioPlayer::reset()
{
    pumpTimer_->stop();
    pending_.clear();
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    device_ = nullptr;
}

void PcmAudioPlayer::ensureSink()
{
    if (sink_ || failed_ || !enabled_) {
        return;
    }
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        fail(QStringLiteral("未找到音频输出设备"));
        return;
    }
    if (!output.isFormatSupported(format_)) {
        fail(QStringLiteral("音频设备不支持 16 kHz 单声道 PCM"));
        return;
    }

    sink_ = new QAudioSink(output, format_, this);
    sink_->setBufferSize(
        format_.bytesForDuration(160000));
    sink_->setVolume(1.0);
    device_ = sink_->start();
    if (!device_) {
        fail(QStringLiteral("无法启动音频播放"));
        if (sink_) {
            delete sink_;
            sink_ = nullptr;
        }
        return;
    }
    error_.clear();
    pumpTimer_->start();
}

void PcmAudioPlayer::pump()
{
    if (!sink_ || !device_ || pending_.isEmpty()) {
        return;
    }
    if (sink_->state() == QAudio::StoppedState) {
        fail(sink_->error() == QAudio::NoError
                 ? QStringLiteral("音频播放已停止")
                 : QStringLiteral("音频播放错误"));
        reset();
        return;
    }

    const qint64 room = std::max<qint64>(0, sink_->bytesFree());
    if (room <= 0) {
        return;
    }
    const qsizetype amount =
        std::min<qsizetype>(pending_.size(), static_cast<qsizetype>(room));
    const qint64 written = device_->write(pending_.constData(), amount);
    if (written > 0) {
        pending_.remove(0, static_cast<qsizetype>(written));
    }
}

void PcmAudioPlayer::fail(const QString &message)
{
    error_ = message;
    failed_ = true;
    pumpTimer_->stop();
}

} // namespace pd
