#pragma once

#include <QDateTime>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QTimer;
class QUdpSocket;

namespace pd {

struct NearbyDevice {
    QString instanceId;
    QString name;
    QString code;
    QString server;
    QDateTime seenAt;
    QHostAddress address;
};

class LanDiscovery final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 7902;
    static constexpr int kMaxPayloadBytes = 768;
    static constexpr int kMaxDevices = 32;

    struct Options {
        QHostAddress bindAddress = QHostAddress::AnyIPv4;
        quint16 bindPort = kDefaultPort;
        QHostAddress targetAddress = QHostAddress::Broadcast;
        quint16 targetPort = kDefaultPort;
        bool allowLoopback = false;
        int announceIntervalMs = 2000;
        int expireAfterMs = 8000;
        int sweepIntervalMs = 500;
        int maxDevices = kMaxDevices;
    };

    explicit LanDiscovery(QObject *parent = nullptr);
    ~LanDiscovery() override;

    LanDiscovery(const LanDiscovery &) = delete;
    LanDiscovery &operator=(const LanDiscovery &) = delete;

    void setOptions(const Options &options);
    [[nodiscard]] Options options() const { return options_; }

    bool start(QString *error = nullptr);
    void stop();
    [[nodiscard]] bool active() const;

    void publish(
        const QString &name,
        const QString &code,
        const QString &server);
    void withdraw();

    [[nodiscard]] QList<NearbyDevice> devices() const;
    [[nodiscard]] quint16 boundPort() const;
    [[nodiscard]] QString instanceId() const { return instanceId_; }

signals:
    void devicesChanged(const QList<pd::NearbyDevice> &devices);
    void errorOccurred(const QString &message);
    void logMessage(const QString &message);

private:
    struct Announcement {
        QString name;
        QString code;
        QString server;

        [[nodiscard]] bool valid() const
        {
            return !code.isEmpty() && !server.isEmpty();
        }
    };

    void onReadyRead();
    void sendAnnouncement();
    void sendQuery();
    void sendAnnouncementTo(const QHostAddress &address, quint16 port);
    void sendBye(const Announcement &announcement);
    void sendPayload(
        const QByteArray &payload,
        const QHostAddress &address,
        quint16 port);
    void sendPayloadEverywhere(const QByteArray &payload);
    void pruneExpired();
    void applyDevices(const QList<NearbyDevice> &devices);
    void updateAnnounceTimer();
    [[nodiscard]] QByteArray announcementPayload(
        const Announcement &announcement) const;

    Options options_;
    QUdpSocket *socket_ = nullptr;
    QTimer *announceTimer_ = nullptr;
    QTimer *sweepTimer_ = nullptr;
    QString instanceId_;
    QString lastSendError_;
    QList<NearbyDevice> devices_;
    Announcement published_;
};

} // namespace pd

Q_DECLARE_METATYPE(pd::NearbyDevice)
