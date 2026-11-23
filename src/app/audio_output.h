#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QThread;

namespace pd {

class IAudioPlayback {
public:
    virtual ~IAudioPlayback() = default;

    virtual void setEnabled(bool enabled) = 0;
    [[nodiscard]] virtual bool enabled() const = 0;
    virtual void pushPcm16(const QByteArray &pcm) = 0;
    virtual void reset() = 0;
    [[nodiscard]] virtual QString errorString() const = 0;
};

class AudioSinkBackend {
public:
    virtual ~AudioSinkBackend() = default;

    virtual bool start(const QAudioFormat &format, QString *error) = 0;
    [[nodiscard]] virtual bool hasStopped() const = 0;
    [[nodiscard]] virtual qint64 bytesFree() const = 0;
    virtual qint64 write(const char *data, qint64 size) = 0;
    virtual void stop() = 0;
};

using AudioSinkFactory = std::function<std::unique_ptr<AudioSinkBackend>()>;

struct AudioPlaybackState;
class AudioWorker;

class PcmAudioPlayer final : public QObject, public IAudioPlayback {
    Q_OBJECT

public:
    explicit PcmAudioPlayer(QObject *parent = nullptr);
    PcmAudioPlayer(AudioSinkFactory sinkFactory, QObject *parent = nullptr);
    ~PcmAudioPlayer() override;

    void setEnabled(bool enabled) override;
    [[nodiscard]] bool enabled() const override;
    void pushPcm16(const QByteArray &pcm) override;
    void reset() override;
    [[nodiscard]] QString errorString() const override;

private:
    void shutdown();

    std::shared_ptr<AudioPlaybackState> state_;
    AudioSinkFactory sinkFactory_;
    QThread *audioThread_ = nullptr;
    AudioWorker *worker_ = nullptr;
};

} // namespace pd
