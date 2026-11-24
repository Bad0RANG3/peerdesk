#include "session/viewer_session.h"

#include "core/protocol.h"
#include "transport/peer_link.h"

#include <QFileInfo>

#include <algorithm>

namespace pd {

namespace {

constexpr int kClipboardLimit = 256 * 1024;

QString jsonString(
    const QJsonObject &object,
    const char *key,
    const QString &fallback = {})
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

} // namespace

ViewerSession::ViewerSession(PeerLink *link, QObject *parent)
    : QObject(parent)
    , link_(link)
    , fileTransfer_(defaultReceiveDirectory())
{
    qRegisterMetaType<pd::VideoCodec>("pd::VideoCodec");
    if (link_) {
        connect(
            link_,
            &PeerLink::stateChanged,
            this,
            &ViewerSession::onLinkStateChanged);
        connect(
            link_,
            &PeerLink::videoFrameReceived,
            this,
            &ViewerSession::onVideoFrame);
        connect(
            link_,
            &PeerLink::audioReceived,
            this,
            &ViewerSession::onAudioPacket);
        connect(
            link_,
            &PeerLink::controlMessageReceived,
            this,
            &ViewerSession::dispatch);
    }

    connect(
        &fileTransfer_,
        &FileTransferPipeline::controlMessage,
        this,
        &ViewerSession::sendControlNoReply);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileStarted,
        this,
        &ViewerSession::outgoingFileStarted);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileProgress,
        this,
        &ViewerSession::outgoingFileProgress);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileFinished,
        this,
        &ViewerSession::outgoingFileFinished);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileFailed,
        this,
        &ViewerSession::outgoingFileFailed);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileCancelled,
        this,
        &ViewerSession::outgoingFileCancelled);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::outgoingFileInterrupted,
        this,
        &ViewerSession::outgoingFileInterrupted);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::receiveFileStarted,
        this,
        &ViewerSession::onIncomingFileStarted);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::receiveFileProgress,
        this,
        &ViewerSession::onIncomingFileProgress);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::receiveFileFinished,
        this,
        &ViewerSession::onIncomingFileFinished);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::receiveFileFailed,
        this,
        &ViewerSession::onIncomingFileFailed);
    connect(
        &fileTransfer_,
        &FileTransferPipeline::receiveFileCancelled,
        this,
        &ViewerSession::onIncomingFileCancelled);
    connect(
        &video_,
        &ViewerVideoPipeline::frameDecoded,
        this,
        [this](
            const QImage &image,
            quint32,
            bool keyframe) {
            if (started_) {
                emit frameDecoded(image, keyframe);
            }
        });
    connect(
        &video_,
        &ViewerVideoPipeline::decodeFailed,
        this,
        &ViewerSession::logMessage);
    connect(
        &audio_,
        &ViewerAudioPipeline::audioDecoded,
        this,
        [this](
            const QByteArray &pcm16,
            quint32 sequence,
            quint32 timestampMs) {
            if (started_) {
                ++audioFrames_;
                emit audioReceived(pcm16, sequence, timestampMs);
            }
        });
    connect(
        &audio_,
        &ViewerAudioPipeline::decodeFailed,
        this,
        &ViewerSession::logMessage);
}

ViewerSession::~ViewerSession()
{
    blockSignals(true);
    stop();
}

void ViewerSession::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    video_.start();
    audio_.start();
    if (link_ && link_->established()) {
        sendControlNoReply(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"), QStringLiteral("viewer")},
        });
        requestKeyframe();
    }
}

void ViewerSession::suspend()
{
    started_ = false;
    video_.stop();
    audio_.stop();
    (void)fileTransfer_.interruptSend(QStringLiteral("interrupted"));
    if (uploadActive_) {
        uploadActive_ = false;
        announceUpload(QStringLiteral("cancelled"));
    }
}

void ViewerSession::stop()
{
    suspend();
    fileTransfer_.abortAllReceives();
    fileTransfer_.abortSend();
    const bool hadInfo = !info_.isEmpty();
    const bool hadStats = !stats_.isEmpty();
    const bool hadAudio = !audioState_.isEmpty();
    info_ = {};
    stats_ = {};
    audioState_ = {};
    videoSequence_ = 0;
    audioFrames_ = 0;
    uploadActive_ = false;
    uploadId_.clear();
    uploadName_.clear();
    uploadSize_ = 0;
    uploadSent_ = 0;
    pendingUploadId_.clear();
    pendingUploadName_.clear();
    pendingUploadSize_ = 0;
    if (hadInfo) {
        emit infoChanged(info_);
    }
    if (hadStats) {
        emit statsChanged(stats_);
    }
    if (hadAudio) {
        emit audioStateChanged(audioState_);
    }
}

