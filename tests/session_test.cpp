#include "server/server.h"
#ifdef PD_HAS_H264
#include "codec/media_foundation_h264.h"
#endif
#include "session/adaptive_video_rate_controller.h"
#include "session/audio_codec.h"
#include "session/clipboard_codec.h"
#include "session/cursor_codec.h"
#include "session/file_transfer.h"
#include "session/file_transfer_pipeline.h"
#include "session/host_input_pipeline.h"
#include "session/host_session.h"
#include "session/media_sources.h"
#include "session/session_types.h"
#include "session/viewer_session.h"
#include "transport/peer_link.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageWriter>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
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
    struct Snapshot {
        QList<QPointF> absoluteMoves;
        QList<QPoint> relativeMoves;
        QList<QPair<QString, bool>> buttons;
        QList<QString> clicks;
        QList<QPair<int, bool>> wheels;
        QList<QPair<QString, bool>> keys;
        QString typed;
        int cadCount = 0;
        int releaseCount = 0;
        QStringList operations;
        QStringList threadNames;
    };

    [[nodiscard]] QRect virtualDesktop() const override
    {
        return QRect(0, 0, 2304, 768);
    }

    void moveAbsolute(double x, double y) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.absoluteMoves.append(QPointF(x, y));
        state_.operations.append(QStringLiteral("move:%1:%2")
                                     .arg(x, 0, 'f', 6)
                                     .arg(y, 0, 'f', 6));
        recordThread();
    }

    void moveRelative(int dx, int dy) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.relativeMoves.append(QPoint(dx, dy));
        state_.operations.append(
            QStringLiteral("relative:%1:%2").arg(dx).arg(dy));
        recordThread();
    }

    void mouseButton(const QString &button, bool down) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.buttons.append(qMakePair(button, down));
        state_.operations.append(QStringLiteral("button:%1:%2")
                                     .arg(button, down ? QStringLiteral("down")
                                                       : QStringLiteral("up")));
        recordThread();
    }

    void click(const QString &button) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.clicks.append(button);
        state_.operations.append(QStringLiteral("click:%1").arg(button));
        recordThread();
    }

    void wheel(int delta, bool horizontal) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.wheels.append(qMakePair(delta, horizontal));
        state_.operations.append(QStringLiteral("wheel:%1:%2")
                                     .arg(delta)
                                     .arg(horizontal ? 1 : 0));
        recordThread();
    }

    void key(const QString &code, bool down) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        state_.keys.append(qMakePair(code, down));
        state_.operations.append(QStringLiteral("key:%1:%2")
                                     .arg(code, down ? QStringLiteral("down")
                                                     : QStringLiteral("up")));
        recordThread();
        if (!gateCode_.isEmpty() && code == gateCode_) {
            gateCode_.clear();
            gateEntered_ = true;
            gateCondition_.notify_all();
            gateCondition_.wait(lock, [this]() { return gateOpen_; });
        }
    }

    void typeText(const QString &text) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_.typed.append(text);
        state_.operations.append(QStringLiteral("text:%1").arg(text));
        recordThread();
    }

    void ctrlAltDelete() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++state_.cadCount;
        state_.operations.append(QStringLiteral("cad"));
        recordThread();
    }

    void releaseAll() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++state_.releaseCount;
        state_.operations.append(QStringLiteral("release"));
        recordThread();
    }

    [[nodiscard]] Snapshot snapshot() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void armKeyGate(const QString &code)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        gateCode_ = code;
        gateEntered_ = false;
        gateOpen_ = false;
    }

    [[nodiscard]] bool waitForKeyGate(int timeoutMs = 3000)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return gateCondition_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this]() { return gateEntered_; });
    }

    void openKeyGate()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            gateOpen_ = true;
        }
        gateCondition_.notify_all();
    }

private:
    void recordThread()
    {
        state_.threadNames.append(
            QThread::currentThread()->objectName());
    }

    mutable std::mutex mutex_;
    Snapshot state_;
    QString gateCode_;
    bool gateEntered_ = false;
    bool gateOpen_ = false;
    std::condition_variable gateCondition_;
};

class FakeClipboard final : public IClipboard {
public:
    [[nodiscard]] QString text() const override { return value; }
    [[nodiscard]] QImage image() const override { return imageValue; }

    bool setText(const QString &text) override
    {
        value = text;
        imageValue = {};
        ++writes;
        return true;
    }

    bool setImage(const QImage &image) override
    {
        imageValue = image;
        value.clear();
        ++imageWrites;
        return true;
    }

    QString value;
    QImage imageValue;
    int writes = 0;
    int imageWrites = 0;
};

class ThreadAwareScreenSource final : public IScreenSource {
public:
    ThreadAwareScreenSource(
        int width = 1280,
        int height = 720,
        bool twoMonitors = false)
        : source_(width, height, twoMonitors)
    {
    }

    [[nodiscard]] QString backend() const override
    {
        return source_.backend();
    }

    [[nodiscard]] QList<MonitorInfo> monitors() const override
    {
        return source_.monitors();
    }

    CaptureResult capture(const CaptureRequest &request) override
    {
        lastCaptureThread_.store(QThread::currentThreadId());
        captureCount_.fetch_add(1);
        return source_.capture(request);
    }

    void close() override
    {
        source_.close();
    }

    [[nodiscard]] Qt::HANDLE lastCaptureThread() const
    {
        return lastCaptureThread_.load();
    }

    [[nodiscard]] int captureCount() const
    {
        return captureCount_.load();
    }

private:
    SyntheticScreenSource source_;
    std::atomic<Qt::HANDLE> lastCaptureThread_{nullptr};
    std::atomic<int> captureCount_{0};
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

QImage cursorTestImage(quint32 seed = 0, int size = 32)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.setPixelColor(
                x,
                y,
                QColor(
                    static_cast<int>((seed * 29 + x * 7) % 256),
                    static_cast<int>((seed * 47 + y * 11) % 256),
                    static_cast<int>((seed * 61 + x * 3 + y * 5) % 256),
                    static_cast<int>((seed * 17 + x * 13 + y * 19) % 256)));
        }
    }
    image.setPixelColor(0, 0, QColor(255, 32, 128, 255));
    image.setPixelColor(size - 1, size - 1, QColor(12, 220, 255, 1));
    return image;
}

