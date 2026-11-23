#include "transport/udp_io_pipeline.h"

#include "core/reliable.h"

#include <QHostInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <memory>

namespace pd {

class UdpIoWorker final : public QObject {
    Q_OBJECT

public:
    explicit UdpIoWorker(
        std::atomic<quint64> *sentBytes,
        std::atomic<quint64> *sentPackets)
        : sentBytes_(sentBytes)
        , sentPackets_(sentPackets)
    {
    }

    void start(
        const QString &bindAddress,
        quint16 port,
        quint64 generation,
        bool *ok,
        QString *error,
        QHostAddress *localAddress,
        quint16 *localPort,
        QString *threadName)
    {
        *ok = false;
        error->clear();
        localAddress->clear();
        *localPort = 0;
        *threadName = QThread::currentThread()->objectName();
        generation_ = generation;

        if (!socket_) {
            socket_ = new QUdpSocket(this);
            connect(
                socket_,
                &QUdpSocket::readyRead,
                this,
                &UdpIoWorker::pumpIncoming);
            connect(
                socket_,
                &QUdpSocket::errorOccurred,
                this,
                [this](QAbstractSocket::SocketError) {
                    if (running_) {
                        emit logMessage(
                            QStringLiteral("udp socket error: %1")
                                .arg(socket_->errorString()));
                    }
                });
        }
        if (socket_->state() != QAbstractSocket::UnconnectedState) {
            *error = QStringLiteral("UDP socket is already open");
            return;
        }

        QHostAddress address(bindAddress);
        if (address.isNull() && bindAddress != QStringLiteral("0.0.0.0")) {
            const QHostInfo info = QHostInfo::fromName(bindAddress);
            if (!info.addresses().isEmpty()) {
                address = info.addresses().constFirst();
            }
        }
        if (address.isNull()) {
            *error = QStringLiteral("Invalid bind address: %1").arg(bindAddress);
            return;
        }

        if (!socket_->bind(address, port, QAbstractSocket::DefaultForPlatform)) {
            *error = socket_->errorString();
            return;
        }

        socket_->setSocketOption(
            QAbstractSocket::ReceiveBufferSizeSocketOption,
            4 * 1024 * 1024);
        socket_->setSocketOption(
            QAbstractSocket::SendBufferSizeSocketOption,
            1024 * 1024);
        *localAddress = socket_->localAddress();
        *localPort = socket_->localPort();
        running_ = true;
        *ok = true;
    }

    void stop()
    {
        running_ = false;
        crypto_.reset();
        if (socket_) {
            socket_->close();
        }
    }

    void setSession(
        const SessionKeys &keys,
        bool isHost,
        quint64 generation)
    {
        crypto_ = std::make_unique<CryptoContext>(keys, isHost);
        generation_ = generation;
    }

    void resetSession(quint64 generation)
    {
        crypto_.reset();
        generation_ = generation;
        sentBytes_->store(0);
        sentPackets_->store(0);
    }

    void sendJob(const UdpSendJob &job)
    {
        if (!running_ || !socket_
            || socket_->state() != QAbstractSocket::BoundState
            || job.address.isNull() || job.port == 0) {
            return;
        }

        if (!job.plain && !crypto_) {
            return;
        }

        quint64 sequenceValue = 0;
        QByteArray body;
        quint16 payloadLength = 0;
        if (job.plain) {
            if (job.payload.size() > 0xFFFF) {
                emit logMessage(QStringLiteral("plain UDP payload is too large"));
                return;
            }
            body = job.payload;
            payloadLength = static_cast<quint16>(job.payload.size());
            if (job.sequence) {
                sequenceValue = *job.sequence;
            }
        } else {
            if (job.payload.size() + kAuthTagLen > 0xFFFF) {
                emit logMessage(QStringLiteral("encrypted UDP payload is too large"));
                return;
            }
            sequenceValue = job.sequence
                ? *job.sequence
                : crypto_->nextSeq();
            payloadLength = static_cast<quint16>(
                job.payload.size() + kAuthTagLen);
        }

        Header header;
        header.type = job.type;
        header.seq = static_cast<quint32>(sequenceValue);
        header.tsMs = static_cast<quint32>(
            static_cast<quint64>(
                ReliableStream::currentTimeSeconds() * 1000.0));
        header.stream = job.stream;
        header.flags = job.flags;
        header.plen = payloadLength;
        header.session = job.sessionTag;
        const QByteArray headerBytes = header.pack();

        if (!job.plain) {
            body = crypto_->seal(
                headerBytes,
                job.payload,
                sequenceValue);
        }
        QByteArray datagram = headerBytes + body;
        if (job.wrapRelay) {
            datagram = wrapRelay(
                job.relayTag != 0 ? job.relayTag : job.sessionTag,
                datagram);
        }

        const qint64 written = socket_->writeDatagram(
            datagram,
            job.address,
            job.port);
        if (written < 0) {
            emit logMessage(
                QStringLiteral("udp write failed: %1")
                    .arg(socket_->errorString()));
            return;
        }
        sentBytes_->fetch_add(static_cast<quint64>(datagram.size()));
        sentPackets_->fetch_add(1);
    }

