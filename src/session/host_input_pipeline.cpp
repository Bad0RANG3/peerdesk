#include "session/host_input_pipeline.h"

#include <QMetaObject>
#include <QMutexLocker>

#include <algorithm>
#include <deque>
#include <utility>

namespace pd {

HostInputCommand HostInputCommand::moveAbsolute(
    double x,
    double y,
    const QRect &monitor)
{
    HostInputCommand command;
    command.type = Type::MoveAbsolute;
    command.point = QPointF(x, y);
    command.monitor = monitor;
    return command;
}

HostInputCommand HostInputCommand::moveRelative(int dx, int dy)
{
    HostInputCommand command;
    command.type = Type::MoveRelative;
    command.dx = dx;
    command.dy = dy;
    return command;
}

HostInputCommand HostInputCommand::mouseButton(
    const QString &button,
    bool down)
{
    HostInputCommand command;
    command.type = Type::MouseButton;
    command.name = button;
    command.down = down;
    return command;
}

HostInputCommand HostInputCommand::click(const QString &button)
{
    HostInputCommand command;
    command.type = Type::Click;
    command.name = button;
    return command;
}

HostInputCommand HostInputCommand::wheel(int delta, bool horizontal)
{
    HostInputCommand command;
    command.type = Type::Wheel;
    command.delta = delta;
    command.horizontal = horizontal;
    return command;
}

HostInputCommand HostInputCommand::key(const QString &code, bool down)
{
    HostInputCommand command;
    command.type = Type::Key;
    command.name = code;
    command.down = down;
    return command;
}

HostInputCommand HostInputCommand::typeText(const QString &text)
{
    HostInputCommand command;
    command.type = Type::Text;
    command.text = text;
    return command;
}

HostInputCommand HostInputCommand::ctrlAltDelete()
{
    HostInputCommand command;
    command.type = Type::CtrlAltDelete;
    return command;
}

class HostInputWorker final : public QObject {
public:
    void setSink(std::shared_ptr<IInputSink> sink)
    {
        if (sink_) {
            sink_->releaseAll();
        }
        sink_ = std::move(sink);
    }

    void start(quint64 generation)
    {
        {
            QMutexLocker locker(&mutex_);
            generation_ = generation;
            accepting_ = true;
            scheduled_ = false;
            pending_.clear();
        }
        if (sink_) {
            sink_->releaseAll();
        }
    }

    void discard(quint64 generation)
    {
        {
            QMutexLocker locker(&mutex_);
            generation_ = generation;
            accepting_ = false;
            scheduled_ = false;
            pending_.clear();
        }
    }

    void releaseAfterStop(quint64 generation)
    {
        {
            QMutexLocker locker(&mutex_);
            if (generation != generation_ || accepting_) {
                return;
            }
        }
        if (sink_) {
            sink_->releaseAll();
        }
    }

    void releaseNow(quint64 generation)
    {
        {
            QMutexLocker locker(&mutex_);
            if (generation != generation_ || !accepting_) {
                return;
            }
            scheduled_ = false;
            pending_.clear();
        }
        if (sink_) {
            sink_->releaseAll();
        }
    }

    void submit(quint64 generation, const HostInputCommand &command)
    {
        bool schedule = false;
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_ || generation != generation_) {
                return;
            }
            if (command.type == HostInputCommand::Type::MoveAbsolute
                && !pending_.empty()
                && pending_.back().generation == generation
                && pending_.back().command.type
                    == HostInputCommand::Type::MoveAbsolute) {
                pending_.back().command = command;
            } else {
                pending_.push_back({generation, command});
            }
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (schedule) {
            scheduleDrain(generation);
        }
    }

private:
    struct PendingCommand {
        quint64 generation = 0;
        HostInputCommand command;
    };

    void scheduleDrain(quint64 generation)
    {
        QMetaObject::invokeMethod(
            this,
            [this, generation]() { drain(generation); },
            Qt::QueuedConnection);
    }

