#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <memory>

namespace pd {

struct H264AccessUnit {
    QByteArray data;
    bool keyframe = false;
};

class H264Encoder final {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder &) = delete;
    H264Encoder &operator=(const H264Encoder &) = delete;
    H264Encoder(H264Encoder &&) = delete;
    H264Encoder &operator=(H264Encoder &&) = delete;

    [[nodiscard]] bool open(
        int width,
        int height,
        int fps,
        int bitrateBps,
        QString *error = nullptr);
    void close();

    [[nodiscard]] bool encode(
        const QImage &image,
        bool forceKeyframe,
        H264AccessUnit *accessUnit,
        QString *error = nullptr);
    [[nodiscard]] bool setBitrate(
        int bitrateBps,
        QString *error = nullptr);

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString backend() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class H264Decoder final {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;
    H264Decoder(H264Decoder &&) = delete;

    [[nodiscard]] bool open(QString *error = nullptr);
    void close();

    [[nodiscard]] bool decode(
        const QByteArray &data,
        quint32 sequence,
        int fps,
        QImage *image,
        bool *keyframe = nullptr,
        QString *error = nullptr);
    void reset();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString backend() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool isH264Keyframe(const QByteArray &data);

} // namespace pd
