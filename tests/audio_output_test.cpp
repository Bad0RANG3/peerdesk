#include "app/audio_output.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QThread>
#include <QtTest>

#include <functional>
#include <memory>
#include <mutex>

namespace {

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

class FakeAudioController final {
public:
    void recordFactoryThread(QThread *thread)
    {
        std::lock_guard<std::mutex> locker(mutex_);
        factoryThread_ = thread;
    }

    void recordStart(const QAudioFormat &format, QThread *thread)
    {
        std::lock_guard<std::mutex> locker(mutex_);
        ++starts_;
        startThread_ = thread;
        startThreadName_ = thread->objectName();
        sampleRate_ = format.sampleRate();
        channelCount_ = format.channelCount();
        sampleFormat_ = format.sampleFormat();
        if (!startError_.isEmpty()) {
            startError_ = failureText_;
        }
    }

    [[nodiscard]] bool shouldFailStart() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return !startError_.isEmpty();
    }

    [[nodiscard]] QString startError() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return startError_;
    }

    [[nodiscard]] qint64 bytesFree() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        ++freeChecks_;
        return freeBytes_;
    }

    qint64 append(const QByteArray &data)
    {
        std::lock_guard<std::mutex> locker(mutex_);
        written_.append(data);
        return data.size();
    }

    void recordStop()
    {
        std::lock_guard<std::mutex> locker(mutex_);
        ++stops_;
    }

    void setFreeBytes(qint64 bytes)
    {
        std::lock_guard<std::mutex> locker(mutex_);
        freeBytes_ = bytes;
    }

    void failNextStart(const QString &message)
    {
        std::lock_guard<std::mutex> locker(mutex_);
        failureText_ = message;
        startError_ = message;
    }

    void allowStart()
    {
        std::lock_guard<std::mutex> locker(mutex_);
        startError_.clear();
    }

    [[nodiscard]] QByteArray written() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return written_;
    }

    [[nodiscard]] QThread *factoryThread() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return factoryThread_;
    }

    [[nodiscard]] QThread *startThread() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return startThread_;
    }

    [[nodiscard]] QString startThreadName() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return startThreadName_;
    }

    [[nodiscard]] int starts() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return starts_;
    }

    [[nodiscard]] int stops() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return stops_;
    }

    [[nodiscard]] int freeChecks() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return freeChecks_;
    }

    [[nodiscard]] int sampleRate() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return sampleRate_;
    }

    [[nodiscard]] int channelCount() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return channelCount_;
    }

    [[nodiscard]] QAudioFormat::SampleFormat sampleFormat() const
    {
        std::lock_guard<std::mutex> locker(mutex_);
        return sampleFormat_;
    }

private:
    mutable std::mutex mutex_;
    QByteArray written_;
    QString failureText_;
    QString startError_;
    QString startThreadName_;
    QThread *factoryThread_ = nullptr;
    QThread *startThread_ = nullptr;
    qint64 freeBytes_ = 1024 * 1024;
    int starts_ = 0;
    int stops_ = 0;
    mutable int freeChecks_ = 0;
    int sampleRate_ = 0;
    int channelCount_ = 0;
    QAudioFormat::SampleFormat sampleFormat_ =
        QAudioFormat::Unknown;
};

class FakeAudioSink final : public pd::AudioSinkBackend {
public:
    explicit FakeAudioSink(std::shared_ptr<FakeAudioController> controller)
        : controller_(std::move(controller))
    {
    }

    ~FakeAudioSink() override
    {
        stop();
    }

    bool start(const QAudioFormat &format, QString *error) override
    {
        controller_->recordStart(format, QThread::currentThread());
        if (controller_->shouldFailStart()) {
            if (error) {
                *error = controller_->startError();
            }
            return false;
        }
        stopped_ = false;
        if (error) {
            error->clear();
        }
        return true;
    }

    [[nodiscard]] bool hasStopped() const override
    {
        return stopped_;
    }

    [[nodiscard]] qint64 bytesFree() const override
    {
        return controller_->bytesFree();
    }

    qint64 write(const char *data, qint64 size) override
    {
        return controller_->append(QByteArray(data, static_cast<int>(size)));
    }

    void stop() override
    {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        controller_->recordStop();
    }

private:
    std::shared_ptr<FakeAudioController> controller_;
    bool stopped_ = false;
};

pd::AudioSinkFactory fakeFactory(
    const std::shared_ptr<FakeAudioController> &controller)
{
    return [controller] {
        controller->recordFactoryThread(QThread::currentThread());
        return std::make_unique<FakeAudioSink>(controller);
    };
}

} // namespace

class AudioOutputTest final : public QObject {
    Q_OBJECT

private slots:
    void workerOwnsPlaybackAndPreservesFifo();
    void partialWriteContinuesOnPumpTimer();
    void muteStopsPlaybackAndRestartsSink();
    void resetDropsPendingAudioAndAcceptsFreshAudio();
    void startFailureCanBeRetriedAfterReset();
    void destructorStopsPendingPlayback();
};

