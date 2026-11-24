#pragma once

#include "core/protocol.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

namespace pd {

class VideoReceiveWorker;

class VideoReceivePipeline final : public QObject {
    Q_OBJECT

public:
    explicit VideoReceivePipeline(QObject *parent = nullptr);
    ~VideoReceivePipeline() override;

    void start();
    void stop();
    void resetSession();

    void submitData(
        const QByteArray &payload,
        bool keyframe,
        VideoCodec codec = VideoCodec::Jpeg);
    void submitParity(
        const QByteArray &payload,
        VideoCodec codec = VideoCodec::Jpeg);

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] quint64 sessionGeneration() const { return generation_; }
    [[nodiscard]] QString reassemblerThreadName() const;

signals:
    void frameReady(
        const QByteArray &frame,
        bool keyframe,
        pd::VideoCodec codec);
    void nackReady(quint32 frameId, const QList<quint16> &missing);
    void keyframeRequested();

private:
    void onWorkerFrame(
        quint64 generation,
        const QByteArray &frame,
        bool keyframe,
        pd::VideoCodec codec,
        const QString &threadName);
    void onWorkerNack(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing,
        const QString &threadName);
    void onWorkerKeyframe(quint64 generation, const QString &threadName);

    QThread thread_;
    VideoReceiveWorker *worker_ = nullptr;
    QString reassemblerThreadName_;
    quint64 generation_ = 0;
    bool running_ = false;
};

} // namespace pd
