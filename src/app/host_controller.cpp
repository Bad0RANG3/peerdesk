#include "app/host_controller.h"

#include "app/quality_presets.h"
#include "client/signaling_client.h"
#include "core/util.h"
#include "discovery/lan_discovery.h"
#include "session/host_session.h"
#include "transport/peer_link.h"

#include <QDateTime>
#include <QSysInfo>

#include <algorithm>

namespace pd {
namespace {

QString machineName()
{
    QString name = QSysInfo::machineHostName().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("PeerDesk Host");
    }
    return name;
}

} // namespace

HostController::HostController(QObject *parent)
    : QObject(parent)
    , link_(new PeerLink(true, {}, this))
    , signaling_(new SignalingClient(this))
    , session_(new HostSession(link_, this))
{
    qRegisterMetaType<HostController::State>();
    connectComponents();
    setPreferences(std::make_shared<Preferences>());
}

HostController::~HostController()
{
    stop();
}

void HostController::connectComponents()
{
    connect(
        signaling_,
        &SignalingClient::registered,
        this,
        &HostController::onRegistered);
    connect(
        signaling_,
        &SignalingClient::peerJoined,
        this,
        &HostController::onPeerJoined);
    connect(
        signaling_,
        &SignalingClient::peerCandidatesChanged,
        this,
        &HostController::onPeerCandidatesChanged);
    connect(
        signaling_,
        &SignalingClient::peerLeft,
        this,
        &HostController::onPeerLeft);
    connect(
        signaling_,
        &SignalingClient::errorOccurred,
        this,
        &HostController::onSignalingError);
    connect(
        signaling_,
        &SignalingClient::logMessage,
        this,
        &HostController::logMessage);
    connect(
        link_,
        &PeerLink::stateChanged,
        this,
        &HostController::onLinkState);
    connect(
        link_,
        &PeerLink::reflexiveDiscoveryFinished,
        this,
        [this](bool, const QString &, quint16) {
            updateCandidates();
        });
    connect(
        link_,
        &PeerLink::logMessage,
        this,
        &HostController::logMessage);

    connect(
        session_,
        &HostSession::eventReceived,
        this,
        &HostController::onSessionEvent);
    connect(
        session_,
        &HostSession::statsChanged,
        this,
        [this](const QJsonObject &stats) {
            statistics_ = stats;
            emit statisticsChanged(stats);
        });
    connect(
        session_,
        &HostSession::audioStateChanged,
        this,
        [this](const QJsonObject &) {
            video_ = session_->settings();
        });
    connect(
        session_,
        &HostSession::fileReceived,
        this,
        &HostController::fileReceived);
    connect(
        session_,
        &HostSession::outgoingFileStarted,
        this,
        &HostController::outgoingFileStarted);
    connect(
        session_,
        &HostSession::outgoingFileProgress,
        this,
        &HostController::outgoingFileProgress);
    connect(
        session_,
        &HostSession::outgoingFileFinished,
        this,
        &HostController::outgoingFileFinished);
    connect(
        session_,
        &HostSession::outgoingFileFailed,
        this,
        &HostController::outgoingFileFailed);
    connect(
        session_,
        &HostSession::outgoingFileCancelled,
        this,
        &HostController::outgoingFileCancelled);
    connect(
        session_,
        &HostSession::outgoingFileInterrupted,
        this,
        &HostController::outgoingFileInterrupted);
    connect(
        session_,
        &HostSession::logMessage,
        this,
        &HostController::logMessage);
}

void HostController::setPreferences(
    std::shared_ptr<Preferences> preferences)
{
    preferences_ = preferences
        ? std::move(preferences)
        : std::make_shared<Preferences>();
    applyPreferences();
}

void HostController::applyPreferences()
{
    if (!preferences_) {
        return;
    }
    const HostPreferences preferences = preferences_->host();
    server_ = preferences.server;
    token_ = preferences.token;
    code_ = codeIsValid(preferences.code)
        ? normalizeCode(preferences.code)
        : generateCode();
    video_ = preferences.video;
    video_.clamp();
    allowInput_ = preferences.allowInput;
    forceRelay_ = preferences.forceRelay;
    receiveDirectory_ = preferences.receiveDirectory;
    applySessionSettings();
}

