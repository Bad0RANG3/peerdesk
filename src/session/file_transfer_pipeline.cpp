#include "session/file_transfer_pipeline.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <utility>

namespace pd {

namespace {

struct SendStartResult {
    bool ok = false;
    quint64 generation = 0;
    QString error;
    QString id;
    QString name;
    QString path;
    qint64 size = 0;
};

struct SendStopResult {
    bool active = false;
    quint64 generation = 0;
    QString id;
    QString reason;
};

QString effectiveReason(const QString &reason)
{
    return reason.isEmpty() ? QStringLiteral("io") : reason;
}

bool validResumeEnvelope(
    const QJsonObject &message,
    QString *reason)
{
    const bool resume =
        message.value(QStringLiteral("resume")).toBool(false);
    const QString resumeOf =
        message.value(QStringLiteral("resume_of")).toString();
    if ((resume && resumeOf.isEmpty())
        || (!resume && !resumeOf.isEmpty())) {
        if (reason) {
            *reason = QStringLiteral("resume");
        }
        return false;
    }
    return true;
}

} // namespace

class FileTransferWorker final : public QObject {
    Q_OBJECT

public:
    explicit FileTransferWorker(QString receiveDirectory)
        : receiver_(std::move(receiveDirectory))
        , sender_(new FileSender(this))
    {
        connect(
            sender_,
            &FileSender::progress,
            this,
            [this](const QString &id, qint64 sent, qint64 total) {
                emit sendProgress(
                    sendGeneration_, id, sent, total);
            });
        connect(
            sender_,
            &FileSender::completed,
            this,
            [this](
                const QString &id,
                const QString &name,
                qint64 size) {
                if (!suppressTerminalSignal_) {
                    emit sendCompleted(
                        sendGeneration_, id, name, size);
                }
            });
        connect(
            sender_,
            &FileSender::failed,
            this,
            [this](const QString &id, const QString &reason) {
                if (!suppressTerminalSignal_) {
                    emit sendFailed(
                        sendGeneration_, id, reason);
                }
            });
    }

    [[nodiscard]] SendStartResult startSend(
        const QString &path,
        bool resumeInterrupted)
    {
        SendStartResult result;
        if (sender_->active()) {
            result.error = QStringLiteral("busy");
            return result;
        }

        FileSender::ResumeOffer offer;
        QString actualPath = path;
        if (resumeInterrupted) {
            offer = sender_->interruptedOffer();
            if (!offer.valid()) {
                result.error = QStringLiteral("resume");
                return result;
            }
            actualPath = offer.path;
        } else {
            const FileSender::ResumeOffer saved =
                sender_->interruptedOffer();
            if (saved.valid()) {
                if (isSameFilePath(saved.path, path)) {
                    offer = saved;
                } else {
                    sender_->discardResume();
                }
            }
        }

        result.generation = ++sendSequence_;
        sendGeneration_ = result.generation;
        const quint64 generation = result.generation;
        QString error;
        if (!sender_->start(
                actualPath,
                [this, generation](const QJsonObject &message) {
                    emit controlMessage(generation, message);
                },
                &error,
                offer)) {
            result.error = error;
            return result;
        }

        result.ok = true;
        result.id = sender_->id();
        result.name = sender_->fileName();
        result.size = sender_->size();
        result.path = QFileInfo(actualPath).absoluteFilePath();
        return result;
    }

    [[nodiscard]] SendStopResult cancelSend()
    {
        SendStopResult result;
        result.generation = sendGeneration_;
        if (!sender_->active()) {
            return result;
        }
        result.active = true;
        result.id = sender_->id();
        result.reason = QStringLiteral("cancelled");
        sender_->cancel();
        return result;
    }

    [[nodiscard]] SendStopResult interruptSend(const QString &reason)
    {
        SendStopResult result;
        result.generation = sendGeneration_;
        if (!sender_->active()) {
            return result;
        }
        result.active = true;
        result.id = sender_->id();
        result.reason = reason.isEmpty()
            ? QStringLiteral("interrupted")
            : reason;
        sender_->interrupt(result.reason);
        result.generation = ++sendSequence_;
        sendGeneration_ = result.generation;
        return result;
    }

