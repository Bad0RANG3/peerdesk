#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace pd::ui {

class OutgoingFileQueue final {
public:
    enum class TaskState {
        Waiting,
        Sending,
        Succeeded,
        Failed,
        Cancelled,
        Interrupted,
    };

    struct Task {
        QString path;
        QString name;
        QString id;
        QString error;
        qint64 size = 0;
        qint64 sent = 0;
        int attempts = 0;
        TaskState state = TaskState::Waiting;
    };

    enum class StartResult {
        NoTask,
        Started,
        Rejected,
    };

    using Sender = std::function<bool(const QString &, QString *)>;

    int enqueue(const QStringList &paths);
    void clear();

    StartResult startNext(const Sender &sender, QString *error = nullptr);
    void pause();
    void resume();
    bool retryFailed();
    bool interruptActive(const QString &reason = {});

    bool markStarted(
        const QString &id,
        const QString &name,
        qint64 size);
    bool markProgress(
        const QString &id,
        qint64 sent,
        qint64 total);
    bool markFinished(
        const QString &id,
        const QString &name,
        qint64 size);
    bool markFailed(const QString &id, const QString &reason);
    bool markCancelled(const QString &id, const QString &reason);
    bool markInterrupted(const QString &id, const QString &reason);

    [[nodiscard]] bool empty() const { return tasks_.empty(); }
    [[nodiscard]] bool active() const { return activeIndex_ >= 0; }
    [[nodiscard]] bool paused() const { return paused_; }
    [[nodiscard]] bool hasPending() const;
    [[nodiscard]] bool hasFailed() const;
    [[nodiscard]] bool hasRetryable() const;
    [[nodiscard]] int size() const
    {
        return static_cast<int>(tasks_.size());
    }
    [[nodiscard]] int activePosition() const
    {
        return activeIndex_ >= 0 ? activeIndex_ + 1 : 0;
    }
    [[nodiscard]] int succeededCount() const;
    [[nodiscard]] int failedCount() const;
    [[nodiscard]] int cancelledCount() const;
    [[nodiscard]] qint64 succeededBytes() const;
    [[nodiscard]] const Task *activeTask() const;

private:
    Task *mutableActiveTask();

    std::vector<Task> tasks_;
    QSet<QString> seenIds_;
    int activeIndex_ = -1;
    bool paused_ = false;
};

} // namespace pd::ui
