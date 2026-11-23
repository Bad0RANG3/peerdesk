#include "app/audio_output.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace pd {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kMaxPendingMs = 360;
constexpr int kPumpIntervalMs = 10;
constexpr int kSinkBufferMs = 160;

qint64 byteLimit()
{
    return static_cast<qint64>(kSampleRate)
        * kChannels
        * static_cast<int>(sizeof(qint16))
        * kMaxPendingMs
        / 1000;
}

class QtAudioSinkBackend final : public AudioSinkBackend {
public:
    ~QtAudioSinkBackend() override
    {
        stop();
    }

    bool start(const QAudioFormat &format, QString *error) override
    {
        stop();
        const QAudioDevice output = QMediaDevices::defaultAudioOutput();
        if (output.isNull()) {
            if (error) {
                *error = QStringLiteral("未找到音频输出设备");
            }
            return false;
        }
        if (!output.isFormatSupported(format)) {
            if (error) {
                *error = QStringLiteral(
                    "音频设备不支持 16 kHz 单声道 PCM");
            }
            return false;
        }

        auto sink = std::make_unique<QAudioSink>(output, format);
        sink->setBufferSize(
            format.bytesForDuration(kSinkBufferMs * 1000));
        sink->setVolume(1.0);
        QIODevice *device = sink->start();
        if (!device) {
            if (error) {
                *error = QStringLiteral("无法启动音频播放");
            }
            return false;
        }

        sink_ = std::move(sink);
        device_ = device;
        if (error) {
            error->clear();
        }
        return true;
    }

    [[nodiscard]] bool hasStopped() const override
    {
        return !sink_ || sink_->state() == QAudio::StoppedState;
    }

    [[nodiscard]] qint64 bytesFree() const override
    {
        return sink_ ? std::max<qint64>(0, sink_->bytesFree()) : 0;
    }

    qint64 write(const char *data, qint64 size) override
    {
        return device_ ? device_->write(data, size) : -1;
    }

    void stop() override
    {
        if (sink_) {
            sink_->stop();
            sink_.reset();
        }
        device_ = nullptr;
    }

private:
    std::unique_ptr<QAudioSink> sink_;
    QIODevice *device_ = nullptr;
};

AudioSinkFactory defaultSinkFactory()
{
    return [] {
        return std::make_unique<QtAudioSinkBackend>();
    };
}

} // namespace

struct AudioPlaybackState {
    mutable std::mutex mutex;
    QString error;
    std::atomic_bool enabled = true;
};

class AudioWorker final : public QObject {
public:
    AudioWorker(
        AudioSinkFactory sinkFactory,
        std::shared_ptr<AudioPlaybackState> state)
        : QObject(nullptr)
        , sinkFactory_(std::move(sinkFactory))
        , state_(std::move(state))
    {
        format_.setSampleRate(kSampleRate);
        format_.setChannelCount(kChannels);
        format_.setSampleFormat(QAudioFormat::Int16);
    }

    ~AudioWorker() override
    {
        resetDevice();
    }

    void prepare()
    {
        pumpTimer_ = new QTimer(this);
        pumpTimer_->setInterval(kPumpIntervalMs);
        pumpTimer_->setTimerType(Qt::PreciseTimer);
        connect(pumpTimer_, &QTimer::timeout, this, [this] {
            pump();
        });
    }

    void setEnabled(bool enabled)
    {
        if (!enabled) {
            resetDevice();
            failed_ = false;
            clearError();
        }
    }

    void push(QByteArray pcm)
    {
        if (shuttingDown_ || pcm.isEmpty()
            || !state_->enabled.load(std::memory_order_acquire)
            || failed_) {
            return;
        }

        ensureSink();
        if (!sink_) {
            return;
        }

        pending_.append(pcm);
        const qint64 limit = byteLimit();
        if (pending_.size() > limit) {
            pending_.remove(
                0,
                pending_.size() - static_cast<qsizetype>(limit));
        }
        pump();
        if (!pending_.isEmpty() && pumpTimer_
            && !pumpTimer_->isActive()) {
            pumpTimer_->start();
        }
    }

    void reset()
    {
        resetDevice();
        failed_ = false;
        clearError();
    }

    void shutdown()
    {
        shuttingDown_ = true;
        resetDevice();
    }

private:
    void ensureSink()
    {
        if (sink_ || failed_ || !state_->enabled.load(
                std::memory_order_acquire)) {
            return;
        }
        sink_ = sinkFactory_ ? sinkFactory_() : nullptr;
        if (!sink_) {
            fail(QStringLiteral("无法创建音频播放"));
            return;
        }

        QString error;
        if (!sink_->start(format_, &error)) {
            fail(error.isEmpty()
                     ? QStringLiteral("无法启动音频播放")
                     : error);
            sink_.reset();
            return;
        }
        if (pumpTimer_) {
            pumpTimer_->start();
        }
    }

