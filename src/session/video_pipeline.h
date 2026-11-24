#pragma once

#include "core/protocol.h"
#include "session/media_sources.h"
#include "session/session_types.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QThread>

#include <memory>

namespace pd {

struct EncodedVideoFrame {
    QByteArray data;
    bool keyframe = false;
    int quality = 0;
    int effectiveFps = 0;
    qint64 captureMs = 0;
    qint64 encodeMs = 0;
    VideoCodec codec = VideoCodec::Jpeg;
};

class VideoCaptureWorker;

class VideoPipeline final : public QObject {
    Q_OBJECT

public:
    explicit VideoPipeline(QObject *parent = nullptr);
    ~VideoPipeline() override;

    void setSource(std::shared_ptr<IScreenSource> source);
    [[nodiscard]] bool start(
        const VideoSettings &settings,
        int quality,
        QString *error = nullptr);
    void stop();
    void update(const VideoSettings &settings, int quality);
    void setCodec(VideoCodec codec);
    void setRateLimit(double mbps);
    void setAdaptiveLimits(int qualityLimit, int fpsLimit);
    void requestKeyframe();

    [[nodiscard]] VideoCodec codec() const { return codec_; }
    [[nodiscard]] QList<MonitorInfo> monitors() const;
    [[nodiscard]] QString backend() const;
    [[nodiscard]] bool hasSource() const { return hasSource_; }
    [[nodiscard]] bool running() const { return running_; }

signals:
    void frameReady(const pd::EncodedVideoFrame &frame);
    void codecChanged(pd::VideoCodec codec, const QString &reason);
    void failed(const QString &message);
    void started(const QString &backend);

private:
    void onWorkerFrame(
        quint64 generation,
        const pd::EncodedVideoFrame &frame);
    void onWorkerFailed(quint64 generation, const QString &message);
    void onWorkerStarted(quint64 generation, const QString &backend);
    void onWorkerCodecChanged(
        quint64 generation,
        pd::VideoCodec codec,
        const QString &reason);

    QThread thread_;
    VideoCaptureWorker *worker_ = nullptr;
    mutable QList<MonitorInfo> monitorsCache_;
    mutable QString backendCache_;
    double rateLimitMbps_ = 0.0;
    VideoCodec codec_ = VideoCodec::Jpeg;
    int adaptiveQualityLimit_ = 92;
    int adaptiveFpsLimit_ = 120;
    quint64 generation_ = 0;
    bool hasSource_ = false;
    bool running_ = false;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::EncodedVideoFrame)
