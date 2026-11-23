#pragma once

#include "core/protocol.h"
#include "transport/authenticated_packet_pipeline.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

namespace pd {

class UdpIoPipeline;
class LinkFeedbackWorker;

struct LinkFeedbackRoute {
    quint64 generation = 0;
    quint64 udpGeneration = 0;
    quint64 authGeneration = 0;
    bool sessionReady = false;
    bool established = false;
    QHostAddress address;
    quint16 port = 0;
    bool wrapRelay = false;
    quint32 sessionTag = 0;
    quint32 relayTag = 0;
};

class LinkFeedbackPipeline final : public QObject {
    Q_OBJECT

public:
    explicit LinkFeedbackPipeline(
        UdpIoPipeline *udpIo,
        AuthenticatedPacketPipeline *authenticatedPackets,
        QObject *parent = nullptr);
    ~LinkFeedbackPipeline() override;

    void start();
    void stop();
    void resetSession();
    void setRoute(const LinkFeedbackRoute &route);

    void submitNack(
        quint64 generation,
        quint32 frameId,
        const QList<quint16> &missing);
    void submitKeyframe(quint64 generation);
    void submitWireCommand(
        quint64 generation,
        quint64 authGeneration,
        const LinkWireCommand &command);

    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] quint64 generation() const { return generation_.load(); }
    [[nodiscard]] QString workerThreadName() const
    {
        return workerThreadName_;
    }

private:
    QThread thread_;
    LinkFeedbackWorker *worker_ = nullptr;
    std::atomic_bool running_{false};
    std::atomic<quint64> generation_{0};
    QString workerThreadName_;
};

} // namespace pd
