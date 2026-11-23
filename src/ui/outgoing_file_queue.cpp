#include "ui/outgoing_file_queue.h"

#include <QFileInfo>

#include <algorithm>

namespace pd::ui {

int OutgoingFileQueue::enqueue(const QStringList &paths)
{
    if (!active() && !hasPending() && !hasRetryable()) {
        tasks_.clear();
        activeIndex_ = -1;
    }

    int added = 0;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const auto duplicate = std::find_if(
            tasks_.cbegin(),
            tasks_.cend(),
            [&trimmed](const Task &task) {
                return task.path == trimmed
                    && task.state != TaskState::Succeeded
                    && task.state != TaskState::Cancelled;
            });
        if (duplicate != tasks_.cend()) {
            continue;
        }

        Task task;
        task.path = trimmed;
        task.name = QFileInfo(trimmed).fileName();
        if (task.name.isEmpty()) {
            task.name = trimmed;
        }
        tasks_.push_back(std::move(task));
        ++added;
    }
    return added;
}

void OutgoingFileQueue::clear()
{
    tasks_.clear();
    seenIds_.clear();
    activeIndex_ = -1;
    paused_ = false;
}

OutgoingFileQueue::StartResult OutgoingFileQueue::startNext(
    const Sender &sender,
    QString *error)
{
    if (active() || paused_ || !sender) {
        return StartResult::NoTask;
    }
    const auto waiting = std::find_if(
        tasks_.begin(),
        tasks_.end(),
        [](const Task &task) {
            return task.state == TaskState::Waiting;
        });
    if (waiting == tasks_.end()) {
        return StartResult::NoTask;
    }

    const int index = static_cast<int>(waiting - tasks_.begin());
    Task &task = tasks_[static_cast<std::size_t>(index)];
    task.state = TaskState::Sending;
    task.id.clear();
    task.error.clear();
    ++task.attempts;
    activeIndex_ = index;

    QString sendError;
    if (!sender(task.path, &sendError)) {
        if (activeIndex_ == index
            && tasks_[static_cast<std::size_t>(index)].state
                == TaskState::Sending) {
            Task &rejected = tasks_[static_cast<std::size_t>(index)];
            rejected.state = TaskState::Failed;
            rejected.error = sendError;
            rejected.sent = 0;
            activeIndex_ = -1;
        }
        if (error) {
            *error = sendError;
        }
        return StartResult::Rejected;
    }
    return StartResult::Started;
}

void OutgoingFileQueue::pause()
{
    paused_ = true;
}

void OutgoingFileQueue::resume()
{
    paused_ = false;
    for (Task &task : tasks_) {
        if (task.state == TaskState::Interrupted) {
            task.state = TaskState::Waiting;
        }
    }
}

bool OutgoingFileQueue::retryFailed()
{
    bool changed = false;
    for (Task &task : tasks_) {
        if (task.state != TaskState::Failed) {
            continue;
        }
        task.state = TaskState::Waiting;
        task.id.clear();
        task.error.clear();
        task.sent = 0;
        changed = true;
    }
    return changed;
}

bool OutgoingFileQueue::interruptActive(const QString &reason)
{
    Task *task = mutableActiveTask();
    if (!task) {
        return false;
    }
    task->state = TaskState::Interrupted;
    task->error = reason;
    activeIndex_ = -1;
    return true;
}

bool OutgoingFileQueue::markStarted(
    const QString &id,
    const QString &name,
    qint64 size)
{
    if (id.isEmpty() || seenIds_.contains(id)) {
        return false;
    }
    if (Task *task = mutableActiveTask()) {
        if (!task->id.isEmpty() && task->id != id) {
            return false;
        }
        seenIds_.insert(id);
        task->id = id;
        if (!name.isEmpty()) {
            task->name = name;
        }
        if (size > 0) {
            task->size = size;
        }
        task->state = TaskState::Sending;
        return true;
    }

    const auto matches = [&name, size](const Task &task) {
        if (name.isEmpty() || task.name != name) {
            return false;
        }
        return size <= 0 || task.size <= 0 || task.size == size;
    };
    const auto reusable = [&matches](const Task &task) {
        return task.state == TaskState::Interrupted && matches(task);
    };
    auto candidate = std::find_if(
        tasks_.rbegin(),
        tasks_.rend(),
        reusable);
    if (candidate != tasks_.rend()) {
        const int index = static_cast<int>(
            std::distance(tasks_.begin(), candidate.base()) - 1);
        Task &task = tasks_[static_cast<std::size_t>(index)];
        seenIds_.insert(id);
        task.id = id;
        task.error.clear();
        if (size > 0) {
            task.size = size;
        }
        task.state = TaskState::Sending;
        activeIndex_ = index;
        return true;
    }

    const auto waiting = std::find_if(
        tasks_.begin(),
        tasks_.end(),
        [&matches](const Task &task) {
            return task.state == TaskState::Waiting && matches(task);
        });
    if (waiting != tasks_.end()) {
        const int index = static_cast<int>(
            std::distance(tasks_.begin(), waiting));
        Task &task = *waiting;
        seenIds_.insert(id);
        task.id = id;
        task.error.clear();
        if (size > 0) {
            task.size = size;
        }
        if (task.attempts == 0) {
            task.attempts = 1;
        }
        task.state = TaskState::Sending;
        activeIndex_ = index;
        return true;
    }

    Task task;
    task.name = name;
    task.id = id;
    task.size = std::max<qint64>(size, 0);
    task.attempts = 1;
    task.state = TaskState::Sending;
    seenIds_.insert(id);
    tasks_.push_back(std::move(task));
    activeIndex_ = static_cast<int>(tasks_.size()) - 1;
    return true;
}

