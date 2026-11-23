#include "session/file_transfer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace pd {

namespace {

QString normalizedForCompare(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath())
        .toCaseFolded();
}

QString reservedName(const QString &name)
{
    QString stem = name.section(QLatin1Char('.'), 0, 0);
    if (stem.endsWith(QLatin1Char(' '))) {
        stem.chop(1);
    }
    return stem.toCaseFolded();
}

} // namespace

QString makeTransferId()
{
    const quint64 first = QRandomGenerator::system()->generate64();
    const quint64 second = QRandomGenerator::system()->generate64();
    return QStringLiteral("%1%2")
        .arg(first, 8, 16, QLatin1Char('0'))
        .arg(second, 8, 16, QLatin1Char('0'));
}

bool decodeFileChunk(
    const QString &encoded,
    QByteArray *data,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    const qsizetype maxEncoded = ((kFileChunkBytes + 2) / 3) * 4;
    if (encoded.size() > maxEncoded || encoded.size() % 4 != 0) {
        return reject(QStringLiteral("size"));
    }
    for (const QChar character : encoded) {
        const bool alphanumeric =
            (character >= QLatin1Char('A') && character <= QLatin1Char('Z'))
            || (character >= QLatin1Char('a')
                && character <= QLatin1Char('z'))
            || (character >= QLatin1Char('0')
                && character <= QLatin1Char('9'));
        if (character.unicode() > 0x7F
            || (alphanumeric == false
                && character != u'+' && character != u'/'
                && character != u'=')) {
            return reject(QStringLiteral("data"));
        }
    }
    const QByteArray wire = encoded.toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(wire);
    if (decoded.size() > kFileChunkBytes
        || decoded.toBase64() != wire) {
        return reject(QStringLiteral("data"));
    }
    if (data) {
        *data = decoded;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool isSafeBaseName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".")
        || name == QStringLiteral("..")) {
        return false;
    }
    if (name.size() > 255 || name.contains(QChar::Null)
        || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))) {
        return false;
    }
    if (QDir::isAbsolutePath(name)
        || (name.size() >= 2 && name.at(1) == QLatin1Char(':'))) {
        return false;
    }
    for (const QChar character : name) {
        if (character.unicode() < 32
            || QStringLiteral("<>:\"|?*").contains(character)) {
            return false;
        }
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return false;
    }

    static const QSet<QString> reservedNames{
        QStringLiteral("con"), QStringLiteral("prn"),
        QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"),
        QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"),
        QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"),
        QStringLiteral("lpt8"), QStringLiteral("lpt9"),
    };
    return !reservedNames.contains(reservedName(name));
}

QString uniqueFilePath(
    const QString &directory,
    const QString &fileName,
    const QSet<QString> &reserved)
{
    const QSet<QString> normalizedReserved = [&reserved]() {
        QSet<QString> values;
        for (const QString &path : reserved) {
            values.insert(normalizedForCompare(path));
        }
        return values;
    }();
    QDir dir(directory);
    QString candidate = dir.filePath(fileName);
    const auto taken = [&normalizedReserved](const QString &path) {
        return QFileInfo::exists(path)
            || normalizedReserved.contains(normalizedForCompare(path));
    };
    if (!taken(candidate)) {
        return candidate;
    }

    const QFileInfo info(fileName);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString dotSuffix =
        suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;
    for (int index = 1; index <= 9999; ++index) {
        candidate = dir.filePath(QStringLiteral("%1 (%2)%3")
                                     .arg(stem)
                                     .arg(index)
                                     .arg(dotSuffix));
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return {};
}

FileReceiver::FileReceiver(QString directory)
    : directory_(std::move(directory))
{
}

QString FileReceiver::directory() const
{
    return directory_;
}

void FileReceiver::setDirectory(const QString &directory)
{
    abortAll();
    directory_ = directory;
}

bool FileReceiver::begin(
    const QString &id,
    const QString &name,
    qint64 size,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (id.isEmpty() || id.size() > 128) {
        return reject(QStringLiteral("id"));
    }
    if (!isSafeBaseName(name)) {
        return reject(QStringLiteral("name"));
    }
    if (size <= 0 || size > kMaxFileBytes) {
        return reject(QStringLiteral("size"));
    }
    if (entries_.contains(id)) {
        abort(id);
    }
    if (directory_.isEmpty()
        || !QDir().mkpath(directory_)) {
        return reject(QStringLiteral("io"));
    }

    const QString path = uniqueFilePath(directory_, name, reserved_);
    if (path.isEmpty()) {
        return reject(QStringLiteral("name"));
    }
    auto file = std::make_unique<QSaveFile>(path);
    file->setDirectWriteFallback(false);
    if (!file->open(QIODevice::WriteOnly)) {
        return reject(QStringLiteral("io"));
    }

    Entry entry;
    entry.path = path;
    entry.size = size;
    entry.file = std::move(file);
    entries_.emplace(id, std::move(entry));
    reserved_.insert(path);
    if (error) {
        error->clear();
    }
    return true;
}

bool FileReceiver::append(
    const QString &id,
    const QByteArray &data,
    QString *error)
{
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        if (error) {
            *error = QStringLiteral("unknown");
        }
        return false;
    }
    if (data.isEmpty()) {
        return true;
    }
    if (it->second.got + data.size() > it->second.size) {
        abort(id);
        if (error) {
            *error = QStringLiteral("size");
        }
        return false;
    }
    if (it->second.file->write(data) != data.size()) {
        abort(id);
        if (error) {
            *error = QStringLiteral("io");
        }
        return false;
    }
    it->second.got += data.size();
    if (error) {
        error->clear();
    }
    return true;
}

