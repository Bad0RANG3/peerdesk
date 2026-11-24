#include "platform/pcm_resampler.h"
#include "platform/windows_clipboard.h"
#include "platform/windows_input.h"
#include "platform/windows_screen_source.h"
#include "platform/windows_startup_registration.h"

#include <QDir>
#include <QImage>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSettings>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using namespace pd;

namespace {

QByteArray floatStereo(
    int frames,
    float left,
    float right)
{
    QByteArray data(frames * 2 * static_cast<int>(sizeof(float)), '\0');
    auto *samples = reinterpret_cast<float *>(data.data());
    for (int frame = 0; frame < frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return data;
}

QByteArray int16Stereo(
    int frames,
    qint16 left,
    qint16 right)
{
    QByteArray data(frames * 2 * static_cast<int>(sizeof(qint16)), '\0');
    auto *samples = reinterpret_cast<qint16 *>(data.data());
    for (int frame = 0; frame < frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return data;
}

QList<qint16> decodePcm16(const QByteArray &pcm)
{
    QList<qint16> samples;
    samples.reserve(pcm.size() / 2);
    const char *data = pcm.constData();
    for (qsizetype offset = 0; offset + 1 < pcm.size(); offset += 2) {
        qint16 value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        samples.append(value);
    }
    return samples;
}

class TemporaryRegistryKey {
public:
    explicit TemporaryRegistryKey(QString parent)
        : parent_(std::move(parent))
        , name_(QStringLiteral("startup-test-%1").arg(
              QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        QSettings settings(parent_, QSettings::NativeFormat);
        settings.remove(name_);
        settings.sync();
    }

    ~TemporaryRegistryKey()
    {
        QSettings settings(parent_, QSettings::NativeFormat);
        settings.remove(name_);
        settings.sync();
    }

    [[nodiscard]] QString path() const
    {
        return parent_ + QLatin1Char('\\') + name_;
    }

private:
    QString parent_;
    QString name_;
};

} // namespace

class PlatformTest : public QObject {
    Q_OBJECT

private slots:
    void floatStereoDownsamplesToMonoPcm16()
    {
        PcmMonoResampler resampler(
            48000,
            2,
            PcmMonoResampler::SampleFormat::Float32);
        const QByteArray output =
            resampler.feed(floatStereo(4800, 0.5F, 0.25F).constData(), 4800 * 8);
        QCOMPARE(output.size(), 1600 * 2);

        const QList<qint16> samples = decodePcm16(output);
        QCOMPARE(samples.size(), 1600);
        for (const qint16 sample : samples) {
            QVERIFY(std::abs(sample - 12288) <= 2);
        }
    }

    void int16StereoDownsamplesToMonoPcm16()
    {
        PcmMonoResampler resampler(
            48000,
            2,
            PcmMonoResampler::SampleFormat::Int16);
        const QByteArray input = int16Stereo(4800, 1000, 3000);
        const QByteArray output = resampler.feed(input.constData(), input.size());
        QCOMPARE(output.size(), 1600 * 2);

        const QList<qint16> samples = decodePcm16(output);
        QCOMPARE(samples.size(), 1600);
        for (const qint16 sample : samples) {
            QVERIFY(std::abs(sample - 2000) <= 1);
        }
    }

    void chunkedResamplingHasStableRateAndContinuity()
    {
        PcmMonoResampler resampler(
            44100,
            2,
            PcmMonoResampler::SampleFormat::Int16);
        constexpr int kTotalFrames = 44100;
        const QByteArray source =
            int16Stereo(kTotalFrames, 12000, 6000);

        QByteArray output;
        constexpr qsizetype kChunkBytes = 666 * 2 * 2;
        for (qsizetype offset = 0; offset < source.size();
             offset += kChunkBytes) {
            const qsizetype count =
                std::min(kChunkBytes, source.size() - offset);
            output.append(resampler.feed(source.constData() + offset, count));
        }

        const QList<qint16> samples = decodePcm16(output);
        QVERIFY(std::abs(samples.size() - 16000) <= 2);
        for (const qint16 sample : samples) {
            QVERIFY(std::abs(sample - 9000) <= 1);
        }
    }

    void floatSamplesAreClamped()
    {
        PcmMonoResampler resampler(
            16000,
            1,
            PcmMonoResampler::SampleFormat::Float32);
        QByteArray input(8 * static_cast<int>(sizeof(float)), '\0');
        auto *inputSamples = reinterpret_cast<float *>(input.data());
        for (int index = 0; index < 8; ++index) {
            inputSamples[index] = 2.0F;
        }
        const QByteArray output = resampler.feed(input.constData(), input.size());
        const QList<qint16> samples = decodePcm16(output);
        QVERIFY(!samples.isEmpty());
        for (const qint16 sample : samples) {
            QCOMPARE(sample, static_cast<qint16>(32767));
        }
    }

    void javascriptKeyCodesMapToWindowsKeys()
    {
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("KeyA")), 0x41);
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("Digit0")), 0x30);
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("F12")), 0x7B);
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("Enter")), 0x0D);
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("ShiftLeft")), 0xA0);
        QCOMPARE(windowsVirtualKeyForCode(QStringLiteral("ArrowLeft")), 0x25);
        QVERIFY(!windowsVirtualKeyForCode(QStringLiteral("BogusKey")).has_value());

        QVERIFY(windowsIsExtendedKey(0x25));
        QVERIFY(windowsIsExtendedKey(0x6F));
        QVERIFY(!windowsIsExtendedKey(0x41));
    }

    void numpadEnterUsesExtendedScanCode()
    {
        const auto enter =
            windowsKeyStrokeForCode(QStringLiteral("NumpadEnter"));
        const auto mainEnter =
            windowsKeyStrokeForCode(QStringLiteral("Enter"));
        QVERIFY(enter.has_value());
        QVERIFY(mainEnter.has_value());
        QCOMPARE(enter->virtualKey, quint16(0x0D));
        QVERIFY(enter->extended);
        QVERIFY(!mainEnter->extended);

        const auto numpadDivide =
            windowsKeyStrokeForCode(QStringLiteral("NumpadDivide"));
        QVERIFY(numpadDivide.has_value());
        QVERIFY(numpadDivide->extended);
    }

    void virtualDesktopIsUsable()
    {
        WindowsInputSink input;
        const QRect desktop = input.virtualDesktop();
        QVERIFY(desktop.width() > 0);
        QVERIFY(desktop.height() > 0);
    }

    void startupCommandQuotesAbsoluteExecutable()
    {
        const QString executable = QDir::toNativeSeparators(
            QStringLiteral("C:/Program Files/PeerDesk/peerdesk-client.exe"));
        QCOMPARE(
            WindowsStartupRegistration::commandForExecutable(executable),
            QStringLiteral("\"%1\" --background").arg(executable));

        const QString relative = QStringLiteral("peerdesk-client.exe");
        const QString absolute = QDir::toNativeSeparators(
            QDir::current().absoluteFilePath(relative));
        QCOMPARE(
            WindowsStartupRegistration::commandForExecutable(relative),
            QStringLiteral("\"%1\" --background").arg(absolute));

        QVERIFY(WindowsStartupRegistration::commandForExecutable(
                    QString()).isEmpty());
        QVERIFY(WindowsStartupRegistration::commandForExecutable(
                    QStringLiteral("C:\\bad\"path\\peerdesk-client.exe"))
                    .isEmpty());
    }

    void windowsStartupRegistrationRoundTrip()
    {
        TemporaryRegistryKey key(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\PeerDesk\\Tests"));
        const QString executable = QStringLiteral(
            "C:\\Program Files\\PeerDesk\\peerdesk-client.exe");
        const QString valueName = QStringLiteral("PeerDeskStartupTest");
        WindowsStartupRegistration registration(
            executable,
            key.path(),
            valueName);

        QVERIFY(registration.isSupported());
        QVERIFY(!registration.isEnabled());
        QVERIFY(registration.registeredCommand().isEmpty());

        QString error = QStringLiteral("stale");
        QVERIFY2(registration.setEnabled(true, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(registration.isEnabled());
        QCOMPARE(
            registration.registeredCommand(),
            WindowsStartupRegistration::commandForExecutable(executable));

        QVERIFY2(registration.setEnabled(false, &error), qPrintable(error));
        QVERIFY(!registration.isEnabled());
        QVERIFY(registration.registeredCommand().isEmpty());

        QSettings direct(key.path(), QSettings::NativeFormat);
        QVERIFY(!direct.contains(valueName));
    }

    void clipboardRoundTrip()
    {
        WindowsClipboard clipboard;
        const QString original = clipboard.text();
        const QString marker =
            QStringLiteral("PeerDesk platform test %1").arg(
                QRandomGenerator::global()->generate64());
        QVERIFY(clipboard.setText(marker));
        QCOMPARE(clipboard.text(), marker);
        QVERIFY(clipboard.setText(original));
    }

    void clipboardImageRoundTrip()
    {
        WindowsClipboard clipboard;
        const QImage originalImage = clipboard.image();
        const QString originalText = clipboard.text();
        const auto restoreClipboard = qScopeGuard([&]() {
            if (!originalImage.isNull()) {
                clipboard.setImage(originalImage);
            } else {
                clipboard.setText(originalText);
            }
        });

        QImage image(83, 47, QImage::Format_ARGB32);
        image.fill(QColor(QStringLiteral("#18324f")));
        for (int y = 5; y < 32; ++y) {
            for (int x = 7; x < 61; ++x) {
                image.setPixelColor(
                    x,
                    y,
                    QColor(40 + x, 90 + y, 150, 255));
            }
        }

        QVERIFY(clipboard.setImage(image));
        const QImage stored = clipboard.image();
        QVERIFY(!stored.isNull());
        QCOMPARE(stored.size(), image.size());
        QCOMPARE(
            stored.convertToFormat(QImage::Format_RGB32),
            image.convertToFormat(QImage::Format_RGB32));
        QVERIFY(clipboard.text().isEmpty());
    }

    void screenSourceCapturesScaledFrame()
    {
        WindowsScreenSource source;
        QCOMPARE(source.backend(), QStringLiteral("gdi"));
        const QList<MonitorInfo> monitors = source.monitors();
        QVERIFY(!monitors.isEmpty());
        for (const MonitorInfo &monitor : monitors) {
            QVERIFY(monitor.width > 0);
            QVERIFY(monitor.height > 0);
        }

        CaptureRequest request;
        request.monitor = 0;
        request.maxWidth = 320;
        const CaptureResult captured = source.capture(request);
        QVERIFY2(captured.ok(), qPrintable(captured.error));
        QVERIFY(captured.image.width() > 0);
        QVERIFY(captured.image.width() <= 320);
        QVERIFY(captured.image.height() > 0);
    }

    void screenSourceSamplesCursorWithoutCrashing()
    {
        WindowsScreenSource source;
        const CursorSample sample = source.cursorSample();
        if (sample.visible) {
            QVERIFY(
                static_cast<quint8>(sample.shape)
                <= static_cast<quint8>(CursorShape::NotAllowed));
            QVERIFY(source.cursorSample().screenPosition.x() >= -32768);
        }
    }
};

QTEST_MAIN(PlatformTest)

#include "platform_test.moc"
