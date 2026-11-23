#pragma once

#include "core/protocol.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>

namespace pd {

struct LinkWireCommand {
    PacketType type = PacketType::Data;
    quint8 stream = 0;
    quint8 flags = 0;
    QByteArray payload;
};

class AuthenticatedPacketWorker;

class AuthenticatedPacketPipeline final : public QObject {
    Q_OBJECT

public:
    using WireCommandHandler =
        std::function<void(quint64 generation, const LinkWireCommand &command)>;

    explicit AuthenticatedPacketPipeline(QObject *parent = nullptr);
    ~AuthenticatedPacketPipeline() override;

    void start();
    void stop();
    void resetSession();

    void receive(const Header &header, const QByteArray &payload);
    void sendControl(const QJsonObject &message);
    void sendPing();
    void requestKeyframe(quint64 expectedGeneration);
    void setWireCommandHandler(WireCommandHandler handler);

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] quint64 sessionGeneration() const { return generation_; }
    [[nodiscard]] QString workerThreadName() const
    {
        return workerThreadName_;
    }

signals:
    void controlMessageReceived(const QJsonObject &message);
    void videoDataReceived(const QByteArray &payload, bool keyframe);
    void videoParityReceived(const QByteArray &payload);
    void audioReceived(const QByteArray &payload);
    void videoRetransmitRequested(
        quint32 frameId,
        const QList<quint16> &missing);
    void wireCommandReady(const pd::LinkWireCommand &command);
    void rttMeasured(double rttMs);
    void peerByeReceived();

private:
    void observeWorker(quint64 generation, const QString &threadName);

    QThread thread_;
    AuthenticatedPacketWorker *worker_ = nullptr;
    QString workerThreadName_;
    quint64 generation_ = 0;
    bool running_ = false;
    WireCommandHandler wireCommandHandler_;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::LinkWireCommand)