QJsonObject cursorImageMessage(const QImage &image)
{
    QString error;
    const QByteArray png = encodeCursorImage(image, &error);
    if (png.isEmpty()) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("t"), QStringLiteral("cursor_image")},
        {QStringLiteral("id"),
         static_cast<double>(cursorImageId(image))},
        {QStringLiteral("mime"), QStringLiteral("image/png")},
        {QStringLiteral("w"), image.width()},
        {QStringLiteral("h"), image.height()},
        {QStringLiteral("d"), QString::fromLatin1(png.toBase64())},
    };
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

        FramePacer limitedPacer(80, 100, 32, 92);
        limitedPacer.setQualityLimit(45);
        QCOMPARE(limitedPacer.quality(), 45);
        for (int index = 0; index < 12; ++index) {
            limitedPacer.observe(1024, 4.0);
        }
        QCOMPARE(limitedPacer.quality(), 45);
        limitedPacer.setQuality(90);
        QCOMPARE(limitedPacer.quality(), 45);
        limitedPacer.setQualityLimit(60);
        QCOMPARE(limitedPacer.quality(), 45);
        limitedPacer.setQuality(90);
        QCOMPARE(limitedPacer.quality(), 60);
        QCOMPARE(limitedPacer.qualityLimit(), 60);
    }

    void clipboardImageCodecValidatesAndRoundTrips()
    {
        QImage image(96, 64, QImage::Format_ARGB32);
        image.fill(QColor(QStringLiteral("#203a54")));
        for (int y = 8; y < 40; ++y) {
            for (int x = 12; x < 72; ++x) {
                image.setPixelColor(
                    x,
                    y,
                    QColor(220, 96 + y / 2, 48, 180));
            }
        }

        QString error = QStringLiteral("stale");
        const QByteArray png = encodeClipboardImage(image, &error);
        QVERIFY2(png.isEmpty() == false, qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));

        const ClipboardImageDecodeResult decoded =
            decodeClipboardImage(QString::fromLatin1(png.toBase64()));
        QVERIFY2(decoded.ok(), qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(), image.size());
        QCOMPARE(
            decoded.image.convertToFormat(QImage::Format_ARGB32),
            image);
        QCOMPARE(
            clipboardImageFingerprint(decoded.image),
            clipboardImageFingerprint(image));
        QVERIFY(clipboardImageMatchesDimensions(
            decoded.image,
            image.width(),
            image.height()));
        QVERIFY(!clipboardImageMatchesDimensions(
            decoded.image,
            image.width() + 1,
            image.height()));
        QVERIFY(!clipboardImageMatchesDimensions(
            decoded.image,
            image.width(),
            -1));

        QImage changed = image.copy();
        changed.setPixelColor(0, 0, QColor(QStringLiteral("#ffcc00")));
        QVERIFY(
            clipboardImageFingerprint(changed)
            != clipboardImageFingerprint(image));

        QVERIFY(!decodeClipboardImage(
                     QStringLiteral("not base64!")).ok());
        QVERIFY(!decodeClipboardImage(
                     QString::fromLatin1(
                         QByteArrayLiteral("hello").toBase64())).ok());
        constexpr qsizetype kPngDecodedLimit = 4 * 1024 * 1024;
        const qsizetype oversizedLength =
            ((kPngDecodedLimit + 2) / 3) * 4 + 4;
        const QByteArray oversizedBase64(oversizedLength, 'A');
        QVERIFY(!decodeClipboardImage(
                     QString::fromLatin1(oversizedBase64)).ok());

        QByteArray tampered = png;
        QVERIFY(tampered.size() >= 24);
        const quint32 oversizedWidth = 9000;
        tampered[16] = static_cast<char>((oversizedWidth >> 24) & 0xFF);
        tampered[17] = static_cast<char>((oversizedWidth >> 16) & 0xFF);
        tampered[18] = static_cast<char>((oversizedWidth >> 8) & 0xFF);
        tampered[19] = static_cast<char>(oversizedWidth & 0xFF);
        QVERIFY(!decodeClipboardImage(
                     QString::fromLatin1(tampered.toBase64())).ok());

        const quint32 oversizedPixels = 5000;
        tampered[16] = static_cast<char>((oversizedPixels >> 24) & 0xFF);
        tampered[17] = static_cast<char>((oversizedPixels >> 16) & 0xFF);
        tampered[18] = static_cast<char>((oversizedPixels >> 8) & 0xFF);
        tampered[19] = static_cast<char>(oversizedPixels & 0xFF);
        tampered[20] = static_cast<char>((oversizedPixels >> 24) & 0xFF);
        tampered[21] = static_cast<char>((oversizedPixels >> 16) & 0xFF);
        tampered[22] = static_cast<char>((oversizedPixels >> 8) & 0xFF);
        tampered[23] = static_cast<char>(oversizedPixels & 0xFF);
        QVERIFY(!decodeClipboardImage(
                     QString::fromLatin1(tampered.toBase64())).ok());

        QImage tooWide(9000, 1, QImage::Format_RGB32);
        tooWide.fill(Qt::black);
        const QByteArray rejected = encodeClipboardImage(tooWide, &error);
        QVERIFY(rejected.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void cursorImageCodecValidatesAndRoundTrips()
    {
        const QImage image = cursorTestImage(7);
        const quint32 imageId = cursorImageId(image);
        QVERIFY(imageId != 0);
        QCOMPARE(cursorImageId(image.copy()), imageId);

        QString error;
        const QByteArray png = encodeCursorImage(image, &error);
        QVERIFY2(!png.isEmpty(), qPrintable(error));
        QVERIFY(png.size() <= kCursorImageMaxPngBytes);

        const CursorImageDecodeResult decoded = decodeCursorImage(
            QString::fromLatin1(png.toBase64()),
            image.width(),
            image.height());
        QVERIFY2(decoded.ok(), qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(), image.size());
        QCOMPARE(
            decoded.image.convertToFormat(QImage::Format_ARGB32),
            image);
        QCOMPARE(decoded.image.pixelColor(0, 0).alpha(), 255);
        QCOMPARE(
            decoded.image.pixelColor(image.width() - 1, image.height() - 1)
                .alpha(),
            1);
        QCOMPARE(cursorImageId(decoded.image), imageId);

        QImage changed = image.copy();
        changed.setPixelColor(
            0,
            0,
            QColor(255, 32, 128, 128));
        QVERIFY(cursorImageId(changed) != imageId);

        const QByteArray encoded = png.toBase64();
        QVERIFY(!decodeCursorImage(
                     QStringLiteral("not base64!"),
                     image.width(),
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QStringLiteral("AA=A"),
                     image.width(),
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QStringLiteral("-_-_"),
                     image.width(),
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QString::fromLatin1(
                         QByteArray(kCursorImageMaxPngBytes + 4, 'A')),
                     image.width(),
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QString::fromLatin1(encoded),
                     image.width() + 1,
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QString::fromLatin1(
                         QByteArrayLiteral("hello").toBase64()),
                     image.width(),
                     image.height())
                     .ok());
        QVERIFY(!decodeCursorImage(
                     QString(),
                     image.width(),
                     image.height())
                     .ok());

        QImage oversized(1, kCursorImageMaxDimension + 1,
                         QImage::Format_ARGB32);
        oversized.fill(Qt::transparent);
        QVERIFY(encodeCursorImage(oversized, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void adaptiveVideoRateUsesFeedbackAndRecovers()
    {
        AdaptiveVideoRateController controller;
        controller.reset(90, 60);
        QCOMPARE(controller.qualityLimit(), 90);
        QCOMPARE(controller.fpsLimit(), 60);

        AdaptiveVideoLimits limits = controller.observe(
            AdaptiveVideoFeedback{0.0, 20.0, 0, 0, 0});
        QVERIFY(limits.health == AdaptiveLinkHealth::Healthy);
        QCOMPARE(limits.quality, 90);
        QCOMPARE(limits.fps, 60);

        limits = controller.observe(
            AdaptiveVideoFeedback{1.0, 170.0, 0, 0, 0});
        QVERIFY(limits.health == AdaptiveLinkHealth::Congested);
        QCOMPARE(limits.quality, 86);
        QCOMPARE(limits.fps, 57);

        limits = controller.observe(
            AdaptiveVideoFeedback{1.2, 260.0, 0, 0, 0});
        QCOMPARE(limits.quality, 86);
        QCOMPARE(limits.fps, 57);

        limits = controller.observe(
            AdaptiveVideoFeedback{2.0, 260.0, 0, 0, 0});
        QCOMPARE(limits.quality, 78);
        QCOMPARE(limits.fps, 51);

        limits = controller.observe(
            AdaptiveVideoFeedback{2.1, 25.0, 0, 0, 0});
        QVERIFY(limits.health == AdaptiveLinkHealth::Healthy);
        QCOMPARE(limits.quality, 78);
        QCOMPARE(limits.fps, 51);

        limits = controller.observe(
            AdaptiveVideoFeedback{5.9, 25.0, 0, 0, 0});
        QCOMPARE(limits.quality, 78);
        QCOMPARE(limits.fps, 51);

        limits = controller.observe(
            AdaptiveVideoFeedback{6.2, 25.0, 0, 0, 0});
        QCOMPARE(limits.quality, 80);
        QCOMPARE(limits.fps, 53);

        limits = controller.observe(
            AdaptiveVideoFeedback{6.3, 50.0, 0, 2, 0});
        QVERIFY(limits.health == AdaptiveLinkHealth::Guarded);
        QCOMPARE(limits.quality, 80);
        QCOMPARE(limits.fps, 53);

        limits = controller.observe(
            AdaptiveVideoFeedback{7.4, 250.0, 2, 12, 2});
        QCOMPARE(limits.quality, 72);
        QCOMPARE(limits.fps, 47);

        controller.setCeilings(70, 45);
        QCOMPARE(controller.qualityLimit(), 70);
        QCOMPARE(controller.fpsLimit(), 45);

        controller.reset(55, 24);
        QCOMPARE(controller.qualityLimit(), 55);
        QCOMPARE(controller.fpsLimit(), 24);
        QVERIFY(controller.health() == AdaptiveLinkHealth::Unknown);
    }

    void videoPipelineRunsOffMainThread()
    {
        auto screen = std::make_shared<ThreadAwareScreenSource>(
            1280,
            720,
            true);
        VideoPipeline pipeline;
        QList<EncodedVideoFrame> frames;
        QList<QString> errors;
        connect(
            &pipeline,
            &VideoPipeline::frameReady,
            [&frames](const EncodedVideoFrame &frame) {
                frames.append(frame);
            });
        connect(
            &pipeline,
            &VideoPipeline::failed,
            [&errors](const QString &error) { errors.append(error); });

        pipeline.setSource(screen);
        QCOMPARE(pipeline.monitors().size(), qsizetype(2));
        QCOMPARE(pipeline.backend(), QStringLiteral("synthetic"));

        VideoSettings settings;
        settings.fps = 20;
        settings.maxWidth = 640;
        settings.quality = 71;
        settings.skipStatic = false;
        QString error;
        QVERIFY2(
            pipeline.start(settings, settings.quality, &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return !frames.isEmpty(); },
            4000));
        if (!errors.isEmpty()) {
            QFAIL(qPrintable(errors.constFirst()));
        }
        QVERIFY(screen->captureCount() > 0);
        QVERIFY(
            screen->lastCaptureThread()
            != QThread::currentThreadId());

        const EncodedVideoFrame first = frames.constLast();
        QVERIFY(first.keyframe);
        QVERIFY(!first.data.isEmpty());
        QVERIFY(first.quality > 0);
        QVERIFY(first.effectiveFps > 0);
        const QImage firstImage =
            QImage::fromData(first.data, "jpeg");
        QVERIFY(!firstImage.isNull());
        QCOMPARE(firstImage.size(), QSize(640, 360));

        const qsizetype beforeKeyframe = frames.size();
        pipeline.requestKeyframe();
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin() + beforeKeyframe,
                    frames.cend(),
                    [](const EncodedVideoFrame &frame) {
                        return frame.keyframe;
                    });
            },
            3000));

        VideoSettings updated = settings;
        updated.maxWidth = 320;
        updated.monitor = 1;
        updated.quality = 48;
        pipeline.update(updated, updated.quality);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin(),
                    frames.cend(),
                    [](const EncodedVideoFrame &frame) {
                        const QImage image =
                            QImage::fromData(frame.data, "jpeg");
                        return image.size() == QSize(320, 240);
                    });
            },
            4000));

        pipeline.setRateLimit(2.0);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin(),
                    frames.cend(),
                    [](const EncodedVideoFrame &frame) {
                        return frame.quality != 0;
                    });
            },
            3000));

        pipeline.stop();
        const qsizetype afterStop = frames.size();
        pumpEvents(200);
        QCOMPARE(frames.size(), afterStop);

        QVERIFY2(
            pipeline.start(updated, updated.quality, &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return frames.size() > afterStop; },
            4000));
        pipeline.stop();
    }

    void videoPipelineAppliesAdaptiveQualityAndFps()
    {
        auto screen = std::make_shared<SyntheticScreenSource>(
            1280,
            720,
            true);
        VideoPipeline pipeline;
        QList<EncodedVideoFrame> frames;
        QList<QString> errors;
        connect(
            &pipeline,
            &VideoPipeline::frameReady,
            [&frames](const EncodedVideoFrame &frame) {
                frames.append(frame);
            });
        connect(
            &pipeline,
            &VideoPipeline::failed,
            [&errors](const QString &error) {
                errors.append(error);
            });

        pipeline.setSource(screen);
        pipeline.setAdaptiveLimits(40, 8);
        VideoSettings settings;
        settings.fps = 60;
        settings.maxWidth = 640;
        settings.quality = 80;
        settings.skipStatic = false;
        QString error;
        QVERIFY2(
            pipeline.start(settings, settings.quality, &error),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin(),
                    frames.cend(),
                    [](const EncodedVideoFrame &frame) {
                        return frame.quality <= 40
                            && frame.effectiveFps <= 8;
                    });
            },
            4000));
        if (!errors.isEmpty()) {
            QFAIL(qPrintable(errors.constFirst()));
        }
        const auto adaptiveFrame = std::find_if(
            frames.cbegin(),
            frames.cend(),
            [](const EncodedVideoFrame &frame) {
                return frame.quality <= 40
                    && frame.effectiveFps <= 8;
            });
        QVERIFY(adaptiveFrame != frames.cend());
        QVERIFY(adaptiveFrame->quality <= 40);
        QCOMPARE(adaptiveFrame->effectiveFps, 8);

        pipeline.setRateLimit(0.01);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    frames.cbegin(),
                    frames.cend(),
                    [](const EncodedVideoFrame &frame) {
                        return frame.effectiveFps < 8;
                    });
            },
            5000));
        pipeline.stop();
    }

    void viewerVideoPipelineDecodesOffMainThread()
    {
        QImage sourceImage(320, 180, QImage::Format_RGB32);
        sourceImage.fill(QColor(32, 96, 160));
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImageWriter writer(&buffer, QByteArrayLiteral("jpeg"));
        QVERIFY(writer.write(sourceImage));
        buffer.close();
        QVERIFY(!jpeg.isEmpty());

        ViewerVideoPipeline pipeline;
        QList<quint32> sequences;
        QList<QImage> frames;
        QList<QString> errors;
        connect(
            &pipeline,
            &ViewerVideoPipeline::frameDecoded,
            [&sequences, &frames](
                const QImage &image,
                quint32 sequence,
                bool) {
                sequences.append(sequence);
                frames.append(image);
            });
        connect(
            &pipeline,
            &ViewerVideoPipeline::decodeFailed,
            [&errors](const QString &error) { errors.append(error); });

        pipeline.start();
        pipeline.submit(jpeg, 7, true);
        QVERIFY(waitUntil(
            [&]() { return !frames.isEmpty(); },
            3000));
        QCOMPARE(sequences.constLast(), 7U);
        QCOMPARE(frames.constLast().size(), sourceImage.size());
        QCOMPARE(
            pipeline.decoderThreadName(),
            QStringLiteral("PeerDeskViewerVideo"));

        pipeline.submit(jpeg, 8, true);
        pipeline.submit(jpeg, 9, false);
        QVERIFY(waitUntil(
            [&]() { return sequences.size() >= 2; },
            3000));
        QCOMPARE(sequences.at(1), 8U);

        pipeline.submit(QByteArrayLiteral("not-a-jpeg"), 10, false);
        QVERIFY(waitUntil(
            [&]() { return !errors.isEmpty(); },
            3000));

        pipeline.stop();
        const qsizetype stoppedCount = frames.size();
        pipeline.submit(jpeg, 11, false);
        pumpEvents(150);
        QCOMPARE(frames.size(), stoppedCount);

        pipeline.start();
        pipeline.submit(jpeg, 12, false);
        QVERIFY(waitUntil(
            [&]() {
                return !sequences.isEmpty()
                    && sequences.constLast() == 12;
            },
            3000));
        pipeline.stop();
    }

    void viewerVideoPipelineHonorsH264Keyframes()
    {
#ifndef PD_HAS_H264
        QSKIP("当前构建不支持 H.264");
#else
        H264Encoder encoder;
        QString error;
        QVERIFY2(
            encoder.open(320, 240, 30, 2'000'000, &error),
            qPrintable(error));

        QByteArray keyframe;
        QByteArray delta;
        for (int index = 0; index < 12; ++index) {
            QImage source(320, 240, QImage::Format_RGB32);
            source.fill(
                QColor(
                    24 + index * 9,
                    64 + index * 5,
                    120 - index * 3));
            H264AccessUnit unit;
            QVERIFY2(
                encoder.encode(
                    source,
                    index == 0,
                    &unit,
                    &error),
                qPrintable(error));
            if (unit.data.isEmpty()) {
                continue;
            }
            if (index == 0) {
                QVERIFY(unit.keyframe);
                keyframe = unit.data;
            } else if (!unit.keyframe && delta.isEmpty()) {
                delta = unit.data;
            }
        }
        QVERIFY(!keyframe.isEmpty());
        QVERIFY(!delta.isEmpty());

        QImage jpegSource(320, 180, QImage::Format_RGB32);
        jpegSource.fill(QColor(32, 96, 160));
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImageWriter writer(&buffer, QByteArrayLiteral("jpeg"));
        QVERIFY(writer.write(jpegSource));
        buffer.close();
        QVERIFY(!jpeg.isEmpty());

        ViewerVideoPipeline pipeline;
        QList<quint32> sequences;
        QList<QImage> frames;
        QList<QString> errors;
        connect(
            &pipeline,
            &ViewerVideoPipeline::frameDecoded,
            [&sequences, &frames](
                const QImage &image,
                quint32 sequence,
                bool) {
                sequences.append(sequence);
                frames.append(image);
            });
        connect(
            &pipeline,
            &ViewerVideoPipeline::decodeFailed,
            [&errors](const QString &message) {
                errors.append(message);
            });

        pipeline.start();
        pipeline.submit(delta, 1, false, VideoCodec::H264);
        pumpEvents(250);
        QVERIFY(frames.isEmpty());

        pipeline.submit(jpeg, 2, true, VideoCodec::Jpeg);
        QVERIFY(waitUntil(
            [&]() { return !frames.isEmpty(); },
            3000));
        QCOMPARE(sequences.constLast(), 2U);
        QCOMPARE(frames.constLast().size(), jpegSource.size());

        pipeline.submit(delta, 3, false, VideoCodec::H264);
        pumpEvents(250);
        QCOMPARE(frames.size(), qsizetype(1));

        pipeline.submit(keyframe, 4, true, VideoCodec::H264);
        for (int index = 0;
             index < 8 && frames.size() < 2;
             ++index) {
            pipeline.submit(
                delta,
                static_cast<quint32>(5 + index),
                false,
                VideoCodec::H264);
            pumpEvents(100);
        }
        QVERIFY(waitUntil(
            [&]() { return frames.size() >= 2; },
            4000));
        QCOMPARE(frames.constLast().size(), QSize(320, 240));
        if (!errors.isEmpty()) {
            QFAIL(qPrintable(errors.constFirst()));
        }
        pipeline.stop();
#endif
    }

    void viewerAudioPipelineDecodesInOrderOffMainThread()
    {
        auto makePacket = [](quint32 sequence, quint8 seed) {
            QByteArray pcm(kAudioPcmBytes, '\0');
            for (qsizetype index = 0; index < pcm.size(); index += 2) {
                const qint16 sample = static_cast<qint16>(
                    (index * 37 + seed * 101) % 12000 - 6000);
                pcm[index] = static_cast<char>(sample & 0xFF);
                pcm[index + 1] =
                    static_cast<char>((sample >> 8) & 0xFF);
            }
            AudioPacket packet;
            packet.sequence = sequence;
            packet.timestampMs = sequence * kAudioFrameMs;
            packet.samples = static_cast<quint16>(kAudioSamples);
            packet.current = ulawEncode(pcm);
            return qMakePair(packAudio(packet), ulawDecode(packet.current));
        };

        ViewerAudioPipeline pipeline;
        QList<quint32> sequences;
        QList<quint32> timestamps;
        QList<QByteArray> frames;
        QList<QString> errors;
        connect(
            &pipeline,
            &ViewerAudioPipeline::audioDecoded,
            [&sequences, &timestamps, &frames](
                const QByteArray &pcm,
                quint32 sequence,
                quint32 timestampMs) {
                sequences.append(sequence);
                timestamps.append(timestampMs);
                frames.append(pcm);
            });
        connect(
            &pipeline,
            &ViewerAudioPipeline::decodeFailed,
            [&errors](const QString &error) { errors.append(error); });

        QList<QPair<QByteArray, QByteArray>> packets;
        for (quint32 index = 0; index < 24; ++index) {
            packets.append(makePacket(100 + index, static_cast<quint8>(index)));
        }

        pipeline.start();
        for (const auto &packet : packets) {
            pipeline.submit(packet.first);
        }
        QVERIFY(waitUntil(
            [&]() { return frames.size() == packets.size(); },
            4000));
        for (qsizetype index = 0; index < packets.size(); ++index) {
            QCOMPARE(sequences.at(index), quint32(100 + index));
            QCOMPARE(
                timestamps.at(index),
                quint32((100 + index) * kAudioFrameMs));
            QCOMPARE(frames.at(index), packets.at(index).second);
        }
        QCOMPARE(
            pipeline.decoderThreadName(),
            QStringLiteral("PeerDeskViewerAudio"));

        pipeline.submit(QByteArrayLiteral("truncated"));
        QVERIFY(waitUntil(
            [&]() { return errors.size() == 1; },
            3000));
        QCOMPARE(errors.constLast(), QStringLiteral("收到损坏的音频包"));

        AudioPacket shortFrame;
        shortFrame.sequence = 999;
        shortFrame.samples = 1;
        shortFrame.current = QByteArray(kAudioSamples, '\0');
        pipeline.submit(packAudio(shortFrame));
        QVERIFY(waitUntil(
            [&]() { return errors.size() == 2; },
            3000));
        QCOMPARE(
            errors.constLast(),
            QStringLiteral("音频包长度校验失败"));

        pipeline.submit(QByteArray{});
        QVERIFY(waitUntil(
            [&]() { return errors.size() == 3; },
            3000));
        QCOMPARE(errors.constLast(), QStringLiteral("收到损坏的音频包"));

        pipeline.stop();
        const qsizetype stoppedCount = frames.size();
        pipeline.submit(makePacket(200, 7).first);
        pumpEvents(150);
        QCOMPARE(frames.size(), stoppedCount);

        pipeline.start();
        pipeline.submit(makePacket(201, 9).first);
        QVERIFY(waitUntil(
            [&]() {
                return !sequences.isEmpty()
                    && sequences.constLast() == 201;
            },
            3000));
        QCOMPARE(frames.constLast().size(), qsizetype(kAudioPcmBytes));
        pipeline.stop();
    }

    void hostInputPipelineSerializesAndCoalescesOffUiThread()
    {
        auto sink = std::make_shared<FakeInputSink>();
        HostInputPipeline pipeline;
        pipeline.setSink(sink);
        pipeline.start();

        QCOMPARE(sink->snapshot().releaseCount, 1);
        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyA"),
            true));
        pipeline.submit(
            HostInputCommand::typeText(QStringLiteral("PeerDesk")));
        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyA"),
            false));
        QVERIFY(waitUntil(
            [&]() {
                const FakeInputSink::Snapshot state = sink->snapshot();
                return state.keys.size() == 2
                    && state.typed == QStringLiteral("PeerDesk");
            },
            3000));

        FakeInputSink::Snapshot state = sink->snapshot();
        QVERIFY(!state.threadNames.isEmpty());
        QVERIFY(std::all_of(
            state.threadNames.cbegin(),
            state.threadNames.cend(),
            [](const QString &name) {
                return name == QStringLiteral("PeerDeskHostInput");
            }));
        const qsizetype keyDown = state.operations.indexOf(
            QStringLiteral("key:KeyA:down"));
        const qsizetype text = state.operations.indexOf(
            QStringLiteral("text:PeerDesk"));
        const qsizetype keyUp = state.operations.indexOf(
            QStringLiteral("key:KeyA:up"));
        QVERIFY(keyDown >= 0);
        QVERIFY(keyDown < text);
        QVERIFY(text < keyUp);

        sink->armKeyGate(QStringLiteral("KeyGate"));
        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyGate"),
            true));
        const bool gated = sink->waitForKeyGate();
        for (int index = 0; index < 200; ++index) {
            const double value = static_cast<double>(index) / 199.0;
            pipeline.submit(HostInputCommand::moveAbsolute(
                value,
                value,
                QRect(0, 0, 2303, 767)));
        }
        sink->openKeyGate();
        QVERIFY(gated);
        QVERIFY(waitUntil(
            [&]() {
                return !sink->snapshot().absoluteMoves.isEmpty();
            },
            3000));
        pumpEvents(150);
        state = sink->snapshot();
        QCOMPARE(state.absoluteMoves.size(), qsizetype(1));
        QCOMPARE(
            state.absoluteMoves.constFirst(),
            QPointF(1.0, 1.0));

        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyGate"),
            false));
        QVERIFY(waitUntil(
            [&]() {
                const QList<QPair<QString, bool>> keys =
                    sink->snapshot().keys;
                return std::find(
                           keys.cbegin(),
                           keys.cend(),
                           qMakePair(QStringLiteral("KeyGate"), false))
                    != keys.cend();
            },
            3000));
        state = sink->snapshot();
        const qsizetype gateDown =
            state.operations.indexOf(QStringLiteral("key:KeyGate:down"));
        const qsizetype move = state.operations.indexOf(
            QStringLiteral("move:1.000000:1.000000"));
        const qsizetype gateUp =
            state.operations.indexOf(QStringLiteral("key:KeyGate:up"));
        QVERIFY(gateDown >= 0);
        QVERIFY(gateDown < move);
        QVERIFY(move < gateUp);

        pipeline.stop();
        const int releases = sink->snapshot().releaseCount;
        QVERIFY(releases >= 2);
        const qsizetype stoppedOperations =
            sink->snapshot().operations.size();
        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyLate"),
            true));
        pumpEvents(150);
        QCOMPARE(
            sink->snapshot().operations.size(),
            stoppedOperations);

        pipeline.start();
        pipeline.submit(HostInputCommand::key(
            QStringLiteral("KeyB"),
            true));
        QVERIFY(waitUntil(
            [&]() {
                const QList<QPair<QString, bool>> keys =
                    sink->snapshot().keys;
                return std::find(
                           keys.cbegin(),
                           keys.cend(),
                           qMakePair(QStringLiteral("KeyB"), true))
                    != keys.cend();
            },
            3000));
        pipeline.stop();
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

    void fileTransferPipelineRunsIoOffUiThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload =
            makePayload(2 * 1024 * 1024, 0x71A5C3);
        const QString path =
            directory.filePath(QStringLiteral("pipeline.bin"));
        QVERIFY(writeBytes(path, payload));

        FileTransferPipeline transfer;
        const QString ioThread = transfer.ioThreadName();
        QCOMPARE(
            ioThread,
            QStringLiteral("PeerDeskFileTransfer"));
        QVERIFY(ioThread != QThread::currentThread()->objectName());

        QSignalSpy started(
            &transfer,
            &FileTransferPipeline::outgoingFileStarted);
        QSignalSpy finished(
            &transfer,
            &FileTransferPipeline::outgoingFileFinished);
        QSignalSpy failed(
            &transfer,
            &FileTransferPipeline::outgoingFileFailed);

        int heartbeatCount = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&heartbeatCount]() {
            ++heartbeatCount;
        });
        heartbeat.start(10);

        QString error;
        QVERIFY2(transfer.startSend(path, &error), qPrintable(error));
        QCOMPARE(started.count(), 1);
        QVERIFY(waitUntil(
            [&]() {
                return finished.count() + failed.count() == 1;
            },
            15000));
        heartbeat.stop();

        QCOMPARE(started.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(heartbeatCount > 0);
        QVERIFY(!transfer.sendActive());
        QCOMPARE(transfer.ioThreadName(), ioThread);
    }

    void fileTransferPipelinePreservesAndDropsResume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("resumable.bin"));
        QVERIFY(writeBytes(path, makePayload(512 * 1024, 0x4A11)));

        FileTransferPipeline transfer;
        QSignalSpy interrupted(
            &transfer,
            &FileTransferPipeline::outgoingFileInterrupted);
        QSignalSpy cancelled(
            &transfer,
            &FileTransferPipeline::outgoingFileCancelled);
        QList<QJsonObject> control;
        connect(
            &transfer,
            &FileTransferPipeline::controlMessage,
            [&control](const QJsonObject &message) {
                control.append(message);
            });

        QString error;
        QVERIFY2(transfer.startSend(path, &error), qPrintable(error));
        QVERIFY(transfer.sendActive());
        QVERIFY2(
            transfer.interruptSend(QStringLiteral("network")),
            "active transfer should be interruptible");
        QVERIFY(!transfer.sendActive());
        QCOMPARE(interrupted.count(), 1);
        QCOMPARE(
            interrupted.constFirst().at(1).toString(),
            QStringLiteral("network"));
        QVERIFY(transfer.hasResumableSend());

        transfer.abortSend();
        QVERIFY(!transfer.hasResumableSend());

        QVERIFY2(transfer.startSend(path, &error), qPrintable(error));
        const QString id = transfer.sendId();
        QVERIFY(!id.isEmpty());
        QVERIFY(transfer.cancelSend());
        QVERIFY(!transfer.cancelSend());
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(cancelled.constFirst().at(0).toString(), id);
        QVERIFY(waitUntil(
            [&]() {
                return countMessages(
                           control,
                           QStringLiteral("file_cancel"),
                           id)
                    == 1;
            },
            1000));
        pumpEvents(80);
        QCOMPARE(
            countMessages(
                control,
                QStringLiteral("file_cancel"),
                id),
            1);
        QVERIFY(!transfer.sendActive());
    }

    void fileTransferPipelineRejectsEmptyReceiveIdOnce()
    {
        FileTransferPipeline transfer;
        QSignalSpy failed(
            &transfer,
            &FileTransferPipeline::receiveFileFailed);
        const QJsonObject message{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("name"), QStringLiteral("invalid.bin")},
            {QStringLiteral("size"), 4},
        };

        transfer.beginReceive(message);
        transfer.beginReceive(message);
        QVERIFY(waitUntil([&]() { return failed.count() == 1; }, 1000));
        pumpEvents(80);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.constFirst().at(0).toString(), QString());
        QCOMPARE(
            failed.constFirst().at(1).toString(),
            QStringLiteral("id"));
    }

    void fileTransferPipelineReceivesOffUiThread()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload =
            makePayload(180000, 0xB17E);
        const QString id = QStringLiteral("pipeline-receive");

        FileTransferPipeline transfer(directory.path());
        QSignalSpy started(
            &transfer,
            &FileTransferPipeline::receiveFileStarted);
        QSignalSpy finished(
            &transfer,
            &FileTransferPipeline::receiveFileFinished);
        QSignalSpy failed(
            &transfer,
            &FileTransferPipeline::receiveFileFailed);

        transfer.beginReceive(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("file_begin")},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), QStringLiteral("received.bin")},
            {QStringLiteral("size"), payload.size()},
        });
        for (qsizetype offset = 0;
             offset < payload.size();
             offset += kFileChunkBytes) {
            transfer.appendReceive(QJsonObject{
                {QStringLiteral("t"), QStringLiteral("file_data")},
                {QStringLiteral("id"), id},
                {QStringLiteral("d"),
                 QString::fromLatin1(
                     payload.mid(offset, kFileChunkBytes).toBase64())},
            });
        }
        transfer.finishReceive(id);

        QCOMPARE(started.count(), 0);
        QCOMPARE(finished.count(), 0);
        QVERIFY(waitUntil(
            [&]() { return finished.count() == 1; },
            5000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.constFirst().at(0).toString(), id);
        QCOMPARE(
            started.constFirst().at(1).toString(),
            QStringLiteral("received.bin"));
        QCOMPARE(
            readBytes(finished.constFirst().at(1).toString()),
            payload);
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

    void fileReceiverResumesWithNewId()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload = makePayload(300000, 0xA11CE);
        const qsizetype split = 110000;

        FileReceiver receiver(directory.path());
        QString error;
        QVERIFY2(
            receiver.begin(
                QStringLiteral("resume-old"),
                QStringLiteral("resume.bin"),
                payload.size(),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.append(
                QStringLiteral("resume-old"),
                payload.left(split),
                &error),
            qPrintable(error));

        qint64 offset = -1;
        bool resumed = false;
        QVERIFY2(
            receiver.begin(
                QStringLiteral("resume-new"),
                QStringLiteral("resume.bin"),
                payload.size(),
                &error,
                QStringLiteral("resume-old"),
                &offset,
                &resumed),
            qPrintable(error));
        QVERIFY(resumed);
        QCOMPARE(offset, qint64(split));
        QVERIFY(!receiver.contains(QStringLiteral("resume-old")));
        QVERIFY(receiver.contains(QStringLiteral("resume-new")));
        QCOMPARE(
            receiver.received(QStringLiteral("resume-new")),
            qint64(split));
        QCOMPARE(
            receiver.expected(QStringLiteral("resume-new")),
            qint64(payload.size()));
        QVERIFY2(
            receiver.append(
                QStringLiteral("resume-new"),
                payload.mid(split),
                &error),
            qPrintable(error));

        QString savedPath;
        qint64 savedBytes = 0;
        QVERIFY2(
            receiver.finish(
                QStringLiteral("resume-new"),
                &savedPath,
                &savedBytes,
                &error),
            qPrintable(error));
        QCOMPARE(savedBytes, qint64(payload.size()));
        QCOMPARE(
            QFileInfo(savedPath).fileName(),
            QStringLiteral("resume.bin"));
        QCOMPARE(readBytes(savedPath), payload);
        QVERIFY(!receiver.contains(QStringLiteral("resume-new")));
    }

    void fileReceiverDisambiguatesSameNameAndSizeResumes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray firstPayload = makePayload(360000, 0xD44F2);
        const QByteArray secondPayload = makePayload(360000, 0xE55A3);
        QCOMPARE(firstPayload.size(), secondPayload.size());
        QVERIFY(firstPayload != secondPayload);
        const qsizetype split = 145000;

        FileReceiver receiver(directory.path());
        QString error;
        QVERIFY2(
            receiver.begin(
                QStringLiteral("same-old-a"),
                QStringLiteral("same.bin"),
                firstPayload.size(),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.append(
                QStringLiteral("same-old-a"),
                firstPayload.left(split),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.begin(
                QStringLiteral("same-old-b"),
                QStringLiteral("same.bin"),
                secondPayload.size(),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.append(
                QStringLiteral("same-old-b"),
                secondPayload.left(split),
                &error),
            qPrintable(error));

        qint64 firstOffset = -1;
        qint64 secondOffset = -1;
        bool firstResumed = false;
        bool secondResumed = false;
        QVERIFY2(
            receiver.begin(
                QStringLiteral("same-new-a"),
                QStringLiteral("same.bin"),
                firstPayload.size(),
                &error,
                QStringLiteral("same-old-a"),
                &firstOffset,
                &firstResumed),
            qPrintable(error));
        QVERIFY2(
            receiver.begin(
                QStringLiteral("same-new-b"),
                QStringLiteral("same.bin"),
                secondPayload.size(),
                &error,
                QStringLiteral("same-old-b"),
                &secondOffset,
                &secondResumed),
            qPrintable(error));
        QVERIFY(firstResumed);
        QVERIFY(secondResumed);
        QCOMPARE(firstOffset, qint64(split));
        QCOMPARE(secondOffset, qint64(split));
        QVERIFY(!receiver.contains(QStringLiteral("same-old-a")));
        QVERIFY(!receiver.contains(QStringLiteral("same-old-b")));
        QVERIFY(receiver.contains(QStringLiteral("same-new-a")));
        QVERIFY(receiver.contains(QStringLiteral("same-new-b")));

        QVERIFY2(
            receiver.append(
                QStringLiteral("same-new-a"),
                firstPayload.mid(split),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.append(
                QStringLiteral("same-new-b"),
                secondPayload.mid(split),
                &error),
            qPrintable(error));

        QString firstPath;
        QString secondPath;
        QVERIFY2(
            receiver.finish(
                QStringLiteral("same-new-a"),
                &firstPath,
                nullptr,
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.finish(
                QStringLiteral("same-new-b"),
                &secondPath,
                nullptr,
                &error),
            qPrintable(error));
        QCOMPARE(
            QFileInfo(firstPath).fileName(),
            QStringLiteral("same.bin"));
        QCOMPARE(
            QFileInfo(secondPath).fileName(),
            QStringLiteral("same (1).bin"));
        QCOMPARE(readBytes(firstPath), firstPayload);
        QCOMPARE(readBytes(secondPath), secondPayload);
    }

    void uniqueFileNameKeepsCollisionSuffixWithinWindowsLimit()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString stem(251, QLatin1Char('a'));
        const QString fileName = stem + QStringLiteral(".txt");
        QCOMPARE(fileName.size(), 255);
        const QString occupied = directory.filePath(fileName);
        const QSet<QString> reserved{occupied};

        const QString alternative =
            uniqueFilePath(directory.path(), fileName, reserved);
        QVERIFY(!alternative.isEmpty());
        QVERIFY(QFileInfo(alternative).absoluteFilePath()
                != QFileInfo(occupied).absoluteFilePath());
        const QString alternativeName =
            QFileInfo(alternative).fileName();
        QVERIFY(alternativeName.size() <= 255);
        QVERIFY(alternativeName.endsWith(QStringLiteral(".txt")));
        QVERIFY(alternativeName.contains(QStringLiteral(" (1)")));
        QVERIFY(alternativeName.startsWith(
            QString(247, QLatin1Char('a'))));

        const QString surrogateName =
            QString(246, QLatin1Char('b')) + QStringLiteral("😀.txt");
        const QString surrogateOccupied =
            directory.filePath(surrogateName);
        const QSet<QString> surrogateReserved{surrogateOccupied};
        const QString surrogateAlternative =
            uniqueFilePath(
                directory.path(),
                surrogateName,
                surrogateReserved);
        QVERIFY(!surrogateAlternative.isEmpty());
        QCOMPARE(
            QFileInfo(surrogateAlternative).fileName(),
            QString(246, QLatin1Char('b'))
                + QStringLiteral(" (1).txt"));
    }

    void fileReceiverRejectsInvalidOrExpiredResume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload = makePayload(180000, 0xB22DF);

        FileReceiver receiver(directory.path());
        QString error;
        QVERIFY2(
            receiver.begin(
                QStringLiteral("held-old"),
                QStringLiteral("held.bin"),
                payload.size(),
                &error),
            qPrintable(error));
        QVERIFY2(
            receiver.append(
                QStringLiteral("held-old"),
                payload.left(70000),
                &error),
            qPrintable(error));

        QVERIFY(!receiver.begin(
            QStringLiteral("wrong-name"),
            QStringLiteral("renamed.bin"),
            payload.size(),
            &error,
            QStringLiteral("held-old")));
        QCOMPARE(error, QStringLiteral("resume"));
        QVERIFY(!receiver.begin(
            QStringLiteral("wrong-size"),
            QStringLiteral("held.bin"),
            payload.size() + 1,
            &error,
            QStringLiteral("held-old")));
        QCOMPARE(error, QStringLiteral("resume"));
        QVERIFY(!receiver.begin(
            QStringLiteral("held-old"),
            QStringLiteral("held.bin"),
            payload.size(),
            &error,
            QStringLiteral("held-old")));
        QCOMPARE(error, QStringLiteral("resume"));
        QVERIFY(!receiver.begin(
            QStringLiteral("unknown"),
            QStringLiteral("held.bin"),
            payload.size(),
            &error,
            QStringLiteral("missing-old")));
        QCOMPARE(error, QStringLiteral("resume"));
        QVERIFY(receiver.contains(QStringLiteral("held-old")));
        QVERIFY(!receiver.contains(QStringLiteral("wrong-name")));
        QVERIFY(!receiver.contains(QStringLiteral("wrong-size")));

        receiver.pruneIdle(0);
        QVERIFY(!receiver.contains(QStringLiteral("held-old")));
        QVERIFY(!QFileInfo::exists(
            directory.filePath(QStringLiteral("held.bin"))));
        QVERIFY(!receiver.begin(
            QStringLiteral("too-late"),
            QStringLiteral("held.bin"),
            payload.size(),
            &error,
            QStringLiteral("held-old")));
        QCOMPARE(error, QStringLiteral("resume"));
    }

    void fileSenderWaitsForResumeAndSendsTail()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload = makePayload(360000, 0xC33E1);
        const QString path =
            directory.filePath(QStringLiteral("resume-source.bin"));
        QVERIFY(writeBytes(path, payload));

        QList<QJsonObject> wire;
        int unconfirmedBytes = 0;
        const auto collect = [&wire, &unconfirmedBytes](
                                 const QJsonObject &message) {
            wire.append(message);
            if (message.value(QStringLiteral("t")).toString()
                == QStringLiteral("file_data")) {
                unconfirmedBytes +=
                    message.value(QStringLiteral("d"))
                        .toString()
                        .size();
            }
        };

        FileSender sender;
        QSignalSpy started(&sender, &FileSender::started);
        QSignalSpy completed(&sender, &FileSender::completed);
        FileSender::ResumeOffer offer{
            QStringLiteral("previous-transfer"),
            path,
            QStringLiteral("resume-source.bin"),
            payload.size(),
            84000,
        };
        QString error;
        QVERIFY2(
            sender.start(path, collect, &error, offer),
            qPrintable(error));
        QCOMPARE(started.count(), 1);
        QCOMPARE(
            started.at(0).at(3).toString(),
            QFileInfo(path).absoluteFilePath());
        QVERIFY(sender.active());
        QVERIFY(sender.waitingForResume());
        QCOMPARE(sender.sent(), qint64(84000));
        QCOMPARE(wire.size(), 1);
        QCOMPARE(
            wire.constFirst().value(QStringLiteral("t")).toString(),
            QStringLiteral("file_begin"));
        QCOMPARE(
            wire.constFirst().value(QStringLiteral("resume")).toBool(),
            true);
        QCOMPARE(
            wire.constFirst()
                .value(QStringLiteral("resume_of"))
                .toString(),
            QStringLiteral("previous-transfer"));
        pumpEvents(40);
        QCOMPARE(unconfirmedBytes, 0);

        const QString id = sender.id();
        QVERIFY(!sender.acceptResume(
            QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("offset"), payload.size() + 1},
            },
            &error));
        QCOMPARE(error, QStringLiteral("resume_offset"));
        QVERIFY(sender.waitingForResume());
        QVERIFY(sender.acceptResume(
            QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("offset"), QString::number(84000)},
            },
            &error));
        QVERIFY(!sender.waitingForResume());

        wire.clear();
        QVERIFY(waitUntil(
            [&]() {
                return completed.count() == 1 && !sender.active();
            },
            10000));
        QByteArray tail;
        for (const QJsonObject &message : wire) {
            if (message.value(QStringLiteral("t")).toString()
                == QStringLiteral("file_data")) {
                tail.append(
                    QByteArray::fromBase64(
                        message.value(QStringLiteral("d"))
                            .toString()
                            .toLatin1()));
            }
        }
        QCOMPARE(tail, payload.mid(84000));
        QCOMPARE(countMessages(wire, QStringLiteral("file_end"), id), 1);
        QCOMPARE(completed.at(0).at(0).toString(), id);
        QCOMPARE(completed.at(0).at(1).toString(), QStringLiteral("resume-source.bin"));
        QCOMPARE(completed.at(0).at(2).toLongLong(), qint64(payload.size()));
        QVERIFY(!sender.acceptResume(
            QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("offset"), 84000},
            },
            &error));
        QCOMPARE(error, QStringLiteral("inactive"));

        FileSender timedOut;
        timedOut.setResumeTimeoutMs(20);
        QSignalSpy failed(&timedOut, &FileSender::failed);
        QList<QJsonObject> timeoutWire;
        QVERIFY2(
            timedOut.start(
                path,
                [&timeoutWire](const QJsonObject &message) {
                    timeoutWire.append(message);
                },
                &error,
                offer),
            qPrintable(error));
        QVERIFY(waitUntil(
            [&]() { return failed.count() == 1; },
            1000));
        QCOMPARE(
            failed.at(0).at(1).toString(),
            QStringLiteral("resume_timeout"));
        QVERIFY(!timedOut.active());
        QCOMPARE(
            countMessages(
                timeoutWire,
                QStringLiteral("file_error"),
                failed.at(0).at(0).toString()),
            1);
        QCOMPARE(
            timeoutWire.constLast()
                .value(QStringLiteral("reason"))
                .toString(),
            QStringLiteral("resume_timeout"));
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

    void hostSessionSuspendsAndResumesFileTransfer()
    {
        const QByteArray psk =
            QByteArrayLiteral("session-resume-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("resume-host"));
        viewerLink.setLabel(QStringLiteral("resume-viewer"));
        hostLink.setDeviceId(QStringLiteral("resume-host-device"));
        viewerLink.setDeviceId(QStringLiteral("resume-viewer-device"));

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

        auto screen =
            std::make_shared<SyntheticScreenSource>(1280, 720, false);
        HostSession host(&hostLink);
        ViewerSession viewer(&viewerLink);
        host.setScreenSource(screen);
        host.setReceiveDirectory(hostReceive.path());
        viewer.setReceiveDirectory(viewerReceive.path());

        QList<QString> viewerFiles;
        QList<QJsonObject> viewerEvents;
        connect(
            &viewer,
            &ViewerSession::fileReceived,
            [&viewerFiles](const QString &path, qint64) {
                viewerFiles.append(path);
            });
        connect(
            &viewer,
            &ViewerSession::eventReceived,
            [&viewerEvents](const QJsonObject &message) {
                viewerEvents.append(message);
            });
        QSignalSpy hostStarted(&host, &HostSession::outgoingFileStarted);
        QSignalSpy hostInterrupted(
            &host,
            &HostSession::outgoingFileInterrupted);

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
        QVERIFY(host.start());
        viewer.start();
        QVERIFY(waitUntil(
            [&]() {
                return host.running() && !viewer.info().isEmpty();
            },
            5000));

        const QByteArray firstPayload =
            makePayload(2 * 1024 * 1024, 0xD44F2);
        const QString firstPath =
            sourceDirectory.filePath(QStringLiteral("host-resume.bin"));
        QVERIFY(writeBytes(firstPath, firstPayload));
        QVERIFY2(host.sendFile(firstPath, &error), qPrintable(error));
        QCOMPARE(hostStarted.count(), 1);
        const QString firstId = hostStarted.at(0).at(0).toString();
        QCOMPARE(
            hostStarted.at(0).at(3).toString(),
            QFileInfo(firstPath).absoluteFilePath());
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [&firstId](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("file_in")
                            && message.value(QStringLiteral("state")).toString()
                                == QStringLiteral("progress")
                            && message.value(QStringLiteral("id")).toString()
                                == firstId
                            && message.value(QStringLiteral("got")).toInt()
                                > 0;
                    });
            },
            5000));

        host.suspend();
        QVERIFY(!host.running());
        QCOMPARE(hostInterrupted.count(), 1);
        QVERIFY(host.start());
        QVERIFY(waitUntil(
            [&]() { return viewerFiles.size() == 1; },
            15000));
        QCOMPARE(readBytes(viewerFiles.constFirst()), firstPayload);
        QVERIFY(hostStarted.count() >= 2);
        const QString resumedId =
            hostStarted.constLast().at(0).toString();
        QVERIFY(resumedId != firstId);
        QCOMPARE(
            hostStarted.constLast().at(3).toString(),
            QFileInfo(firstPath).absoluteFilePath());
        const auto resumedBegin = std::find_if(
            viewerEvents.cbegin(),
            viewerEvents.cend(),
            [&resumedId](const QJsonObject &message) {
                return message.value(QStringLiteral("t")).toString()
                        == QStringLiteral("file_in")
                    && message.value(QStringLiteral("state")).toString()
                        == QStringLiteral("begin")
                    && message.value(QStringLiteral("id")).toString()
                        == resumedId;
            });
        QVERIFY(resumedBegin != viewerEvents.cend());
        QCOMPARE(
            resumedBegin->value(QStringLiteral("resumed")).toBool(),
            true);
        QVERIFY(
            resumedBegin->value(QStringLiteral("offset")).toInt() > 0);

        hostStarted.clear();
        hostInterrupted.clear();
        const QByteArray stoppedPayload =
            makePayload(2 * 1024 * 1024, 0xE55A3);
        const QString stoppedPath =
            sourceDirectory.filePath(QStringLiteral("host-stop-resume.bin"));
        QVERIFY(writeBytes(stoppedPath, stoppedPayload));
        QVERIFY2(host.sendFile(stoppedPath, &error), qPrintable(error));
        QCOMPARE(hostStarted.count(), 1);
        const QString stoppedId = hostStarted.at(0).at(0).toString();
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [&stoppedId](const QJsonObject &message) {
                        return message.value(QStringLiteral("t")).toString()
                                == QStringLiteral("file_in")
                            && message.value(QStringLiteral("state")).toString()
                                == QStringLiteral("progress")
                            && message.value(QStringLiteral("id")).toString()
                                == stoppedId
                            && message.value(QStringLiteral("got")).toInt()
                                > 0;
                    });
            },
            5000));
        host.stop();
        QCOMPARE(hostInterrupted.count(), 1);
        QVERIFY(host.start());
        QVERIFY(waitUntil([&]() { return host.running(); }, 3000));
        pumpEvents(250);
        QCOMPARE(hostStarted.count(), 1);
        QCOMPARE(viewerFiles.size(), 1);
    }

    void viewerReceivesAndResetsRemoteCursor()
    {
        const QByteArray psk =
            QByteArrayLiteral("session-cursor-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("cursor-host"));
        viewerLink.setLabel(QStringLiteral("cursor-viewer"));
        hostLink.setDeviceId(QStringLiteral("cursor-host-device"));
        viewerLink.setDeviceId(QStringLiteral("cursor-viewer-device"));

        QString error;
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));

        auto screen =
            std::make_shared<SyntheticScreenSource>(1280, 720, false);
        CursorSample sample;
        sample.visible = true;
        sample.screenPosition = QPoint(640, 360);
        sample.hotspot = QPoint(7, 9);
        sample.shape = CursorShape::Hand;
        screen->setCursorSample(sample);

        HostSession host(&hostLink);
        ViewerSession viewer(&viewerLink);
        host.setScreenSource(screen);
        QList<CursorPacket> cursors;
        connect(
            &viewer,
            &ViewerSession::remoteCursorChanged,
            [&cursors](const CursorPacket &cursor) {
                cursors.append(cursor);
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
        QVERIFY(host.start());
        QVERIFY(waitUntil(
            [&]() {
                return host.running() && viewer.active()
                    && !cursors.isEmpty();
            },
            5000));
        const CursorPacket visible = cursors.constFirst();
        QVERIFY(visible.visible);
        QCOMPARE(visible.shape, CursorShape::Hand);
        QCOMPARE(visible.x, quint16(32767));
        QCOMPARE(visible.y, quint16(32767));
        QCOMPARE(visible.hotspotX, quint16(358));
        QCOMPARE(visible.hotspotY, quint16(819));

        cursors.clear();
        sample.screenPosition = QPoint(1280, 50);
        screen->setCursorSample(sample);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    cursors.cbegin(),
                    cursors.cend(),
                    [](const CursorPacket &cursor) {
                        return !cursor.visible;
                    });
            },
            3000));

        cursors.clear();
        viewer.stop();
        QCOMPARE(cursors.size(), 1);
        QVERIFY(!cursors.constFirst().visible);

        cursors.clear();
        sample.screenPosition = QPoint(640, 360);
        screen->setCursorSample(sample);
        viewer.start();
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    cursors.cbegin(),
                    cursors.cend(),
                    [](const CursorPacket &cursor) {
                        return cursor.visible;
                    });
            },
            3000));

        viewer.stop();
        host.stop();
    }

    void viewerValidatesCursorImageCacheAndOrdering()
    {
        const QByteArray psk =
            QByteArrayLiteral("session-cursor-image-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("cursor-image-host"));
        viewerLink.setLabel(QStringLiteral("cursor-image-viewer"));
        hostLink.setDeviceId(
            QStringLiteral("cursor-image-host-device"));
        viewerLink.setDeviceId(
            QStringLiteral("cursor-image-viewer-device"));

        QString error;
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));

        ViewerSession viewer(&viewerLink);
        QList<QPair<quint32, QImage>> images;
        QList<CursorPacket> cursors;
        QStringList logs;
        int clearedImages = 0;
        connect(
            &viewer,
            &ViewerSession::remoteCursorImageChanged,
            [&images, &clearedImages](
                quint32 imageId,
                const QImage &image) {
                if (imageId == 0 || image.isNull()) {
                    ++clearedImages;
                    return;
                }
                images.append({imageId, image});
            });
        connect(
            &viewer,
            &ViewerSession::remoteCursorChanged,
            [&cursors](const CursorPacket &cursor) {
                cursors.append(cursor);
            });
        connect(
            &viewer,
            &ViewerSession::logMessage,
            [&logs](const QString &message) {
                logs.append(message);
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

        QList<quint32> imageIds;
        for (int index = 0; index < 9; ++index) {
            const QImage image =
                cursorTestImage(100 + static_cast<quint32>(index));
            const quint32 imageId = cursorImageId(image);
            imageIds.append(imageId);
            hostLink.sendControl(cursorImageMessage(image));
            QVERIFY(waitUntil(
                [&]() {
                    return !viewer.remoteCursorImage(imageId).isNull();
                },
                3000));
        }
        QCOMPARE(viewer.remoteCursorImageCacheSize(), 8);
        QVERIFY(viewer.remoteCursorImage(imageIds.constFirst()).isNull());
        for (qsizetype index = 1; index < imageIds.size(); ++index) {
            QVERIFY(
                !viewer.remoteCursorImage(imageIds.at(index)).isNull());
        }

        const int imageEventsBeforeInvalid = images.size();
        const int logsBeforeInvalid = logs.size();
        QJsonObject invalid =
            cursorImageMessage(cursorTestImage(300));
        invalid.insert(
            QStringLiteral("mime"),
            QStringLiteral("image/jpeg"));
        hostLink.sendControl(invalid);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    logs.cbegin() + logsBeforeInvalid,
                    logs.cend(),
                    [](const QString &message) {
                        return message.contains(
                            QStringLiteral("远程光标图片"));
                    });
            },
            3000));

        const QImage mismatchedImage = cursorTestImage(301);
        QJsonObject mismatched = cursorImageMessage(mismatchedImage);
        const quint32 mismatchedId =
            cursorImageId(mismatchedImage);
        mismatched.insert(
            QStringLiteral("id"),
            static_cast<double>(
                mismatchedId == 0xFFFFFFFFU
                    ? mismatchedId - 1
                    : mismatchedId + 1));
        hostLink.sendControl(mismatched);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    logs.cbegin(),
                    logs.cend(),
                    [](const QString &message) {
                        return message.contains(
                            QStringLiteral("校验失败"));
                    });
            },
            3000));
        QCOMPARE(images.size(), imageEventsBeforeInvalid);
        QCOMPARE(viewer.remoteCursorImageCacheSize(), 8);

        const QImage lateImage = cursorTestImage(401);
        const quint32 lateImageId = cursorImageId(lateImage);
        CursorPacket packetBeforeImage;
        packetBeforeImage.sequence = 9001;
        packetBeforeImage.x = 12345;
        packetBeforeImage.y = 23456;
        packetBeforeImage.visible = true;
        packetBeforeImage.shape = CursorShape::Bitmap;
        packetBeforeImage.imageId = lateImageId;
        hostLink.sendCursor(packCursor(packetBeforeImage));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    cursors.cbegin(),
                    cursors.cend(),
                    [lateImageId](const CursorPacket &cursor) {
                        return cursor.sequence == 9001
                            && cursor.imageId == lateImageId;
                    });
            },
            3000));
        const auto packetBeforeImageIt = std::find_if(
            cursors.cbegin(),
            cursors.cend(),
            [](const CursorPacket &cursor) {
                return cursor.sequence == 9001;
            });
        QVERIFY(packetBeforeImageIt != cursors.cend());
        QVERIFY(packetBeforeImageIt->visible);
        QCOMPARE(packetBeforeImageIt->x, packetBeforeImage.x);
        QCOMPARE(packetBeforeImageIt->y, packetBeforeImage.y);
        QCOMPARE(packetBeforeImageIt->imageId, lateImageId);
        QVERIFY(viewer.remoteCursorImage(lateImageId).isNull());
        hostLink.sendControl(cursorImageMessage(lateImage));
        QVERIFY(waitUntil(
            [&]() {
                return !viewer.remoteCursorImage(lateImageId).isNull();
            },
            3000));

        const QImage earlyImage = cursorTestImage(402);
        const quint32 earlyImageId = cursorImageId(earlyImage);
        hostLink.sendControl(cursorImageMessage(earlyImage));
        QVERIFY(waitUntil(
            [&]() {
                return !viewer.remoteCursorImage(earlyImageId).isNull();
            },
            3000));
        CursorPacket imageBeforePacket;
        imageBeforePacket.sequence = 9002;
        imageBeforePacket.visible = true;
        imageBeforePacket.x = 30000;
        imageBeforePacket.y = 31000;
        imageBeforePacket.shape = CursorShape::Bitmap;
        imageBeforePacket.imageId = earlyImageId;
        hostLink.sendCursor(packCursor(imageBeforePacket));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    cursors.cbegin(),
                    cursors.cend(),
                    [earlyImageId](const CursorPacket &cursor) {
                        return cursor.sequence == 9002
                            && cursor.shape == CursorShape::Bitmap
                            && cursor.imageId == earlyImageId;
                    });
            },
            3000));
        QVERIFY(
            std::any_of(
                images.cbegin(),
                images.cend(),
                [earlyImageId](const QPair<quint32, QImage> &entry) {
                    return entry.first == earlyImageId;
                }));

        clearedImages = 0;
        viewer.stop();
        QVERIFY(clearedImages >= 1);
        QCOMPARE(viewer.remoteCursorImageCacheSize(), 0);

        viewerLink.close();
        hostLink.close();
    }

    void customCursorBitmapTransfersOnceAndResendsAfterHello()
    {
        const QByteArray psk =
            QByteArrayLiteral("session-custom-cursor-secret");
        PeerLink hostLink(true, psk);
        PeerLink viewerLink(false, psk);
        hostLink.setLabel(QStringLiteral("custom-cursor-host"));
        viewerLink.setLabel(QStringLiteral("custom-cursor-viewer"));
        hostLink.setDeviceId(
            QStringLiteral("custom-cursor-host-device"));
        viewerLink.setDeviceId(
            QStringLiteral("custom-cursor-viewer-device"));

        QString error;
        QVERIFY2(
            hostLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));
        QVERIFY2(
            viewerLink.start(QStringLiteral("127.0.0.1"), 0, &error),
            qPrintable(error));

        auto screen =
            std::make_shared<SyntheticScreenSource>(1280, 720, false);
        const QImage firstImage = cursorTestImage(501);
        const QImage secondImage = cursorTestImage(502, 40);
        const quint32 firstImageId = cursorImageId(firstImage);
        const quint32 secondImageId = cursorImageId(secondImage);
        CursorSample sample;
        sample.visible = true;
        sample.screenPosition = QPoint(640, 360);
        sample.hotspot = QPoint(4, 6);
        sample.shape = CursorShape::Bitmap;
        sample.image = firstImage;
        screen->setCursorSample(sample);

        HostSession host(&hostLink);
        ViewerSession viewer(&viewerLink);
        host.setScreenSource(screen);
        QList<QPair<quint32, QImage>> images;
        QList<CursorPacket> cursors;
        QList<QJsonObject> wire;
        connect(
            &viewer,
            &ViewerSession::remoteCursorImageChanged,
            [&images](quint32 imageId, const QImage &image) {
                if (imageId != 0 && !image.isNull()) {
                    images.append({imageId, image});
                }
            });
        connect(
            &viewer,
            &ViewerSession::remoteCursorChanged,
            [&cursors](const CursorPacket &cursor) {
                cursors.append(cursor);
            });
        connect(
            &viewerLink,
            &PeerLink::controlMessageReceived,
            [&wire](const QJsonObject &message) {
                wire.append(message);
            });

        const auto imageMessageCount =
            [&wire](quint32 imageId) {
                return std::count_if(
                    wire.cbegin(),
                    wire.cend(),
                    [imageId](const QJsonObject &message) {
                        return message.value(
                                   QStringLiteral("t")).toString()
                                == QStringLiteral("cursor_image")
                            && static_cast<quint32>(
                                   message.value(
                                       QStringLiteral("id")).toDouble())
                                == imageId;
                    });
            };

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
        QVERIFY(host.start());
        const bool initialCustomCursorReady = waitUntil(
            [&]() {
                return host.running() && viewer.active()
                    && imageMessageCount(firstImageId) >= 1
                    && !viewer.remoteCursorImage(firstImageId).isNull()
                    && std::any_of(
                        cursors.cbegin(),
                        cursors.cend(),
                        [firstImageId](const CursorPacket &cursor) {
                            return cursor.visible
                                && cursor.shape == CursorShape::Bitmap
                                && cursor.imageId == firstImageId;
                        });
            },
            5000);
        if (!initialCustomCursorReady) {
            qWarning().noquote()
                << "custom cursor readiness:"
                << "host=" << host.running()
                << "viewer=" << viewer.active()
                << "wire=" << imageMessageCount(firstImageId)
                << "images="
                << !viewer.remoteCursorImage(firstImageId).isNull()
                << "imageCache="
                << viewer.remoteCursorImageCacheSize()
                << "cursors=" << cursors.size();
        }
        QVERIFY(initialCustomCursorReady);
        const int initialFirstImageMessages =
            imageMessageCount(firstImageId);
        const QImage received =
            viewer.remoteCursorImage(firstImageId);
        QCOMPARE(received.size(), firstImage.size());
        QCOMPARE(received.pixelColor(0, 0).alpha(), 255);
        QCOMPARE(
            received.pixelColor(
                firstImage.width() - 1,
                firstImage.height() - 1)
                .alpha(),
            1);

        pumpEvents(180);
        QCOMPARE(
            imageMessageCount(firstImageId),
            initialFirstImageMessages);
        sample.screenPosition = QPoint(1120, 180);
        screen->setCursorSample(sample);
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    cursors.cbegin(),
                    cursors.cend(),
                    [](const CursorPacket &cursor) {
                        return cursor.visible
                            && cursor.shape == CursorShape::Bitmap
                            && cursor.x > 50000;
                    });
            },
            3000));
        QCOMPARE(
            imageMessageCount(firstImageId),
            initialFirstImageMessages);

        sample.image = secondImage;
        screen->setCursorSample(sample);
        QVERIFY(waitUntil(
            [&]() {
                return imageMessageCount(secondImageId) >= 1
                    && !viewer.remoteCursorImage(secondImageId).isNull()
                    && std::any_of(
                        cursors.cbegin(),
                        cursors.cend(),
                        [secondImageId](const CursorPacket &cursor) {
                            return cursor.visible
                                && cursor.shape == CursorShape::Bitmap
                                && cursor.imageId == secondImageId;
                        });
            },
            5000));
        const int initialSecondImageMessages =
            imageMessageCount(secondImageId);
        pumpEvents(180);
        QCOMPARE(
            imageMessageCount(secondImageId),
            initialSecondImageMessages);

        QVERIFY(viewer.active());
        viewer.stop();
        QCOMPARE(viewer.remoteCursorImageCacheSize(), 0);
        QCOMPARE(
            imageMessageCount(secondImageId),
            initialSecondImageMessages);
        viewer.start();
        QVERIFY(waitUntil(
            [&]() {
                return imageMessageCount(secondImageId)
                        > initialSecondImageMessages
                    && !viewer.remoteCursorImage(secondImageId).isNull();
            },
            5000));

        viewer.stop();
        host.stop();
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
        QList<VideoCodec> encodedCodecs;
        QList<bool> encodedKeyframes;
        QList<QImage> decodedFrames;
        QList<bool> keyframes;
        QList<QByteArray> audioFrames;
        QList<QJsonObject> viewerAudioStates;
        QList<QImage> viewerClipboardImages;
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
            [&encodedFrames, &encodedCodecs, &encodedKeyframes](
                const QByteArray &data,
                quint32,
                bool keyframe,
                VideoCodec codec) {
                encodedFrames.append(data);
                encodedCodecs.append(codec);
                encodedKeyframes.append(keyframe);
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
            &viewer,
            &ViewerSession::clipboardImageReceived,
            [&viewerClipboardImages](const QImage &image) {
                viewerClipboardImages.append(image);
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
#ifdef PD_HAS_H264
        const VideoCodec expectedCodec = VideoCodec::H264;
        const QString expectedCodecName = QStringLiteral("h264");
#else
        const VideoCodec expectedCodec = VideoCodec::Jpeg;
        const QString expectedCodecName = QStringLiteral("jpeg");
#endif
        const bool negotiatedInfo = waitUntil(
            [&]() {
                return viewer.info()
                           .value(QStringLiteral("settings"))
                           .toObject()
                           .value(QStringLiteral("pixel_format"))
                           .toString()
                    == expectedCodecName;
            },
            5000);
        QVERIFY2(
            negotiatedInfo,
            qPrintable(
                QStringLiteral(
                    "viewer_info=%1 host_pixel_format=%2 events=%3")
                    .arg(
                        QString::fromUtf8(
                            QJsonDocument(viewer.info())
                                .toJson(QJsonDocument::Compact)),
                        host.settings().pixelFormat,
                        QString::fromUtf8(
                            QJsonDocument(
                                QJsonArray::fromVariantList(
                                    [&viewerEvents]() {
                                        QVariantList values;
                                        for (const QJsonObject &event :
                                             viewerEvents) {
                                            values.append(event);
                                        }
                                        return values;
                                    }()))
                                .toJson(QJsonDocument::Compact)))));
#ifdef PD_HAS_H264
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin(),
                    viewerEvents.cend(),
                    [](const QJsonObject &message) {
                        return message.value(QStringLiteral("t"))
                                    .toString()
                                == QStringLiteral("media")
                            && message.value(QStringLiteral("codec"))
                                    .toString()
                                == QStringLiteral("h264");
                    });
            },
            5000));
