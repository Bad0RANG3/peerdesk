#include "app/viewer_controller.h"

#include "app/audio_output.h"
#include "app/quality_presets.h"
#include "client/signaling_client.h"
#include "core/util.h"
#include "session/viewer_session.h"
#include "transport/peer_link.h"

#include <QSysInfo>

#include <algorithm>

namespace pd {
namespace {

QString viewerName()
{
    QString name = QSysInfo::machineHostName().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("PeerDesk Viewer");
    }
    return name;
}

} // namespace

ViewerController::ViewerController(QObject *parent)
    : QObject(parent)
    , link_(new PeerLink(false, {}, this))
    , signaling_(new SignalingClient(this))
    , session_(new ViewerSession(link_, this))
{
    qRegisterMetaType<ViewerController::State>();
    connectComponents();
    setPreferences(std::make_shared<Preferences>());
}

ViewerController::~ViewerController()
{
    hangUp();
}

void ViewerController::connectComponents()
{
    connect(
        signaling_,
        &SignalingClient::registered,
        this,
        &ViewerController::onRegistered);
    connect(
        signaling_,
        &SignalingClient::peerJoined,
        this,
        &ViewerController::onPeerJoined);
    connect(
        signaling_,
        &SignalingClient::peerCandidatesChanged,
        this,
        &ViewerController::onPeerCandidatesChanged);
    connect(
        signaling_,
        &SignalingClient::peerLeft,
        this,
        &ViewerController::onPeerLeft);
    connect(
        signaling_,
        &SignalingClient::errorOccurred,
        this,
        &ViewerController::onSignalingError);
    connect(
        signaling_,
        &SignalingClient::logMessage,
        this,
        &ViewerController::logMessage);
    connect(
        signaling_,
        &SignalingClient::stateChanged,
        this,
        [this](SignalingClient::State state) {
            if (!active_ || stopping_
                || state_ == State::Streaming
                || state_ == State::Error) {
                return;
            }
            if (state == SignalingClient::State::Connecting
                || state == SignalingClient::State::Negotiating
                || state == SignalingClient::State::Reconnecting) {
                setState(State::Connecting);
            }
        });

    connect(
        link_,
        &PeerLink::stateChanged,
        this,
        &ViewerController::onLinkState);
    connect(
        link_,
        &PeerLink::reflexiveDiscoveryFinished,
        this,
        [this](bool, const QString &, quint16) {
            if (active_ && signaling_->connected()) {
                signaling_->updateCandidates(link_->localCandidates());
            }
        });
    connect(
        link_,
        &PeerLink::logMessage,
        this,
        &ViewerController::logMessage);

    connect(
        session_,
        &ViewerSession::frameDecoded,
        this,
        &ViewerController::frameDecoded);
    connect(
        session_,
        &ViewerSession::audioReceived,
        this,
        &ViewerController::onAudio);
    connect(
        session_,
        &ViewerSession::eventReceived,
        this,
        &ViewerController::eventReceived);
    connect(
        session_,
        &ViewerSession::infoChanged,
        this,
        [this](const QJsonObject &info) {
            info_ = info;
            emit infoChanged(info_);
        });
    connect(
        session_,
        &ViewerSession::statsChanged,
        this,
        [this](const QJsonObject &stats) {
            statistics_ = stats;
            emit statisticsChanged(stats);
        });
    connect(
        session_,
        &ViewerSession::audioStateChanged,
        this,
        [this](const QJsonObject &state) {
            audioState_ = state;
            emit audioStateChanged(state);
        });
    connect(
        session_,
        &ViewerSession::fileReceived,
        this,
        &ViewerController::fileReceived);
    connect(
        session_,
        &ViewerSession::outgoingFileStarted,
        this,
        &ViewerController::outgoingFileStarted);
    connect(
        session_,
        &ViewerSession::outgoingFileProgress,
        this,
        &ViewerController::outgoingFileProgress);
    connect(
        session_,
        &ViewerSession::outgoingFileFinished,
        this,
        &ViewerController::outgoingFileFinished);
    connect(
        session_,
        &ViewerSession::outgoingFileFailed,
        this,
        &ViewerController::outgoingFileFailed);
    connect(
        session_,
        &ViewerSession::logMessage,
        this,
        &ViewerController::logMessage);
}

