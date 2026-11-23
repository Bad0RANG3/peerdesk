#pragma once

#include "session/file_transfer.h"

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>

namespace pd {

class FileTransferWorker;

class FileTransferPipeline final : public QObject {
    Q_OBJECT

public:
    explicit FileTransferPipeline(
        QString receiveDirectory = {},
        QObject *parent = nullptr);
    ~FileTransferPipeline() override;

    FileTransferPipeline(const FileTransferPipeline &) = delete;
    FileTransferPipeline &operator=(const FileTransferPipeline &) = delete;

    void setReceiveDirectory(const QString &directory);
    [[nodiscard]] QString receiveDirectory() const;

    [[nodiscard]] bool startSend(
        const QString &path,
        QString *error = nullptr);
    [[nodiscard]] bool resumeInterruptedSend(QString *error = nullptr);
    [[nodiscard]] bool cancelSend();
    [[nodiscard]] bool interruptSend(const QString &reason);
    [[nodiscard]] bool failSend(const QString &reason);
    [[nodiscard]] bool failSendFromPeer(
        const QString &id,
        const QString &reason);
    void acceptSendResume(const QJsonObject &message);
    void discardSendResume();
    void abortSend();

    [[nodiscard]] bool hasResumableSend() const;
    [[nodiscard]] bool sendActive() const { return sendActive_; }
    [[nodiscard]] QString sendId() const { return sendId_; }

    void beginReceive(const QJsonObject &message);
    void appendReceive(const QJsonObject &message);
    void finishReceive(const QString &id);
    void cancelReceive(const QString &id, const QString &reason);
    void abortAllReceives();

    [[nodiscard]] QString ioThreadName() const;

signals:
    void controlMessage(const QJsonObject &message);
    void outgoingFileStarted(
        const QString &id,
        const QString &name,
        qint64 size,
        const QString &sourcePath);
    void outgoingFileProgress(const QString &id, qint64 sent, qint64 total);
    void outgoingFileFinished(
        const QString &id,
        const QString &name,
        qint64 size);
    void outgoingFileFailed(const QString &id, const QString &reason);
    void outgoingFileCancelled(const QString &id, const QString &reason);
    void outgoingFileInterrupted(const QString &id, const QString &reason);

    void receiveFileStarted(
        const QString &id,
        const QString &name,
        qint64 size,
        bool resumed,
        qint64 offset);
    void receiveFileProgress(
        const QString &id,
        qint64 received,
        qint64 total);
    void receiveFileFinished(
        const QString &id,
        const QString &path,
        qint64 size);
    void receiveFileFailed(const QString &id, const QString &reason);
    void receiveFileCancelled(const QString &id, const QString &reason);

private:
    void rememberControlGeneration(quint64 generation);
    void onControlMessage(
        quint64 generation,
        const QJsonObject &message);
    void onSendProgress(
        quint64 generation,
        const QString &id,
        qint64 sent,
        qint64 total);
    void onSendCompleted(
        quint64 generation,
        const QString &id,
        const QString &name,
        qint64 size);
    void onSendFailed(
        quint64 generation,
        const QString &id,
        const QString &reason);

    QThread thread_;
    FileTransferWorker *worker_ = nullptr;
    QString receiveDirectory_;
    quint64 sendGeneration_ = 0;
    quint64 receiveGeneration_ = 0;
    QSet<quint64> controlGenerations_;
    bool sendActive_ = false;
    QString sendId_;
};

} // namespace pd
