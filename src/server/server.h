#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace pd {

inline constexpr int kDefaultConnectMax = 10;
inline constexpr double kDefaultConnectWindowSeconds = 60.0;
inline constexpr int kMaxSignalLine = 1 << 20;

class TokenBucket {
public:
    explicit TokenBucket(double mbps = 0.0, double burstSeconds = 1.0);

    [[nodiscard]] bool enabled() const { return rate_ > 0.0; }
    [[nodiscard]] bool allow(
        qint64 size, std::optional<double> now = std::nullopt);

    [[nodiscard]] double mbps() const { return mbps_; }
    [[nodiscard]] double rate() const { return rate_; }
    [[nodiscard]] double capacity() const { return capacity_; }
    [[nodiscard]] double tokens() const { return tokens_; }
    [[nodiscard]] quint64 passed() const { return passed_; }
    [[nodiscard]] quint64 dropped() const { return dropped_; }
    [[nodiscard]] quint64 bytes() const { return bytes_; }

private:
    double mbps_;
    double rate_;
    double capacity_;
    double tokens_;
    double last_ = 0.0;
    quint64 passed_ = 0;
    quint64 dropped_ = 0;
    quint64 bytes_ = 0;
};

class ConnectRateLimiter {
public:
    explicit ConnectRateLimiter(
        int maxAttempts = kDefaultConnectMax,
        double windowSeconds = kDefaultConnectWindowSeconds);

    [[nodiscard]] bool allow(
        const QString &ip, std::optional<double> now = std::nullopt);

    [[nodiscard]] int maxAttempts() const { return maxAttempts_; }
    [[nodiscard]] double windowSeconds() const { return windowSeconds_; }
    [[nodiscard]] int trackedAddresses() const { return attempts_.size(); }
    void clear();

private:
    void prune(double now);

    int maxAttempts_;
    double windowSeconds_;
    QHash<QString, QList<double>> attempts_;
};

class SignalingServer : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString host = QStringLiteral("0.0.0.0");
        quint16 port = 7900;
        quint16 udpPort = 7901;
        QString token;
        double relayMbps = 0.0;
        int connectMax = kDefaultConnectMax;
        double connectWindow = kDefaultConnectWindowSeconds;
    };

    explicit SignalingServer(QObject *parent = nullptr);
    explicit SignalingServer(Options options, QObject *parent = nullptr);
    ~SignalingServer() override;

    [[nodiscard]] bool start(QString *error = nullptr);
    void stop();

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 signalingPort() const;
    [[nodiscard]] quint16 relayPort() const;
    [[nodiscard]] QString publicHost() const;
    [[nodiscard]] Options options() const;

signals:
    void logMessage(const QString &message);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pd
