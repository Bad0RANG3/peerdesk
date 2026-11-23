#include "session/file_transfer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace pd {

namespace {

constexpr int kMaxBaseNameUnits = 255;

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

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString numberedFileName(
    const QString &stem,
    const QString &suffix,
    int index)
{
    const QString marker = QStringLiteral(" (%1)").arg(index);
    const int maximumStemUnits =
        kMaxBaseNameUnits - marker.size() - suffix.size();
    if (maximumStemUnits < 0) {
        return {};
    }
    QString shortened = stem.left(maximumStemUnits);
    if (!shortened.isEmpty()
        && shortened.back().isHighSurrogate()) {
        shortened.chop(1);
    }
    return shortened + marker + suffix;
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

bool isSameFilePath(const QString &left, const QString &right)
{
    return !left.isEmpty() && !right.isEmpty()
        && normalizedForCompare(left) == normalizedForCompare(right);
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
    const QString fileSuffix = info.suffix();
    const QString dotSuffix =
        fileSuffix.isEmpty() ? QString() : QStringLiteral(".") + fileSuffix;
    for (int index = 1; index <= 9999; ++index) {
        const QString numbered =
            numberedFileName(stem, dotSuffix, index);
        if (numbered.isEmpty()) {
            return {};
        }
        candidate = dir.filePath(numbered);
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
    QString *error,
    const QString &resumeOf,
    qint64 *resumeOffset,
    bool *resumed)
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

    pruneIdle();
    if (resumeOffset) {
        *resumeOffset = 0;
    }
    if (resumed) {
        *resumed = false;
    }
    if (!resumeOf.isEmpty()) {
        if (resumeOf == id || entries_.contains(id)) {
            return reject(QStringLiteral("resume"));
        }
        auto previous = entries_.find(resumeOf);
        if (previous == entries_.end()
            || previous->second.name != name
            || previous->second.size != size
            || nowMs() - previous->second.touchedAt
                > kFileResumeRetentionMs) {
            return reject(QStringLiteral("resume"));
        }

        Entry entry = std::move(previous->second);
        entries_.erase(previous);
        entry.touchedAt = nowMs();
        const qint64 offset = entry.got;
        entries_.emplace(id, std::move(entry));
        if (resumeOffset) {
            *resumeOffset = offset;
        }
        if (resumed) {
            *resumed = true;
        }
        if (error) {
            error->clear();
        }
        return true;
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
    entry.name = name;
    entry.size = size;
    entry.touchedAt = nowMs();
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
        it->second.touchedAt = nowMs();
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
    it->second.touchedAt = nowMs();
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

void FileReceiver::pruneIdle(qint64 maxIdleMs)
{
    const qint64 now = nowMs();
    QStringList expired;
    for (const auto &[id, entry] : entries_) {
        if (now - entry.touchedAt >= maxIdleMs) {
            expired.append(id);
        }
    }
    for (const QString &id : expired) {
        abort(id);
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
    , resumeTimer_(new QTimer(this))
{
    timer_->setInterval(5);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &FileSender::pump);

    resumeTimer_->setSingleShot(true);
    resumeTimer_->setTimerType(Qt::CoarseTimer);
    connect(
        resumeTimer_,
        &QTimer::timeout,
        this,
        &FileSender::resumeTimedOut);
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
    const ResumeOffer noResume;
    return start(path, std::move(sender), error, noResume);
}

bool FileSender::start(
    const QString &path,
    Sender sender,
    QString *error,
    const ResumeOffer &resume)
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
    const bool wantsResume =
        !resume.id.isEmpty() || !resume.path.isEmpty()
        || !resume.name.isEmpty() || resume.size != 0
        || resume.offset != 0;
    if (wantsResume
        && (!resume.valid()
            || normalizedForCompare(resume.path)
                != normalizedForCompare(info.absoluteFilePath())
            || resume.name != info.fileName()
            || resume.size != info.size()
            || resume.offset < 0
            || resume.offset > info.size())) {
        return reject(QStringLiteral("resume"));
    }

    file_ = std::make_unique<QFile>(info.absoluteFilePath());
    if (!file_->open(QIODevice::ReadOnly)) {
        file_.reset();
        return reject(QStringLiteral("io"));
    }
    const qint64 offset = wantsResume ? resume.offset : 0;
    if (!file_->seek(offset)) {
        file_->close();
        file_.reset();
        return reject(QStringLiteral("io"));
    }

    sender_ = std::move(sender);
    id_ = makeTransferId();
    name_ = info.fileName();
    path_ = info.absoluteFilePath();
    resumeOf_ = wantsResume ? resume.id : QString();
    size_ = info.size();
    sent_ = offset;
    interruptedOffer_ = ResumeOffer();
    awaitingResume_ = wantsResume;

    QJsonObject begin{
        {QStringLiteral("t"), QStringLiteral("file_begin")},
        {QStringLiteral("id"), id_},
        {QStringLiteral("name"), name_},
        {QStringLiteral("size"), size_},
    };
    if (wantsResume) {
        begin.insert(QStringLiteral("resume"), true);
        begin.insert(QStringLiteral("resume_of"), resumeOf_);
    }
    sender_(begin);
    emit started(id_, name_, size_, path_);
    if (wantsResume) {
        emit progress(id_, sent_, size_);
        resumeTimer_->start(resumeTimeoutMs_);
    } else {
        timer_->start();
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool FileSender::acceptResume(
    const QJsonObject &message,
    QString *error)
{
    const auto reject = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (!active() || !awaitingResume_) {
        return reject(QStringLiteral("inactive"));
    }
    if (message.value(QStringLiteral("id")).toString() != id_) {
        return reject(QStringLiteral("id"));
    }
    const QJsonValue value =
        message.value(QStringLiteral("offset"));
    qint64 offset = -1;
    if (value.isDouble()) {
        offset = static_cast<qint64>(value.toDouble());
    } else if (value.isString()) {
        bool ok = false;
        offset = value.toString().toLongLong(&ok);
        if (!ok) {
            offset = -1;
        }
    }
    if (offset < 0 || offset > size_) {
        return reject(QStringLiteral("resume_offset"));
    }
    if (!file_ || !file_->seek(offset)) {
        return reject(QStringLiteral("io"));
    }

    resumeTimer_->stop();
    awaitingResume_ = false;
    sent_ = offset;
    emit progress(id_, sent_, size_);
    if (sent_ >= size_) {
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_end")},
            {QStringLiteral("id"), id_},
        });
        const QString finishedId = id_;
        const QString finishedName = name_;
        const qint64 finishedSize = size_;
        cleanup();
        discardResume();
        emit completed(finishedId, finishedName, finishedSize);
    } else {
        timer_->start();
    }
    if (error) {
        error->clear();
    }
    return true;
}

void FileSender::setResumeTimeoutMs(int timeoutMs)
{
    resumeTimeoutMs_ = std::max(10, timeoutMs);
}

void FileSender::discardResume()
{
    interruptedOffer_ = ResumeOffer();
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
    discardResume();
    emit cancelled(cancelledId, QStringLiteral("cancelled"));
}

void FileSender::abort()
{
    cleanup();
    discardResume();
}

void FileSender::failFromPeer(const QString &reason)
{
    if (!active()) {
        return;
    }
    const QString failedId = id_;
    cleanup();
    discardResume();
    emit failed(failedId, reason);
}

void FileSender::interrupt(const QString &reason)
{
    if (!active()) {
        return;
    }
    const QString interruptedId = id_;
    interruptedOffer_.id = id_;
    interruptedOffer_.path = path_;
    interruptedOffer_.name = name_;
    interruptedOffer_.size = size_;
    interruptedOffer_.offset = sent_;
    cleanup();
    emit interrupted(
        interruptedId,
        reason.isEmpty() ? QStringLiteral("interrupted") : reason);
}

void FileSender::pump()
{
    if (!active() || awaitingResume_ || !file_ || !sender_) {
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
        discardResume();
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
        discardResume();
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
    discardResume();
    emit failed(failedId, effectiveReason);
}

void FileSender::resumeTimedOut()
{
    if (!active() || !awaitingResume_) {
        return;
    }
    const QString failedId = id_;
    if (sender_) {
        sender_(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_error")},
            {QStringLiteral("id"), failedId},
            {QStringLiteral("reason"), QStringLiteral("resume_timeout")},
        });
    }
    cleanup();
    discardResume();
    emit failed(failedId, QStringLiteral("resume_timeout"));
}

void FileSender::cleanup()
{
    timer_->stop();
    resumeTimer_->stop();
    if (file_) {
        file_->close();
        file_.reset();
    }
    sender_ = {};
    id_.clear();
    name_.clear();
    path_.clear();
    resumeOf_.clear();
    size_ = 0;
    sent_ = 0;
    awaitingResume_ = false;
}

} // namespace pd
