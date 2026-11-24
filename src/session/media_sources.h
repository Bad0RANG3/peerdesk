#pragma once

#include "session/session_types.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

namespace pd {

struct CaptureRequest {
    int monitor = 0;
    int maxWidth = 1920;
    QString pixelFormat = QStringLiteral("jpeg");
};

struct CaptureResult {
    QImage image;
    QString error;

    [[nodiscard]] bool ok() const { return !image.isNull(); }
};

struct CursorSample {
    bool visible = false;
    QPoint screenPosition;
    QPoint hotspot;
    CursorShape shape = CursorShape::Arrow;
    QImage image;
};

class IScreenSource {
public:
    virtual ~IScreenSource() = default;

    [[nodiscard]] virtual QString backend() const = 0;
    [[nodiscard]] virtual QList<MonitorInfo> monitors() const = 0;
    virtual CaptureResult capture(const CaptureRequest &request) = 0;
    [[nodiscard]] virtual CursorSample cursorSample() const { return {}; }
    virtual void close() {}
};

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    [[nodiscard]] virtual QString label() const = 0;
    virtual bool start(QString *error = nullptr) = 0;
    virtual QByteArray readFrame(
        int samples,
        int timeoutMs = 0,
        QString *error = nullptr) = 0;
    virtual void stop() = 0;
};

class IInputSink {
public:
    virtual ~IInputSink() = default;

    [[nodiscard]] virtual QRect virtualDesktop() const = 0;
    virtual void moveAbsolute(double x, double y) = 0;
    virtual void moveRelative(int dx, int dy) = 0;
    virtual void mouseButton(const QString &button, bool down) = 0;
    virtual void click(const QString &button) = 0;
    virtual void wheel(int delta, bool horizontal) = 0;
    virtual void key(const QString &code, bool down) = 0;
    virtual void typeText(const QString &text) = 0;
    virtual void ctrlAltDelete() = 0;
    virtual void releaseAll() {}
};

class IClipboard {
public:
    virtual ~IClipboard() = default;

    [[nodiscard]] virtual QString text() const = 0;
    virtual bool setText(const QString &text) = 0;
    [[nodiscard]] virtual QImage image() const { return {}; }
    virtual bool setImage(const QImage &image)
    {
        (void)image;
        return false;
    }
};

class SyntheticScreenSource final : public IScreenSource {
public:
    explicit SyntheticScreenSource(
        int width = 1280,
        int height = 720,
        bool twoMonitors = false);

    [[nodiscard]] QString backend() const override;
    [[nodiscard]] QList<MonitorInfo> monitors() const override;
    CaptureResult capture(const CaptureRequest &request) override;
    [[nodiscard]] CursorSample cursorSample() const override;

    void setStatic(bool isStatic) { static_ = isStatic; }
    void reset() { frame_ = 0; }
    void setCursorSample(const CursorSample &sample) { cursor_ = sample; }

private:
    QList<MonitorInfo> monitors_;
    int frame_ = 0;
    bool static_ = false;
    CursorSample cursor_;
    QElapsedTimer clock_;
};

class ToneAudioSource final : public IAudioSource {
public:
    explicit ToneAudioSource(double frequency = 440.0);

    [[nodiscard]] QString label() const override;
    bool start(QString *error = nullptr) override;
    QByteArray readFrame(
        int samples,
        int timeoutMs = 0,
        QString *error = nullptr) override;
    void stop() override;

    void setFrequency(double frequency);
    void setSilent(bool silent) { silent_ = silent; }
    [[nodiscard]] bool isStarted() const { return started_; }

private:
    double frequency_;
    double phase_ = 0.0;
    bool started_ = false;
    bool silent_ = false;
};

} // namespace pd