    void sendRawDatagram(
        const QByteArray &datagram,
        const QHostAddress &address,
        quint16 port)
    {
        if (!running_ || !socket_
            || socket_->state() != QAbstractSocket::BoundState
            || address.isNull() || port == 0) {
            return;
        }
        (void)socket_->writeDatagram(datagram, address, port);
    }

signals:
    void packetReceived(const pd::UdpReceivePacket &packet);
    void logMessage(const QString &message);

private:
    void processIncoming(
        const QByteArray &datagram,
        const QHostAddress &sender,
        quint16 senderPort)
    {
        UdpReceivePacket packet;
        packet.generation = generation_;
        packet.wireBytes = static_cast<quint64>(datagram.size());
        packet.sender = sender;
        packet.senderPort = senderPort;

        if (datagram.size() >= 20
            && ((static_cast<quint8>(datagram[0]) == 0
                 && static_cast<quint8>(datagram[1]) == 0x01)
                || (static_cast<quint8>(datagram[0]) == 0x01
                    && static_cast<quint8>(datagram[1]) == 0x01))) {
            packet.kind = UdpReceiveKind::Stun;
            packet.payload = datagram;
            emit packetReceived(packet);
            return;
        }

        QByteArray packetBytes = datagram;
        if (datagram.size() >= kRelayHeaderLen
            && std::memcmp(datagram.constData(), "RLY1", 4) == 0) {
            const auto relayed = unwrapRelay(datagram);
            if (!relayed) {
                return;
            }
            packetBytes = relayed->second;
        }

        const auto parsed = Header::parse(packetBytes);
        if (!parsed) {
            return;
        }
        packet.header = *parsed;
        const QByteArray body = packetBytes.mid(kHeaderLen);
        const bool plaintext =
            parsed->type == PacketType::Hello
            || parsed->type == PacketType::HelloAck
            || parsed->type == PacketType::Punch
            || parsed->type == PacketType::PunchAck
            || (parsed->type == PacketType::Bye && !crypto_);
        if (plaintext) {
            packet.kind = UdpReceiveKind::Plaintext;
            packet.payload = body;
            emit packetReceived(packet);
            return;
        }
        if (!crypto_) {
            return;
        }

        const auto plain = crypto_->open(
            packetBytes.left(kHeaderLen),
            body,
            parsed->seq);
        if (!plain) {
            return;
        }
        packet.kind = UdpReceiveKind::Encrypted;
        packet.payload = *plain;
        emit packetReceived(packet);
    }

    void pumpIncoming()
    {
        if (!running_ || !socket_) {
            return;
        }
        while (socket_->hasPendingDatagrams()) {
            QByteArray datagram(
                static_cast<qsizetype>(
                    std::max<qint64>(0, socket_->pendingDatagramSize())),
                '\0');
            QHostAddress sender;
            quint16 senderPort = 0;
            const qint64 received = socket_->readDatagram(
                datagram.data(),
                datagram.size(),
                &sender,
                &senderPort);
            if (received < 0) {
                continue;
            }
            datagram.truncate(static_cast<qsizetype>(received));
            processIncoming(datagram, sender, senderPort);
        }
    }

