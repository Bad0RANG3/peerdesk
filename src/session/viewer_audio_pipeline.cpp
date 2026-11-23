#include "session/viewer_audio_pipeline.h"

#include "core/protocol.h"
#include "session/audio_codec.h"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>

#include <utility>

namespace pd {

class ViewerAudioWorker final : public QObject {
    Q_OBJECT

public:
    void start()
    {
        QMutexLocker locker(&mutex_);
        queue_.clear();
        accepting_ = true;
    }

    void cancel()
    {
        QMutexLocker locker(&mutex_);
        accepting_ = false;
        queue_.clear();
    }

    void submit(quint64 generation, QByteArray payload)
    {
        bool schedule = false;
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_) {
                return;
            }
            queue_.enqueue(Pending{
                generation,
                std::move(payload),
            });
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (schedule) {
            QMetaObject::invokeMethod(
                this,
                &ViewerAudioWorker::decodePending,
                Qt::QueuedConnection);
        }
    }

signals:
    void audioDecoded(
        quint64 generation,
        const QByteArray &pcm16,
        quint32 sequence,
        quint32 timestampMs,
        const QString &threadName);
    void decodeFailed(quint64 generation, const QString &message);

private:
    struct Pending {
        quint64 generation = 0;
        QByteArray payload;
    };

    void decodePending()
    {
        const QString threadName = QThread::currentThread()->objectName();
        for (;;) {
            Pending pending;
            {
                QMutexLocker locker(&mutex_);
                if (queue_.isEmpty()) {
                    scheduled_ = false;
                    return;
                }
                pending = std::move(queue_.dequeue());
            }

            const auto packet = unpackAudio(pending.payload);
            if (!packet.has_value()) {
                emit decodeFailed(
                    pending.generation,
                    QStringLiteral("收到损坏的音频包"));
                continue;
            }
            if (packet->samples == 0
                || packet->samples > kAudioRate
                || packet->current.size() != packet->samples
                || (!packet->previous.isEmpty()
                    && packet->previous.size() != packet->samples)) {
                emit decodeFailed(
                    pending.generation,
                    QStringLiteral("音频包长度校验失败"));
                continue;
            }
            emit audioDecoded(
                pending.generation,
                ulawDecode(packet->current),
                packet->sequence,
                packet->timestampMs,
                threadName);
        }
    }

    QMutex mutex_;
    QQueue<Pending> queue_;
    bool accepting_ = false;
    bool scheduled_ = false;
};

ViewerAudioPipeline::ViewerAudioPipeline(QObject *parent)
    : QObject(parent)
{
    worker_ = new ViewerAudioWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskViewerAudio"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &ViewerAudioWorker::audioDecoded,
        this,
        &ViewerAudioPipeline::onWorkerDecoded);
    connect(
        worker_,
        &ViewerAudioWorker::decodeFailed,
        this,
        &ViewerAudioPipeline::onWorkerFailed);
    thread_.start();
}

ViewerAudioPipeline::~ViewerAudioPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void ViewerAudioPipeline::start()
{
    running_ = true;
    ++generation_;
    {
        QMutexLocker locker(&threadNameMutex_);
        decoderThreadName_.clear();
    }
    if (worker_) {
        worker_->start();
    }
}

void ViewerAudioPipeline::stop()
{
    running_ = false;
    ++generation_;
    if (worker_) {
        worker_->cancel();
    }
}

void ViewerAudioPipeline::submit(const QByteArray &payload)
{
    if (!running_ || !worker_) {
        return;
    }
    worker_->submit(generation_, payload);
}

QString ViewerAudioPipeline::decoderThreadName() const
{
    QMutexLocker locker(&threadNameMutex_);
    return decoderThreadName_;
}

void ViewerAudioPipeline::onWorkerDecoded(
    quint64 generation,
    const QByteArray &pcm16,
    quint32 sequence,
    quint32 timestampMs,
    const QString &threadName)
{
    if (!running_ || generation != generation_) {
        return;
    }
    {
        QMutexLocker locker(&threadNameMutex_);
        decoderThreadName_ = threadName;
    }
    emit audioDecoded(pcm16, sequence, timestampMs);
}

void ViewerAudioPipeline::onWorkerFailed(
    quint64 generation,
    const QString &message)
{
    if (!running_ || generation != generation_) {
        return;
    }
    emit decodeFailed(message);
}

} // namespace pd

#include "viewer_audio_pipeline.moc"