void HostController::applySessionSettings()
{
    if (!session_) {
        return;
    }
    session_->setSettings(video_);
    session_->setAllowInput(allowInput_);
    session_->setReceiveDirectory(receiveDirectory_);
}

void HostController::persist()
{
    if (!preferences_) {
        return;
    }
    HostPreferences preferences = preferences_->host();
    preferences.server = server_;
    preferences.token = token_;
    preferences.code = code_;
    preferences.video = video_;
    preferences.allowInput = allowInput_;
    preferences.forceRelay = forceRelay_;
    preferences.receiveDirectory = receiveDirectory_;
    preferences_->setHost(preferences);
}

void HostController::setBindAddress(const QString &address)
{
    if (!active_) {
        bindAddress_ = address.trimmed().isEmpty()
            ? QStringLiteral("0.0.0.0")
            : address.trimmed();
    }
}

void HostController::setDeviceId(const QString &deviceId)
{
    deviceId_ = deviceId.trimmed();
}

QString HostController::deviceName() const
{
    return machineName();
}

void HostController::setLanDiscovery(LanDiscovery *discovery)
{
    if (lanDiscovery_ == discovery) {
        return;
    }
    lanDiscovery_ = discovery;
    syncAnnouncement();
}

void HostController::setScreenSource(std::shared_ptr<IScreenSource> source)
{
    screenSource_ = std::move(source);
    session_->setScreenSource(screenSource_);
    emit monitorsChanged(monitors());
}

void HostController::setAudioSource(std::shared_ptr<IAudioSource> source)
{
    audioSource_ = std::move(source);
    session_->setAudioSource(audioSource_);
}

void HostController::setInputSink(std::shared_ptr<IInputSink> sink)
{
    inputSink_ = std::move(sink);
    session_->setInputSink(inputSink_);
}

void HostController::setClipboard(std::shared_ptr<IClipboard> clipboard)
{
    clipboard_ = std::move(clipboard);
    session_->setClipboard(clipboard_);
}

bool HostController::start(QString *error)
{
    if (active_) {
        if (error) {
            error->clear();
        }
        return true;
    }
    if (!codeIsValid(code_)) {
        code_ = generateCode();
        emit codeChanged(code_, formattedCode());
    }
    persist();

    if (!linkStarted_) {
        QString linkError;
        if (!link_->start(bindAddress_, 0, &linkError)) {
            if (error) {
                *error = linkError;
            }
            setError(QStringLiteral("无法启动 UDP 通道：%1").arg(linkError));
            return false;
        }
        linkStarted_ = true;
    } else {
        link_->resetPeer();
    }

    link_->setPsk(pskFromCode(code_));
    link_->setForceRelay(forceRelay_);
    link_->setDeviceId(
        deviceId_.isEmpty() ? codeId(code_) : deviceId_);
    link_->setLabel(machineName());
    applySessionSettings();

    QString signalingError;
    if (!startSignaling(&signalingError)) {
        if (error) {
            *error = signalingError;
        }
        setError(signalingError);
        return false;
    }
    active_ = true;
    state_ = State::Connecting;
    emit stateChanged(state_);
    syncAnnouncement();
    return true;
}

bool HostController::startSignaling(QString *error)
{
    if (error) {
        error->clear();
    }
    const auto address = parseServerAddress(server_);
    if (!address.has_value()) {
        if (error) {
            *error = QStringLiteral("信令服务器地址无效");
        }
        return false;
    }

    signaling_->setAutoReconnect(true);
    signaling_->setServer(address->host, address->port);
    signaling_->setToken(token_);
    if (!signaling_->startHost(
            code_,
            machineName(),
            link_->localCandidates())) {
        if (error) {
            *error = signaling_->lastErrorText().isEmpty()
                ? QStringLiteral("无法注册连接码")
                : signaling_->lastErrorText();
        }
        return false;
    }
    return true;
}

void HostController::stop()
{
    if (!active_ && state_ == State::Idle) {
        return;
    }
    stopping_ = true;
    session_->stop();
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    peerName_.clear();
    peerSid_.clear();
    statistics_ = {};
    active_ = false;
    stopping_ = false;
    emit peerChanged({}, false);
    setState(State::Idle);
}

