#pragma once

#include "session/file_transfer.h"
#include "session/session_types.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QTimer;

namespace pd {

class PeerLink;

class ViewerSession final : public QObject {
    Q_OBJECT

public:
    explicit ViewerSession(PeerLink *link, QObject *parent = nullptr);
    ~ViewerSession() override;

    void start();
    void stop();
    [[nodiscard]] bool active() const { return started_; }

    void setReceiveDirectory(const QString &directory);
    [[nodiscard]] QString receiveDirectory() const;

    [[nodiscard]] QJsonObject info() const { return info_; }
    [[nodiscard]] QJsonObject stats() const { return stats_; }
    [[nodiscard]] QJsonObject audioState() const { return audioState_; }
    [[nodiscard]] quint32 lastVideoSequence() const { return videoSequence_; }

    [[nodiscard]] bool sendControl(const QJsonObject &message);
    void requestKeyframe();
    void setQuality(int quality);
    void setMaxWidth(int maxWidth);
    void setFps(int fps);
    void selectMonitor(int index);
    void setAudioEnabled(bool on);

    void sendMouseMove(double nx, double ny);
    void sendMouseButton(
        const QString &action,
        double nx,
        double ny,
        const QString &button = QStringLiteral("left"));
    void sendMouseWheel(int delta, bool horizontal = false);
    void sendMouseRelative(int dx, int dy);
    [[nodiscard]] bool sendKey(const QString &code, bool down);
    void sendText(const QString &text);
    [[nodiscard]] bool sendClipboard(const QString &text);
    void sendCtrlAltDelete();

    [[nodiscard]] bool beginUpload(
        const QString &name,
        qint64 size,
        QString *error = nullptr);
    [[nodiscard]] bool uploadChunk(
        const QByteArray &data,
        QString *error = nullptr);
    [[nodiscard]] bool endUpload(QString *error = nullptr);
    [[nodiscard]] bool uploadActive() const { return uploadActive_; }
    [[nodiscard]] QString uploadId() const { return uploadId_; }
    [[nodiscard]] qint64 uploadSent() const { return uploadSent_; }
    [[nodiscard]] qint64 uploadSize() const { return uploadSize_; }

    [[nodiscard]] bool sendFile(
        const QString &path,
        QString *error = nullptr);
    [[nodiscard]] bool cancelFile();

signals:
    void videoFrameReceived(
        const QByteArray &jpeg,
        quint32 sequence,
        bool keyframe);
    void frameDecoded(const QImage &image, bool keyframe);
    void audioReceived(
        const QByteArray &pcm16,
        quint32 sequence,
        quint32 timestampMs);
    void eventReceived(const QJsonObject &message);
    void infoChanged(const QJsonObject &info);
    void statsChanged(const QJsonObject &stats);
    void audioStateChanged(const QJsonObject &state);
    void fileReceived(const QString &path, qint64 size);
    void outgoingFileStarted(
        const QString &id,
        const QString &name,
        qint64 size);
    void outgoingFileProgress(const QString &id, qint64 sent, qint64 total);
    void outgoingFileFinished(
        const QString &id,
        const QString &name,
        qint64 size);
    void outgoingFileFailed(const QString &id, const QString &reason);
    void outgoingFileCancelled(const QString &id, const QString &reason);
    void outgoingFileInterrupted(const QString &id, const QString &reason);
    void logMessage(const QString &message);

private:
    void onLinkStateChanged(const QString &state);
    void onVideoFrame(const QByteArray &jpeg, bool keyframe);
    void onAudioPacket(const QByteArray &payload);
    void dispatch(const QJsonObject &message);
    void handleFileBegin(const QJsonObject &message);
    void handleFileData(const QJsonObject &message);
    void handleFileEnd(const QJsonObject &message);
    void handleRemoteFileResult(const QJsonObject &message);
    void rejectFile(const QString &id, const QString &reason);
    void announceUpload(const QString &state, const QJsonObject &extra = {});
    void sendControlNoReply(const QJsonObject &message);

    PeerLink *link_ = nullptr;
    bool started_ = false;
    QJsonObject info_;
    QJsonObject stats_;
    QJsonObject audioState_;
    quint32 videoSequence_ = 0;
    quint64 audioFrames_ = 0;

    QString receiveDirectory_;
    FileReceiver receiver_;
    FileSender *fileSender_ = nullptr;

    bool uploadActive_ = false;
    QString uploadId_;
    QString uploadName_;
    qint64 uploadSize_ = 0;
    qint64 uploadSent_ = 0;
    QString pendingUploadId_;
    QString pendingUploadName_;
    qint64 pendingUploadSize_ = 0;
};

} // namespace pd
