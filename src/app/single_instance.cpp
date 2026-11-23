#include "app/single_instance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <memory>

namespace pd {
namespace {

constexpr char kActivateCommand[] = "peerdesk-activate-v1";
constexpr qsizetype kMaxCommandBytes = 1024;

QString lockPath(const QString &serverName)
{
    const QByteArray digest = QCryptographicHash::hash(
        serverName.toUtf8(),
        QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(20);
    return QDir(
               QStandardPaths::writableLocation(
                   QStandardPaths::TempLocation))
        .filePath(
            QStringLiteral("PeerDesk.single-instance.")
            + QString::fromLatin1(digest)
            + QStringLiteral(".lock"));
}

} // namespace

SingleInstance::SingleInstance(QString serverName, QObject *parent)
    : QObject(parent)
    , serverName_(std::move(serverName))
    , server_(this)
{
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(
        &server_,
        &QLocalServer::newConnection,
        this,
        &SingleInstance::acceptConnections);
}

SingleInstance::~SingleInstance()
{
    if (ownsServer_) {
        server_.close();
        QLocalServer::removeServer(serverName_);
    }
    if (ownsLock_ && lockFile_) {
        lockFile_->unlock();
    }
}

bool SingleInstance::listen(QString *error)
{
    if (ownsServer_ && server_.isListening()) {
        return true;
    }
    if (serverName_.isEmpty()) {
        if (error) {
            *error = QStringLiteral("单实例服务名称为空");
        }
        return false;
    }

    if (!lockFile_) {
        lockFile_ =
            std::make_unique<QLockFile>(lockPath(serverName_));
    }
    if (!lockFile_->tryLock(0)) {
        if (error) {
            *error = QStringLiteral("已有 PeerDesk 实例正在运行");
        }
        return false;
    }
    ownsLock_ = true;

    if (server_.listen(serverName_)) {
        ownsServer_ = true;
        return true;
    }
    const QString firstError = server_.errorString();

    QLocalServer::removeServer(serverName_);
    if (server_.listen(serverName_)) {
        ownsServer_ = true;
        return true;
    }
    lockFile_->unlock();
    ownsLock_ = false;
    if (error) {
        const QString retryError = server_.errorString();
        *error = retryError.isEmpty() ? firstError : retryError;
    }
    return false;
}

bool SingleInstance::isListening() const
{
    return ownsLock_ && server_.isListening();
}

QString SingleInstance::serverName() const
{
    return serverName_;
}

bool SingleInstance::notifyRunningInstance(
    const QString &serverName,
    int timeoutMs)
{
    if (serverName.isEmpty()) {
        return false;
    }

    QByteArray payload(kActivateCommand);
    payload.append('\n');
    const int timeout = std::clamp(timeoutMs, 50, 10000);
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QLocalSocket socket;
        socket.connectToServer(serverName, QIODevice::WriteOnly);
        const int remaining =
            timeout - static_cast<int>(elapsed.elapsed());
        if (socket.waitForConnected(std::clamp(remaining, 1, 250))) {
            if (socket.write(payload)
                != static_cast<qint64>(payload.size())) {
                return false;
            }
            socket.flush();
            return socket.waitForBytesWritten(std::max(50, remaining));
        }
        if (elapsed.elapsed() >= timeout) {
            break;
        }
        QThread::msleep(25);
    } while (true);
    return false;
}

QString SingleInstance::defaultServerName()
{
    const QString identity =
        qEnvironmentVariable("USERDOMAIN")
        + QLatin1Char('\\')
        + qEnvironmentVariable("USERNAME")
        + QLatin1Char('\\')
        + qEnvironmentVariable("USERPROFILE");
    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(),
        QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(16);
    return QStringLiteral("PeerDesk.client.")
        + QString::fromLatin1(digest);
}

void SingleInstance::acceptConnections()
{
    while (server_.hasPendingConnections()) {
        QLocalSocket *socket = server_.nextPendingConnection();
        if (!socket) {
            continue;
        }

        auto buffer = std::make_shared<QByteArray>();
        const auto handleData = [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (buffer->size() > kMaxCommandBytes) {
                socket->disconnectFromServer();
                return;
            }
            const qsizetype newline = buffer->indexOf('\n');
            if (newline < 0) {
                return;
            }
            const QByteArray command = buffer->left(newline);
            buffer->clear();
            if (command == kActivateCommand) {
                emit activationRequested();
            }
            socket->disconnectFromServer();
        };

        connect(
            socket,
            &QLocalSocket::readyRead,
            this,
            handleData);
        connect(
            socket,
            &QLocalSocket::disconnected,
            socket,
            &QObject::deleteLater);
        handleData();
    }
}

} // namespace pd
