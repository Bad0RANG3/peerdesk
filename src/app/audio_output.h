#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QAudioSink;
class QIODevice;
class QTimer;

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

class PcmAudioPlayer final : public QObject, public IAudioPlayback {
    Q_OBJECT

public:
    explicit PcmAudioPlayer(QObject *parent = nullptr);
    ~PcmAudioPlayer() override;

    void setEnabled(bool enabled) override;
    [[nodiscard]] bool enabled() const override { return enabled_; }
    void pushPcm16(const QByteArray &pcm) override;
    void reset() override;
    [[nodiscard]] QString errorString() const override { return error_; }

private:
    void ensureSink();
    void pump();
    void fail(const QString &message);

    QAudioFormat format_;
    QAudioSink *sink_ = nullptr;
    QIODevice *device_ = nullptr;
    QTimer *pumpTimer_ = nullptr;
    QByteArray pending_;
    QString error_;
    bool enabled_ = true;
    bool failed_ = false;
};

} // namespace pd