    QUdpSocket *socket_ = nullptr;
    std::unique_ptr<CryptoContext> crypto_;
    std::atomic<quint64> *sentBytes_ = nullptr;
    std::atomic<quint64> *sentPackets_ = nullptr;
    quint64 generation_ = 0;
    bool running_ = false;
};

UdpIoPipeline::UdpIoPipeline(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QHostAddress>("QHostAddress");
    qRegisterMetaType<pd::UdpReceivePacket>("pd::UdpReceivePacket");

    worker_ = new UdpIoWorker(&sentBytes_, &sentPackets_);
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("PeerDeskUdpIo"));
    connect(
        &thread_,
        &QThread::finished,
        worker_,
        &QObject::deleteLater);
    connect(
        worker_,
        &UdpIoWorker::packetReceived,
        this,
        &UdpIoPipeline::packetReceived);
    connect(
        worker_,
        &UdpIoWorker::logMessage,
        this,
        &UdpIoPipeline::logMessage);
    thread_.start();
}

UdpIoPipeline::~UdpIoPipeline()
{
    stop();
    if (thread_.isRunning()) {
        thread_.quit();
        thread_.wait();
    }
    worker_ = nullptr;
}

bool UdpIoPipeline::start(
    const QString &bindAddress,
    quint16 port,
    QHostAddress *localAddress,
    quint16 *localPort,
    QString *error)
{
    if (!worker_ || localAddress == nullptr || localPort == nullptr) {
        if (error) {
            *error = QStringLiteral("UDP I/O worker is unavailable");
        }
        return false;
    }

    bool ok = false;
    QString failure;
    QHostAddress boundAddress;
    quint16 boundPort = 0;
    QString threadName;
    QMetaObject::invokeMethod(
        worker_,
        [this,
         bindAddress,
         port,
         &ok,
         &failure,
         &boundAddress,
         &boundPort,
         &threadName]() {
            worker_->start(
                bindAddress,
                port,
                receiveGeneration_.load(),
                &ok,
                &failure,
                &boundAddress,
                &boundPort,
                &threadName);
        },
        Qt::BlockingQueuedConnection);

    running_.store(ok);
    if (!ok) {
        if (error) {
            *error = failure;
        }
        return false;
    }

    *localAddress = boundAddress;
    *localPort = boundPort;
    {
        QMutexLocker locker(&threadNameMutex_);
        ioThreadName_ = threadName;
    }
    return true;
}

void UdpIoPipeline::stop()
{
    receiveGeneration_.fetch_add(1);
    if (!worker_) {
        running_.store(false);
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [this]() { worker_->stop(); },
        Qt::BlockingQueuedConnection);
    running_.store(false);
}

void UdpIoPipeline::setSession(const SessionKeys &keys, bool isHost)
{
    if (!worker_) {
        return;
    }
    const quint64 generation = receiveGeneration_.fetch_add(1) + 1;
    QMetaObject::invokeMethod(
        worker_,
        [this, keys, isHost, generation]() {
            worker_->setSession(keys, isHost, generation);
        },
        Qt::BlockingQueuedConnection);
}

void UdpIoPipeline::resetSession()
{
    if (!worker_) {
        return;
    }
    const quint64 generation = receiveGeneration_.fetch_add(1) + 1;
    QMetaObject::invokeMethod(
        worker_,
        [this, generation]() { worker_->resetSession(generation); },
        Qt::BlockingQueuedConnection);
    sentBytes_.store(0);
    sentPackets_.store(0);
}

bool UdpIoPipeline::send(const UdpSendJob &job)
{
    if (!running_.load() || !worker_) {
        return false;
    }
    QMetaObject::invokeMethod(
        worker_,
        [this, job]() { worker_->sendJob(job); },
        Qt::QueuedConnection);
    return true;
}

bool UdpIoPipeline::sendRawDatagram(
    const QByteArray &datagram,
    const QHostAddress &address,
    quint16 port)
{
    if (!running_.load() || !worker_) {
        return false;
    }
    QMetaObject::invokeMethod(
        worker_,
        [this, datagram, address, port]() {
            worker_->sendRawDatagram(datagram, address, port);
        },
        Qt::QueuedConnection);
    return true;
}

QString UdpIoPipeline::ioThreadName() const
{
    QMutexLocker locker(&threadNameMutex_);
    return ioThreadName_;
}

} // namespace pd

#include "udp_io_pipeline.moc"