    [[nodiscard]] SendStopResult failSend(const QString &reason)
    {
        SendStopResult result;
        result.generation = sendGeneration_;
        if (!sender_->active()) {
            return result;
        }
        result.active = true;
        result.id = sender_->id();
        result.reason = effectiveReason(reason);
        suppressTerminalSignal_ = true;
        sender_->fail(result.reason);
        suppressTerminalSignal_ = false;
        return result;
    }

    [[nodiscard]] SendStopResult failSendFromPeer(
        const QString &id,
        const QString &reason)
    {
        SendStopResult result;
        result.generation = sendGeneration_;
        if (!sender_->active() || sender_->id() != id) {
            return result;
        }
        result.active = true;
        result.id = id;
        result.reason = effectiveReason(reason);
        suppressTerminalSignal_ = true;
        sender_->failFromPeer(result.reason);
        suppressTerminalSignal_ = false;
        return result;
    }

    void acceptSendResume(const QJsonObject &message)
    {
        if (!sender_->active() || !sender_->waitingForResume()) {
            return;
        }
        if (message.value(QStringLiteral("id")).toString()
            != sender_->id()) {
            return;
        }
        QString error;
        if (sender_->acceptResume(message, &error)
            || (error != QStringLiteral("resume_offset")
                && error != QStringLiteral("io"))) {
            return;
        }
        const QString id = sender_->id();
        suppressTerminalSignal_ = true;
        sender_->fail(error);
        suppressTerminalSignal_ = false;
        emit sendFailed(sendGeneration_, id, error);
    }

    [[nodiscard]] quint64 abortSend()
    {
        sender_->abort();
        sendGeneration_ = ++sendSequence_;
        return sendGeneration_;
    }

    void discardSendResume()
    {
        sender_->discardResume();
    }

    [[nodiscard]] bool hasResumableSend() const
    {
        return !sender_->active()
            && sender_->interruptedOffer().valid();
    }

    void setReceiveDirectory(
        quint64 generation,
        const QString &directory)
    {
        receiver_.abortAll();
        rejected_.clear();
        rejectedEmpty_ = false;
        receiver_.setDirectory(directory);
        receiveGeneration_ = generation;
    }

    void beginReceive(
        quint64 generation,
        const QJsonObject &message)
    {
        receiveGeneration_ = generation;
        const QString id =
            message.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            if (!rejectedEmpty_) {
                rejectedEmpty_ = true;
                emit receiveFailed(
                    generation,
                    id,
                    QStringLiteral("id"));
            }
            return;
        }
        QString reason;
        if (!validResumeEnvelope(message, &reason)) {
            markRejected(id);
            emit receiveFailed(generation, id, reason);
            return;
        }

        const QString name =
            message.value(QStringLiteral("name")).toString();
        const qint64 size = integerValue(message, "size");
        const QString resumeOf =
            message.value(QStringLiteral("resume_of")).toString();
        qint64 offset = 0;
        bool resumed = false;
        QString error;
        if (!receiver_.begin(
                id,
                name,
                size,
                &error,
                resumeOf,
                &offset,
                &resumed)) {
            markRejected(id);
            emit receiveFailed(generation, id, error);
            return;
        }

        rejected_.remove(id);
        emit receiveStarted(
            generation, id, name, size, resumed, offset);
    }

    void appendReceive(
        quint64 generation,
        const QJsonObject &message)
    {
        receiveGeneration_ = generation;
        const QString id =
            message.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            return;
        }
        if (rejected_.contains(id)) {
            return;
        }