void HostController::resetPeerState()
{
    stopping_ = true;
    session_->suspend();
    if (linkStarted_) {
        link_->resetPeer();
    }
    stopping_ = false;
    peerName_.clear();
    peerSid_.clear();
    statistics_ = {};
}

bool HostController::regenerateCode(QString *error)
{
    QString next;
    do {
        next = generateCode();
    } while (next == code_);

    code_ = next;
    persist();
    emit codeChanged(code_, formattedCode());

    if (!active_) {
        return start(error);
    }

    stopping_ = true;
    session_->stop();
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    peerName_.clear();
    peerSid_.clear();
    statistics_ = {};
    link_->setPsk(pskFromCode(code_));
    link_->setDeviceId(
        deviceId_.isEmpty() ? codeId(code_) : deviceId_);
    QString signalingError;
    const bool started = startSignaling(&signalingError);
    stopping_ = false;
    if (!started) {
        active_ = false;
        if (error) {
            *error = signalingError;
        }
        setError(signalingError);
        return false;
    }
    emit peerChanged({}, false);
    state_ = State::Connecting;
    emit stateChanged(state_);
    syncAnnouncement();
    return true;
}

QString HostController::formattedCode() const
{
    return formatCode(code_);
}

void HostController::setServer(const QString &server)
{
    server_ = server.trimmed();
    persist();
    syncAnnouncement();
}

void HostController::setToken(const QString &token)
{
    token_ = token.trimmed();
    persist();
}

void HostController::setVideoSettings(const VideoSettings &settings)
{
    video_ = settings;
    video_.clamp();
    applySessionSettings();
    persist();
    emit videoSettingsChanged(video_);
    emit monitorsChanged(monitors());
}

void HostController::setQualityPreset(const QString &id)
{
    const QualityPreset &preset = qualityPreset(id);
    VideoSettings settings = video_;
    settings.maxWidth = preset.maxWidth;
    settings.quality = preset.quality;
    settings.fps = preset.fps;
    setVideoSettings(settings);
}

void HostController::setAllowInput(bool allow)
{
    allowInput_ = allow;
    if (session_) {
        session_->setAllowInput(allow);
    }
    persist();
}

void HostController::setClipboardEnabled(bool enabled)
{
    VideoSettings settings = video_;
    settings.clipboard = enabled;
    setVideoSettings(settings);
}

void HostController::setAudioEnabled(bool enabled)
{
    VideoSettings settings = video_;
    settings.audio = enabled;
    setVideoSettings(settings);
}

void HostController::setForceRelay(bool forceRelay)
{
    forceRelay_ = forceRelay;
    link_->setForceRelay(forceRelay_);
    persist();
}

void HostController::setReceiveDirectory(const QString &directory)
{
    receiveDirectory_ = directory;
    if (session_) {
        session_->setReceiveDirectory(directory);
    }
    persist();
}

void HostController::setState(State state)
{
    if (state_ == state) {
        syncAnnouncement();
        return;
    }
    state_ = state;
    emit stateChanged(state_);
    syncAnnouncement();
}

void HostController::setError(const QString &message)
{
    lastError_ = message;
    emit errorOccurred(message);
    setState(State::Error);
}

void HostController::updateCandidates()
{
    if (!active_ || !signaling_->connected()) {
        return;
    }
    signaling_->updateCandidates(link_->localCandidates());
}

void HostController::syncAnnouncement()
{
    if (!lanDiscovery_) {
        return;
    }
    if (!active_
        || state_ == State::Idle
        || state_ == State::Error) {
        lanDiscovery_->withdraw();
        return;
    }
    lanDiscovery_->publish(deviceName(), code_, server_);
}

void HostController::onRegistered(const RegistrationInfo &info)
{
    lastError_.clear();
    const double mbps = std::max(
        info.relay.mbps,
        std::max(0.0, info.relayMbps));
    session_->setRateLimit(mbps);

    if (info.relay.valid()) {
        link_->discoverReflexive(
            info.relay.host,
            info.relay.port);
    } else {
        updateCandidates();
    }
    if (peerSid_.isEmpty()) {
        setState(State::Waiting);
    }
}

