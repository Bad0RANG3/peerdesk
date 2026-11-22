#include "session/viewer_session.h"

#include "core/protocol.h"
#include "session/audio_codec.h"
#include "transport/peer_link.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>

#include <algorithm>

namespace pd {

namespace {

constexpr int kClipboardLimit = 256 * 1024;

qint64 jsonInteger(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : 0;
    }
    return 0;
}

QString jsonString(
    const QJsonObject &object,
    const char *key,
    const QString &fallback = {})
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

QImage decodeJpeg(const QByteArray &jpeg)
{
    if (jpeg.isEmpty()) {
        return {};
    }
    QImage image = QImage::fromData(jpeg, "jpeg");
    if (image.isNull()) {
        image = QImage::fromData(jpeg, "jpg");
    }
    return image;
}

} // namespace

ViewerSession::ViewerSession(PeerLink *link, QObject *parent)
    : QObject(parent)
    , link_(link)
    , receiveDirectory_(defaultReceiveDirectory())
    , receiver_(receiveDirectory_)
    , fileSender_(new FileSender(this))
{
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
        fileSender_,
        &FileSender::started,
        this,
        &ViewerSession::outgoingFileStarted);
    connect(
        fileSender_,
        &FileSender::progress,
        this,
        &ViewerSession::outgoingFileProgress);
    connect(
        fileSender_,
        &FileSender::completed,
        this,
        &ViewerSession::outgoingFileFinished);
    connect(
        fileSender_,
        &FileSender::failed,
        this,
        [this](const QString &id, const QString &reason) {
            QString reported = reason;
            if (id == remoteFileErrorId_ && !remoteFileErrorReason_.isEmpty()) {
                reported = remoteFileErrorReason_;
            }
            if (id == remoteFileErrorId_) {
                remoteFileErrorId_.clear();
                remoteFileErrorReason_.clear();
            }
            emit outgoingFileFailed(id, reported);
        });
}

ViewerSession::~ViewerSession()
{
    stop();
    receiver_.abortAll();
}

void ViewerSession::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    if (link_ && link_->established()) {
        sendControlNoReply(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"), QStringLiteral("viewer")},
        });
        requestKeyframe();
    }
}

void ViewerSession::stop()
{
    started_ = false;
    receiver_.abortAll();
    if (fileSender_->active()) {
        fileSender_->cancel();
    }
    if (uploadActive_) {
        uploadActive_ = false;
        announceUpload(QStringLiteral("cancelled"));
    }
}

void ViewerSession::setReceiveDirectory(const QString &directory)
{
    receiveDirectory_ = directory;
    receiver_.setDirectory(directory);
}

QString ViewerSession::receiveDirectory() const
{
    return receiveDirectory_;
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
    return fileSender_->start(
        path,
        [this](const QJsonObject &message) { sendControlNoReply(message); },
        error);
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
        receiver_.abortAll();
        if (fileSender_->active()) {
            fileSender_->cancel();
        }
        if (uploadActive_) {
            uploadActive_ = false;
            announceUpload(QStringLiteral("cancelled"));
        }
    }
}

void ViewerSession::onVideoFrame(
    const QByteArray &jpeg,
    bool keyframe)
{
    if (!started_ || jpeg.isEmpty()) {
        return;
    }
    ++videoSequence_;
    emit videoFrameReceived(jpeg, videoSequence_, keyframe);
    const QImage image = decodeJpeg(jpeg);
    if (image.isNull()) {
        emit logMessage(QStringLiteral("收到无法解码的视频帧"));
        return;
    }
    emit frameDecoded(image, keyframe);
}

void ViewerSession::onAudioPacket(const QByteArray &payload)
{
    if (!started_) {
        return;
    }
    const auto packet = unpackAudio(payload);
    if (!packet.has_value()) {
        emit logMessage(QStringLiteral("收到损坏的音频包"));
        return;
    }
    if (packet->samples == 0
        || packet->samples > kAudioRate
        || packet->current.size() != packet->samples
        || (!packet->previous.isEmpty()
            && packet->previous.size() != packet->samples)) {
        emit logMessage(QStringLiteral("音频包长度校验失败"));
        return;
    }
    ++audioFrames_;
    emit audioReceived(
        ulawDecode(packet->current),
        packet->sequence,
        packet->timestampMs);
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
        handleFileBegin(message);
        return;
    }
    if (kind == QStringLiteral("file_data")) {
        handleFileData(message);
        return;
    }
    if (kind == QStringLiteral("file_end")) {
        handleFileEnd(message);
        return;
    }
    if (kind == QStringLiteral("file_done")
        || kind == QStringLiteral("file_error")) {
        handleRemoteFileResult(message);
        return;
    }
    emit eventReceived(message);
}

void ViewerSession::handleFileBegin(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    const QString name = jsonString(message, "name");
    const qint64 size = jsonInteger(message, "size");
    QString error;
    if (!receiver_.begin(id, name, size, &error)) {
        rejectFile(id, error);
        return;
    }
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("begin")},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), size},
    });
}

void ViewerSession::handleFileData(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    QByteArray data;
    QString error;
    if (!decodeFileChunk(
            jsonString(message, "d"),
            &data,
            &error)) {
        receiver_.abort(id);
        rejectFile(id, error.isEmpty() ? QStringLiteral("data") : error);
        return;
    }
    if (!receiver_.append(id, data, &error)) {
        rejectFile(id, error);
        return;
    }
    emit eventReceived(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_in")},
        {QStringLiteral("state"), QStringLiteral("progress")},
        {QStringLiteral("id"), id},
        {QStringLiteral("got"), receiver_.received(id)},
        {QStringLiteral("size"), receiver_.expected(id)},
    });
}

void ViewerSession::handleFileEnd(const QJsonObject &message)
{
    const QString id = jsonString(message, "id");
    QString path;
    qint64 size = 0;
    QString error;
    if (!receiver_.finish(id, &path, &size, &error)) {
        rejectFile(id, error);
        return;
    }
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
        && fileSender_->active() && id == fileSender_->id()) {
        remoteFileErrorId_ = id;
        remoteFileErrorReason_ = jsonString(message, "reason");
        fileSender_->cancel();
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