        QByteArray data;
        QString error;
        if (!decodeFileChunk(
                message.value(QStringLiteral("d")).toString(),
                &data,
                &error)) {
            receiver_.abort(id);
            markRejected(id);
            emit receiveFailed(
                generation,
                id,
                error.isEmpty() ? QStringLiteral("data") : error);
            return;
        }
        if (!receiver_.append(id, data, &error)) {
            markRejected(id);
            emit receiveFailed(generation, id, error);
            return;
        }
        emit receiveProgress(
            generation,
            id,
            receiver_.received(id),
            receiver_.expected(id));
    }

    void finishReceive(quint64 generation, const QString &id)
    {
        receiveGeneration_ = generation;
        if (id.isEmpty()) {
            return;
        }
        if (rejected_.contains(id)) {
            return;
        }
        QString path;
        qint64 size = 0;
        QString error;
        if (!receiver_.finish(id, &path, &size, &error)) {
            markRejected(id);
            emit receiveFailed(generation, id, error);
            return;
        }
        rejected_.remove(id);
        emit receiveFinished(generation, id, path, size);
    }

    void cancelReceive(
        quint64 generation,
        const QString &id,
        const QString &reason)
    {
        receiveGeneration_ = generation;
        if (id.isEmpty()) {
            return;
        }
        receiver_.abort(id);
        markRejected(id);
        emit receiveCancelled(
            generation,
            id,
            reason.isEmpty()
                ? QStringLiteral("cancelled")
                : reason);
    }

    void abortAllReceives(quint64 generation)
    {
        receiveGeneration_ = generation;
        receiver_.abortAll();
        rejected_.clear();
        rejectedEmpty_ = false;
    }

    [[nodiscard]] QString threadName() const
    {
        return QThread::currentThread()->objectName();
    }

signals:
    void controlMessage(
        quint64 generation,
        const QJsonObject &message);
    void sendProgress(
        quint64 generation,
        const QString &id,
        qint64 sent,
        qint64 total);
    void sendCompleted(
        quint64 generation,
        const QString &id,
        const QString &name,
        qint64 size);
    void sendFailed(
        quint64 generation,
        const QString &id,
        const QString &reason);
    void receiveStarted(
        quint64 generation,
        const QString &id,
        const QString &name,
        qint64 size,
        bool resumed,
        qint64 offset);
    void receiveProgress(
        quint64 generation,
        const QString &id,
        qint64 received,
        qint64 total);
    void receiveFinished(
        quint64 generation,
        const QString &id,
        const QString &path,
        qint64 size);
    void receiveFailed(
        quint64 generation,
        const QString &id,
        const QString &reason);
    void receiveCancelled(
        quint64 generation,
        const QString &id,
        const QString &reason);

private:
    static qint64 integerValue(
        const QJsonObject &message,
        const char *key)
    {
        const QJsonValue value =
            message.value(QLatin1String(key));
        if (value.isDouble()) {
            return static_cast<qint64>(value.toDouble());
        }
        if (value.isString()) {
            bool ok = false;
            const qint64 parsed =
                value.toString().toLongLong(&ok);
            return ok ? parsed : 0;
        }
        return 0;
    }

    void markRejected(const QString &id)
    {
        if (id.isEmpty()) {
            return;
        }
        if (rejected_.size() >= 256) {
            rejected_.clear();
        }
        rejected_.insert(id);
    }

    FileReceiver receiver_;
    FileSender *sender_ = nullptr;
    QSet<QString> rejected_;
    bool rejectedEmpty_ = false;
    quint64 sendSequence_ = 0;
    quint64 sendGeneration_ = 0;
    quint64 receiveGeneration_ = 0;
    bool suppressTerminalSignal_ = false;
};

FileTransferPipeline::FileTransferPipeline(
    QString receiveDirectory,
    QObject *parent)
    : QObject(parent)
    , receiveDirectory_(std::move(receiveDirectory))
{
    worker_ = new FileTransferWorker(receiveDirectory_);
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskFileTransfer"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &FileTransferWorker::controlMessage,
        this,
        &FileTransferPipeline::onControlMessage);
    connect(
        worker_,
        &FileTransferWorker::sendProgress,
        this,
        &FileTransferPipeline::onSendProgress);
    connect(
        worker_,
        &FileTransferWorker::sendCompleted,
        this,
        &FileTransferPipeline::onSendCompleted);
    connect(
        worker_,
        &FileTransferWorker::sendFailed,
        this,
        &FileTransferPipeline::onSendFailed);
    connect(
        worker_,
        &FileTransferWorker::receiveStarted,
        this,
        [this](
            quint64 generation,
            const QString &id,
            const QString &name,
            qint64 size,
            bool resumed,
            qint64 offset) {
            if (generation == receiveGeneration_) {
                emit receiveFileStarted(
                    id, name, size, resumed, offset);
            }
        });
    connect(
        worker_,
        &FileTransferWorker::receiveProgress,
        this,
        [this](
            quint64 generation,
            const QString &id,
            qint64 received,
            qint64 total) {
            if (generation == receiveGeneration_) {
                emit receiveFileProgress(id, received, total);
            }
        });
    connect(
        worker_,
        &FileTransferWorker::receiveFinished,
        this,
        [this](
            quint64 generation,
            const QString &id,
            const QString &path,
            qint64 size) {
            if (generation == receiveGeneration_) {
                emit receiveFileFinished(id, path, size);
            }
        });
    connect(
        worker_,
        &FileTransferWorker::receiveFailed,
        this,
        [this](
            quint64 generation,
            const QString &id,
            const QString &reason) {
            if (generation == receiveGeneration_) {
                emit receiveFileFailed(id, reason);
            }
        });
    connect(
        worker_,
        &FileTransferWorker::receiveCancelled,
        this,
        [this](
            quint64 generation,
            const QString &id,
            const QString &reason) {
            if (generation == receiveGeneration_) {
                emit receiveFileCancelled(id, reason);
            }
        });
    thread_.start();
}

