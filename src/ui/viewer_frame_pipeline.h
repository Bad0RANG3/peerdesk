#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QThread>

#include <mutex>

namespace pd::ui {

class ViewerFrameWorker;

class ViewerFramePipeline final : public QObject {
    Q_OBJECT

public:
    explicit ViewerFramePipeline(QObject *parent = nullptr);
    ~ViewerFramePipeline() override;

    void setFrame(const QImage &frame);
    void setViewport(const QSize &logicalSize, qreal devicePixelRatio);
    void clear();

    [[nodiscard]] QString presentationThreadName() const;

signals:
    void framePresented(
        const QImage &image,
        const QString &threadName);

private:
    void submit();
    void onWorkerFrame(
        quint64 generation,
        const QImage &image,
        const QString &threadName);

    QThread thread_;
    ViewerFrameWorker *worker_ = nullptr;
    QImage frame_;
    QSize viewport_;
    qreal devicePixelRatio_ = 1.0;
    quint64 generation_ = 0;
    mutable std::mutex threadNameMutex_;
    QString presentationThreadName_;
};

} // namespace pd::ui
