#include "session/media_sources.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QRect>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pd {

SyntheticScreenSource::SyntheticScreenSource(
    int width,
    int height,
    bool twoMonitors)
{
    MonitorInfo primary;
    primary.index = 0;
    primary.left = 0;
    primary.top = 0;
    primary.width = std::max(320, width);
    primary.height = std::max(180, height);
    primary.primary = true;
    primary.name = QStringLiteral("显示器 1");
    monitors_.append(primary);

    if (twoMonitors) {
        MonitorInfo secondary;
        secondary.index = 1;
        secondary.left = primary.width;
        secondary.top = 0;
        secondary.width = 1024;
        secondary.height = 768;
        secondary.name = QStringLiteral("显示器 2");
        monitors_.append(secondary);
    }
    clock_.start();
}

QString SyntheticScreenSource::backend() const
{
    return QStringLiteral("synthetic");
}

QList<MonitorInfo> SyntheticScreenSource::monitors() const
{
    return monitors_;
}

CaptureResult SyntheticScreenSource::capture(const CaptureRequest &request)
{
    CaptureResult result;
    if (monitors_.isEmpty()) {
        result.error = QStringLiteral("没有可用显示器");
        return result;
    }
    const int index = std::clamp(
        request.monitor,
        0,
        static_cast<int>(monitors_.size()) - 1);
    const MonitorInfo monitor = monitors_.at(index);
    const double scale = request.maxWidth > 0
            && monitor.width > request.maxWidth
        ? static_cast<double>(request.maxWidth)
            / static_cast<double>(monitor.width)
        : 1.0;
    const int width = std::max(2, static_cast<int>(
        std::lround(monitor.width * scale)));
    const int height = std::max(2, static_cast<int>(
        std::lround(monitor.height * scale)));

    QImage image(width, height, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient background(0, 0, width, height);
    if (index == 0) {
        background.setColorAt(0.0, QColor(9, 14, 28));
        background.setColorAt(0.55, QColor(23, 42, 78));
        background.setColorAt(1.0, QColor(21, 92, 105));
    } else {
        background.setColorAt(0.0, QColor(20, 14, 30));
        background.setColorAt(0.6, QColor(58, 28, 70));
        background.setColorAt(1.0, QColor(108, 55, 71));
    }
    painter.fillRect(image.rect(), background);

    painter.setPen(QPen(QColor(110, 155, 220, 40), 1));
    const int grid = std::max(32, width / 16);
    for (int x = 0; x < width; x += grid) {
        painter.drawLine(x, 0, x, height);
    }
    for (int y = 0; y < height; y += grid) {
        painter.drawLine(0, y, width, y);
    }

    const int glyph = std::max(20, width / 44);
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(glyph);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(238, 245, 255));
    painter.drawText(
        QRect(28, 22, width - 56, glyph * 2),
        Qt::AlignLeft | Qt::AlignTop,
        QStringLiteral("PeerDesk · C++ 会话测试画面"));

    QFont mono(QStringLiteral("Cascadia Mono"));
    mono.setPixelSize(std::max(14, glyph / 2));
    painter.setFont(mono);
    painter.setPen(QColor(104, 210, 232));
    const double seconds =
        static_cast<double>(clock_.elapsed()) / 1000.0;
    painter.drawText(
        QRect(28, height - glyph * 3, width - 56, glyph * 2),
        Qt::AlignLeft | Qt::AlignBottom,
        QStringLiteral("t=%1s  frame=%2  %3×%4")
            .arg(seconds, 0, 'f', 2)
            .arg(frame_)
            .arg(width)
            .arg(height));

    const int phase = static_ ? 0 : frame_ * 9;
    const int boxWidth = std::max(80, width / 8);
    const int boxHeight = std::max(48, height / 9);
    const int travel = std::max(1, width - boxWidth - 56);
    const int boxX = 28 + (phase * 3) % travel;
    const int boxY = height / 2 - boxHeight / 2
        + static_cast<int>(
            std::sin(phase * 0.035) * height * 0.22);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(66, 145, 255, 220));
    painter.drawRoundedRect(
        QRect(boxX, boxY, boxWidth, boxHeight), 18, 18);
    painter.setBrush(QColor(100, 245, 210, 230));
    painter.drawEllipse(
        QPoint(boxX + boxWidth / 2, boxY + boxHeight / 2),
        boxHeight / 3,
        boxHeight / 3);

    painter.end();
    if (!static_) {
        ++frame_;
    }
    result.image = image;
    return result;
}

ToneAudioSource::ToneAudioSource(double frequency)
    : frequency_(frequency)
{
}

QString ToneAudioSource::label() const
{
    return QStringLiteral("%1 Hz 合成音")
        .arg(frequency_, 0, 'f', 0);
}

bool ToneAudioSource::start(QString *error)
{
    Q_UNUSED(error)
    phase_ = 0.0;
    started_ = true;
    return true;
}

QByteArray ToneAudioSource::readFrame(
    int samples,
    int timeoutMs,
    QString *error)
{
    Q_UNUSED(timeoutMs)
    if (error) {
        error->clear();
    }
    if (!started_) {
        if (error) {
            *error = QStringLiteral("合成音频源尚未启动");
        }
        return {};
    }
    const int count = std::max(0, samples);
    QByteArray pcm(count * 2, '\0');
    const double step = 2.0 * std::numbers::pi * frequency_ / 16000.0;
    for (int index = 0; index < count; ++index) {
        const double envelope = silent_
            ? 0.0
            : std::sin(phase_) * 8000.0;
        const auto sample = static_cast<qint16>(
            std::clamp(
                static_cast<long>(std::lround(envelope)),
                -32768L,
                32767L));
        pcm[index * 2] = static_cast<char>(sample & 0xFF);
        pcm[index * 2 + 1] =
            static_cast<char>((sample >> 8) & 0xFF);
        phase_ = std::fmod(phase_ + step, 2.0 * std::numbers::pi);
    }
    return pcm;
}

void ToneAudioSource::stop()
{
    started_ = false;
}

void ToneAudioSource::setFrequency(double frequency)
{
    frequency_ = std::clamp(frequency, 20.0, 8000.0);
}

} // namespace pd