void ViewerController::setPreferences(
    std::shared_ptr<Preferences> preferences)
{
    preferences_ = preferences
        ? std::move(preferences)
        : std::make_shared<Preferences>();
    applyPreferences();
}

void ViewerController::applyPreferences()
{
    if (!preferences_) {
        return;
    }
    const ViewerPreferences preferences = preferences_->viewer();
    server_ = preferences.server;
    token_ = preferences.token;
    code_ = preferences.code;
    qualityId_ = qualityPreset(preferences.qualityId).id;
    const QualityPreset &preset = qualityPreset(qualityId_);
    maxWidth_ = preset.maxWidth;
    quality_ = preset.quality;
    fps_ = preset.fps;
    audioEnabled_ = preferences.audio;
    receiveDirectory_ = preferences.receiveDirectory;
    if (session_) {
        session_->setReceiveDirectory(receiveDirectory_);
    }
    if (playback_) {
        playback_->setEnabled(audioEnabled_);
    }
}

void ViewerController::setAudioPlayback(
    std::shared_ptr<IAudioPlayback> playback)
{
    if (playback_ && playback_ != playback) {
        playback_->reset();
    }
    playback_ = std::move(playback);
    if (playback_) {
        playback_->setEnabled(audioEnabled_);
    }
}

void ViewerController::setBindAddress(const QString &address)
{
    if (!active_) {
        bindAddress_ = address.trimmed().isEmpty()
            ? QStringLiteral("0.0.0.0")
            : address.trimmed();
    }
}

bool ViewerController::connectTo(
    const QString &code,
    const QString &server,
    const QString &token,
    const QString &name,
    QString *error)
{
    const QString normalized = normalizeCode(code);
    if (!codeIsValid(normalized)) {
        const QString message = QStringLiteral("请输入 9 位数字连接码");
        if (error) {
            *error = message;
        }
        setError(message);
        return false;
    }
    const auto address = parseServerAddress(server);
    if (!address.has_value()) {
        const QString message = QStringLiteral("信令服务器地址无效");
        if (error) {
            *error = message;
        }
        setError(message);
        return false;
    }

    if (active_) {
        hangUp();
    }
    stopping_ = true;
    session_->stop();
    if (playback_) {
        playback_->reset();
        playback_->setEnabled(audioEnabled_);
    }
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    stopping_ = false;

    code_ = normalized;
    server_ = server.trimmed();
    token_ = token.trimmed();
    name_ = name.trimmed().isEmpty() ? viewerName() : name.trimmed();
    lastError_.clear();
    info_ = {};
    statistics_ = {};
    persist();

    if (!linkStarted_) {
        QString linkError;
        if (!link_->start(bindAddress_, 0, &linkError)) {
            const QString message =
                QStringLiteral("无法启动 UDP 通道：%1").arg(linkError);
            if (error) {
                *error = message;
            }
            setError(message);
            return false;
        }
        linkStarted_ = true;
    } else {
        link_->resetPeer();
    }
    link_->setPsk(pskFromCode(code_));
    link_->setForceRelay(forceRelay_);
    link_->setDeviceId(codeId(code_));
    link_->setLabel(name_);

    session_->setReceiveDirectory(receiveDirectory_);
    session_->setAudioEnabled(audioEnabled_);
    session_->start();

    signaling_->setAutoReconnect(true);
    signaling_->setServer(address->host, address->port);
    signaling_->setToken(token_);
    active_ = true;
    state_ = State::Connecting;
    emit stateChanged(state_);

    if (!signaling_->startViewer(
            code_,
            name_,
            link_->localCandidates())) {
        stopping_ = true;
        session_->stop();
        if (playback_) {
            playback_->reset();
        }
        if (linkStarted_) {
            link_->resetPeer();
        }
        signaling_->stop();
        stopping_ = false;
        active_ = false;
        const QString message =
            signaling_->lastErrorText().isEmpty()
            ? QStringLiteral("无法连接会话")
            : signaling_->lastErrorText();
        if (error) {
            *error = message;
        }
        setError(message);
        return false;
    }
    return true;
}

void ViewerController::hangUp()
{
    lastError_.clear();
    stopSession(false);
}

