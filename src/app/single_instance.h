#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

#include <memory>

class QLocalSocket;
class QLockFile;

namespace pd {

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(
        QString serverName,
        QObject *parent = nullptr);
    ~SingleInstance() override;

    [[nodiscard]] bool listen(QString *error = nullptr);
    [[nodiscard]] bool isListening() const;
    [[nodiscard]] QString serverName() const;

    static bool notifyRunningInstance(
        const QString &serverName,
        int timeoutMs = 1200);
    static QString defaultServerName();

signals:
    void activationRequested();

private:
    void acceptConnections();

    QString serverName_;
    QLocalServer server_;
    std::unique_ptr<QLockFile> lockFile_;
    bool ownsServer_ = false;
    bool ownsLock_ = false;
};

} // namespace pd