bool FileReceiver::finish(
    const QString &id,
    QString *savedPath,
    qint64 *savedBytes,
    QString *error)
{
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        if (error) {
            *error = QStringLiteral("unknown");
        }
        return false;
    }
    if (it->second.got != it->second.size) {
        const qint64 got = it->second.got;
        const qint64 size = it->second.size;
        abort(id);
        if (error) {
            *error = QStringLiteral("size:%1/%2").arg(got).arg(size);
        }
        return false;
    }
    if (!it->second.file->commit()) {
        abort(id);
        if (error) {
            *error = QStringLiteral("io");
        }
        return false;
    }
    if (savedPath) {
        *savedPath = it->second.path;
    }
    if (savedBytes) {
        *savedBytes = it->second.got;
    }
    releaseReservation(it->second);
    entries_.erase(it);
    if (error) {
        error->clear();
    }
    return true;
}

void FileReceiver::abort(const QString &id)
{
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    if (it->second.file) {
        it->second.file->cancelWriting();
    }
    releaseReservation(it->second);
    entries_.erase(it);
}

void FileReceiver::abortAll()
{
    while (!entries_.empty()) {
        abort(entries_.begin()->first);
    }
}

bool FileReceiver::contains(const QString &id) const
{
    return entries_.contains(id);
}

qint64 FileReceiver::received(const QString &id) const
{
    const auto it = entries_.find(id);
    return it == entries_.cend() ? 0 : it->second.got;
}

qint64 FileReceiver::expected(const QString &id) const
{
    const auto it = entries_.find(id);
    return it == entries_.cend() ? 0 : it->second.size;
}

void FileReceiver::releaseReservation(const Entry &entry)
{
    reserved_.remove(entry.path);
}

FileSender::FileSender(QObject *parent)
    : QObject(parent)
    , timer_(new QTimer(this))
{
    timer_->setInterval(5);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &FileSender::pump);
}

FileSender::~FileSender()
{
    cleanup();
}

bool FileSender::start(
    const QString &path,
    Sender sender,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (active()) {
        return reject(QStringLiteral("busy"));
    }
    if (!sender) {
        return reject(QStringLiteral("sender"));
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return reject(QStringLiteral("missing"));
    }
    if (!isSafeBaseName(info.fileName())) {
        return reject(QStringLiteral("name"));
    }
    if (info.size() <= 0 || info.size() > kMaxFileBytes) {
        return reject(QStringLiteral("size"));
    }

    file_ = std::make_unique<QFile>(info.absoluteFilePath());
    if (!file_->open(QIODevice::ReadOnly)) {
        file_.reset();
        return reject(QStringLiteral("io"));
    }

    sender_ = std::move(sender);
    id_ = makeTransferId();
    name_ = info.fileName();
    size_ = info.size();
    sent_ = 0;
    sender_(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_begin")},
        {QStringLiteral("id"), id_},
        {QStringLiteral("name"), name_},
        {QStringLiteral("size"), size_},
    });
    emit started(id_, name_, size_);
    timer_->start();
    if (error) {
        error->clear();
    }
    return true;
}

void FileSender::cancel()
{
    if (!active()) {
        return;
    }
    const QString cancelledId = id_;
    if (sender_) {
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_cancel")},
            {QStringLiteral("id"), cancelledId},
            {QStringLiteral("reason"), QStringLiteral("cancelled")},
        });
    }
    cleanup();
    emit cancelled(cancelledId, QStringLiteral("cancelled"));
}

void FileSender::abort()
{
    cleanup();
}

void FileSender::failFromPeer(const QString &reason)
{
    if (!active()) {
        return;
    }
    const QString failedId = id_;
    cleanup();
    emit failed(failedId, reason);
}

void FileSender::interrupt(const QString &reason)
{
    if (!active()) {
        return;
    }
    const QString interruptedId = id_;
    cleanup();
    emit interrupted(
        interruptedId,
        reason.isEmpty() ? QStringLiteral("interrupted") : reason);
}

void FileSender::pump()
{
    if (!active() || !file_ || !sender_) {
        return;
    }
    const QByteArray chunk = file_->read(kFileChunkBytes);
    if (chunk.isEmpty()) {
        if (file_->error() != QFileDevice::NoError || sent_ != size_) {
            fail(QStringLiteral("io"));
            return;
        }
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_end")},
            {QStringLiteral("id"), id_},
        });
        const QString finishedId = id_;
        const QString finishedName = name_;
        const qint64 finishedSize = size_;
        cleanup();
        emit completed(finishedId, finishedName, finishedSize);
        return;
    }
    sent_ += chunk.size();
    sender_(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("file_data")},
        {QStringLiteral("id"), id_},
        {QStringLiteral("d"), QString::fromLatin1(chunk.toBase64())},
    });
    emit progress(id_, sent_, size_);
    if (sent_ >= size_ && file_->atEnd()) {
        timer_->stop();
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_end")},
            {QStringLiteral("id"), id_},
        });
        const QString finishedId = id_;
        const QString finishedName = name_;
        const qint64 finishedSize = size_;
        cleanup();
        emit completed(finishedId, finishedName, finishedSize);
    }
}

void FileSender::fail(const QString &reason)
{
    if (!active()) {
        return;
    }
    const QString failedId = id_;
    const QString effectiveReason =
        reason.isEmpty() ? QStringLiteral("io") : reason;
    if (sender_) {
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_error")},
            {QStringLiteral("id"), failedId},
            {QStringLiteral("reason"), effectiveReason},
        });
    }
    cleanup();
    emit failed(failedId, effectiveReason);
}

void FileSender::cleanup()
{
    timer_->stop();
    if (file_) {
        file_->close();
        file_.reset();
    }
    sender_ = {};
    id_.clear();
    name_.clear();
    size_ = 0;
    sent_ = 0;
}

} // namespace pd