void AudioOutputTest::workerOwnsPlaybackAndPreservesFifo()
{
    auto controller = std::make_shared<FakeAudioController>();
    pd::PcmAudioPlayer player(fakeFactory(controller));
    const QByteArray first(256, 'a');
    const QByteArray second(128, 'b');

    QVERIFY(player.enabled());
    player.pushPcm16(first);
    player.pushPcm16(second);

    QVERIFY(waitUntil([&] {
        return controller->written() == first + second;
    }));
    QCOMPARE(controller->starts(), 1);
    QVERIFY(controller->factoryThread() != QThread::currentThread());
    QVERIFY(controller->startThread() != QThread::currentThread());
    QCOMPARE(
        controller->startThreadName(),
        QStringLiteral("PeerDeskAudio"));
    QCOMPARE(controller->sampleRate(), 16000);
    QCOMPARE(controller->channelCount(), 1);
    QCOMPARE(
        controller->sampleFormat(),
        QAudioFormat::Int16);
    QVERIFY(player.errorString().isEmpty());
}

void AudioOutputTest::partialWriteContinuesOnPumpTimer()
{
    auto controller = std::make_shared<FakeAudioController>();
    pd::PcmAudioPlayer player(fakeFactory(controller));
    const QByteArray first(64, 'a');
    const QByteArray second(192, 'b');

    player.pushPcm16(first);
    QVERIFY(waitUntil([&] {
        return controller->written() == first;
    }));

    controller->setFreeBytes(0);
    player.pushPcm16(second);
    QTest::qWait(30);
    QCOMPARE(controller->written(), first);
    controller->setFreeBytes(1024);
    QVERIFY(waitUntil([&] {
        return controller->written() == first + second;
    }));
}

void AudioOutputTest::muteStopsPlaybackAndRestartsSink()
{
    auto controller = std::make_shared<FakeAudioController>();
    pd::PcmAudioPlayer player(fakeFactory(controller));
    const QByteArray before(160, 'x');
    const QByteArray muted(160, 'm');
    const QByteArray after(160, 'y');

    player.pushPcm16(before);
    QVERIFY(waitUntil([&] {
        return controller->written() == before;
    }));

    player.setEnabled(false);
    QVERIFY(!player.enabled());
    QCOMPARE(controller->starts(), 1);
    QVERIFY(controller->stops() >= 1);
    player.pushPcm16(muted);
    QTest::qWait(40);
    QCOMPARE(controller->written(), before);

    player.setEnabled(true);
    QVERIFY(player.enabled());
    player.pushPcm16(after);
    QVERIFY(waitUntil([&] {
        return controller->written() == before + after;
    }));
    QCOMPARE(controller->starts(), 2);
    QVERIFY(player.errorString().isEmpty());
}

void AudioOutputTest::resetDropsPendingAudioAndAcceptsFreshAudio()
{
    auto controller = std::make_shared<FakeAudioController>();
    controller->setFreeBytes(0);
    pd::PcmAudioPlayer player(fakeFactory(controller));
    const QByteArray stale(640, 's');
    const QByteArray fresh(320, 'f');

    player.pushPcm16(stale);
    QVERIFY(waitUntil([&] {
        return controller->freeChecks() >= 1;
    }));
    QCOMPARE(controller->written(), QByteArray());

    player.reset();
    QVERIFY(player.errorString().isEmpty());
    controller->setFreeBytes(1024 * 1024);
    player.pushPcm16(fresh);
    QVERIFY(waitUntil([&] {
        return controller->written() == fresh;
    }));
    QCOMPARE(controller->starts(), 2);
}

void AudioOutputTest::startFailureCanBeRetriedAfterReset()
{
    auto controller = std::make_shared<FakeAudioController>();
    controller->failNextStart(QStringLiteral("模拟音频设备不可用"));
    pd::PcmAudioPlayer player(fakeFactory(controller));
    const QByteArray audio(320, 'e');

    player.pushPcm16(audio);
    QVERIFY(waitUntil([&] {
        return !player.errorString().isEmpty();
    }));
    QCOMPARE(
        player.errorString(),
        QStringLiteral("模拟音频设备不可用"));
    QCOMPARE(controller->starts(), 1);

    player.pushPcm16(audio);
    QTest::qWait(40);
    QCOMPARE(controller->starts(), 1);

    controller->allowStart();
    player.reset();
    QVERIFY(player.errorString().isEmpty());
    player.pushPcm16(audio);
    QVERIFY(waitUntil([&] {
        return controller->written() == audio;
    }));
    QCOMPARE(controller->starts(), 2);
    QVERIFY(player.errorString().isEmpty());
}

void AudioOutputTest::destructorStopsPendingPlayback()
{
    auto controller = std::make_shared<FakeAudioController>();
    controller->setFreeBytes(0);

    {
        pd::PcmAudioPlayer player(fakeFactory(controller));
        for (int index = 0; index < 20; ++index) {
            player.pushPcm16(QByteArray(320, 'p'));
        }
        QVERIFY(waitUntil([&] {
            return controller->freeChecks() >= 1;
        }));
    }

    QVERIFY(controller->starts() >= 1);
    QVERIFY(controller->stops() >= 1);
    QCOMPARE(controller->written(), QByteArray());
}

QTEST_GUILESS_MAIN(AudioOutputTest)

#include "audio_output_test.moc"
