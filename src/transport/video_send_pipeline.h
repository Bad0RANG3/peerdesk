#pragma once

#include "core/video.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

namespace pd {

struct VideoPacketBatch {
    quint32 frameId = 0;
    bool keyframe = false;
    bool retransmit = false;
    QList<WirePacket> packets;
};

class VideoSendWorker;

class VideoSendPipeline final : public QObject {
    Q_OBJECT

public:
    explicit VideoSendPipeline(QObject *parent = nullptr);
    ~VideoSendPipeline() override;

    void start();
    void stop();
    void submit(const QByteArray &jpeg, bool keyframe);
    void retransmit(quint32 frameId, const QList<quint16> &missing);

    [[nodiscard]] QString packetizerThreadName() const;

signals:
    void packetsReady(const pd::VideoPacketBatch &batch);
    void packetizationFailed(const QString &message);

private:
    void onWorkerPackets(
        quint64 generation,
        const pd::VideoPacketBatch &batch,
        const QString &threadName);
    void onWorkerFailed(quint64 generation, const QString &message);

    QThread thread_;
    VideoSendWorker *worker_ = nullptr;
    mutable QMutex threadNameMutex_;
    QString packetizerThreadName_;
    quint64 generation_ = 0;
    bool running_ = false;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::VideoPacketBatch)
