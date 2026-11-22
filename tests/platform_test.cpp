#include "platform/pcm_resampler.h"
#include "platform/windows_clipboard.h"
#include "platform/windows_input.h"
#include "platform/windows_screen_source.h"

#include <QRandomGenerator>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>

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
};

QTEST_MAIN(PlatformTest)

#include "platform_test.moc"
