#include "server/server.h"
#include "session/audio_codec.h"
#include "session/file_transfer.h"
#include "session/host_session.h"
#include "session/media_sources.h"
#include "session/session_types.h"
#include "session/viewer_session.h"
#include "transport/peer_link.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

using namespace pd;

namespace {

void pumpEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (timer.elapsed() < milliseconds) {
            QThread::msleep(1);
        }
    } while (timer.elapsed() < milliseconds);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        pumpEvents(20);
    }
    return predicate();
}

class SignalClient {
public:
    bool connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil(
            [this]() {
                return socket.state() == QAbstractSocket::ConnectedState;
            },
            3000);
    }

    void send(const QJsonObject &object)
    {
        QByteArray wire =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        wire.append('\n');
        socket.write(wire);
        socket.flush();
    }

    [[nodiscard]] std::optional<QJsonObject> take(
        const QString &type,
        int timeoutMs = 3000)
    {
        return waitUntil(
                   [this, &type]() {
                       poll();
                       return std::any_of(
                           inbox.cbegin(),
                           inbox.cend(),
                           [&type](const QJsonObject &message) {
                               return message.value(QStringLiteral("t"))
                                          .toString()
                                   == type;
                           });
                   },
                   timeoutMs)
            ? takeNow(type)
            : std::nullopt;
    }

    QTcpSocket socket;

private:
    void poll()
    {
        buffer.append(socket.readAll());
        for (;;) {
            const qsizetype newline = buffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            const QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) {
                inbox.append(document.object());
            }
        }
    }

    std::optional<QJsonObject> takeNow(const QString &type)
    {
        for (qsizetype index = 0; index < inbox.size(); ++index) {
            if (inbox.at(index).value(QStringLiteral("t")).toString() == type) {
                return inbox.takeAt(index);
            }
        }
        return std::nullopt;
    }

    QByteArray buffer;
    QList<QJsonObject> inbox;
};

quint32 relayTag(const QJsonObject &message)
{
    return static_cast<quint32>(
        static_cast<quint64>(
            message.value(QStringLiteral("relay_tag")).toDouble()));
}

bool hasMessage(
    const QList<QJsonObject> &messages,
    const QString &type,
    const QString &id = {})
{
    return std::any_of(
        messages.cbegin(),
        messages.cend(),
        [&type, &id](const QJsonObject &message) {
            if (message.value(QStringLiteral("t")).toString() != type) {
                return false;
            }
            return id.isEmpty()
                || message.value(QStringLiteral("id")).toString() == id;
        });
}

int countMessages(
    const QList<QJsonObject> &messages,
    const QString &type,
    const QString &id = {})
{
    return static_cast<int>(std::count_if(
        messages.cbegin(),
        messages.cend(),
        [&type, &id](const QJsonObject &message) {
            if (message.value(QStringLiteral("t")).toString() != type) {
                return false;
            }
            return id.isEmpty()
                || message.value(QStringLiteral("id")).toString() == id;
        }));
}

bool hasFileState(
    const QList<QJsonObject> &messages,
    const QString &state,
    const QString &id = {})
{
    return std::any_of(
        messages.cbegin(),
        messages.cend(),
        [&state, &id](const QJsonObject &message) {
            return message.value(QStringLiteral("t")).toString()
                    == QStringLiteral("file_in")
                && message.value(QStringLiteral("state")).toString() == state
                && (id.isEmpty()
                    || message.value(QStringLiteral("id")).toString() == id);
        });
}

class FakeInputSink final : public IInputSink {
public:
    [[nodiscard]] QRect virtualDesktop() const override
    {
        return QRect(0, 0, 2304, 768);
    }

    void moveAbsolute(double x, double y) override
    {
        absoluteMoves.append(QPointF(x, y));
    }

    void moveRelative(int dx, int dy) override
    {
        relativeMoves.append(QPoint(dx, dy));
    }

    void mouseButton(const QString &button, bool down) override
    {
        buttons.append(qMakePair(button, down));
    }

    void click(const QString &button) override
    {
        clicks.append(button);
    }

    void wheel(int delta, bool horizontal) override
    {
        wheels.append(qMakePair(delta, horizontal));
    }

    void key(const QString &code, bool down) override
    {
        keys.append(qMakePair(code, down));
    }

    void typeText(const QString &text) override
    {
        typed.append(text);
    }

    void ctrlAltDelete() override
    {
        ++cadCount;
    }

    QList<QPointF> absoluteMoves;
    QList<QPoint> relativeMoves;
    QList<QPair<QString, bool>> buttons;
    QList<QString> clicks;
    QList<QPair<int, bool>> wheels;
    QList<QPair<QString, bool>> keys;
    QString typed;
    int cadCount = 0;
};

class FakeClipboard final : public IClipboard {
public:
    [[nodiscard]] QString text() const override { return value; }

    bool setText(const QString &text) override
    {
        value = text;
        ++writes;
        return true;
    }

    QString value;
    int writes = 0;
};

QByteArray makePayload(qsizetype size, quint32 seed)
{
    QByteArray payload(size, '\0');
    quint32 state = seed | 1U;
    for (qsizetype index = 0; index < payload.size(); ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[index] = static_cast<char>(state & 0xFF);
    }
    return payload;
}

bool writeBytes(const QString &path, const QByteArray &payload)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(payload) == payload.size() && file.flush();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

int countKeyframes(const QList<bool> &keyframes)
{
    return static_cast<int>(
        std::count(keyframes.cbegin(), keyframes.cend(), true));
}

} // namespace