void ViewerSession::setReceiveDirectory(const QString &directory)
{
    fileTransfer_.setReceiveDirectory(directory);
}

QString ViewerSession::receiveDirectory() const
{
    return fileTransfer_.receiveDirectory();
}

bool ViewerSession::sendControl(const QJsonObject &message)
{
    if (!link_ || !link_->established()) {
        return false;
    }
    link_->sendControl(message);
    return true;
}

void ViewerSession::sendControlNoReply(const QJsonObject &message)
{
    (void)sendControl(message);
}

void ViewerSession::requestKeyframe()
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("keyframe_req")},
    });
}

void ViewerSession::setQuality(int quality)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("settings")},
        {QStringLiteral("quality"), std::clamp(quality, 20, 95)},
    });
}

void ViewerSession::setMaxWidth(int maxWidth)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("settings")},
        {QStringLiteral("max_width"), std::clamp(maxWidth, 320, 3840)},
    });
}

void ViewerSession::setFps(int fps)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("settings")},
        {QStringLiteral("fps"), std::clamp(fps, 1, 120)},
    });
}

void ViewerSession::selectMonitor(int index)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("settings")},
        {QStringLiteral("monitor"), std::max(0, index)},
    });
    requestKeyframe();
}

void ViewerSession::setAudioEnabled(bool on)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("audio")},
        {QStringLiteral("on"), on},
    });
    QJsonObject pending = audioState_;
    pending.insert(QStringLiteral("on"), on);
    pending.insert(QStringLiteral("pending"), true);
    emit audioStateChanged(pending);
}

void ViewerSession::sendMouseMove(double nx, double ny)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("mouse")},
        {QStringLiteral("act"), QStringLiteral("move")},
        {QStringLiteral("x"), nx},
        {QStringLiteral("y"), ny},
    });
}

void ViewerSession::sendMouseButton(
    const QString &action,
    double nx,
    double ny,
    const QString &button)
{
    if (action != QStringLiteral("down")
        && action != QStringLiteral("up")
        && action != QStringLiteral("click")) {
        return;
    }
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("mouse")},
        {QStringLiteral("act"), action},
        {QStringLiteral("x"), nx},
        {QStringLiteral("y"), ny},
        {QStringLiteral("btn"), button},
    });
}

void ViewerSession::sendMouseWheel(int delta, bool horizontal)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("mouse")},
        {QStringLiteral("act"), QStringLiteral("wheel")},
        {QStringLiteral("dy"), delta},
        {QStringLiteral("h"), horizontal},
    });
}

void ViewerSession::sendMouseRelative(int dx, int dy)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("mouse")},
        {QStringLiteral("act"), QStringLiteral("rel")},
        {QStringLiteral("dx"), dx},
        {QStringLiteral("dy"), dy},
    });
}

void ViewerSession::sendMouseRelativeButton(
    const QString &action,
    const QString &button)
{
    if (action != QStringLiteral("reldown")
        && action != QStringLiteral("relup")) {
        return;
    }
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("mouse")},
        {QStringLiteral("act"), action},
        {QStringLiteral("btn"), button},
    });
}

bool ViewerSession::sendKey(const QString &code, bool down)
{
    if (code.isEmpty()) {
        return false;
    }
    return sendControl(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("key")},
        {QStringLiteral("code"), code},
        {QStringLiteral("down"), down},
    });
}

void ViewerSession::sendText(const QString &text)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("text")},
        {QStringLiteral("s"), text},
    });
}

bool ViewerSession::sendClipboard(const QString &text)
{
    return sendControl(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("clip")},
        {QStringLiteral("s"), text.left(kClipboardLimit)},
    });
}

void ViewerSession::sendCtrlAltDelete()
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("cad")},
    });
}

bool ViewerSession::beginUpload(
    const QString &name,
    qint64 size,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (uploadActive_) {
        return reject(QStringLiteral("busy"));
    }
    if (!link_ || !link_->established()) {
        return reject(QStringLiteral("offline"));
    }
    if (!isSafeBaseName(name)) {
        return reject(QStringLiteral("name"));
    }
    if (size <= 0 || size > kMaxFileBytes) {
        return reject(QStringLiteral("size"));
    }

    uploadActive_ = true;
    uploadId_ = makeTransferId();
    uploadName_ = name;
    uploadSize_ = size;
    uploadSent_ = 0;
    if (!sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("id"), uploadId_},
            {QStringLiteral("name"), uploadName_},
            {QStringLiteral("size"), uploadSize_},
        })) {
        uploadActive_ = false;
        uploadId_.clear();
        uploadName_.clear();
        uploadSize_ = 0;
        return reject(QStringLiteral("offline"));
    }
    announceUpload(QStringLiteral("begin"), {
        {QStringLiteral("name"), uploadName_},
        {QStringLiteral("size"), uploadSize_},
    });
    if (error) {
        error->clear();
    }
    return true;
}