void ViewerController::stopSession(bool notify, const QString &reason)
{
    if (stopping_) {
        return;
    }
    stopping_ = true;
    const bool wasActive = active_ || state_ != State::Idle;
    active_ = false;
    session_->stop();
    if (playback_) {
        playback_->reset();
    }
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    peerName_.clear();
    peerSid_.clear();
    info_ = {};
    statistics_ = {};
    audioState_ = {};
    stopping_ = false;

    setState(State::Idle);
    emit peerChanged({}, false);
    if (notify && wasActive) {
        emit sessionEnded(reason);
    }
}

void ViewerController::persist()
{
    if (!preferences_) {
        return;
    }
    ViewerPreferences preferences = preferences_->viewer();
    preferences.server = server_;
    preferences.token = token_;
    preferences.code = codeIsValid(code_) ? code_ : QString();
    preferences.qualityId = qualityPreset(qualityId_).id;
    preferences.audio = audioEnabled_;
    preferences.receiveDirectory = receiveDirectory_;
    preferences_->setViewer(preferences);
}

void ViewerController::setServer(const QString &server)
{
    server_ = server.trimmed();
    persist();
}

void ViewerController::setToken(const QString &token)
{
    token_ = token.trimmed();
    persist();
}

void ViewerController::setState(State state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}

void ViewerController::setError(const QString &message)
{
    lastError_ = message;
    emit errorOccurred(message);
    setState(State::Error);
}

void ViewerController::onRegistered(const RegistrationInfo &)
{
    lastError_.clear();
}

void ViewerController::onPeerJoined(const PeerInfo &info)
{
    if (!active_) {
        return;
    }
    peerSid_ = info.sid;
    peerName_ = info.name;
    emit peerChanged(peerName_, true);
    link_->addCandidates(info.candidates);
    if (info.relay.valid()) {
        link_->offerRelay(info.relay.host, info.relay.port);
        link_->setRelayOutTag(info.relay.tag);
    }
    session_->start();
    setState(State::Connecting);
    link_->beginHandshake();
}

void ViewerController::onPeerCandidatesChanged(
    const QList<Candidate> &candidates)
{
    if (linkStarted_) {
        link_->addCandidates(candidates);
    }
}

void ViewerController::onPeerLeft(const QString &reason)
{
    if (!active_ || stopping_) {
        return;
    }
    stopSession(
        true,
        reason.isEmpty()
            ? QStringLiteral("对方已断开连接")
            : QStringLiteral("对方已离开：%1").arg(reason));
}

void ViewerController::onSignalingError(
    const QString &reason,
    const QString &text)
{
    if (!active_ || stopping_) {
        return;
    }
    const QString message = text.isEmpty()
        ? QStringLiteral("连接失败：%1").arg(reason)
        : text;
    if (reason == QStringLiteral("network")
        || reason == QStringLiteral("timeout")) {
        if (state_ != State::Streaming) {
            setState(State::Connecting);
        }
        emit logMessage(message);
        return;
    }
    failActive(message);
}

void ViewerController::onLinkState(const QString &state)
{
    emit linkStateChanged(state);
    if (stopping_ || !active_) {
        return;
    }
    if (state == QStringLiteral("direct")
        || state == QStringLiteral("relay")) {
        setState(State::Streaming);
        applyVideoSettings();
        session_->setAudioEnabled(audioEnabled_);
        if (playback_) {
            playback_->setEnabled(audioEnabled_);
        }
        session_->requestKeyframe();
    } else if (state == QStringLiteral("punching")) {
        setState(State::Connecting);
    } else if (state == QStringLiteral("closed")) {
        stopSession(true, QStringLiteral("传输通道已关闭"));
    }
}

void ViewerController::applyVideoSettings()
{
    session_->setMaxWidth(maxWidth_);
    session_->setFps(fps_);
    session_->setQuality(quality_);
    session_->selectMonitor(monitor_);
}

void ViewerController::onAudio(
    const QByteArray &pcm,
    quint32,
    quint32)
{
    if (!active_ || !audioEnabled_ || state_ != State::Streaming) {
        return;
    }
    if (playback_) {
        playback_->pushPcm16(pcm);
    }
}

void ViewerController::setAudioEnabled(bool enabled)
{
    if (audioEnabled_ == enabled) {
        if (playback_) {
            playback_->setEnabled(enabled);
        }
        return;
    }
    audioEnabled_ = enabled;
    if (playback_) {
        playback_->setEnabled(enabled);
        if (!enabled) {
            playback_->reset();
        }
    }
    if (active_) {
        session_->setAudioEnabled(enabled);
    }
    persist();
    emit audioEnabledChanged(enabled);
}