FileTransferPipeline::~FileTransferPipeline()
{
    blockSignals(true);
    abortAllReceives();
    abortSend();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

void FileTransferPipeline::setReceiveDirectory(
    const QString &directory)
{
    receiveDirectory_ = directory;
    const quint64 generation = ++receiveGeneration_;
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, directory]() {
            worker->setReceiveDirectory(generation, directory);
        },
        Qt::BlockingQueuedConnection);
}

QString FileTransferPipeline::receiveDirectory() const
{
    return receiveDirectory_;
}

bool FileTransferPipeline::startSend(
    const QString &path,
    QString *error)
{
    if (!worker_ || !thread_.isRunning()) {
        if (error) {
            *error = QStringLiteral("offline");
        }
        return false;
    }
    SendStartResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, path, &result]() {
            result = worker->startSend(path, false);
        },
        Qt::BlockingQueuedConnection);
    if (!result.ok) {
        if (error) {
            *error = result.error;
        }
        return false;
    }

    sendGeneration_ = result.generation;
    sendActive_ = true;
    sendId_ = result.id;
    controlGenerations_.remove(result.generation);
    emit outgoingFileStarted(
        result.id, result.name, result.size, result.path);
    if (error) {
        error->clear();
    }
    return true;
}

bool FileTransferPipeline::resumeInterruptedSend(QString *error)
{
    if (!worker_ || !thread_.isRunning()) {
        if (error) {
            *error = QStringLiteral("offline");
        }
        return false;
    }
    SendStartResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, &result]() {
            result = worker->startSend({}, true);
        },
        Qt::BlockingQueuedConnection);
    if (!result.ok) {
        if (error) {
            *error = result.error;
        }
        return false;
    }

    sendGeneration_ = result.generation;
    sendActive_ = true;
    sendId_ = result.id;
    controlGenerations_.remove(result.generation);
    emit outgoingFileStarted(
        result.id, result.name, result.size, result.path);
    if (error) {
        error->clear();
    }
    return true;
}

bool FileTransferPipeline::cancelSend()
{
    if (!worker_ || !thread_.isRunning()) {
        return false;
    }
    SendStopResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, &result]() {
            result = worker->cancelSend();
        },
        Qt::BlockingQueuedConnection);
    if (!result.active) {
        return false;
    }
    rememberControlGeneration(result.generation);
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileCancelled(result.id, result.reason);
    return true;
}

bool FileTransferPipeline::interruptSend(const QString &reason)
{
    if (!worker_ || !thread_.isRunning()) {
        return false;
    }
    SendStopResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, reason, &result]() {
            result = worker->interruptSend(reason);
        },
        Qt::BlockingQueuedConnection);
    if (!result.active) {
        return false;
    }
    controlGenerations_.remove(sendGeneration_);
    sendGeneration_ = result.generation;
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileInterrupted(result.id, result.reason);
    return true;
}

bool FileTransferPipeline::failSend(const QString &reason)
{
    if (!worker_ || !thread_.isRunning()) {
        return false;
    }
    SendStopResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, reason, &result]() {
            result = worker->failSend(reason);
        },
        Qt::BlockingQueuedConnection);
    if (!result.active) {
        return false;
    }
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileFailed(result.id, result.reason);
    return true;
}

