#include "codec/media_foundation_h264.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QtTest>

using namespace pd;

namespace {

QImage testFrame(int index)
{
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(QColor(24, 48, 96));
    QPainter painter(&image);
    painter.fillRect(
        QRect(20 + index * 8, 40, 96, 72),
        QColor(64, 210, 180));
    painter.fillRect(
        QRect(160, 100 + (index % 4) * 6, 120, 80),
        QColor(220, 96, 64));
    painter.end();
    return image;
}

} // namespace

class CodecTest final : public QObject {
    Q_OBJECT

private slots:
    void roundTrip();
    void keyframeRefresh();
    void invalidInput();
};

void CodecTest::roundTrip()
{
    H264Encoder encoder;
    QString error;
    QVERIFY2(
        encoder.open(320, 240, 30, 2'000'000, &error),
        qPrintable(error));
    QVERIFY(encoder.isOpen());
    QCOMPARE(encoder.backend(), QStringLiteral("media-foundation-h264"));

    H264Decoder decoder;
    QVERIFY2(decoder.open(&error), qPrintable(error));
    QVERIFY(decoder.isOpen());

    QImage decoded;
    for (int index = 0; index < 8; ++index) {
        H264AccessUnit unit;
        QVERIFY2(
            encoder.encode(
                testFrame(index),
                index == 0,
                &unit,
                &error),
            qPrintable(error));
        QVERIFY(!unit.data.isEmpty());
        if (index == 0) {
            QVERIFY(unit.keyframe);
        }

        bool keyframe = false;
        QImage candidate;
        QVERIFY2(
            decoder.decode(
                unit.data,
                static_cast<quint32>(index),
                30,
                &candidate,
                &keyframe,
                &error),
            qPrintable(error));
        if (!candidate.isNull()) {
            decoded = std::move(candidate);
        }
    }

    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.size(), QSize(320, 240));
    const QColor sample(decoded.pixel(120, 70));
    QVERIFY(std::abs(sample.green() - 210) < 30);
    QVERIFY(std::abs(sample.blue() - 180) < 30);
}

void CodecTest::keyframeRefresh()
{
    H264Encoder encoder;
    QString error;
    QVERIFY2(
        encoder.open(320, 240, 30, 2'000'000, &error),
        qPrintable(error));

    int keyframes = 0;
    for (int index = 0; index < 10; ++index) {
        H264AccessUnit unit;
        QVERIFY2(
            encoder.encode(
                testFrame(index),
                index == 0 || index == 6,
                &unit,
                &error),
            qPrintable(error));
        QVERIFY(!unit.data.isEmpty());
        QCOMPARE(unit.keyframe, isH264Keyframe(unit.data));
        if (unit.keyframe) {
            ++keyframes;
        }
    }
    QVERIFY(keyframes >= 2);
    QVERIFY(encoder.setBitrate(1'500'000, &error));
}

void CodecTest::invalidInput()
{
    H264Encoder encoder;
    QString error;
    QVERIFY(!encoder.open(321, 240, 30, 2'000'000, &error));
    QVERIFY(!error.isEmpty());

    H264Decoder decoder;
    QVERIFY2(decoder.open(&error), qPrintable(error));
    QImage image;
    const bool accepted = decoder.decode(
        QByteArrayLiteral("not an access unit"),
        0,
        30,
        &image,
        nullptr,
        &error);
    QVERIFY(!accepted || image.isNull());
}

QTEST_GUILESS_MAIN(CodecTest)

#include "codec_test.moc"
