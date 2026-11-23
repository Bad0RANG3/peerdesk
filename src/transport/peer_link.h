#pragma once

#include "core/crypto.h"
#include "core/protocol.h"
#include "core/reliable.h"
#include "core/video.h"
#include "transport/video_send_pipeline.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <memory>
#include <optional>

class QTimer;
class QUdpSocket;

namespace pd {

inline constexpr int kHelloIntervalMs = 250;
inline constexpr int kHelloTimeoutMs = 15000;
inline constexpr int kPunchIntervalMs = 100;
inline constexpr int kKeepaliveMs = 4000;
inline constexpr int kHandshakePingIntervalMs = 1000;
inline constexpr int kRelayEscalateAfterMs = 6000;
inline constexpr int kRxTimeoutMs = 25000;
inline constexpr int kLinkTickMs = 20;

struct Candidate {
    QString host;
    quint16 port = 0;
    QString kind = QStringLiteral("host");

    [[nodiscard]] bool operator==(const Candidate &other) const
    {
        return port == other.port && host.compare(other.host, Qt::CaseInsensitive) == 0;
    }
};

struct LinkStats {
    double rttMs = 0;
    quint64 sentBytes = 0;
    quint64 recvBytes = 0;
    quint64 sentPackets = 0;
    quint64 recvPackets = 0;
    double fps = 0;
    double mbps = 0;
    QString mode = QStringLiteral("idle");
    QString path;
    bool usingRelay = false;
    QString reflexive;
};

class PeerLink : public QObject {
    Q_OBJECT

public:
    explicit PeerLink(
        bool isHost,
        const QByteArray &psk = {},
        QObject *parent = nullptr);
    ~PeerLink() override;

    [[nodiscard]] bool start(
        const QString &bindAddress = QStringLiteral("0.0.0.0"),
        quint16 port = 0,
        QString *error = nullptr);
    void resetPeer();
    void close();

    void setPsk(const QByteArray &psk);
    void setDeviceId(const QString &deviceId);
    void setLabel(const QString &label);
    void setForceRelay(bool forceRelay);

    void addCandidates(const QList<Candidate> &candidates);
    void beginHandshake();
    void offerRelay(const QString &host, quint16 port);
    void setRelayOutTag(quint32 tag);

    void discoverReflexive(
        const QString &host,
        quint16 port,
        int timeoutMs = 2500);

    void sendControl(const QJsonObject &message);
    void sendVideo(const QByteArray &jpeg, bool keyframe = false);
    void sendAudio(const QByteArray &payload);

    [[nodiscard]] bool established() const { return established_; }
    [[nodiscard]] bool isHost() const { return isHost_; }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] LinkStats stats() const { return stats_; }
    [[nodiscard]] QString remoteDeviceId() const { return remoteDeviceId_; }
    [[nodiscard]] QString remoteSessionId() const { return remoteSessionId_; }
    [[nodiscard]] QString localAddressText() const;
    [[nodiscard]] quint16 localPort() const;
    [[nodiscard]] QString videoPacketizerThreadName() const;
    [[nodiscard]] QList<Candidate> localCandidates() const;
    [[nodiscard]] QList<Candidate> peerCandidates() const;

signals:
    void stateChanged(const QString &state);
    void statsChanged(const pd::LinkStats &stats);
    void controlMessageReceived(const QJsonObject &message);
    void videoFrameReceived(const QByteArray &frame, bool keyframe);
    void audioReceived(const QByteArray &payload);
    void peerHello(const QString &deviceId, const QString &sessionId);
    void reflexiveDiscoveryFinished(bool found, const QString &host, quint16 port);
    void logMessage(const QString &message);

private:
    struct Destination {
        QHostAddress address;
        quint16 port = 0;
    };

    void tick();
    void processDatagram(
        const QByteArray &datagram,
        const QHostAddress &sender,
        quint16 senderPort);
    void handleStun(const QByteArray &datagram);
    void handleHello(const QByteArray &body, const QHostAddress &sender, quint16 port);
    void handleHelloAck(const QByteArray &body, const QHostAddress &sender, quint16 port);
    void handleEncrypted(
        const QByteArray &headerBytes,
        const Header &header,
        const QByteArray &body,
        const QHostAddress &sender,
        quint16 port);

    void sendHello(bool force);
    [[nodiscard]] QByteArray buildHelloBody();
    [[nodiscard]] QByteArray pskVerifier() const;
    [[nodiscard]] QList<Destination> targetDestinations() const;
    [[nodiscard]] QList<Destination> remoteDestinations() const;
    bool sendPacket(
        PacketType type,
        quint8 stream,
        quint8 flags,
        const QByteArray &payload,
        const std::optional<Destination> &destination = std::nullopt,
        bool plain = false,
        std::optional<quint64> sequence = std::nullopt);

    void sendControlRaw(quint32 sequence, const QByteArray &payload, quint8 flags);
    void maybeAck();
    void deliverControl(const QByteArray &payload);
    void sendPing();
    void handlePong(const QByteArray &payload);
    void requestKeyframe();
    void onVideoPacketsReady(const VideoPacketBatch &batch);

    void setState(const QString &state);
    void updateStats(double now);
    void pruneWindows(double now);
    void emitLog(const QString &message);

    [[nodiscard]] bool hasPeer() const { return peerPort_ != 0 && !peerAddress_.isNull(); }

    bool isHost_ = false;
    QByteArray psk_;
    QString deviceId_;
    QString label_;
    bool forceRelay_ = false;
    bool closing_ = false;

    QUdpSocket *socket_ = nullptr;
    QTimer *timer_ = nullptr;

    QHostAddress localAddress_;
    quint16 localPort_ = 0;

    QList<Candidate> peerCandidates_;
    QHostAddress peerAddress_;
    quint16 peerPort_ = 0;

    QHostAddress relayAddress_;
    quint16 relayPort_ = 0;
    quint32 relayOutTag_ = 0;
    bool usingRelay_ = false;

    QHostAddress reflexiveAddress_;
    quint16 reflexivePort_ = 0;
    bool stunPending_ = false;
    QByteArray stunTransactionId_;
    double stunDeadline_ = 0;

    QByteArray sessionId_;
    quint32 sessionTag_ = 0;
    KeyPair keyPair_;
    std::unique_ptr<CryptoContext> crypto_;
    QByteArray helloClientBytes_;
    QByteArray helloHostBytes_;

    double handshakeStarted_ = 0;
    double helloSentAt_ = -1e9;
    double lastRxAt_ = 0;
    double lastPingAt_ = 0;
    double lastAckAt_ = 0;
    double lastStatsAt_ = 0;
    bool established_ = false;
    QString remoteSessionId_;
    QString remoteDeviceId_;

    VideoSendPipeline videoSend_;
    VideoReceiver videoReceiver_;
    ControlFragments controlFragments_;
    ReliableStream controlStream_;
    quint16 pingSequence_ = 0;
    QHash<quint16, double> pingsOut_;
    quint64 keyframeRequests_ = 0;

    QString state_ = QStringLiteral("idle");
    LinkStats stats_;
    QList<QPair<double, int>> sentBytesWindow_;
    QList<QPair<double, int>> recvBytesWindow_;
    QList<double> sentFramesWindow_;
    QList<double> recvFramesWindow_;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::LinkStats)