bool OutgoingFileQueue::markProgress(
    const QString &id,
    qint64 sent,
    qint64 total)
{
    Task *task = mutableActiveTask();
    if (!task || task->id != id) {
        return false;
    }
    if (total > 0) {
        task->size = total;
        task->sent = std::clamp<qint64>(sent, 0, total);
    } else {
        task->sent = std::max<qint64>(sent, 0);
    }
    return true;
}

bool OutgoingFileQueue::markFinished(
    const QString &id,
    const QString &name,
    qint64 size)
{
    Task *task = mutableActiveTask();
    if (!task || task->id != id) {
        return false;
    }
    task->state = TaskState::Succeeded;
    if (!name.isEmpty()) {
        task->name = name;
    }
    if (size > 0) {
        task->size = size;
        task->sent = size;
    } else {
        task->sent = task->size;
    }
    activeIndex_ = -1;
    return true;
}

bool OutgoingFileQueue::markFailed(
    const QString &id,
    const QString &reason)
{
    Task *task = mutableActiveTask();
    if (!task || task->id != id) {
        return false;
    }
    task->state = TaskState::Failed;
    task->error = reason;
    task->sent = 0;
    activeIndex_ = -1;
    return true;
}

bool OutgoingFileQueue::markCancelled(
    const QString &id,
    const QString &reason)
{
    Task *task = mutableActiveTask();
    if (!task || task->id != id) {
        return false;
    }
    task->state = TaskState::Cancelled;
    task->error = reason;
    activeIndex_ = -1;
    return true;
}

bool OutgoingFileQueue::markInterrupted(
    const QString &id,
    const QString &reason)
{
    Task *task = mutableActiveTask();
    if (!task || task->id != id) {
        return false;
    }
    task->state = TaskState::Interrupted;
    task->error = reason;
    activeIndex_ = -1;
    return true;
}

bool OutgoingFileQueue::hasPending() const
{
    return std::any_of(
        tasks_.cbegin(),
        tasks_.cend(),
        [](const Task &task) {
            return task.state == TaskState::Waiting
                || task.state == TaskState::Sending;
        });
}

bool OutgoingFileQueue::hasFailed() const
{
    return failedCount() > 0;
}

bool OutgoingFileQueue::hasRetryable() const
{
    return std::any_of(
        tasks_.cbegin(),
        tasks_.cend(),
        [](const Task &task) {
            return task.state == TaskState::Failed
                || task.state == TaskState::Interrupted;
        });
}

int OutgoingFileQueue::succeededCount() const
{
    return static_cast<int>(std::count_if(
        tasks_.cbegin(),
        tasks_.cend(),
        [](const Task &task) {
            return task.state == TaskState::Succeeded;
        }));
}

int OutgoingFileQueue::failedCount() const
{
    return static_cast<int>(std::count_if(
        tasks_.cbegin(),
        tasks_.cend(),
        [](const Task &task) {
            return task.state == TaskState::Failed;
        }));
}

int OutgoingFileQueue::cancelledCount() const
{
    return static_cast<int>(std::count_if(
        tasks_.cbegin(),
        tasks_.cend(),
        [](const Task &task) {
            return task.state == TaskState::Cancelled;
        }));
}

qint64 OutgoingFileQueue::succeededBytes() const
{
    qint64 bytes = 0;
    for (const Task &task : tasks_) {
        if (task.state == TaskState::Succeeded) {
            bytes += std::max<qint64>(task.size, 0);
        }
    }
    return bytes;
}

const OutgoingFileQueue::Task *OutgoingFileQueue::activeTask() const
{
    if (activeIndex_ < 0
        || activeIndex_ >= static_cast<int>(tasks_.size())) {
        return nullptr;
    }
    return &tasks_[static_cast<std::size_t>(activeIndex_)];
}

OutgoingFileQueue::Task *OutgoingFileQueue::mutableActiveTask()
{
    if (activeIndex_ < 0
        || activeIndex_ >= static_cast<int>(tasks_.size())) {
        return nullptr;
    }
    return &tasks_[static_cast<std::size_t>(activeIndex_)];
}

} // namespace pd::ui