class SessionTest : public QObject {
    Q_OBJECT

private slots:
    void syntheticCaptureSettingsAndPacer()
    {
        SyntheticScreenSource screen(1280, 720, true);
        const QList<MonitorInfo> monitors = screen.monitors();
        QCOMPARE(monitors.size(), 2);
        QCOMPARE(monitors.at(0).width, 1280);
        QCOMPARE(monitors.at(1).left, 1280);
        QCOMPARE(monitors.at(1).width, 1024);

        CaptureRequest request;
        request.monitor = 0;
        request.maxWidth = 640;
        CaptureResult primary = screen.capture(request);
        QVERIFY(primary.ok());
        QCOMPARE(primary.image.size(), QSize(640, 360));

        request.monitor = 1;
        request.maxWidth = 512;
        CaptureResult secondary = screen.capture(request);
        QVERIFY(secondary.ok());
        QCOMPARE(secondary.image.size(), QSize(512, 384));

        request.monitor = 99;
        request.maxWidth = 400;
        CaptureResult clamped = screen.capture(request);
        QVERIFY(clamped.ok());
        QCOMPARE(clamped.image.width(), 400);

        VideoSettings settings;
        settings.update(QJsonObject{
            {QStringLiteral("fps"), 999},
            {QStringLiteral("max_width"), 100},
            {QStringLiteral("quality"), 1},
            {QStringLiteral("monitor"), -4},
            {QStringLiteral("skip_static"), false},
            {QStringLiteral("audio"), false},
            {QStringLiteral("pixel_format"), QStringLiteral("raw")},
        });
        QCOMPARE(settings.fps, 120);
        QCOMPARE(settings.maxWidth, 320);
        QCOMPARE(settings.quality, 20);
        QCOMPARE(settings.monitor, 0);
        QCOMPARE(settings.skipStatic, false);
        QCOMPARE(settings.audio, false);
        QCOMPARE(settings.pixelFormat, QStringLiteral("raw"));

        FramePacer pacer(65, 100, 32, 92);
        pacer.observe(160 * 1024, 12.0);
        QCOMPARE(pacer.quality(), 62);
        QCOMPARE(pacer.lastSize(), qint64(160 * 1024));
        pacer.observe(30 * 1024, 8.0);
        QCOMPARE(pacer.quality(), 63);
        pacer.setBudgetKb(0.0);
        QCOMPARE(pacer.targetKb(), 140);
    }

    void audioCodecPreservesFramesAndSuppressesSilence()
    {
        ToneAudioSource tone(440.0);
        QString error;
        QVERIFY2(tone.start(&error), qPrintable(error));
        QVERIFY(tone.isStarted());
        const QByteArray pcm = tone.readFrame(kAudioSamples, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(pcm.size(), qsizetype(kAudioPcmBytes));
        QVERIFY(pcm16Rms(pcm) > 3000.0);

        const QByteArray ulaw = ulawEncode(pcm);
        QCOMPARE(ulaw.size(), qsizetype(kAudioSamples));
        const QByteArray decoded = ulawDecode(ulaw);
        QCOMPARE(decoded.size(), pcm.size());
        QVERIFY(pcm16Rms(decoded) > 2500.0);

        AudioEncoder encoder;
        const auto first = encoder.next(pcm);
        QVERIFY(first.has_value());
        QCOMPARE(first->sequence, 0U);
        QCOMPARE(int(first->samples), kAudioSamples);
        QCOMPARE(first->current.size(), qsizetype(kAudioSamples));
        QVERIFY(first->previous.isEmpty());

        const auto second = encoder.next(pcm);
        QVERIFY(second.has_value());
        QCOMPARE(second->sequence, 1U);
        QCOMPARE(second->previous, first->current);
        QVERIFY(!encoder.next(QByteArray(10, '\0')).has_value());

        const QByteArray silence(kAudioPcmBytes, '\0');
        AudioEncoder silenceEncoder;
        for (int frame = 1; frame <= kSilenceHoldFrames; ++frame) {
            QVERIFY(silenceEncoder.next(silence).has_value());
        }
        QVERIFY(!silenceEncoder.next(silence).has_value());
        for (int frame = kSilenceHoldFrames + 2;
             frame < kSilenceHoldFrames + kSilenceKeepFrames;
             ++frame) {
            QVERIFY(!silenceEncoder.next(silence).has_value());
        }
        QVERIFY(silenceEncoder
                    .next(silence)
                    .has_value());
        QCOMPARE(pcm16Rms(silence), 0.0);
    }

    void fileTransferRejectsHostileNamesAndCommitsAtomically()
    {
        QVERIFY(isSafeBaseName(QStringLiteral("report.txt")));
        QVERIFY(isSafeBaseName(QStringLiteral("résumé 2026.pdf")));
        QVERIFY(!isSafeBaseName(QString()));
        QVERIFY(!isSafeBaseName(QStringLiteral(".")));
        QVERIFY(!isSafeBaseName(QStringLiteral("..")));
        QVERIFY(!isSafeBaseName(QStringLiteral("../escape.txt")));
        QVERIFY(!isSafeBaseName(QStringLiteral("C:\\temp\\escape.txt")));
        QVERIFY(!isSafeBaseName(QStringLiteral("C:/temp/escape.txt")));
        QVERIFY(!isSafeBaseName(QStringLiteral("CON")));
        QVERIFY(!isSafeBaseName(QStringLiteral("aux.txt")));
        QVERIFY(!isSafeBaseName(QStringLiteral("COM1.tar.gz")));
        QVERIFY(!isSafeBaseName(QStringLiteral("bad?.txt")));
        QVERIFY(!isSafeBaseName(QStringLiteral("trailing.")));
        QVERIFY(!isSafeBaseName(QStringLiteral("trailing ")));
        QVERIFY(!isSafeBaseName(QString(256, QLatin1Char('a'))));
        QVERIFY(!isSafeBaseName(
            QStringLiteral("bad") + QChar(1) + QStringLiteral(".txt")));

        QByteArray decoded;
        QString error;
        const QByteArray chunk = makePayload(1024, 7);
        QVERIFY2(
            decodeFileChunk(
                QString::fromLatin1(chunk.toBase64()),
                &decoded,
                &error),
            qPrintable(error));
        QCOMPARE(decoded, chunk);
        QVERIFY(!decodeFileChunk(QStringLiteral("!!!!"), &decoded, &error));
        QVERIFY(!decodeFileChunk(QStringLiteral("AA="), &decoded, &error));
        const qsizetype maxEncoded = ((kFileChunkBytes + 2) / 3) * 4;
        QVERIFY(!decodeFileChunk(
            QString(maxEncoded + 4, QLatin1Char('A')),
            &decoded,
            &error));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(writeBytes(
            directory.filePath(QStringLiteral("report.txt")),
            QByteArrayLiteral("old")));

        FileReceiver receiver(directory.path());
        QVERIFY2(
            receiver.begin(
                QStringLiteral("one"),
                QStringLiteral("report.txt"),
                11,
                &error),
            qPrintable(error));
        QVERIFY(receiver.append(QStringLiteral("one"), QByteArrayLiteral("hello "), &error));
        QVERIFY(receiver.append(QStringLiteral("one"), QByteArrayLiteral("world"), &error));
        QCOMPARE(receiver.received(QStringLiteral("one")), qint64(11));
        QCOMPARE(receiver.expected(QStringLiteral("one")), qint64(11));
        QString savedPath;
        qint64 savedBytes = 0;
        QVERIFY2(
            receiver.finish(
                QStringLiteral("one"),
                &savedPath,
                &savedBytes,
                &error),
            qPrintable(error));
        QCOMPARE(savedBytes, qint64(11));
        QCOMPARE(QFileInfo(savedPath).fileName(), QStringLiteral("report (1).txt"));
        QCOMPARE(readBytes(savedPath), QByteArrayLiteral("hello world"));
        QVERIFY(!receiver.contains(QStringLiteral("one")));

        const QStringList hostileNames{
            QStringLiteral("../escape.txt"),
            QStringLiteral("..\\escape.txt"),
            QStringLiteral("C:\\escape.txt"),
            QStringLiteral("CON"),
            QStringLiteral("AUX.txt"),
            QStringLiteral("bad?.txt"),
            QStringLiteral("trailing."),
            QString(),
        };
        int id = 0;
        for (const QString &name : hostileNames) {
            QVERIFY(!receiver.begin(
                QStringLiteral("bad%1").arg(++id),
                name,
                10,
                &error));
            QCOMPARE(error, QStringLiteral("name"));
        }
        QVERIFY(!receiver.begin(QStringLiteral("zero"), QStringLiteral("zero.bin"), 0, &error));
        QCOMPARE(error, QStringLiteral("size"));
        QVERIFY(!receiver.begin(
            QStringLiteral("huge"),
            QStringLiteral("huge.bin"),
            kMaxFileBytes + 1,
            &error));
        QCOMPARE(error, QStringLiteral("size"));

        QVERIFY(receiver.begin(
            QStringLiteral("overflow"),
            QStringLiteral("overflow.bin"),
            3,
            &error));
        QVERIFY(!receiver.append(
            QStringLiteral("overflow"),
            QByteArrayLiteral("four"),
            &error));
        QCOMPARE(error, QStringLiteral("size"));
        QVERIFY(!receiver.contains(QStringLiteral("overflow")));

        QVERIFY(receiver.begin(
            QStringLiteral("short"),
            QStringLiteral("short.bin"),
            4,
            &error));
        QVERIFY(receiver.append(
            QStringLiteral("short"),
            QByteArrayLiteral("a"),
            &error));
        QVERIFY(!receiver.finish(
            QStringLiteral("short"),
            nullptr,
            nullptr,
            &error));
        QVERIFY(error.startsWith(QStringLiteral("size:")));
        QVERIFY(!QFileInfo::exists(
            directory.filePath(QStringLiteral("short.bin"))));
    }

    void fileSenderSeparatesCancelFailureAndInterruption()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("payload.bin"));
        QVERIFY(writeBytes(path, makePayload(256 * 1024, 0x51A7)));