bool ViewerSession::uploadChunk(
    const QByteArray &data,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (!uploadActive_) {
        return reject(QStringLiteral("inactive"));
    }
    if (data.isEmpty()) {
        return reject(QStringLiteral("empty"));
    }
    if (data.size() > kFileChunkBytes) {
        return reject(QStringLiteral("chunk"));
    }
    if (uploadSent_ + data.size() > uploadSize_) {
        return reject(QStringLiteral("size"));
    }
    if (!sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_data")},
            {QStringLiteral("id"), uploadId_},
            {QStringLiteral("d"), QString::fromLatin1(data.toBase64())},
        })) {
        return reject(QStringLiteral("offline"));
    }
    uploadSent_ += data.size();
    announceUpload(QStringLiteral("progress"), {
        {QStringLiteral("sent"), uploadSent_},
        {QStringLiteral("size"), uploadSize_},
    });
    if (error) {
        error->clear();
    }
    return true;
}

bool ViewerSession::endUpload(QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (!uploadActive_) {
        return reject(QStringLiteral("inactive"));
    }
    if (uploadSent_ != uploadSize_) {
        return reject(QStringLiteral("incomplete"));
    }
    if (!sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_end")},
            {QStringLiteral("id"), uploadId_},
        })) {
        return reject(QStringLiteral("offline"));
    }

    pendingUploadId_ = uploadId_;
    pendingUploadName_ = uploadName_;
    pendingUploadSize_ = uploadSize_;
    uploadActive_ = false;
    announceUpload(QStringLiteral("done"), {
        {QStringLiteral("name"), pendingUploadName_},
        {QStringLiteral("size"),
         static_cast<double>(pendingUploadSize_)},
    });
    uploadId_.clear();
    uploadName_.clear();
    uploadSize_ = 0;
    uploadSent_ = 0;
    if (error) {
        error->clear();
    }
    return true;
}

bool ViewerSession::sendFile(const QString &path, QString *error)
{
    if (!link_ || !link_->established()) {
        if (error) {
            *error = QStringLiteral("offline");
        }
        return false;
    }
    return fileTransfer_.startSend(path, error);
}

bool ViewerSession::cancelFile()
{
    return fileTransfer_.cancelSend();
}

void ViewerSession::onLinkStateChanged(const QString &state)
{
    const LinkStats linkStats = link_ ? link_->stats() : LinkStats{};
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("link")},
        {QStringLiteral("state"), state},
        {QStringLiteral("path"), linkStats.path},
        {QStringLiteral("relay"), linkStats.usingRelay},
    });
    if ((state == QStringLiteral("direct")
         || state == QStringLiteral("relay"))
        && started_) {
        sendControlNoReply(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"), QStringLiteral("viewer")},
        });
        requestKeyframe();
    } else if (state == QStringLiteral("closed")) {
        suspend();
    }
}

void ViewerSession::onVideoFrame(
    const QByteArray &data,
    bool keyframe,
    VideoCodec codec)
{
    if (!started_ || data.isEmpty()) {
        return;
    }
    ++videoSequence_;
    emit videoFrameReceived(data, videoSequence_, keyframe, codec);
    video_.submit(data, videoSequence_, keyframe);
}

void ViewerSession::onAudioPacket(const QByteArray &payload)
{
    if (!started_) {
        return;
    }
    audio_.submit(payload);
}

