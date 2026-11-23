#include "app/viewer_controller.h"

#include "app/audio_output.h"
#include "app/quality_presets.h"
#include "client/signaling_client.h"
#include "core/util.h"
#include "session/viewer_session.h"
#include "transport/peer_link.h"

#include <QSysInfo>
#include <QTimer>

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
    , deadlineTimer_(new QTimer(this))
{
    qRegisterMetaType<ViewerController::State>();
    deadlineTimer_->setSingleShot(true);
    connect(deadlineTimer_, &QTimer::timeout, this, [this] {
        if (!active_ || stopping_) {
            return;
        }
        failActive(
            hadLiveSession_
                ? QStringLiteral("连接恢复超时，请返回后重新连接。")
                : QStringLiteral("连接超时，请检查网络和连接码后重试。"));
    });
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
                || state_ == State::Reconnecting
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
        &ViewerSession::outgoingFileCancelled,
        this,
        &ViewerController::outgoingFileCancelled);
    connect(
        session_,
        &ViewerSession::outgoingFileInterrupted,
        this,
        &ViewerController::outgoingFileInterrupted);
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

void ViewerController::setReconnectTimeoutMs(int milliseconds)
{
    reconnectTimeoutMs_ = std::max(100, milliseconds);
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
        deadlineTimer_->stop();
        const QString message = QStringLiteral("请输入 9 位数字连接码");
        if (error) {
            *error = message;
        }
        setError(message);
        return false;
    }
    const auto address = parseServerAddress(server);
    if (!address.has_value()) {
        deadlineTimer_->stop();
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
    hadLiveSession_ = false;
    recovering_ = false;
    recoveryHandshakeArmed_ = false;

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
        deadlineTimer_->stop();
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
    ensureInitialDeadline();
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
    deadlineTimer_->stop();
    hadLiveSession_ = false;
    recovering_ = false;
    recoveryHandshakeArmed_ = false;
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

void ViewerController::ensureInitialDeadline()
{
    if (!active_ || stopping_ || hadLiveSession_
        || deadlineTimer_->isActive()) {
        return;
    }
    deadlineTimer_->start(reconnectTimeoutMs_);
}

void ViewerController::beginRecovery()
{
    if (!active_ || stopping_ || recovering_ || !hadLiveSession_) {
        return;
    }

    recovering_ = true;
    recoveryHandshakeArmed_ = false;
    emit inputReleaseRequested();
    stopping_ = true;
    session_->stop();
    if (playback_) {
        playback_->reset();
        playback_->setEnabled(audioEnabled_);
    }
    if (linkStarted_) {
        link_->resetPeer();
    }
    info_ = {};
    statistics_ = {};
    audioState_ = {};
    stopping_ = false;
    setState(State::Reconnecting);
    deadlineTimer_->start(reconnectTimeoutMs_);
    emit logMessage(QStringLiteral("连接中断，正在尝试恢复会话"));
    if (signaling_->connected()) {
        signaling_->requestPairRetry();
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
    if (active_ && state_ == State::Reconnecting
        && signaling_->connected()) {
        signaling_->requestPairRetry();
    }
}

void ViewerController::onPeerJoined(const PeerInfo &info)
{
    if (!active_ || stopping_) {
        return;
    }
    const bool recovering =
        recovering_ || state_ == State::Reconnecting;
    const bool replaceLink =
        recovering
        || (!peerSid_.isEmpty() && info.sid != peerSid_)
        || link_->state() == QStringLiteral("closed");
    if (replaceLink) {
        recoveryHandshakeArmed_ = false;
        stopping_ = true;
        session_->stop();
        if (linkStarted_) {
            link_->resetPeer();
        }
        stopping_ = false;
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
    if (recovering) {
        recovering_ = true;
        recoveryHandshakeArmed_ = true;
    } else {
        setState(State::Connecting);
    }
    ensureInitialDeadline();
    link_->beginHandshake();
}

void ViewerController::onPeerCandidatesChanged(
    const QList<Candidate> &candidates)
{
    if (linkStarted_) {
        link_->addCandidates(candidates);
    }
    if (!active_ || stopping_ || state_ != State::Reconnecting) {
        return;
    }

    const PeerInfo info = signaling_->peer();
    if (info.sid.isEmpty()) {
        return;
    }
    if (!peerSid_.isEmpty() && peerSid_ != info.sid) {
        peerSid_ = info.sid;
        peerName_ = info.name;
        emit peerChanged(peerName_, true);
    }
    link_->addCandidates(info.candidates);
    if (info.relay.valid()) {
        link_->offerRelay(info.relay.host, info.relay.port);
        link_->setRelayOutTag(info.relay.tag);
    }
    const bool hasPath =
        link_->peerCandidates().isEmpty() == false
        || info.relay.valid();
    if (!hasPath || link_->state() != QStringLiteral("idle")) {
        return;
    }
    recoveryHandshakeArmed_ = true;
    session_->start();
    link_->beginHandshake();
}

void ViewerController::onPeerLeft(const QString &reason)
{
    if (!active_ || stopping_) {
        return;
    }
    const QString message = reason.isEmpty()
        ? QStringLiteral("对方已断开连接")
        : QStringLiteral("对方已离开：%1").arg(reason);
    if (recovering_) {
        emit logMessage(message);
        recoveryHandshakeArmed_ = false;
        stopping_ = true;
        session_->stop();
        if (linkStarted_) {
            link_->resetPeer();
        }
        stopping_ = false;
        peerSid_.clear();
        setState(State::Reconnecting);
        if (signaling_->connected()) {
            signaling_->requestPairRetry();
        }
        return;
    }
    if (hadLiveSession_) {
        emit logMessage(message);
        beginRecovery();
        return;
    }

    emit logMessage(message);
    stopping_ = true;
    session_->stop();
    if (linkStarted_) {
        link_->resetPeer();
    }
    stopping_ = false;
    peerSid_.clear();
    setState(State::Connecting);
    ensureInitialDeadline();
    if (signaling_->connected()) {
        signaling_->requestPairRetry();
    }
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
        || reason == QStringLiteral("timeout")
        || reason == QStringLiteral("offline")
        || reason == QStringLiteral("not_found")) {
        if (state_ != State::Streaming
            && state_ != State::Reconnecting) {
            setState(State::Connecting);
            ensureInitialDeadline();
        }
        emit logMessage(message);
        return;
    }
    failActive(message);
}

void ViewerController::onLinkState(const QString &state)
{
    if (stopping_ || !active_) {
        return;
    }
    if (state == QStringLiteral("idle")) {
        return;
    }
    if (recovering_ && state == QStringLiteral("closed")) {
        recoveryHandshakeArmed_ = false;
        stopping_ = true;
        session_->stop();
        if (linkStarted_) {
            link_->resetPeer();
        }
        stopping_ = false;
        peerSid_.clear();
        setState(State::Reconnecting);
        if (signaling_->connected()) {
            signaling_->requestPairRetry();
        }
        return;
    }
    if (recovering_ && !recoveryHandshakeArmed_) {
        return;
    }
    emit linkStateChanged(state);
    if (state == QStringLiteral("direct")
        || state == QStringLiteral("relay")) {
        recovering_ = false;
        recoveryHandshakeArmed_ = false;
        deadlineTimer_->stop();
        hadLiveSession_ = true;
        setState(State::Streaming);
        applyVideoSettings();
        session_->setAudioEnabled(audioEnabled_);
        if (playback_) {
            playback_->setEnabled(audioEnabled_);
        }
        session_->requestKeyframe();
    } else if (state == QStringLiteral("punching")) {
        if (state_ == State::Streaming) {
            beginRecovery();
        } else if (state_ != State::Reconnecting) {
            setState(State::Connecting);
            ensureInitialDeadline();
        }
    } else if (state == QStringLiteral("closed")) {
        if (hadLiveSession_) {
            beginRecovery();
        } else {
            onPeerLeft(QStringLiteral("传输通道已关闭"));
        }
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

bool ViewerController::cancelFile()
{
    return active_ && session_->cancelFile();
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
    deadlineTimer_->stop();
    const bool notify = hadLiveSession_;
    hadLiveSession_ = false;
    recovering_ = false;
    recoveryHandshakeArmed_ = false;
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
    if (notify) {
        emit sessionEnded(message);
    }
}

} // namespace pd
