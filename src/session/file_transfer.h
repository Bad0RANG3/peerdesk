#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QString>

#include <functional>
#include <map>
#include <memory>

class QFile;
class QTimer;

namespace pd {

inline constexpr int kFileChunkBytes = 24 * 1024;
inline constexpr qint64 kMaxFileBytes = 512LL * 1024 * 1024;
inline constexpr int kFileResumeTimeoutMs = 8000;
inline constexpr qint64 kFileResumeRetentionMs = 10LL * 60 * 1000;

[[nodiscard]] bool isSafeBaseName(const QString &name);
[[nodiscard]] bool isSameFilePath(
    const QString &left,
    const QString &right);
[[nodiscard]] QString uniqueFilePath(
    const QString &directory,
    const QString &fileName,
    const QSet<QString> &reserved = {});
[[nodiscard]] QString makeTransferId();
[[nodiscard]] bool decodeFileChunk(
    const QString &encoded,
    QByteArray *data,
    QString *error = nullptr);

class FileReceiver {
public:
    explicit FileReceiver(QString directory);

    [[nodiscard]] QString directory() const;
    void setDirectory(const QString &directory);

    [[nodiscard]] bool begin(
        const QString &id,
        const QString &name,
        qint64 size,
        QString *error = nullptr,
        const QString &resumeOf = {},
        qint64 *resumeOffset = nullptr,
        bool *resumed = nullptr);
    [[nodiscard]] bool append(
        const QString &id,
        const QByteArray &data,
        QString *error = nullptr);
    [[nodiscard]] bool finish(
        const QString &id,
        QString *savedPath = nullptr,
        qint64 *savedBytes = nullptr,
        QString *error = nullptr);

    void abort(const QString &id);
    void abortAll();
    void pruneIdle(qint64 maxIdleMs = kFileResumeRetentionMs);

    [[nodiscard]] bool contains(const QString &id) const;
    [[nodiscard]] qint64 received(const QString &id) const;
    [[nodiscard]] qint64 expected(const QString &id) const;

private:
    struct Entry {
        QString path;
        QString name;
        qint64 size = 0;
        qint64 got = 0;
        qint64 touchedAt = 0;
        std::unique_ptr<QSaveFile> file;
    };

    void releaseReservation(const Entry &entry);

    QString directory_;
    std::map<QString, Entry> entries_;
    QSet<QString> reserved_;
};

class FileSender : public QObject {
    Q_OBJECT

public:
    using Sender = std::function<void(const QJsonObject &)>;

    struct ResumeOffer {
        QString id;
        QString path;
        QString name;
        qint64 size = 0;
        qint64 offset = 0;

        [[nodiscard]] bool valid() const
        {
            return !id.isEmpty() && !path.isEmpty();
        }
    };

    explicit FileSender(QObject *parent = nullptr);
    ~FileSender() override;

    [[nodiscard]] bool start(
        const QString &path,
        Sender sender,
        QString *error = nullptr);
    [[nodiscard]] bool start(
        const QString &path,
        Sender sender,
        QString *error,
        const ResumeOffer &resume);
    [[nodiscard]] bool acceptResume(
        const QJsonObject &message,
        QString *error = nullptr);
    void setResumeTimeoutMs(int timeoutMs);
    void discardResume();
    void cancel();
    void fail(const QString &reason);
    void failFromPeer(const QString &reason);
    void interrupt(const QString &reason);
    void abort();

    [[nodiscard]] bool active() const { return !id_.isEmpty(); }
    [[nodiscard]] QString id() const { return id_; }
    [[nodiscard]] QString fileName() const { return name_; }
    [[nodiscard]] qint64 size() const { return size_; }
    [[nodiscard]] qint64 sent() const { return sent_; }
    [[nodiscard]] bool waitingForResume() const { return awaitingResume_; }
    [[nodiscard]] ResumeOffer interruptedOffer() const
    {
        return interruptedOffer_;
    }

signals:
    void started(
        const QString &id,
        const QString &name,
        qint64 size,
        const QString &sourcePath);
    void progress(const QString &id, qint64 sent, qint64 total);
    void completed(const QString &id, const QString &name, qint64 size);
    void failed(const QString &id, const QString &reason);
    void cancelled(const QString &id, const QString &reason);
    void interrupted(const QString &id, const QString &reason);

private:
    void pump();
    void resumeTimedOut();
    void cleanup();

    QTimer *timer_ = nullptr;
    QTimer *resumeTimer_ = nullptr;
    std::unique_ptr<QFile> file_;
    Sender sender_;
    ResumeOffer interruptedOffer_;
    QString path_;
    QString id_;
    QString name_;
    QString resumeOf_;
    qint64 size_ = 0;
    qint64 sent_ = 0;
    int resumeTimeoutMs_ = kFileResumeTimeoutMs;
    bool awaitingResume_ = false;
};

} // namespace pd