    void drain(quint64 generation)
    {
        constexpr int kBatchSize = 128;
        for (int index = 0; index < kBatchSize; ++index) {
            PendingCommand pending;
            {
                QMutexLocker locker(&mutex_);
                if (!accepting_
                    || generation != generation_
                    || pending_.empty()) {
                    scheduled_ = false;
                    return;
                }
                pending = std::move(pending_.front());
                pending_.pop_front();
            }
            if (pending.generation == generation) {
                apply(pending.command);
            }
        }

        bool schedule = false;
        {
            QMutexLocker locker(&mutex_);
            if (!accepting_
                || generation != generation_
                || pending_.empty()) {
                scheduled_ = false;
            } else {
                schedule = true;
            }
        }
        if (schedule) {
            scheduleDrain(generation);
        }
    }

    [[nodiscard]] QPointF mapToVirtualDesktop(
        const HostInputCommand &command) const
    {
        const double x = std::clamp(command.point.x(), 0.0, 1.0);
        const double y = std::clamp(command.point.y(), 0.0, 1.0);
        if (!sink_ || command.monitor.isEmpty()) {
            return {x, y};
        }
        const double desktopX =
            command.monitor.left() + x * command.monitor.width();
        const double desktopY =
            command.monitor.top() + y * command.monitor.height();
        const QRect desktop = sink_->virtualDesktop();
        return {
            std::clamp(
                (desktopX - desktop.left())
                    / std::max(1, desktop.width() - 1),
                0.0,
                1.0),
            std::clamp(
                (desktopY - desktop.top())
                    / std::max(1, desktop.height() - 1),
                0.0,
                1.0),
        };
    }

    void apply(const HostInputCommand &command)
    {
        if (!sink_) {
            return;
        }
        switch (command.type) {
        case HostInputCommand::Type::MoveAbsolute: {
            const QPointF point = mapToVirtualDesktop(command);
            sink_->moveAbsolute(point.x(), point.y());
            break;
        }
        case HostInputCommand::Type::MoveRelative:
            sink_->moveRelative(command.dx, command.dy);
            break;
        case HostInputCommand::Type::MouseButton:
            sink_->mouseButton(command.name, command.down);
            break;
        case HostInputCommand::Type::Click:
            sink_->click(command.name);
            break;
        case HostInputCommand::Type::Wheel:
            sink_->wheel(command.delta, command.horizontal);
            break;
        case HostInputCommand::Type::Key:
            sink_->key(command.name, command.down);
            break;
        case HostInputCommand::Type::Text:
            sink_->typeText(command.text);
            break;
        case HostInputCommand::Type::CtrlAltDelete:
            sink_->ctrlAltDelete();
            break;
        }
    }

    mutable QMutex mutex_;
    std::deque<PendingCommand> pending_;
    std::shared_ptr<IInputSink> sink_;
    quint64 generation_ = 0;
    bool accepting_ = false;
    bool scheduled_ = false;
};

HostInputPipeline::HostInputPipeline()
{
    worker_ = new HostInputWorker;
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskHostInput"));
    QObject::connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    thread_.start();
}

HostInputPipeline::~HostInputPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void HostInputPipeline::setSink(std::shared_ptr<IInputSink> sink)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, sink = std::move(sink)]() {
            worker->setSink(sink);
        },
        Qt::BlockingQueuedConnection);
}

void HostInputPipeline::start()
{
    if (running_ || !worker_ || !thread_.isRunning()) {
        return;
    }
    running_ = true;
    const quint64 generation = ++generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->start(generation);
        },
        Qt::BlockingQueuedConnection);
}

void HostInputPipeline::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    const quint64 generation = ++generation_;
    if (worker_) {
        worker_->discard(generation);
    }
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_, generation]() {
                worker->releaseAfterStop(generation);
            },
            Qt::BlockingQueuedConnection);
    }
}

void HostInputPipeline::releaseAll()
{
    if (!running_ || !worker_ || !thread_.isRunning()) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->releaseNow(generation);
        },
        Qt::BlockingQueuedConnection);
}

void HostInputPipeline::submit(const HostInputCommand &command)
{
    if (!running_ || !worker_) {
        return;
    }
    worker_->submit(generation_, command);
}

} // namespace pd