void ViewerController::setQualityPreset(const QString &id)
{
    const QualityPreset &preset = qualityPreset(id);
    qualityId_ = preset.id;
    maxWidth_ = preset.maxWidth;
    quality_ = preset.quality;
    fps_ = preset.fps;
    persist();
    if (active_) {
        applyVideoSettings();
    }
}

void ViewerController::setMaxWidth(int maxWidth)
{
    maxWidth_ = std::clamp(maxWidth, 320, 3840);
    qualityId_ = qualityIdFor(maxWidth_, fps_, quality_);
    persist();
    if (active_) {
        session_->setMaxWidth(maxWidth_);
    }
}

void ViewerController::setFps(int fps)
{
    fps_ = std::clamp(fps, 1, 120);
    qualityId_ = qualityIdFor(maxWidth_, fps_, quality_);
    persist();
    if (active_) {
        session_->setFps(fps_);
    }
}

void ViewerController::setQuality(int quality)
{
    quality_ = std::clamp(quality, 20, 95);
    qualityId_ = qualityIdFor(maxWidth_, fps_, quality_);
    persist();
    if (active_) {
        session_->setQuality(quality_);
    }
}

void ViewerController::selectMonitor(int index)
{
    monitor_ = std::max(0, index);
    if (active_) {
        session_->selectMonitor(monitor_);
    }
}

void ViewerController::requestKeyframe()
{
    if (active_) {
        session_->requestKeyframe();
    }
}

void ViewerController::sendMouseMove(double nx, double ny)
{
    if (active_) {
        session_->sendMouseMove(nx, ny);
    }
}

void ViewerController::sendMouseButton(
    const QString &action,
    double nx,
    double ny,
    const QString &button)
{
    if (active_) {
        session_->sendMouseButton(action, nx, ny, button);
    }
}

void ViewerController::sendMouseWheel(int delta, bool horizontal)
{
    if (active_) {
        session_->sendMouseWheel(delta, horizontal);
    }
}

void ViewerController::sendMouseRelative(int dx, int dy)
{
    if (active_) {
        session_->sendMouseRelative(dx, dy);
    }
}

bool ViewerController::sendKey(const QString &code, bool down)
{
    return active_ && session_->sendKey(code, down);
}

void ViewerController::sendText(const QString &text)
{
    if (active_) {
        session_->sendText(text);
    }
}

bool ViewerController::sendClipboard(const QString &text)
{
    return active_ && session_->sendClipboard(text);
}

void ViewerController::sendCtrlAltDelete()
{
    if (active_) {
        session_->sendCtrlAltDelete();
    }
}

bool ViewerController::beginUpload(
    const QString &name,
    qint64 size,
    QString *error)
{
    return active_ && session_->beginUpload(name, size, error);
}

bool ViewerController::uploadChunk(
    const QByteArray &data,
    QString *error)
{
    return active_ && session_->uploadChunk(data, error);
}

bool ViewerController::endUpload(QString *error)
{
    return active_ && session_->endUpload(error);
}

bool ViewerController::sendFile(
    const QString &path,
    QString *error)
{
    return active_ && session_->sendFile(path, error);
}

void ViewerController::setReceiveDirectory(const QString &directory)
{
    receiveDirectory_ = directory;
    if (session_) {
        session_->setReceiveDirectory(directory);
    }
    persist();
}

bool ViewerController::streaming() const
{
    return active_
        && state_ == State::Streaming
        && session_
        && session_->active();
}

QJsonObject ViewerController::info() const
{
    return info_;
}

QJsonObject ViewerController::audioState() const
{
    return session_ ? session_->audioState() : QJsonObject{};
}

void ViewerController::failActive(const QString &message)
{
    if (stopping_) {
        return;
    }
    stopping_ = true;
    active_ = false;
    session_->stop();
    if (playback_) {
        playback_->reset();
    }
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    peerName_.clear();
    peerSid_.clear();
    info_ = {};
    statistics_ = {};
    audioState_ = {};
    stopping_ = false;
    emit peerChanged({}, false);
    setError(message);
}

} // namespace pd