        QList<QJsonObject> wire;
        const auto collect = [&wire](const QJsonObject &message) {
            wire.append(message);
        };
        FileSender sender;
        QSignalSpy cancelled(&sender, &FileSender::cancelled);
        QSignalSpy failed(&sender, &FileSender::failed);
        QSignalSpy interrupted(&sender, &FileSender::interrupted);

        QString error;
        QVERIFY2(sender.start(path, collect, &error), qPrintable(error));
        const QString cancelledId = sender.id();
        QVERIFY(!cancelledId.isEmpty());
        wire.clear();
        sender.cancel();
        sender.cancel();
        QVERIFY(!sender.active());
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(cancelled.at(0).at(0).toString(), cancelledId);
        QCOMPARE(
            cancelled.at(0).at(1).toString(),
            QStringLiteral("cancelled"));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(interrupted.count(), 0);
        QCOMPARE(countMessages(wire, QStringLiteral("file_cancel")), 1);
        QCOMPARE(countMessages(wire, QStringLiteral("file_error")), 0);

        cancelled.clear();
        wire.clear();
        QVERIFY2(sender.start(path, collect, &error), qPrintable(error));
        const QString failedId = sender.id();
        wire.clear();
        sender.fail(QStringLiteral("disk"));
        QVERIFY(!sender.active());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), failedId);
        QCOMPARE(
            failed.at(0).at(1).toString(),
            QStringLiteral("disk"));
        QCOMPARE(countMessages(wire, QStringLiteral("file_error")), 1);
        QCOMPARE(
            wire.constFirst().value(QStringLiteral("reason")).toString(),
            QStringLiteral("disk"));

        failed.clear();
        wire.clear();
        QVERIFY2(sender.start(path, collect, &error), qPrintable(error));
        const QString peerFailedId = sender.id();
        wire.clear();
        sender.failFromPeer(QStringLiteral("checksum"));
        QVERIFY(!sender.active());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), peerFailedId);
        QCOMPARE(
            failed.at(0).at(1).toString(),
            QStringLiteral("checksum"));
        QVERIFY(wire.isEmpty());

        failed.clear();
        wire.clear();
        QVERIFY2(sender.start(path, collect, &error), qPrintable(error));
        const QString interruptedId = sender.id();
        wire.clear();
        sender.interrupt(QStringLiteral("network"));
        QVERIFY(!sender.active());
        QCOMPARE(interrupted.count(), 1);
        QCOMPARE(interrupted.at(0).at(0).toString(), interruptedId);
        QCOMPARE(
            interrupted.at(0).at(1).toString(),
            QStringLiteral("network"));
        QVERIFY(wire.isEmpty());

        interrupted.clear();
        QVERIFY2(sender.start(path, collect, &error), qPrintable(error));
        wire.clear();
        sender.abort();
        QVERIFY(!sender.active());
        QVERIFY(wire.isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(cancelled.count(), 0);
        QCOMPARE(interrupted.count(), 0);
    }

    void fileReceiverAbortReleasesReservation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FileReceiver receiver(directory.path());
        QString error;

        QVERIFY(receiver.begin(
            QStringLiteral("aborted"),
            QStringLiteral("report.txt"),
            10,
            &error));
        QVERIFY(receiver.append(
            QStringLiteral("aborted"),
            QByteArrayLiteral("partial"),
            &error));
        receiver.abort(QStringLiteral("aborted"));
        QVERIFY(!receiver.contains(QStringLiteral("aborted")));
        QVERIFY(!QFileInfo::exists(
            directory.filePath(QStringLiteral("report.txt"))));

        QVERIFY(receiver.begin(
            QStringLiteral("retry"),
            QStringLiteral("report.txt"),
            5,
            &error));
        QVERIFY(receiver.append(
            QStringLiteral("retry"),
            QByteArrayLiteral("hello"),
            &error));
        QString savedPath;
        QVERIFY(receiver.finish(
            QStringLiteral("retry"),
            &savedPath,
            nullptr,
            &error));
        QCOMPARE(
            QFileInfo(savedPath).fileName(),
            QStringLiteral("report.txt"));
        QCOMPARE(readBytes(savedPath), QByteArrayLiteral("hello"));
    }

    void cancellableFileTransferRoundTrip()
    {
        const QByteArray psk =
            QByteArrayLiteral("session-cancel-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("cancel-host"));
        viewerLink.setLabel(QStringLiteral("cancel-viewer"));
        hostLink.setDeviceId(QStringLiteral("cancel-host-device"));
        viewerLink.setDeviceId(QStringLiteral("cancel-viewer-device"));

        QString error;
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));

        QTemporaryDir sourceDirectory;
        QTemporaryDir hostReceive;
        QTemporaryDir viewerReceive;
        QVERIFY(sourceDirectory.isValid());
        QVERIFY(hostReceive.isValid());
        QVERIFY(viewerReceive.isValid());

        HostSession host(&hostLink);
        ViewerSession viewer(&viewerLink);
        host.setReceiveDirectory(hostReceive.path());
        viewer.setReceiveDirectory(viewerReceive.path());

        QList<QJsonObject> hostEvents;
        QList<QJsonObject> viewerEvents;
        QList<QJsonObject> hostWire;
        QList<QJsonObject> viewerWire;
        connect(
            &host,
            &HostSession::eventReceived,
            [&hostEvents](const QJsonObject &message) {
                hostEvents.append(message);
            });
        connect(
            &viewer,
            &ViewerSession::eventReceived,
            [&viewerEvents](const QJsonObject &message) {
                viewerEvents.append(message);
            });
        connect(
            &hostLink,
            &PeerLink::controlMessageReceived,
            [&hostWire](const QJsonObject &message) {
                hostWire.append(message);
            });
        connect(
            &viewerLink,
            &PeerLink::controlMessageReceived,
            [&viewerWire](const QJsonObject &message) {
                viewerWire.append(message);
            });

        hostLink.addCandidates(viewerLink.localCandidates());
        viewerLink.addCandidates(hostLink.localCandidates());
        hostLink.beginHandshake();
        viewerLink.beginHandshake();
        QVERIFY(waitUntil(
            [&]() {
                return hostLink.established() && viewerLink.established()
                    && hostLink.state() == QStringLiteral("direct")
                    && viewerLink.state() == QStringLiteral("direct");
            },
            7000));
        viewer.start();

        QSignalSpy hostStarted(&host, &HostSession::outgoingFileStarted);
        QSignalSpy hostCancelled(&host, &HostSession::outgoingFileCancelled);
        QSignalSpy hostInterrupted(
            &host,
            &HostSession::outgoingFileInterrupted);
        QSignalSpy viewerStarted(&viewer, &ViewerSession::outgoingFileStarted);
        QSignalSpy viewerCancelled(
            &viewer,
            &ViewerSession::outgoingFileCancelled);
        QSignalSpy viewerInterrupted(
            &viewer,
            &ViewerSession::outgoingFileInterrupted);

        const QString hostCancelPath =
            sourceDirectory.filePath(QStringLiteral("host-cancel.bin"));
        QVERIFY(writeBytes(hostCancelPath, makePayload(300000, 0xC001)));
        QVERIFY2(host.sendFile(hostCancelPath, &error), qPrintable(error));
        QCOMPARE(hostStarted.count(), 1);
        const QString hostCancelId =
            hostStarted.at(0).at(0).toString();
        QVERIFY(host.cancelFile());
        QVERIFY(!host.cancelFile());
        QCOMPARE(hostCancelled.count(), 1);
        QCOMPARE(hostCancelled.at(0).at(0).toString(), hostCancelId);
        QVERIFY(waitUntil(
            [&]() {
                return hasFileState(
                    viewerEvents,
                    QStringLiteral("cancelled"),
                    hostCancelId);
            },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return hasMessage(
                    hostEvents,
                    QStringLiteral("file_cancelled"),
                    hostCancelId);
            },
            3000));
        QCOMPARE(
            countMessages(
                viewerWire,
                QStringLiteral("file_cancel"),
                hostCancelId),
            1);
        QVERIFY(!QFileInfo::exists(
            viewerReceive.filePath(QStringLiteral("host-cancel.bin"))));

        const QString viewerCancelPath =
            sourceDirectory.filePath(QStringLiteral("viewer-cancel.bin"));
        QVERIFY(writeBytes(viewerCancelPath, makePayload(280000, 0xC002)));
        QVERIFY2(viewer.sendFile(viewerCancelPath, &error), qPrintable(error));
        QCOMPARE(viewerStarted.count(), 1);
        const QString viewerCancelId =
            viewerStarted.at(0).at(0).toString();
        QVERIFY(viewer.cancelFile());
        QVERIFY(!viewer.cancelFile());
        QCOMPARE(viewerCancelled.count(), 1);
        QVERIFY(waitUntil(
            [&]() {
                return hasFileState(
                    hostEvents,
                    QStringLiteral("cancelled"),
                    viewerCancelId);
            },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return hasMessage(
                    viewerEvents,
                    QStringLiteral("file_cancelled"),
                    viewerCancelId);
            },
            3000));
        QCOMPARE(
            countMessages(
                hostWire,
                QStringLiteral("file_cancel"),
                viewerCancelId),
            1);
        QVERIFY(!QFileInfo::exists(
            hostReceive.filePath(QStringLiteral("viewer-cancel.bin"))));

        hostStarted.clear();
        hostCancelled.clear();
        hostInterrupted.clear();
        const QString hostStopPath =
            sourceDirectory.filePath(QStringLiteral("host-stop.bin"));
        QVERIFY(writeBytes(hostStopPath, makePayload(240000, 0xC003)));
        QVERIFY2(host.sendFile(hostStopPath, &error), qPrintable(error));
        QCOMPARE(hostStarted.count(), 1);
        const QString hostStopId =
            hostStarted.at(0).at(0).toString();
        host.stop();
        QCOMPARE(hostInterrupted.count(), 1);
        QCOMPARE(hostInterrupted.at(0).at(0).toString(), hostStopId);
        QCOMPARE(hostCancelled.count(), 0);
        pumpEvents(150);
        QCOMPARE(
            countMessages(
                viewerWire,
                QStringLiteral("file_cancel"),
                hostStopId),
            0);
        viewer.stop();

        viewerStarted.clear();
        viewerCancelled.clear();
        viewerInterrupted.clear();
        const QString viewerClosePath =
            sourceDirectory.filePath(QStringLiteral("viewer-close.bin"));
        QVERIFY(writeBytes(viewerClosePath, makePayload(220000, 0xC004)));
        QVERIFY2(viewer.sendFile(viewerClosePath, &error), qPrintable(error));
        QCOMPARE(viewerStarted.count(), 1);
        const QString viewerCloseId =
            viewerStarted.at(0).at(0).toString();
        viewerLink.close();
        QCOMPARE(viewerInterrupted.count(), 1);
        QCOMPARE(viewerInterrupted.at(0).at(0).toString(), viewerCloseId);
        QCOMPARE(viewerCancelled.count(), 0);
        pumpEvents(150);
        QCOMPARE(
            countMessages(
                hostWire,
                QStringLiteral("file_cancel"),
                viewerCloseId),
            0);
    }

    void directSessionEndToEnd()
    {
        const QByteArray psk = QByteArrayLiteral("session-direct-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("host"));
        viewerLink.setLabel(QStringLiteral("viewer"));
        hostLink.setDeviceId(QStringLiteral("host-device"));
        viewerLink.setDeviceId(QStringLiteral("viewer-device"));

        QString error;
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));

        QTemporaryDir sourceDirectory;
        QTemporaryDir hostReceive;
        QTemporaryDir viewerReceive;
        QVERIFY(sourceDirectory.isValid());
        QVERIFY(hostReceive.isValid());
        QVERIFY(viewerReceive.isValid());

        auto screen = std::make_shared<SyntheticScreenSource>(1280, 720, true);
        auto audio = std::make_shared<ToneAudioSource>(523.25);
        auto input = std::make_shared<FakeInputSink>();
        auto clipboard = std::make_shared<FakeClipboard>();
        clipboard->value = QStringLiteral("host initial clipboard");

        QList<QJsonObject> hostEvents;
        QList<QJsonObject> viewerEvents;
        QList<QByteArray> encodedFrames;
        QList<QImage> decodedFrames;
        QList<bool> keyframes;
        QList<QByteArray> audioFrames;
        QList<QJsonObject> viewerAudioStates;
        QList<QString> hostFiles;
        QList<QString> viewerFiles;
        QList<QJsonObject> viewerStats;

        HostSession host(&hostLink);
        ViewerSession viewer(&viewerLink);
        host.setScreenSource(screen);
        host.setAudioSource(audio);
        host.setInputSink(input);
        host.setClipboard(clipboard);
        host.setReceiveDirectory(hostReceive.path());
        viewer.setReceiveDirectory(viewerReceive.path());

        connect(
            &host,
            &HostSession::eventReceived,
            [&hostEvents](const QJsonObject &message) {
                hostEvents.append(message);
            });
        connect(
            &viewer,
            &ViewerSession::eventReceived,
            [&viewerEvents](const QJsonObject &message) {
                viewerEvents.append(message);
            });
        connect(
            &viewer,
            &ViewerSession::videoFrameReceived,
            [&encodedFrames](
                const QByteArray &jpeg,
                quint32,
                bool) {
                encodedFrames.append(jpeg);
            });
        connect(
            &viewer,
            &ViewerSession::frameDecoded,
            [&decodedFrames, &keyframes](
                const QImage &image,
                bool keyframe) {
                decodedFrames.append(image);
                keyframes.append(keyframe);
            });
        connect(
            &viewer,
            &ViewerSession::audioReceived,
            [&audioFrames](
                const QByteArray &pcm,
                quint32,
                quint32) {
                audioFrames.append(pcm);
            });
        connect(
            &viewer,
            &ViewerSession::statsChanged,
            [&viewerStats](const QJsonObject &stats) {
                viewerStats.append(stats);
            });
        connect(
            &viewer,
            &ViewerSession::audioStateChanged,
            [&viewerAudioStates](const QJsonObject &state) {
                viewerAudioStates.append(state);
            });
        connect(
            &host,
            &HostSession::fileReceived,
            [&hostFiles](const QString &path, qint64) {
                hostFiles.append(path);
            });
        connect(
            &viewer,
            &ViewerSession::fileReceived,
            [&viewerFiles](const QString &path, qint64) {
                viewerFiles.append(path);
            });

        QVERIFY(host.start());
        viewer.start();
        hostLink.addCandidates(viewerLink.localCandidates());
        viewerLink.addCandidates(hostLink.localCandidates());
        hostLink.beginHandshake();
        viewerLink.beginHandshake();

        QVERIFY(waitUntil(
            [&]() {
                return hostLink.established() && viewerLink.established()
                    && hostLink.state() == QStringLiteral("direct")
                    && viewerLink.state() == QStringLiteral("direct");
            },
            7000));
        QVERIFY(waitUntil(
            [&]() { return host.running() && viewer.active(); },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return viewer.info()
                    .value(QStringLiteral("monitors"))
                    .toArray()
                    .size()
                    == 2;
            },
            5000));
        const QJsonObject info = viewer.info();
        QCOMPARE(
            info.value(QStringLiteral("monitors")).toArray().size(),
            qsizetype(2));
        QCOMPARE(
            info.value(QStringLiteral("settings"))
                .toObject()
                .value(QStringLiteral("pixel_format"))
                .toString(),
            QStringLiteral("jpeg"));
        QVERIFY(info.value(QStringLiteral("audio")).isObject());

        QVERIFY(waitUntil(
            [&]() { return !decodedFrames.isEmpty(); },
            6000));
        QCOMPARE(decodedFrames.constFirst().size(), QSize(1280, 720));
        QVERIFY(!encodedFrames.constFirst().isEmpty());
        QVERIFY(waitUntil(
            [&]() { return !audioFrames.isEmpty(); },
            5000));
        QCOMPARE(audioFrames.constFirst().size(), qsizetype(kAudioPcmBytes));
        QVERIFY(host.audioState().value(QStringLiteral("on")).toBool());
        QCOMPARE(
            host.audioState().value(QStringLiteral("t")).toString(),
            QStringLiteral("audio"));
        QVERIFY(waitUntil(
            [&]() {
                return !viewer.audioState().isEmpty()
                    && viewer.audioState()
                           .value(QStringLiteral("on"))
                           .toBool();
            },
            3000));
        QCOMPARE(
            viewer.audioState().value(QStringLiteral("t")).toString(),
            QStringLiteral("audio"));
        QCOMPARE(
            viewer.audioState().value(QStringLiteral("codec")).toString(),
            QStringLiteral("pcmu"));
        QVERIFY(!viewerAudioStates.isEmpty());

        viewer.setAudioEnabled(false);
        QVERIFY(waitUntil(
            [&]() {
                return !host.audioState()
                            .value(QStringLiteral("enabled"))
                            .toBool()
                    && !viewer.audioState()
                            .value(QStringLiteral("enabled"))
                            .toBool()
                    && !viewer.audioState()
                            .value(QStringLiteral("on"))
                            .toBool();
            },
            3000));

        viewer.setAudioEnabled(true);
        QVERIFY(waitUntil(
            [&]() {
                return host.audioState()
                           .value(QStringLiteral("enabled"))
                           .toBool()
                    && host.audioState()
                           .value(QStringLiteral("on"))
                           .toBool()
                    && viewer.audioState()
                           .value(QStringLiteral("enabled"))
                           .toBool()
                    && viewer.audioState()
                           .value(QStringLiteral("on"))
                           .toBool();
            },
            3000));
        QVERIFY(waitUntil(
            [&]() { return !viewerStats.isEmpty(); },
            5000));
        QCOMPARE(
            viewerStats.constLast().value(QStringLiteral("t")).toString(),
            QStringLiteral("stats"));

        const int keyframesBefore = countKeyframes(keyframes);
        viewer.requestKeyframe();
        QVERIFY(waitUntil(
            [&]() { return countKeyframes(keyframes) > keyframesBefore; },
            4000));

        viewer.setQuality(47);
        QVERIFY(waitUntil(
            [&]() { return host.settings().quality == 47; },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("settings_ack")
                            && message.value(QStringLiteral("settings"))
                                       .toObject()
                                       .value(QStringLiteral("quality"))
                                       .toInt()
                                == 47;
                    });
            },
            3000));

        viewer.setMaxWidth(640);
        QVERIFY(waitUntil(
            [&]() { return host.settings().maxWidth == 640; },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    decodedFrames.cbegin(),
                    decodedFrames.cend(),
                    [](const QImage &image) { return image.width() == 640; });
            },
            4000));

        viewer.selectMonitor(1);
        QVERIFY(waitUntil(
            [&]() { return host.settings().monitor == 1; },
            3000));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    decodedFrames.cbegin(),
                    decodedFrames.cend(),
                    [](const QImage &image) {
                        return image.size() == QSize(640, 480);
                    });
            },
            4000));

        const qsizetype movesBefore = input->absoluteMoves.size();
        viewer.sendMouseMove(0.5, 0.5);
        QVERIFY(waitUntil(
            [&]() { return input->absoluteMoves.size() > movesBefore; },
            3000));
        const QPointF mapped = input->absoluteMoves.constLast();
        QVERIFY(qAbs(mapped.x() - (1792.0 / 2303.0)) < 0.005);
        QVERIFY(qAbs(mapped.y() - (384.0 / 767.0)) < 0.005);

        QVERIFY(viewer.sendKey(QStringLiteral("KeyA"), true));
        QVERIFY(waitUntil(
            [&]() {
                return std::find(
                           input->keys.cbegin(),
                           input->keys.cend(),
                           qMakePair(QStringLiteral("KeyA"), true))
                    != input->keys.cend();
            },
            3000));
        viewer.sendText(QStringLiteral("PeerDesk 输入"));
        QVERIFY(waitUntil(
            [&]() { return input->typed == QStringLiteral("PeerDesk 输入"); },
            3000));
        viewer.sendMouseButton(QStringLiteral("click"), 0.25, 0.75, QStringLiteral("right"));
        QVERIFY(waitUntil(
            [&]() {
                return !input->clicks.isEmpty()
                    && input->clicks.constLast() == QStringLiteral("right");
            },
            3000));
        viewer.sendMouseWheel(120, false);
        QVERIFY(waitUntil(
            [&]() { return !input->wheels.isEmpty(); },
            3000));
        QCOMPARE(input->wheels.constLast(), qMakePair(120, false));
        viewer.sendMouseRelative(7, -4);
        QVERIFY(waitUntil(
            [&]() { return !input->relativeMoves.isEmpty(); },
            3000));
        QCOMPARE(input->relativeMoves.constLast(), QPoint(7, -4));
        viewer.sendCtrlAltDelete();
        QVERIFY(waitUntil(
            [&]() { return input->cadCount == 1; },
            3000));

        const int mutedMoves = static_cast<int>(input->absoluteMoves.size());
        const int mutedKeys = static_cast<int>(input->keys.size());
        const int mutedCad = input->cadCount;
        const QString mutedTyped = input->typed;
        host.setAllowInput(false);
        QVERIFY(viewer.sendKey(QStringLiteral("KeyB"), true));
        viewer.sendText(QStringLiteral("blocked"));
        viewer.sendCtrlAltDelete();
        viewer.sendMouseMove(0.1, 0.2);

        const int infoEventsBefore = static_cast<int>(std::count_if(
            viewerEvents.cbegin(),
            viewerEvents.cend(),
            [](const QJsonObject &message) {
                return message.value(QStringLiteral("t")).toString()
                    == QStringLiteral("info");
            }));
        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("hello")},
            {QStringLiteral("role"), QStringLiteral("viewer")},
        }));
        QVERIFY(waitUntil(
            [&]() {
                return static_cast<int>(std::count_if(
                           viewerEvents.cbegin(),
                           viewerEvents.cend(),
                           [](const QJsonObject &message) {
                               return message.value(QStringLiteral("t"))
                                          .toString()
                                   == QStringLiteral("info");
                           }))
                    > infoEventsBefore;
            },
            3000));

        host.setAllowInput(true);
        QVERIFY(viewer.sendKey(QStringLiteral("KeyC"), true));
        QVERIFY(waitUntil(
            [&]() {
                return std::find(
                           input->keys.cbegin(),
                           input->keys.cend(),
                           qMakePair(QStringLiteral("KeyC"), true))
                    != input->keys.cend();
            },
            3000));
        QCOMPARE(static_cast<int>(input->absoluteMoves.size()), mutedMoves);
        QCOMPARE(static_cast<int>(input->keys.size()), mutedKeys + 1);
        QCOMPARE(input->cadCount, mutedCad);
        QCOMPARE(input->typed, mutedTyped);
        QVERIFY(std::none_of(
            input->keys.cbegin(),
            input->keys.cend(),
            [](const QPair<QString, bool> &entry) {
                return entry.first == QStringLiteral("KeyB");
            }));

        QVERIFY(viewer.sendClipboard(QStringLiteral("remote clipboard")));
        QVERIFY(waitUntil(
            [&]() {
                return clipboard->value == QStringLiteral("remote clipboard");
            },
            3000));
        const int clipboardWrites = clipboard->writes;
        clipboard->value = QStringLiteral("host clipboard update");
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("clip")
                            && message.value(QStringLiteral("s")).toString()
                                == QStringLiteral("host clipboard update");
                    });
            },
            3000));
        QVERIFY(clipboard->writes >= clipboardWrites);

        const QByteArray hostPayload = makePayload(150000, 0x12345678);
        const QString hostSource =
            sourceDirectory.filePath(QStringLiteral("host-to-viewer.bin"));
        QVERIFY(writeBytes(hostSource, hostPayload));
        QString sendError;
        QVERIFY2(host.sendFile(hostSource, &sendError), qPrintable(sendError));
        QVERIFY(waitUntil(
            [&]() { return viewerFiles.size() == 1; },
            12000));
        QCOMPARE(readBytes(viewerFiles.constFirst()), hostPayload);
        QVERIFY(waitUntil(
            [&]() { return hasMessage(hostEvents, QStringLiteral("file_done")); },
            5000));

        const QByteArray viewerPayload = makePayload(90000, 0x87654321);
        const QString viewerSource =
            sourceDirectory.filePath(QStringLiteral("viewer-to-host.bin"));
        QVERIFY(writeBytes(viewerSource, viewerPayload));
        QVERIFY2(viewer.sendFile(viewerSource, &sendError), qPrintable(sendError));
        QVERIFY(waitUntil(
            [&]() { return hostFiles.size() == 1; },
            12000));
        QCOMPARE(readBytes(hostFiles.constFirst()), viewerPayload);

        const QByteArray uploadPayload = makePayload(6000, 0xABCDEF01);
        QVERIFY2(
            viewer.beginUpload(
                QStringLiteral("api-upload.bin"),
                uploadPayload.size(),
                &sendError),
            qPrintable(sendError));
        QVERIFY2(
            viewer.uploadChunk(uploadPayload.left(4096), &sendError),
            qPrintable(sendError));
        QVERIFY2(
            viewer.uploadChunk(uploadPayload.mid(4096), &sendError),
            qPrintable(sendError));
        QVERIFY2(viewer.endUpload(&sendError), qPrintable(sendError));
        QVERIFY(waitUntil(
            [&]() { return hostFiles.size() == 2; },
            12000));
        QCOMPARE(readBytes(hostFiles.constLast()), uploadPayload);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("upload")
                            && message.value(QStringLiteral("state")).toString()
                                == QStringLiteral("acknowledged");
                    });
            },
            5000));

        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("id"), QStringLiteral("attack")},
            {QStringLiteral("name"), QStringLiteral("../escape.txt")},
            {QStringLiteral("size"), 3},
        }));
        QVERIFY(waitUntil(
            [&]() {
                return hasMessage(
                    viewerEvents,
                    QStringLiteral("file_error"),
                    QStringLiteral("attack"));
            },
            3000));
        QVERIFY(!QFileInfo::exists(
            hostReceive.filePath(QStringLiteral("../escape.txt"))));
        QVERIFY(!QFileInfo::exists(
            hostReceive.filePath(QStringLiteral("escape.txt"))));

        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("id"), QStringLiteral("bad64")},
            {QStringLiteral("name"), QStringLiteral("bad.bin")},
            {QStringLiteral("size"), 3},
        }));
        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_data")},
            {QStringLiteral("id"), QStringLiteral("bad64")},
            {QStringLiteral("d"), QStringLiteral("!!!!")},
        }));
        QVERIFY(waitUntil(
            [&]() {
                return hasMessage(
                    viewerEvents,
                    QStringLiteral("file_error"),
                    QStringLiteral("bad64"));
            },
            3000));
        QVERIFY(!QFileInfo::exists(
            hostReceive.filePath(QStringLiteral("bad.bin"))));

        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("id"), QStringLiteral("truncated")},
            {QStringLiteral("name"), QStringLiteral("truncated.bin")},
            {QStringLiteral("size"), 4},
        }));
        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_data")},
            {QStringLiteral("id"), QStringLiteral("truncated")},
            {QStringLiteral("d"), QStringLiteral("AA==")},
        }));
        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_end")},
            {QStringLiteral("id"), QStringLiteral("truncated")},
        }));
        QVERIFY(waitUntil(
            [&]() {
                return hasMessage(
                    viewerEvents,
                    QStringLiteral("file_error"),
                    QStringLiteral("truncated"));
            },
            3000));
        QVERIFY(!QFileInfo::exists(
            hostReceive.filePath(QStringLiteral("truncated.bin"))));
    }

    void sessionTeardownDoesNotNotifyObservers()
    {
        int infoChanges = 0;
        int statsChanges = 0;
        int eventChanges = 0;
        int infoAfterTeardown = 0;
        int statsAfterTeardown = 0;
        int eventsAfterTeardown = 0;

        {
            const QByteArray psk =
                QByteArrayLiteral("session-teardown-secret");
            PeerLink hostLink(true, psk);
            PeerLink viewerLink(false, psk);
            hostLink.setLabel(QStringLiteral("teardown-host"));
            viewerLink.setLabel(QStringLiteral("teardown-viewer"));
            hostLink.setDeviceId(QStringLiteral("teardown-host-device"));
            viewerLink.setDeviceId(QStringLiteral("teardown-viewer-device"));

            QString error;
            QVERIFY2(
                hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
                qPrintable(error));
            QVERIFY2(
                viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
                qPrintable(error));

            QTemporaryDir hostReceive;
            QTemporaryDir viewerReceive;
            QVERIFY(hostReceive.isValid());
            QVERIFY(viewerReceive.isValid());

            auto screen =
                std::make_shared<SyntheticScreenSource>(960, 540, false);

            HostSession host(&hostLink);
            ViewerSession viewer(&viewerLink);
            host.setScreenSource(screen);
            host.setReceiveDirectory(hostReceive.path());
            viewer.setReceiveDirectory(viewerReceive.path());

            connect(
                &viewer,
                &ViewerSession::infoChanged,
                [&infoChanges](const QJsonObject &) { ++infoChanges; });
            connect(
                &viewer,
                &ViewerSession::statsChanged,
                [&statsChanges](const QJsonObject &) { ++statsChanges; });
            connect(
                &viewer,
                &ViewerSession::eventReceived,
                [&eventChanges](const QJsonObject &) { ++eventChanges; });

            QVERIFY(host.start());
            viewer.start();
            hostLink.addCandidates(viewerLink.localCandidates());
            viewerLink.addCandidates(hostLink.localCandidates());
            hostLink.beginHandshake();
            viewerLink.beginHandshake();

            QVERIFY(waitUntil(
                [&]() {
                    return hostLink.established() && viewerLink.established()
                        && hostLink.state() == QStringLiteral("direct")
                        && viewerLink.state() == QStringLiteral("direct");
                },
                9000));
            QVERIFY(waitUntil(
                [&]() {
                    return host.running() && !viewer.info().isEmpty();
                },
                7000));
            QVERIFY(waitUntil(
                [&]() { return !viewer.stats().isEmpty(); },
                7000));

            const int infoBeforeStop = infoChanges;
            const int statsBeforeStop = statsChanges;
            const int eventsBeforeStop = eventChanges;
            viewer.stop();

            QCOMPARE(infoChanges, infoBeforeStop + 1);
            QCOMPARE(statsChanges, statsBeforeStop + 1);
            QCOMPARE(eventChanges, eventsBeforeStop);
            QVERIFY(viewer.info().isEmpty());
            QVERIFY(viewer.stats().isEmpty());

            viewer.start();
            QVERIFY(waitUntil(
                [&]() {
                    return !viewer.info().isEmpty()
                        && !viewer.stats().isEmpty();
                },
                7000));

            QString uploadError;
            QVERIFY2(
                viewer.beginUpload(
                    QStringLiteral("held.bin"),
                    8,
                    &uploadError),
                qPrintable(uploadError));

            infoAfterTeardown = infoChanges;
            statsAfterTeardown = statsChanges;
            eventsAfterTeardown = eventChanges;
        }

        QCOMPARE(infoChanges, infoAfterTeardown);
        QCOMPARE(statsChanges, statsAfterTeardown);
        QCOMPARE(eventChanges, eventsAfterTeardown);
    }

    void relaySessionTransfersFile()
    {
        SignalingServer::Options options;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 0;
        options.udpPort = 0;
        SignalingServer server(options);
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));

        SignalClient agent;
        SignalClient viewer;
        QVERIFY(agent.connectTo(server.signalingPort()));
        QVERIFY(viewer.connectTo(server.signalingPort()));
        agent.send(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("register")},
            {QStringLiteral("code"), QStringLiteral("604173830")},
            {QStringLiteral("name"), QStringLiteral("session-host")},
            {QStringLiteral("cands"), QJsonArray{}},
        });
        QVERIFY(agent.take(QStringLiteral("registered")).has_value());
        viewer.send(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("connect")},
            {QStringLiteral("code"), QStringLiteral("604173830")},
            {QStringLiteral("name"), QStringLiteral("session-viewer")},
            {QStringLiteral("cands"), QJsonArray{}},
        });
        const auto agentViewer = agent.take(QStringLiteral("viewer"));
        const auto viewerPeer = viewer.take(QStringLiteral("peer"));
        QVERIFY(agentViewer.has_value());
        QVERIFY(viewerPeer.has_value());

        const QByteArray psk = QByteArrayLiteral("session-relay-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setForceRelay(true);
        viewerLink.setForceRelay(true);
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        hostLink.offerRelay(server.publicHost(), server.relayPort());
        viewerLink.offerRelay(server.publicHost(), server.relayPort());
        hostLink.setRelayOutTag(relayTag(*agentViewer));
        viewerLink.setRelayOutTag(relayTag(*viewerPeer));

        QTemporaryDir sourceDirectory;
        QTemporaryDir hostReceive;
        QTemporaryDir viewerReceive;
        QVERIFY(sourceDirectory.isValid());
        QVERIFY(hostReceive.isValid());
        QVERIFY(viewerReceive.isValid());

        auto screen = std::make_shared<SyntheticScreenSource>(1280, 720, false);
        QList<QImage> frames;
        QList<QString> receivedFiles;
        QList<QJsonObject> viewerEvents;

        HostSession host(&hostLink);
        ViewerSession remote(&viewerLink);
        host.setScreenSource(screen);
        host.setReceiveDirectory(hostReceive.path());
        remote.setReceiveDirectory(viewerReceive.path());

        connect(
            &remote,
            &ViewerSession::frameDecoded,
            [&frames](const QImage &image, bool) {
                frames.append(image);
            });
        connect(
            &remote,
            &ViewerSession::fileReceived,
            [&receivedFiles](const QString &path, qint64) {
                receivedFiles.append(path);
            });
        connect(
            &remote,
            &ViewerSession::eventReceived,
            [&viewerEvents](const QJsonObject &message) {
                viewerEvents.append(message);
            });

        QVERIFY(host.start());
        remote.start();
        hostLink.beginHandshake();
        viewerLink.beginHandshake();
        QVERIFY(waitUntil(
            [&]() {
                return hostLink.established() && viewerLink.established()
                    && hostLink.state() == QStringLiteral("relay")
                    && viewerLink.state() == QStringLiteral("relay");
            },
            9000));
        QVERIFY(hostLink.stats().usingRelay);
        QVERIFY(viewerLink.stats().usingRelay);
        QVERIFY(waitUntil(
            [&]() { return host.running() && !remote.info().isEmpty(); },
            5000));
        QVERIFY(waitUntil(
            [&]() { return !frames.isEmpty(); },
            7000));

        const QByteArray payload = makePayload(64000, 0x13579BDF);
        const QString sourcePath =
            sourceDirectory.filePath(QStringLiteral("relay-transfer.bin"));
        QVERIFY(writeBytes(sourcePath, payload));
        QVERIFY2(host.sendFile(sourcePath, &error), qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return receivedFiles.size() == 1; },
            15000));
        QCOMPARE(readBytes(receivedFiles.constFirst()), payload);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("file_in")
                            && message.value(QStringLiteral("state")).toString()
                                == QStringLiteral("done");
                    });
            },
            5000));
    }
};

QTEST_MAIN(SessionTest)

#include "session_test.moc"