bool FileTransferPipeline::failSendFromPeer(
    const QString &id,
    const QString &reason)
{
    if (!worker_ || !thread_.isRunning()) {
        return false;
    }
    SendStopResult result;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, id, reason, &result]() {
            result = worker->failSendFromPeer(id, reason);
        },
        Qt::BlockingQueuedConnection);
    if (!result.active) {
        return false;
    }
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileFailed(result.id, result.reason);
    return true;
}

void FileTransferPipeline::acceptSendResume(
    const QJsonObject &message)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, message]() {
            worker->acceptSendResume(message);
        },
        Qt::QueuedConnection);
}

void FileTransferPipeline::discardSendResume()
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_]() {
            worker->discardSendResume();
        },
        Qt::BlockingQueuedConnection);
}

void FileTransferPipeline::abortSend()
{
    const quint64 previous = sendGeneration_;
    sendActive_ = false;
    sendId_.clear();
    controlGenerations_.remove(previous);
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    quint64 generation = 0;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, &generation]() {
            generation = worker->abortSend();
        },
        Qt::BlockingQueuedConnection);
    sendGeneration_ = generation;
}

bool FileTransferPipeline::hasResumableSend() const
{
    if (!worker_ || !thread_.isRunning()) {
        return false;
    }
    bool resumable = false;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, &resumable]() {
            resumable = worker->hasResumableSend();
        },
        Qt::BlockingQueuedConnection);
    return resumable;
}

void FileTransferPipeline::beginReceive(
    const QJsonObject &message)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    const quint64 generation = receiveGeneration_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, message]() {
            worker->beginReceive(generation, message);
        },
        Qt::QueuedConnection);
}

void FileTransferPipeline::appendReceive(
    const QJsonObject &message)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    const quint64 generation = receiveGeneration_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, message]() {
            worker->appendReceive(generation, message);
        },
        Qt::QueuedConnection);
}

void FileTransferPipeline::finishReceive(const QString &id)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    const quint64 generation = receiveGeneration_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, id]() {
            worker->finishReceive(generation, id);
        },
        Qt::QueuedConnection);
}

void FileTransferPipeline::cancelReceive(
    const QString &id,
    const QString &reason)
{
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    const quint64 generation = receiveGeneration_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, id, reason]() {
            worker->cancelReceive(generation, id, reason);
        },
        Qt::QueuedConnection);
}

void FileTransferPipeline::abortAllReceives()
{
    const quint64 generation = ++receiveGeneration_;
    if (!worker_ || !thread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            worker->abortAllReceives(generation);
        },
        Qt::BlockingQueuedConnection);
}

QString FileTransferPipeline::ioThreadName() const
{
    if (!worker_ || !thread_.isRunning()) {
        return {};
    }
    QString name;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, &name]() {
            name = worker->threadName();
        },
        Qt::BlockingQueuedConnection);
    return name;
}

void FileTransferPipeline::onSendProgress(
    quint64 generation,
    const QString &id,
    qint64 sent,
    qint64 total)
{
    if (!sendActive_ || generation != sendGeneration_) {
        return;
    }
    emit outgoingFileProgress(id, sent, total);
}

void FileTransferPipeline::onSendCompleted(
    quint64 generation,
    const QString &id,
    const QString &name,
    qint64 size)
{
    if (!sendActive_ || generation != sendGeneration_) {
        return;
    }
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileFinished(id, name, size);
}

void FileTransferPipeline::onSendFailed(
    quint64 generation,
    const QString &id,
    const QString &reason)
{
    if (!sendActive_ || generation != sendGeneration_) {
        return;
    }
    sendActive_ = false;
    sendId_.clear();
    emit outgoingFileFailed(id, reason);
}

void FileTransferPipeline::rememberControlGeneration(
    quint64 generation)
{
    if (generation != 0) {
        controlGenerations_.insert(generation);
    }
}

void FileTransferPipeline::onControlMessage(
    quint64 generation,
    const QJsonObject &message)
{
    if (generation == sendGeneration_) {
        emit controlMessage(message);
        return;
    }
    if (controlGenerations_.contains(generation)) {
        emit controlMessage(message);
        if (message.value(QStringLiteral("t")).toString()
            == QStringLiteral("file_cancel")) {
            controlGenerations_.remove(generation);
        }
    }
}

} // namespace pd

#include "file_transfer_pipeline.moc"
