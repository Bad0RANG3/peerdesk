#pragma once

#include "session/media_sources.h"

#include <QPointF>
#include <QRect>
#include <QString>
#include <QThread>

#include <memory>

namespace pd {

struct HostInputCommand {
    enum class Type {
        MoveAbsolute,
        MoveRelative,
        MouseButton,
        Click,
        Wheel,
        Key,
        Text,
        CtrlAltDelete,
    };

    Type type = Type::MoveAbsolute;
    QPointF point;
    QRect monitor;
    int dx = 0;
    int dy = 0;
    int delta = 0;
    bool down = false;
    bool horizontal = false;
    QString name;
    QString text;

    static HostInputCommand moveAbsolute(
        double x,
        double y,
        const QRect &monitor);
    static HostInputCommand moveRelative(int dx, int dy);
    static HostInputCommand mouseButton(
        const QString &button,
        bool down);
    static HostInputCommand click(const QString &button);
    static HostInputCommand wheel(int delta, bool horizontal);
    static HostInputCommand key(const QString &code, bool down);
    static HostInputCommand typeText(const QString &text);
    static HostInputCommand ctrlAltDelete();
};

class HostInputWorker;

class HostInputPipeline final {
public:
    HostInputPipeline();
    ~HostInputPipeline();

    HostInputPipeline(const HostInputPipeline &) = delete;
    HostInputPipeline &operator=(const HostInputPipeline &) = delete;

    void setSink(std::shared_ptr<IInputSink> sink);
    void start();
    void stop();
    void releaseAll();
    void submit(const HostInputCommand &command);

    [[nodiscard]] bool running() const { return running_; }

private:
    QThread thread_;
    HostInputWorker *worker_ = nullptr;
    quint64 generation_ = 0;
    bool running_ = false;
};

} // namespace pd