void HostController::onPeerJoined(const PeerInfo &info)
{
    if (!info.sid.isEmpty() && info.sid == peerSid_) {
        return;
    }

    resetPeerState();
    link_->setPsk(pskFromCode(code_));
    link_->setForceRelay(forceRelay_);
    link_->setDeviceId(
        deviceId_.isEmpty() ? codeId(code_) : deviceId_);
    link_->addCandidates(info.candidates);
    if (info.relay.valid()) {
        link_->offerRelay(info.relay.host, info.relay.port);
        link_->setRelayOutTag(info.relay.tag);
    }

    applySessionSettings();
    (void)session_->start();
    peerSid_ = info.sid;
    peerName_ = info.name;
    emit peerChanged(peerName_, true);
    setState(State::Connecting);
    link_->beginHandshake();
}

void HostController::onPeerCandidatesChanged(
    const QList<Candidate> &candidates)
{
    if (linkStarted_) {
        link_->addCandidates(candidates);
    }
}

void HostController::onPeerLeft(const QString &reason)
{
    if (stopping_ || !active_) {
        return;
    }
    emit logMessage(QStringLiteral("观看端已离开：%1").arg(reason));
    resetPeerState();
    emit peerChanged({}, false);
    setState(State::Waiting);
}

void HostController::onSignalingError(
    const QString &reason,
    const QString &text)
{
    if (stopping_ || !active_) {
        return;
    }
    const QString message = text.isEmpty()
        ? QStringLiteral("信令错误：%1").arg(reason)
        : text;
    if (reason == QStringLiteral("network")
        || reason == QStringLiteral("timeout")) {
        emit logMessage(message);
        return;
    }
    failActive(message);
}

void HostController::onLinkState(const QString &state)
{
    emit linkStateChanged(state);
    if (stopping_ || !active_) {
        return;
    }
    if (state == QStringLiteral("direct")
        || state == QStringLiteral("relay")) {
        (void)session_->start();
        setState(
            session_->running() ? State::Streaming : State::Connecting);
        emit monitorsChanged(monitors());
    } else if (state == QStringLiteral("punching")) {
        setState(State::Connecting);
    } else if (state == QStringLiteral("closed")) {
        resetPeerState();
        emit peerChanged({}, false);
        setState(State::Waiting);
        emit logMessage(QStringLiteral("观看端链路已关闭，等待重新接入"));
    }
}

void HostController::onSessionEvent(const QJsonObject &message)
{
    const QString kind =
        message.value(QStringLiteral("t")).toString();
    if (kind == QStringLiteral("settings_ack")
        || kind == QStringLiteral("audio")
        || kind == QStringLiteral("info")) {
        video_ = session_->settings();
        persist();
        emit videoSettingsChanged(video_);
        emit monitorsChanged(monitors());
    } else if (kind == QStringLiteral("link")) {
        emit linkStateChanged(
            message.value(QStringLiteral("state")).toString());
    }
    emit eventReceived(message);
}

bool HostController::streaming() const
{
    return active_ && session_ && session_->running();
}

QJsonObject HostController::viewerInfo() const
{
    return session_ ? session_->info() : QJsonObject{};
}

QList<MonitorInfo> HostController::monitors() const
{
    if (session_) {
        const QList<MonitorInfo> values = session_->monitors();
        if (!values.isEmpty()) {
            return values;
        }
    }
    return screenSource_ ? screenSource_->monitors() : QList<MonitorInfo>{};
}

bool HostController::sendFile(
    const QString &path,
    QString *error)
{
    return session_ && session_->sendFile(path, error);
}

bool HostController::cancelFile()
{
    return session_ && session_->cancelFile();
}

void HostController::failActive(const QString &message)
{
    if (stopping_) {
        return;
    }
    stopping_ = true;
    active_ = false;
    session_->stop();
    if (linkStarted_) {
        link_->resetPeer();
    }
    signaling_->stop();
    peerName_.clear();
    peerSid_.clear();
    statistics_ = {};
    stopping_ = false;
    emit peerChanged({}, false);
    setError(message);
}

} // namespace pd
