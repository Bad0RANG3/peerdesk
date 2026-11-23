#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

namespace pd {

class ViewerVideoWorker;

class ViewerVideoPipeline final : public QObject {
    Q_OBJECT

public:
    explicit ViewerVideoPipeline(QObject *parent = nullptr);
    ~ViewerVideoPipeline() override;

    void start();
    void stop();
    void submit(
        const QByteArray &jpeg,
        quint32 sequence,
        bool keyframe);

    [[nodiscard]] QString decoderThreadName() const;

signals:
    void frameDecoded(
        const QImage &image,
        quint32 sequence,
        bool keyframe);
    void decodeFailed(const QString &message);

private:
    void onWorkerFrame(
        quint64 generation,
        quint32 sequence,
        const QImage &image,
        bool keyframe,
        const QString &threadName);
    void onWorkerFailed(
        quint64 generation,
        quint32 sequence,
        const QString &message);

    QThread thread_;
    ViewerVideoWorker *worker_ = nullptr;
    mutable QMutex threadNameMutex_;
    QString decoderThreadName_;
    quint64 generation_ = 0;
    bool running_ = false;
};

} // namespace pd