#endif
        const QJsonObject info = viewer.info();
        QCOMPARE(
            info.value(QStringLiteral("monitors")).toArray().size(),
            qsizetype(2));
        QCOMPARE(
            info.value(QStringLiteral("settings"))
                .toObject()
                .value(QStringLiteral("pixel_format"))
                .toString(),
            expectedCodecName);
        QVERIFY(info.value(QStringLiteral("audio")).isObject());

        const qsizetype settingsAckStart = viewerEvents.size();
        viewerLink.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("settings")},
            {QStringLiteral("settings"),
             QJsonObject{
                 {QStringLiteral("pixel_format"),
                  QStringLiteral("raw")},
                 {QStringLiteral("quality"), 63},
             }},
        });
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerEvents.cbegin() + settingsAckStart,
                    viewerEvents.cend(),
                    [&expectedCodecName](
                        const QJsonObject &message) {
                        const QJsonObject settings =
                            message.value(QStringLiteral("settings"))
                                .toObject();
                        return message.value(QStringLiteral("t"))
                                    .toString()
                                == QStringLiteral("settings_ack")
                            && settings.value(QStringLiteral("quality"))
                                    .toInt()
                                == 63
                            && settings
                                       .value(QStringLiteral(
                                           "pixel_format"))
                                       .toString()
                                == expectedCodecName;
                    });
            },
            3000));
        QCOMPARE(
            host.settings().pixelFormat,
            expectedCodecName);

        const qsizetype decodedBeforeCodec = decodedFrames.size();
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    encodedCodecs.cbegin(),
                    encodedCodecs.cend(),
                    [&expectedCodec](VideoCodec codec) {
                        return codec == expectedCodec;
                    });
            },
            6000));
        const auto expectedFrame = std::find(
            encodedCodecs.cbegin(),
            encodedCodecs.cend(),
            expectedCodec);
        QVERIFY(expectedFrame != encodedCodecs.cend());
        const qsizetype expectedFrameIndex =
            std::distance(encodedCodecs.cbegin(), expectedFrame);
