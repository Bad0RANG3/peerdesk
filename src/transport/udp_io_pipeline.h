#pragma once

#include "core/crypto.h"
#include "core/protocol.h"

#include <QByteArray>
#include <QHostAddress>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <optional>

namespace pd {

struct UdpSendJob {
    PacketType type = PacketType::Data;
    quint8 stream = 0;
    quint8 flags = 0;
    QByteArray payload;
    QHostAddress address;
    quint16 port = 0;
    bool plain = false;
    bool wrapRelay = false;
    quint32 sessionTag = 0;
    quint32 relayTag = 0;
    std::optional<quint64> sequence;
};

class UdpIoWorker;

class UdpIoPipeline final : public QObject {
    Q_OBJECT

public:
    explicit UdpIoPipeline(QObject *parent = nullptr);
    ~UdpIoPipeline() override;

    bool start(
        const QString &bindAddress,
        quint16 port,
        QHostAddress *localAddress,
        quint16 *localPort,
        QString *error = nullptr);
    void stop();

    void setSession(const SessionKeys &keys, bool isHost);
    void resetSession();

    bool send(const UdpSendJob &job);
    bool sendRawDatagram(
        const QByteArray &datagram,
        const QHostAddress &address,
        quint16 port);

    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] quint64 sentBytes() const { return sentBytes_.load(); }
    [[nodiscard]] quint64 sentPackets() const { return sentPackets_.load(); }
    [[nodiscard]] QString ioThreadName() const;

signals:
    void datagramReceived(
        const QByteArray &datagram,
        const QHostAddress &sender,
        quint16 senderPort);
    void logMessage(const QString &message);

private:
    QThread thread_;
    UdpIoWorker *worker_ = nullptr;
    std::atomic_bool running_{false};
    std::atomic<quint64> sentBytes_{0};
    std::atomic<quint64> sentPackets_{0};
    mutable QMutex threadNameMutex_;
    QString ioThreadName_;
};

} // namespace pd
