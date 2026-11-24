#pragma once

#include "core/protocol.h"
#include "session/file_transfer_pipeline.h"
#include "session/session_types.h"
#include "session/viewer_audio_pipeline.h"
#include "session/viewer_video_pipeline.h"

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
    void suspend();
    void stop();
    [[nodiscard]] bool active() const { return started_; }

    void setReceiveDirectory(const QString &directory);
    [[nodiscard]] QString receiveDirectory() const;
    [[nodiscard]] QString fileTransferThreadName() const
    {
        return fileTransfer_.ioThreadName();
    }

    [[nodiscard]] QJsonObject info() const { return info_; }
    [[nodiscard]] QJsonObject stats() const { return stats_; }
    [[nodiscard]] QJsonObject audioState() const { return audioState_; }
    [[nodiscard]] quint32 lastVideoSequence() const { return videoSequence_; }
    [[nodiscard]] QString videoDecoderThreadName() const
    {
        return video_.decoderThreadName();
    }
    [[nodiscard]] QString audioDecoderThreadName() const
    {
        return audio_.decoderThreadName();
    }

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
    void sendMouseRelativeButton(
        const QString &action,
        const QString &button = QStringLiteral("left"));
    [[nodiscard]] bool sendKey(const QString &code, bool down);
    void sendText(const QString &text);
    [[nodiscard]] bool sendClipboard(const QString &text);
    [[nodiscard]] bool sendClipboardImage(const QImage &image);
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
        const QByteArray &data,
        quint32 sequence,
        bool keyframe,
        pd::VideoCodec codec);
    void frameDecoded(const QImage &image, bool keyframe);
    void audioReceived(
        const QByteArray &pcm16,
        quint32 sequence,
        quint32 timestampMs);
    void remoteCursorChanged(const pd::CursorPacket &cursor);
    void eventReceived(const QJsonObject &message);
    void infoChanged(const QJsonObject &info);
    void statsChanged(const QJsonObject &stats);
    void audioStateChanged(const QJsonObject &state);
    void clipboardImageReceived(const QImage &image);
    void fileReceived(const QString &path, qint64 size);
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
    void logMessage(const QString &message);

private:
    void onLinkStateChanged(const QString &state);
    void onVideoFrame(
        const QByteArray &data,
        bool keyframe,
        VideoCodec codec);
    void onAudioPacket(const QByteArray &payload);
    void onCursorPacket(const QByteArray &payload);
    void resetRemoteCursor(bool notify);
    void dispatch(const QJsonObject &message);
    void handleRemoteFileResult(const QJsonObject &message);
    void onIncomingFileStarted(
        const QString &id,
        const QString &name,
        qint64 size,
        bool resumed,
        qint64 offset);
    void onIncomingFileProgress(
        const QString &id,
        qint64 received,
        qint64 total);
    void onIncomingFileFinished(
        const QString &id,
        const QString &path,
        qint64 size);
    void onIncomingFileFailed(
        const QString &id,
        const QString &reason);
    void onIncomingFileCancelled(
        const QString &id,
        const QString &reason);
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
    quint32 cursorSequence_ = 0;
    bool cursorSeen_ = false;

    FileTransferPipeline fileTransfer_;
    ViewerVideoPipeline video_;
    ViewerAudioPipeline audio_;

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