    void pump()
    {
        if (shuttingDown_ || !sink_ || pending_.isEmpty()) {
            if (pending_.isEmpty() && pumpTimer_) {
                pumpTimer_->stop();
            }
            return;
        }
        if (sink_->hasStopped()) {
            fail(QStringLiteral("音频播放已停止"));
            return;
        }

        const qint64 room = sink_->bytesFree();
        if (room <= 0) {
            return;
        }
        const qsizetype amount = std::min<qsizetype>(
            pending_.size(),
            static_cast<qsizetype>(room));
        const qint64 written =
            sink_->write(pending_.constData(), amount);
        if (written < 0) {
            fail(QStringLiteral("音频播放写入失败"));
            return;
        }
        if (written > 0) {
            pending_.remove(0, static_cast<qsizetype>(written));
        }
        if (pending_.isEmpty() && pumpTimer_) {
            pumpTimer_->stop();
        }
    }

    void fail(const QString &message)
    {
        {
            std::lock_guard<std::mutex> locker(state_->mutex);
            state_->error = message;
        }
        failed_ = true;
        resetDevice();
    }

    void clearError()
    {
        std::lock_guard<std::mutex> locker(state_->mutex);
        state_->error.clear();
    }

    void resetDevice()
    {
        if (pumpTimer_) {
            pumpTimer_->stop();
        }
        pending_.clear();
        sink_.reset();
    }

    AudioSinkFactory sinkFactory_;
    std::shared_ptr<AudioPlaybackState> state_;
    QAudioFormat format_;
    std::unique_ptr<AudioSinkBackend> sink_;
    QTimer *pumpTimer_ = nullptr;
    QByteArray pending_;
    bool failed_ = false;
    bool shuttingDown_ = false;
};

PcmAudioPlayer::PcmAudioPlayer(QObject *parent)
    : PcmAudioPlayer(defaultSinkFactory(), parent)
{
}

PcmAudioPlayer::PcmAudioPlayer(
    AudioSinkFactory sinkFactory,
    QObject *parent)
    : QObject(parent)
    , state_(std::make_shared<AudioPlaybackState>())
    , sinkFactory_(std::move(sinkFactory))
    , audioThread_(new QThread(this))
    , worker_(new AudioWorker(sinkFactory_, state_))
{
    audioThread_->setObjectName(QStringLiteral("PeerDeskAudio"));
    worker_->moveToThread(audioThread_);
    connect(
        audioThread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    audioThread_->start();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_] {
            worker->prepare();
        },
        Qt::BlockingQueuedConnection);
}

PcmAudioPlayer::~PcmAudioPlayer()
{
    shutdown();
}

bool PcmAudioPlayer::enabled() const
{
    return state_->enabled.load(std::memory_order_acquire);
}

void PcmAudioPlayer::setEnabled(bool enabled)
{
    if (state_->enabled.exchange(
            enabled,
            std::memory_order_acq_rel)
        == enabled) {
        return;
    }
    if (!audioThread_->isRunning()) {
        return;
    }
    if (QThread::currentThread() == audioThread_) {
        worker_->setEnabled(enabled);
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, enabled] {
            worker->setEnabled(enabled);
        },
        Qt::BlockingQueuedConnection);
}

void PcmAudioPlayer::pushPcm16(const QByteArray &pcm)
{
    if (pcm.isEmpty() || !state_->enabled.load(
            std::memory_order_acquire)
        || !audioThread_->isRunning()) {
        return;
    }
    if (QThread::currentThread() == audioThread_) {
        worker_->push(pcm);
        return;
    }
    QByteArray copy = pcm;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, copy = std::move(copy)]() mutable {
            worker->push(std::move(copy));
        },
        Qt::QueuedConnection);
}

void PcmAudioPlayer::reset()
{
    if (!audioThread_->isRunning()) {
        return;
    }
    if (QThread::currentThread() == audioThread_) {
        worker_->reset();
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_] {
            worker->reset();
        },
        Qt::BlockingQueuedConnection);
}

QString PcmAudioPlayer::errorString() const
{
    std::lock_guard<std::mutex> locker(state_->mutex);
    return state_->error;
}

void PcmAudioPlayer::shutdown()
{
    if (!audioThread_ || !audioThread_->isRunning()) {
        return;
    }
    if (QThread::currentThread() == audioThread_) {
        worker_->shutdown();
        audioThread_->quit();
        worker_ = nullptr;
        return;
    }

    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_] {
            worker->shutdown();
        },
        Qt::BlockingQueuedConnection);
    audioThread_->quit();
    audioThread_->wait();
    worker_ = nullptr;
}

} // namespace pd
