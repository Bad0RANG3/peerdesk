#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

namespace pd {

class ViewerAudioWorker;

class ViewerAudioPipeline final : public QObject {
    Q_OBJECT

public:
    explicit ViewerAudioPipeline(QObject *parent = nullptr);
    ~ViewerAudioPipeline() override;

    void start();
    void stop();
    void submit(const QByteArray &payload);

    [[nodiscard]] QString decoderThreadName() const;

signals:
    void audioDecoded(
        const QByteArray &pcm16,
        quint32 sequence,
        quint32 timestampMs);
    void decodeFailed(const QString &message);

private:
    void onWorkerDecoded(
        quint64 generation,
        const QByteArray &pcm16,
        quint32 sequence,
        quint32 timestampMs,
        const QString &threadName);
    void onWorkerFailed(quint64 generation, const QString &message);

    QThread thread_;
    ViewerAudioWorker *worker_ = nullptr;
    mutable QMutex threadNameMutex_;
    QString decoderThreadName_;
    quint64 generation_ = 0;
    bool running_ = false;
};

} // namespace pd