void ViewerSession::dispatch(const QJsonObject &message)
{
    const QString kind =
        message.value(QStringLiteral("t")).toString();
    if (!started_) {
        return;
    }
    if (kind == QStringLiteral("info")) {
        info_ = message;
        emit infoChanged(info_);
        emit eventReceived(message);
        return;
    }
    if (kind == QStringLiteral("stats")) {
        stats_ = message;
        emit statsChanged(stats_);
        emit eventReceived(message);
        return;
    }
    if (kind == QStringLiteral("audio")) {
        audioState_ = message;
        emit audioStateChanged(audioState_);
        emit eventReceived(message);
        return;
    }
    if (kind == QStringLiteral("file_begin")) {
        fileTransfer_.beginReceive(message);
        return;
    }
    if (kind == QStringLiteral("file_data")) {
        fileTransfer_.appendReceive(message);
        return;
    }
    if (kind == QStringLiteral("file_end")) {
        fileTransfer_.finishReceive(jsonString(message, "id"));
        return;
    }
    if (kind == QStringLiteral("file_resume")) {
        fileTransfer_.acceptSendResume(message);
        return;
    }
    if (kind == QStringLiteral("file_done")
        || kind == QStringLiteral("file_error")) {
        handleRemoteFileResult(message);
        return;
    }
    if (kind == QStringLiteral("file_cancel")) {
        fileTransfer_.cancelReceive(
            jsonString(message, "id"),
            jsonString(message, "reason"));
        return;
    }
    if (kind == QStringLiteral("file_cancelled")) {
        emit eventReceived(message);
        return;
    }
    emit eventReceived(message);
}

void ViewerSession::onIncomingFileStarted(
    const QString &id,
    const QString &name,
    qint64 size,
    bool resumed,
    qint64 offset)
{
    if (resumed) {
        sendControlNoReply(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_resume")},
            {QStringLiteral("id"), id},
            {QStringLiteral("offset"), offset},
        });
    }
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("begin")},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), size},
        {QStringLiteral("resumed"), resumed},
        {QStringLiteral("offset"), offset},
    });
}

void ViewerSession::onIncomingFileProgress(
    const QString &id,
    qint64 received,
    qint64 total)
{
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("progress")},
        {QStringLiteral("id"), id},
        {QStringLiteral("got"), received},
        {QStringLiteral("size"), total},
    });
}

void ViewerSession::onIncomingFileFinished(
    const QString &id,
    const QString &path,
    qint64 size)
{
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_done")},
        {QStringLiteral("id"), id},
        {QStringLiteral("path"), path},
        {QStringLiteral("size"), size},
    });
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("done")},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("path"), path},
        {QStringLiteral("size"), size},
    });
    emit fileReceived(path, size);
}

void ViewerSession::onIncomingFileFailed(
    const QString &id,
    const QString &reason)
{
    rejectFile(id, reason);
}

void ViewerSession::onIncomingFileCancelled(
    const QString &id,
    const QString &reason)
{
    const QString effective =
        reason.isEmpty() ? QStringLiteral("cancelled") : reason;
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_cancelled")},
        {QStringLiteral("id"), id},
        {QStringLiteral("reason"), effective},
    });
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("cancelled")},
        {QStringLiteral("id"), id},
        {QStringLiteral("reason"), effective},
    });
}

void ViewerSession::handleRemoteFileResult(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    const QString kind =
        message.value(QStringLiteral("t")).toString();
    if (!pendingUploadId_.isEmpty() && id == pendingUploadId_) {
        if (kind == QStringLiteral("file_done")) {
            announceUpload(QStringLiteral("acknowledged"), {
                {QStringLiteral("name"), pendingUploadName_},
                {QStringLiteral("size"),
                 static_cast<double>(pendingUploadSize_)},
                {QStringLiteral("path"), jsonString(message, "path")},
            });
        } else {
            announceUpload(QStringLiteral("error"), {
                {QStringLiteral("reason"), jsonString(message, "reason")},
            });
        }
        pendingUploadId_.clear();
        pendingUploadName_.clear();
        pendingUploadSize_ = 0;
    }
    if (kind == QStringLiteral("file_error")
        && fileTransfer_.sendActive()
        && id == fileTransfer_.sendId()) {
        (void)fileTransfer_.failSendFromPeer(
            id,
            jsonString(message, "reason"));
    }
    emit eventReceived(message);
}

void ViewerSession::rejectFile(
    const QString &id,
    const QString &reason)
{
    if (id.isEmpty()) {
        return;
    }
    sendControlNoReply(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_error")},
        {QStringLiteral("id"), id},
        {QStringLiteral("reason"),
         reason.isEmpty() ? QStringLiteral("io") : reason},
    });
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("error")},
        {QStringLiteral("id"), id},
        {QStringLiteral("reason"),
         reason.isEmpty() ? QStringLiteral("io") : reason},
    });
}

void ViewerSession::announceUpload(
    const QString &state,
    const QJsonObject &extra)
{
    QJsonObject message{
        {QStringLiteral("t"), QStringLiteral("upload")},
        {QStringLiteral("state"), state},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        message.insert(it.key(), it.value());
    }
    emit eventReceived(message);
}

} // namespace pd