#ifdef PD_HAS_H264
        QVERIFY(encodedKeyframes.at(expectedFrameIndex));
        QVERIFY(isH264Keyframe(
            encodedFrames.at(expectedFrameIndex)));
#endif
        QVERIFY(waitUntil(
            [&]() { return decodedFrames.size() > decodedBeforeCodec; },
            6000));

        QVERIFY(waitUntil(
            [&]() { return !decodedFrames.isEmpty(); },
            6000));
        QCOMPARE(
            viewer.videoDecoderThreadName(),
            QStringLiteral("PeerDeskViewerVideo"));
        QCOMPARE(decodedFrames.constFirst().size(), QSize(1280, 720));
        QVERIFY(!encodedFrames.constFirst().isEmpty());
        QVERIFY(waitUntil(
            [&]() { return !audioFrames.isEmpty(); },
            5000));
        QCOMPARE(audioFrames.constFirst().size(), qsizetype(kAudioPcmBytes));
        QCOMPARE(
            viewer.audioDecoderThreadName(),
            QStringLiteral("PeerDeskViewerAudio"));
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
        const QJsonObject initialAdaptiveStats = viewerStats.constLast();
        const int initialAdaptiveQuality =
            initialAdaptiveStats.value(QStringLiteral("aq")).toInt();
        const int initialAdaptiveFps =
            initialAdaptiveStats.value(QStringLiteral("afps")).toInt();
        QVERIFY(initialAdaptiveQuality > 0);
        QVERIFY(initialAdaptiveFps > 0);
        QVERIFY(initialAdaptiveStats.contains(QStringLiteral("nack")));
        QVERIFY(initialAdaptiveStats.contains(QStringLiteral("nack_shards")));
        QVERIFY(initialAdaptiveStats.contains(QStringLiteral("keyreq")));
        QVERIFY(initialAdaptiveStats.contains(QStringLiteral("health")));

        const qsizetype beforeLossStats = viewerStats.size();
        viewerLink.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("keyframe_req")},
            {QStringLiteral("loss"), true},
        });
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerStats.cbegin() + beforeLossStats,
                    viewerStats.cend(),
                    [](const QJsonObject &stats) {
                        return stats.value(QStringLiteral("keyreq")).toInt()
                            >= 1;
                    });
            },
            4000));
        const auto lossStats = std::find_if(
            viewerStats.cbegin() + beforeLossStats,
            viewerStats.cend(),
            [](const QJsonObject &stats) {
                return stats.value(QStringLiteral("keyreq")).toInt() >= 1;
            });
        QVERIFY(lossStats != viewerStats.cend());
        QVERIFY(
            lossStats->value(QStringLiteral("aq")).toInt()
            < initialAdaptiveQuality);
        QVERIFY(
            lossStats->value(QStringLiteral("afps")).toInt()
            < initialAdaptiveFps);
        QCOMPARE(
            lossStats->value(QStringLiteral("health")).toString(),
            QStringLiteral("congested"));

        const int keyframesBefore = countKeyframes(keyframes);
        viewer.requestKeyframe();
        QVERIFY(waitUntil(
            [&]() { return countKeyframes(keyframes) > keyframesBefore; },
            4000));
        const qsizetype beforeManualStats = viewerStats.size();
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerStats.cbegin() + beforeManualStats,
                    viewerStats.cend(),
                    [](const QJsonObject &stats) {
                        return stats.value(QStringLiteral("keyreq")).toInt()
                            == 0;
                    });
            },
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

        const qsizetype movesBefore =
            input->snapshot().absoluteMoves.size();
        viewer.sendMouseMove(0.5, 0.5);
        QVERIFY(waitUntil(
            [&]() {
                return input->snapshot().absoluteMoves.size() > movesBefore;
            },
            3000));
        const FakeInputSink::Snapshot moved = input->snapshot();
        const QPointF mapped = moved.absoluteMoves.constLast();
        QVERIFY(qAbs(mapped.x() - (1792.0 / 2303.0)) < 0.005);
        QVERIFY(qAbs(mapped.y() - (384.0 / 767.0)) < 0.005);
        QVERIFY(
            moved.threadNames.contains(QStringLiteral("PeerDeskHostInput")));

        QVERIFY(viewer.sendKey(QStringLiteral("KeyA"), true));
        QVERIFY(waitUntil(
            [&]() {
                const QList<QPair<QString, bool>> keys =
                    input->snapshot().keys;
                return std::find(
                           keys.cbegin(),
                           keys.cend(),
                           qMakePair(QStringLiteral("KeyA"), true))
                    != keys.cend();
            },
            3000));
        viewer.sendText(QStringLiteral("PeerDesk 输入"));
        QVERIFY(waitUntil(
            [&]() {
                return input->snapshot().typed
                    == QStringLiteral("PeerDesk 输入");
            },
            3000));
        viewer.sendMouseButton(QStringLiteral("click"), 0.25, 0.75, QStringLiteral("right"));
        QVERIFY(waitUntil(
            [&]() {
                const QList<QString> clicks = input->snapshot().clicks;
                return !clicks.isEmpty()
                    && clicks.constLast() == QStringLiteral("right");
            },
            3000));
        viewer.sendMouseWheel(120, false);
        QVERIFY(waitUntil(
            [&]() { return !input->snapshot().wheels.isEmpty(); },
            3000));
        QCOMPARE(
            input->snapshot().wheels.constLast(),
            qMakePair(120, false));
        viewer.sendMouseRelative(7, -4);
        QVERIFY(waitUntil(
            [&]() { return !input->snapshot().relativeMoves.isEmpty(); },
            3000));
        QCOMPARE(
            input->snapshot().relativeMoves.constLast(),
            QPoint(7, -4));
        const FakeInputSink::Snapshot beforeRelativeButtons =
            input->snapshot();
        const qsizetype absoluteBeforeRelativeButtons =
            beforeRelativeButtons.absoluteMoves.size();
        const qsizetype clicksBeforeRelativeButtons =
            beforeRelativeButtons.clicks.size();
        const qsizetype buttonsBeforeRelativeButtons =
            beforeRelativeButtons.buttons.size();
        viewer.sendMouseRelativeButton(QStringLiteral("reldown"));
        viewer.sendMouseRelativeButton(
            QStringLiteral("reldown"),
            QStringLiteral("right"));
        viewer.sendMouseRelativeButton(
            QStringLiteral("relup"),
            QStringLiteral("right"));
        viewer.sendMouseRelativeButton(QStringLiteral("relup"));
        QVERIFY(waitUntil(
            [&]() {
                return input->snapshot().buttons.size()
                    >= buttonsBeforeRelativeButtons + 4;
            },
            3000));
        const FakeInputSink::Snapshot relativeButtons =
            input->snapshot();
        QCOMPARE(
            relativeButtons.buttons.at(buttonsBeforeRelativeButtons),
            qMakePair(QStringLiteral("left"), true));
        QCOMPARE(
            relativeButtons.buttons.at(buttonsBeforeRelativeButtons + 1),
            qMakePair(QStringLiteral("right"), true));
        QCOMPARE(
            relativeButtons.buttons.at(buttonsBeforeRelativeButtons + 2),
            qMakePair(QStringLiteral("right"), false));
        QCOMPARE(
            relativeButtons.buttons.at(buttonsBeforeRelativeButtons + 3),
            qMakePair(QStringLiteral("left"), false));
        QCOMPARE(
            relativeButtons.absoluteMoves.size(),
            absoluteBeforeRelativeButtons);
        QCOMPARE(
            relativeButtons.clicks.size(),
            clicksBeforeRelativeButtons);
        viewer.sendCtrlAltDelete();
        QVERIFY(waitUntil(
            [&]() { return input->snapshot().cadCount == 1; },
            3000));

        const FakeInputSink::Snapshot muted = input->snapshot();
        const int mutedMoves =
            static_cast<int>(muted.absoluteMoves.size());
        const int mutedKeys = static_cast<int>(muted.keys.size());
        const int mutedCad = muted.cadCount;
        const QString mutedTyped = muted.typed;
        host.setAllowInput(false);
        QVERIFY(
            input->snapshot().releaseCount > muted.releaseCount);
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
                const QList<QPair<QString, bool>> keys =
                    input->snapshot().keys;
                return std::find(
                           keys.cbegin(),
                           keys.cend(),
                           qMakePair(QStringLiteral("KeyC"), true))
                    != keys.cend();
            },
            3000));
        const FakeInputSink::Snapshot resumed = input->snapshot();
        QCOMPARE(
            static_cast<int>(resumed.absoluteMoves.size()),
            mutedMoves);
        QCOMPARE(static_cast<int>(resumed.keys.size()), mutedKeys + 1);
        QCOMPARE(resumed.cadCount, mutedCad);
        QCOMPARE(resumed.typed, mutedTyped);
        QVERIFY(std::none_of(
            resumed.keys.cbegin(),
            resumed.keys.cend(),
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

        QImage viewerImage(72, 48, QImage::Format_RGB32);
        viewerImage.fill(QColor(QStringLiteral("#b34a20")));
        viewerImage.setPixelColor(
            4,
            4,
            QColor(QStringLiteral("#20c080")));
        QVERIFY(viewer.sendClipboardImage(viewerImage));
        QVERIFY(waitUntil(
            [&]() {
                return clipboard->imageValue.size()
                    == viewerImage.size();
            },
            4000));
        QCOMPARE(
            clipboard->imageValue.pixelColor(4, 4),
            viewerImage.pixelColor(4, 4));
        QVERIFY(clipboard->value.isEmpty());
        QVERIFY(clipboard->imageWrites > 0);

        const int hostPollImageEvents =
            static_cast<int>(viewerClipboardImages.size());
        pumpEvents(900);
        QCOMPARE(
            static_cast<int>(viewerClipboardImages.size()),
            hostPollImageEvents);

        QImage hostImage(80, 50, QImage::Format_RGB32);
        hostImage.fill(QColor(QStringLiteral("#2c5f8a")));
        hostImage.setPixelColor(
            10,
            12,
            QColor(QStringLiteral("#f0d040")));
        QVERIFY(clipboard->setImage(hostImage));
        QVERIFY(waitUntil(
            [&]() {
                return std::any_of(
                    viewerClipboardImages.cbegin(),
                    viewerClipboardImages.cend(),
                    [&hostImage](const QImage &received) {
                        return received.size() == hostImage.size()
                            && received.pixelColor(10, 12)
                                == hostImage.pixelColor(10, 12);
                    });
            },
            4000));
        const int imageEventsAfterHostUpdate =
            static_cast<int>(viewerClipboardImages.size());
        pumpEvents(900);
        QCOMPARE(
            static_cast<int>(viewerClipboardImages.size()),
            imageEventsAfterHostUpdate);

        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("clip")},
            {QStringLiteral("kind"), QStringLiteral("image")},
            {QStringLiteral("mime"), QStringLiteral("image/png")},
            {QStringLiteral("w"), 80},
            {QStringLiteral("h"), 50},
            {QStringLiteral("d"), QStringLiteral("AAAA")},
        }));
        pumpEvents(500);
        QCOMPARE(clipboard->imageValue.size(), hostImage.size());
        QCOMPARE(
            clipboard->imageValue.pixelColor(10, 12),
            hostImage.pixelColor(10, 12));

        QString imageError;
        const QByteArray hostPng =
            encodeClipboardImage(hostImage, &imageError);
        QVERIFY2(hostPng.isEmpty() == false, qPrintable(imageError));
        QVERIFY(viewer.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("clip")},
            {QStringLiteral("kind"), QStringLiteral("image")},
            {QStringLiteral("mime"), QStringLiteral("image/png")},
            {QStringLiteral("w"), hostImage.width() + 1},
            {QStringLiteral("h"), hostImage.height()},
            {QStringLiteral("d"),
             QString::fromLatin1(hostPng.toBase64())},
        }));
        pumpEvents(500);
        QCOMPARE(clipboard->imageValue.size(), hostImage.size());
        QCOMPARE(
            clipboard->imageValue.pixelColor(10, 12),
            hostImage.pixelColor(10, 12));

        const int imageEventsBeforeMismatch =
            static_cast<int>(viewerClipboardImages.size());
        hostLink.sendControl(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("clip")},
            {QStringLiteral("kind"), QStringLiteral("image")},
            {QStringLiteral("mime"), QStringLiteral("image/png")},
            {QStringLiteral("w"), hostImage.width() + 1},
            {QStringLiteral("h"), hostImage.height()},
            {QStringLiteral("d"),
             QString::fromLatin1(hostPng.toBase64())},
        });
        pumpEvents(500);
        QCOMPARE(
            static_cast<int>(viewerClipboardImages.size()),
            imageEventsBeforeMismatch);

        QImage largeImage(1000, 900, QImage::Format_RGB32);
        quint32 randomState = 0x51A7C3E9u;
        for (int y = 0; y < largeImage.height(); ++y) {
            auto *line =
                reinterpret_cast<QRgb *>(largeImage.scanLine(y));
            for (int x = 0; x < largeImage.width(); ++x) {
                randomState =
                    randomState * 1664525u + 1013904223u;
                line[x] = qRgb(
                    static_cast<int>(randomState & 0xFF),
                    static_cast<int>((randomState >> 8) & 0xFF),
                    static_cast<int>((randomState >> 16) & 0xFF));
            }
        }
        const QByteArray largePng =
            encodeClipboardImage(largeImage, &imageError);
        QVERIFY2(largePng.isEmpty() == false, qPrintable(imageError));
        QVERIFY(largePng.size() > 2 * 1024 * 1024);
        QVERIFY(largePng.size() <= kClipboardPngMaxBytes);
        QVERIFY(viewer.sendClipboardImage(largeImage));
        QVERIFY(waitUntil(
            [&]() {
                return clipboard->imageValue.size()
                        == largeImage.size()
                    && clipboard->imageValue.pixelColor(137, 211)
                        == largeImage.pixelColor(137, 211)
                    && clipboard->imageValue.pixelColor(902, 733)
                        == largeImage.pixelColor(902, 733);
            },
            15000));

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
        const auto agentRequest =
            agent.take(QStringLiteral("viewer_request"));
        const auto viewerWaiting =
            viewer.take(QStringLiteral("waiting"));
        QVERIFY(agentRequest.has_value());
        QVERIFY(viewerWaiting.has_value());
        agent.send(QJsonObject{
            {QStringLiteral("t"), QStringLiteral("approve")},
            {QStringLiteral("request_id"),
             agentRequest->value(QStringLiteral("request_id"))},
            {QStringLiteral("access"), QStringLiteral("control")},
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
