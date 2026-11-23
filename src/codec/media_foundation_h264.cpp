#include "codec/media_foundation_h264.h"

#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <QColor>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace pd {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ComPtr(ComPtr &&other) noexcept
        : value_(std::exchange(other.value_, nullptr))
    {
    }

    ComPtr &operator=(ComPtr &&other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    T **put()
    {
        reset();
        return &value_;
    }

    void attach(T *value)
    {
        reset();
        value_ = value;
    }

    [[nodiscard]] T *get() const { return value_; }
    [[nodiscard]] T *operator->() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }

    void reset()
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    T *value_ = nullptr;
};

QString hresultText(HRESULT result)
{
    return QStringLiteral("0x%1")
        .arg(
            static_cast<qulonglong>(
                static_cast<std::uint32_t>(result)),
            8,
            16,
            QLatin1Char('0'));
}

bool failed(
    HRESULT result,
    const QString &operation,
    QString *error)
{
    if (SUCCEEDED(result)) {
        return false;
    }
    if (error != nullptr) {
        *error = QStringLiteral("%1失败：%2")
                     .arg(operation, hresultText(result));
    }
    return true;
}

bool isComInitialized(HRESULT result)
{
    return result == S_OK || result == S_FALSE;
}

QByteArray toNv12(const QImage &source, int width, int height)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || image.width() != width || image.height() != height) {
        return {};
    }

    QByteArray output(width * height * 3 / 2, Qt::Uninitialized);
    auto *luma = reinterpret_cast<uchar *>(output.data());
    uchar *chroma = luma + width * height;

    for (int y = 0; y < height; y += 2) {
        const auto *row0 =
            reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const auto *row1 =
            reinterpret_cast<const QRgb *>(image.constScanLine(y + 1));
        for (int x = 0; x < width; x += 2) {
            const QRgb pixels[4] = {
                row0[x],
                row0[x + 1],
                row1[x],
                row1[x + 1],
            };
            int cb = 0;
            int cr = 0;
            for (int index = 0; index < 4; ++index) {
                const int r = qRed(pixels[index]);
                const int g = qGreen(pixels[index]);
                const int b = qBlue(pixels[index]);
                const int py = std::clamp(
                    ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16,
                    0,
                    255);
                luma[(y + index / 2) * width + x + index % 2] =
                    static_cast<uchar>(py);
                cb += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                cr += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            }
            const int chromaIndex =
                (y / 2) * width + x;
            chroma[chromaIndex] =
                static_cast<uchar>(std::clamp(cb / 4, 0, 255));
            chroma[chromaIndex + 1] =
                static_cast<uchar>(std::clamp(cr / 4, 0, 255));
        }
    }
    return output;
}

QImage fromNv12(
    const uchar *data,
    qsizetype size,
    int width,
    int height,
    int stride)
{
    if (stride == 0 || stride == std::numeric_limits<int>::min()) {
        return {};
    }
    const bool bottomUp = stride < 0;
    const qint64 absoluteStride =
        bottomUp ? -static_cast<qint64>(stride) : stride;
    if (absoluteStride > (1 << 20)) {
        return {};
    }
    const qsizetype rowStride =
        static_cast<qsizetype>(absoluteStride);
    const qsizetype lumaBytes =
        rowStride * height;
    const qsizetype chromaBytes =
        rowStride * (height / 2);
    if (data == nullptr || width <= 0 || height <= 0
        || (width % 2) != 0 || (height % 2) != 0
        || rowStride < width || size < lumaBytes + chromaBytes) {
        return {};
    }

    const uchar *chroma = data + lumaBytes;
    QImage image(width, height, QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        auto *target = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int lumaY = bottomUp ? height - 1 - y : y;
        const uchar *lumaRow =
            data + static_cast<qsizetype>(lumaY) * rowStride;
        const int chromaY = bottomUp
            ? height / 2 - 1 - y / 2
            : y / 2;
        const uchar *chromaRow =
            chroma + static_cast<qsizetype>(chromaY) * rowStride;
        for (int x = 0; x < width; x += 2) {
            const int u = chromaRow[x];
            const int v = chromaRow[x + 1];
            const int d = u - 128;
            const int e = v - 128;
            for (int offset = 0; offset < 2; ++offset) {
                const int c = lumaRow[x + offset] - 16;
                const int r = std::clamp(
                    (298 * c + 409 * e + 128) >> 8, 0, 255);
                const int g = std::clamp(
                    (298 * c - 100 * d - 208 * e + 128) >> 8,
                    0,
                    255);
                const int b = std::clamp(
                    (298 * c + 516 * d + 128) >> 8, 0, 255);
                target[x + offset] = qRgb(r, g, b);
            }
        }
    }
    return image;
}

bool setCodecValue(
    ICodecAPI *codec,
    const GUID &property,
    VARIANT value)
{
    if (codec == nullptr) {
        return false;
    }
    const HRESULT result = codec->SetValue(&property, &value);
    VariantClear(&value);
    return SUCCEEDED(result);
}

bool setBoolCodecValue(
    ICodecAPI *codec,
    const GUID &property,
    bool enabled)
{
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    return setCodecValue(codec, property, value);
}

bool setUintCodecValue(
    ICodecAPI *codec,
    const GUID &property,
    ULONG number)
{
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = number;
    return setCodecValue(codec, property, value);
}

bool setMediaSize(IMFMediaType *type, int width, int height)
{
    return SUCCEEDED(MFSetAttributeSize(
        type,
        MF_MT_FRAME_SIZE,
        static_cast<UINT32>(width),
        static_cast<UINT32>(height)));
}

bool setMediaRate(IMFMediaType *type, int fps)
{
    return SUCCEEDED(MFSetAttributeRatio(
        type,
        MF_MT_FRAME_RATE,
        static_cast<UINT32>(fps),
        1));
}

bool setMediaAspectRatio(IMFMediaType *type)
{
    return SUCCEEDED(MFSetAttributeRatio(
        type,
        MF_MT_PIXEL_ASPECT_RATIO,
        1,
        1));
}

bool readSampleBytes(
    IMFSample *sample,
    QByteArray *bytes)
{
    if (sample == nullptr || bytes == nullptr) {
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
        return false;
    }
    BYTE *data = nullptr;
    DWORD current = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &current))) {
        return false;
    }
    *bytes = QByteArray(
        reinterpret_cast<const char *>(data),
        static_cast<qsizetype>(current));
    buffer->Unlock();
    return true;
}

ComPtr<IMFSample> makeSample(
    const QByteArray &bytes,
    LONGLONG time,
    LONGLONG duration)
{
    if (bytes.isEmpty()
        || static_cast<quint64>(bytes.size())
            > std::numeric_limits<DWORD>::max()) {
        return {};
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(
            static_cast<DWORD>(bytes.size()),
            buffer.put()))) {
        return {};
    }
    BYTE *data = nullptr;
    if (FAILED(buffer->Lock(&data, nullptr, nullptr))) {
        return {};
    }
    std::memcpy(data, bytes.constData(), static_cast<size_t>(bytes.size()));
    buffer->Unlock();
    if (FAILED(buffer->SetCurrentLength(
            static_cast<DWORD>(bytes.size())))) {
        return {};
    }

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.put()))
        || FAILED(sample->AddBuffer(buffer.get()))) {
        return {};
    }
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    return sample;
}

} // namespace

bool isH264Keyframe(const QByteArray &data)
{
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
    const int size = data.size();
    for (int index = 0; index + 3 < size; ++index) {
        int header = -1;
        if (bytes[index] == 0 && bytes[index + 1] == 0) {
            if (bytes[index + 2] == 1) {
                header = index + 3;
            } else if (index + 4 < size
                       && bytes[index + 2] == 0
                       && bytes[index + 3] == 1) {
                header = index + 4;
            }
        }
        if (header >= 0 && header < size
            && (bytes[header] & 0x1F) == 5) {
            return true;
        }
    }
    return false;
}

struct H264Encoder::Impl {
    ~Impl() { close(); }

    bool open(
        int width,
        int height,
        int fps,
        int bitrateBps,
        QString *error)
    {
        close();
        if (width < 2 || height < 2 || (width % 2) != 0
            || (height % 2) != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("H.264 尺寸必须是正偶数");
            }
            return false;
        }
        width_ = width;
        height_ = height;
        fps_ = std::clamp(fps, 1, 120);
        bitrateBps_ = std::clamp(bitrateBps, 64'000, 100'000'000);
        frameIndex_ = 0;

        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (isComInitialized(comResult)) {
            comInitialized_ = true;
        } else if (comResult != RPC_E_CHANGED_MODE) {
            return failed(
                comResult,
                QStringLiteral("初始化 COM"),
                error);
        }

        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(result)) {
            failed(result, QStringLiteral("启动 Media Foundation"), error);
            close();
            return false;
        }
        mfInitialized_ = true;

        result = CoCreateInstance(
            CLSID_CMSH264EncoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(transform_.put()));
        if (FAILED(result)) {
            failed(
                result,
                QStringLiteral("创建 H.264 编码器"),
                error);
            close();
            return false;
        }

        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_->GetAttributes(attributes.put()))) {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (FAILED(transform_->QueryInterface(
                IID_ICodecAPI,
                reinterpret_cast<void **>(codec_.put())))) {
            codec_.reset();
        }
        if (codec_) {
            setBoolCodecValue(
                codec_.get(),
                CODECAPI_AVLowLatencyMode,
                true);
            setUintCodecValue(
                codec_.get(),
                CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR);
            setUintCodecValue(
                codec_.get(),
                CODECAPI_AVEncCommonMeanBitRate,
                static_cast<ULONG>(bitrateBps_));
            setUintCodecValue(
                codec_.get(),
                CODECAPI_AVEncMPVDefaultBPictureCount,
                0);
            setUintCodecValue(
                codec_.get(),
                CODECAPI_AVEncVideoMaxNumRefFrame,
                1);
            setUintCodecValue(
                codec_.get(),
                CODECAPI_AVEncMPVGOPSize,
                static_cast<ULONG>(std::max(fps_ * 2, 30)));
        }

        ComPtr<IMFMediaType> outputType;
        result = MFCreateMediaType(outputType.put());
        if (SUCCEEDED(result)) {
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            outputType->SetUINT32(
                MF_MT_AVG_BITRATE,
                static_cast<UINT32>(bitrateBps_));
            outputType->SetUINT32(
                MF_MT_INTERLACE_MODE,
                MFVideoInterlace_Progressive);
            setMediaSize(outputType.get(), width_, height_);
            setMediaRate(outputType.get(), fps_);
            setMediaAspectRatio(outputType.get());
            result = transform_->SetOutputType(
                0,
                outputType.get(),
                0);
        }
        if (failed(result, QStringLiteral("设置 H.264 输出格式"), error)) {
            close();
            return false;
        }

        ComPtr<IMFMediaType> inputType;
        result = MFCreateMediaType(inputType.put());
        if (SUCCEEDED(result)) {
            inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inputType->SetUINT32(
                MF_MT_INTERLACE_MODE,
                MFVideoInterlace_Progressive);
            inputType->SetUINT32(
                MF_MT_DEFAULT_STRIDE,
                static_cast<UINT32>(width_));
            setMediaSize(inputType.get(), width_, height_);
            setMediaRate(inputType.get(), fps_);
            setMediaAspectRatio(inputType.get());
            result = transform_->SetInputType(0, inputType.get(), 0);
        }
        if (failed(result, QStringLiteral("设置 NV12 输入格式"), error)) {
            close();
            return false;
        }

        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
            0);
        transform_->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM,
            0);
        open_ = true;
        return true;
    }

    void close()
    {
        if (transform_) {
            transform_->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_OF_STREAM,
                0);
            transform_->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING,
                0);
        }
        codec_.reset();
        transform_.reset();
        if (mfInitialized_) {
            MFShutdown();
            mfInitialized_ = false;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        open_ = false;
        frameIndex_ = 0;
    }

    bool collectOutput(
        QByteArray *bytes,
        bool *keyframe,
        QString *error)
    {
        bytes->clear();
        *keyframe = false;
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            HRESULT result = transform_->GetOutputStreamInfo(0, &info);
            if (failed(
                    result,
                    QStringLiteral("读取编码输出信息"),
                    error)) {
                return false;
            }

            const bool providesSamples =
                (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            const DWORD capacity = info.cbSize > 0
                ? info.cbSize
                : 4U * 1024U * 1024U;
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            if (!providesSamples) {
                result = MFCreateMemoryBuffer(capacity, buffer.put());
                if (SUCCEEDED(result)) {
                    result = MFCreateSample(sample.put());
                }
                if (SUCCEEDED(result)) {
                    result = sample->AddBuffer(buffer.get());
                }
                if (failed(
                        result,
                        QStringLiteral("分配编码输出缓冲"),
                        error)) {
                    return false;
                }
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = sample.get();
            DWORD status = 0;
            result = transform_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFSample> returnedSample;
            if (providesSamples && output.pSample != nullptr) {
                returnedSample.attach(output.pSample);
            }
            if (output.pEvents != nullptr) {
                output.pEvents->Release();
                output.pEvents = nullptr;
            }
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return true;
            }
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                continue;
            }
            if (failed(
                    result,
                    QStringLiteral("读取 H.264 编码结果"),
                    error)) {
                return false;
            }

            IMFSample *encoded = output.pSample != nullptr
                ? output.pSample
                : sample.get();
            if (returnedSample) {
                encoded = returnedSample.get();
            }
            if (encoded == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("H.264 编码器没有返回样本");
                }
                return false;
            }
            QByteArray chunk;
            if (!readSampleBytes(encoded, &chunk)) {
                if (error != nullptr) {
                    *error = QStringLiteral("无法读取 H.264 编码数据");
                }
                return false;
            }
            bytes->append(chunk);
            *keyframe = *keyframe || isH264Keyframe(chunk);
        }
    }

    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrateBps_ = 2'000'000;
    qint64 frameIndex_ = 0;
    bool comInitialized_ = false;
    bool mfInitialized_ = false;
    bool open_ = false;
};

H264Encoder::H264Encoder()
    : impl_(std::make_unique<Impl>())
{
}

H264Encoder::~H264Encoder() = default;

bool H264Encoder::open(
    int width,
    int height,
    int fps,
    int bitrateBps,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    return impl_->open(width, height, fps, bitrateBps, error);
}

void H264Encoder::close()
{
    impl_->close();
}

bool H264Encoder::encode(
    const QImage &image,
    bool forceKeyframe,
    H264AccessUnit *accessUnit,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!impl_->open_ || accessUnit == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("H.264 编码器尚未打开");
        }
        return false;
    }
    accessUnit->data.clear();
    accessUnit->keyframe = false;

    const QByteArray nv12 =
        toNv12(image, impl_->width_, impl_->height_);
    if (nv12.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("无法将画面转换为 NV12");
        }
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(
        static_cast<DWORD>(nv12.size()),
        buffer.put());
    if (SUCCEEDED(result)) {
        BYTE *data = nullptr;
        result = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(result)) {
            std::memcpy(
                data,
                nv12.constData(),
                static_cast<size_t>(nv12.size()));
            buffer->Unlock();
            result = buffer->SetCurrentLength(
                static_cast<DWORD>(nv12.size()));
        }
    }

    ComPtr<IMFSample> sample;
    if (SUCCEEDED(result)) {
        result = MFCreateSample(sample.put());
    }
    if (SUCCEEDED(result)) {
        result = sample->AddBuffer(buffer.get());
    }
    if (failed(result, QStringLiteral("创建编码输入帧"), error)) {
        return false;
    }

    const LONGLONG duration =
        std::max<LONGLONG>(1, 10'000'000 / impl_->fps_);
    const LONGLONG time = impl_->frameIndex_ * duration;
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);

    if (forceKeyframe && impl_->codec_) {
        setUintCodecValue(
            impl_->codec_.get(),
            CODECAPI_AVEncVideoForceKeyFrame,
            1);
    }

    result = impl_->transform_->ProcessInput(0, sample.get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        QByteArray ignored;
        bool ignoredKeyframe = false;
        if (!impl_->collectOutput(
                &ignored,
                &ignoredKeyframe,
                error)) {
            return false;
        }
        result = impl_->transform_->ProcessInput(0, sample.get(), 0);
    }
    if (failed(result, QStringLiteral("提交 H.264 编码帧"), error)) {
        return false;
    }

    ++impl_->frameIndex_;
    return impl_->collectOutput(
        &accessUnit->data,
        &accessUnit->keyframe,
        error);
}

bool H264Encoder::setBitrate(int bitrateBps, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!impl_->open_ || !impl_->codec_) {
        if (error != nullptr) {
            *error = QStringLiteral("H.264 编码器尚未打开");
        }
        return false;
    }
    impl_->bitrateBps_ =
        std::clamp(bitrateBps, 64'000, 100'000'000);
    if (!setUintCodecValue(
            impl_->codec_.get(),
            CODECAPI_AVEncCommonMeanBitRate,
            static_cast<ULONG>(impl_->bitrateBps_))) {
        if (error != nullptr) {
            *error = QStringLiteral("更新 H.264 码率失败");
        }
        return false;
    }
    return true;
}

bool H264Encoder::isOpen() const
{
    return impl_->open_;
}

QString H264Encoder::backend() const
{
    return QStringLiteral("media-foundation-h264");
}

struct H264Decoder::Impl {
    ~Impl() { close(); }

    bool open(QString *error)
    {
        close();
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (isComInitialized(comResult)) {
            comInitialized_ = true;
        } else if (comResult != RPC_E_CHANGED_MODE) {
            return failed(
                comResult,
                QStringLiteral("初始化 COM"),
                error);
        }

        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(result)) {
            failed(result, QStringLiteral("启动 Media Foundation"), error);
            close();
            return false;
        }
        mfInitialized_ = true;

        result = CoCreateInstance(
            CLSID_CMSH264DecoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(transform_.put()));
        if (FAILED(result)) {
            failed(
                result,
                QStringLiteral("创建 H.264 解码器"),
                error);
            close();
            return false;
        }

        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_->GetAttributes(attributes.put()))) {
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        ComPtr<IMFMediaType> inputType;
        result = MFCreateMediaType(inputType.put());
        if (SUCCEEDED(result)) {
            inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            inputType->SetUINT32(
                MF_MT_INTERLACE_MODE,
                MFVideoInterlace_Progressive);
            result = transform_->SetInputType(0, inputType.get(), 0);
        }
        if (failed(result, QStringLiteral("设置 H.264 解码输入"), error)) {
            close();
            return false;
        }

        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
            0);
        transform_->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM,
            0);
        open_ = true;
        return true;
    }

    void close()
    {
        if (transform_) {
            transform_->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_OF_STREAM,
                0);
            transform_->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING,
                0);
        }
        outputType_.reset();
        inputType_.reset();
        transform_.reset();
        if (mfInitialized_) {
            MFShutdown();
            mfInitialized_ = false;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        width_ = 0;
        height_ = 0;
        stride_ = 0;
        open_ = false;
    }

    bool selectOutputType(QString *error)
    {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT result = transform_->GetOutputAvailableType(
                0,
                index,
                candidate.put());
            if (result == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(result)) {
                return !failed(
                    result,
                    QStringLiteral("枚举解码输出格式"),
                    error);
            }
            GUID subtype{};
            if (FAILED(candidate->GetGUID(
                    MF_MT_SUBTYPE,
                    &subtype))) {
                continue;
            }
            if (!IsEqualGUID(subtype, MFVideoFormat_NV12)) {
                continue;
            }
            UINT32 width = 0;
            UINT32 height = 0;
            if (FAILED(MFGetAttributeSize(
                    candidate.get(),
                    MF_MT_FRAME_SIZE,
                    &width,
                    &height))
                || width == 0 || height == 0
                || (width % 2) != 0 || (height % 2) != 0) {
                continue;
            }
            const HRESULT setResult = transform_->SetOutputType(
                0,
                candidate.get(),
                0);
            if (FAILED(setResult)) {
                continue;
            }
            outputType_ = std::move(candidate);
            width_ = static_cast<int>(width);
            height_ = static_cast<int>(height);
            UINT32 stride = static_cast<UINT32>(width_);
            outputType_->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
            stride_ = static_cast<int>(
                static_cast<std::int32_t>(stride));
            const qint64 absoluteStride = std::abs(
                static_cast<qint64>(stride_));
            if (stride_ == 0
                || absoluteStride < width_
                || absoluteStride > (1 << 20)) {
                if (error != nullptr) {
                    *error = QStringLiteral(
                        "H.264 解码输出行跨度无效");
                }
                return false;
            }
            return true;
        }
        if (error != nullptr) {
            *error = QStringLiteral("H.264 解码器没有 NV12 输出");
        }
        return false;
    }

    bool collectOutput(
        QImage *image,
        QString *error)
    {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            HRESULT result = transform_->GetOutputStreamInfo(0, &info);
            if (failed(
                    result,
                    QStringLiteral("读取解码输出信息"),
                    error)) {
                return false;
            }
            const bool providesSamples =
                (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            const DWORD capacity = info.cbSize > 0
                ? info.cbSize
                : static_cast<DWORD>(width_ * height_ * 2);
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            if (!providesSamples) {
                result = MFCreateMemoryBuffer(capacity, buffer.put());
                if (SUCCEEDED(result)) {
                    result = MFCreateSample(sample.put());
                }
                if (SUCCEEDED(result)) {
                    result = sample->AddBuffer(buffer.get());
                }
                if (failed(
                        result,
                        QStringLiteral("分配解码输出缓冲"),
                        error)) {
                    return false;
                }
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = sample.get();
            DWORD status = 0;
            result = transform_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFSample> returnedSample;
            if (providesSamples && output.pSample != nullptr) {
                returnedSample.attach(output.pSample);
            }
            if (output.pEvents != nullptr) {
                output.pEvents->Release();
                output.pEvents = nullptr;
            }
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return true;
            }
            if (result == MF_E_TRANSFORM_STREAM_CHANGE
                || result == MF_E_TRANSFORM_TYPE_NOT_SET) {
                if (!selectOutputType(error)) {
                    return false;
                }
                continue;
            }
            if (failed(
                    result,
                    QStringLiteral("读取 H.264 解码结果"),
                    error)) {
                return false;
            }

            IMFSample *decoded = output.pSample != nullptr
                ? output.pSample
                : sample.get();
            if (returnedSample) {
                decoded = returnedSample.get();
            }
            ComPtr<IMFMediaBuffer> contiguous;
            if (decoded == nullptr
                || FAILED(decoded->ConvertToContiguousBuffer(
                    contiguous.put()))) {
                if (error != nullptr) {
                    *error = QStringLiteral("无法读取解码图像");
                }
                return false;
            }
            BYTE *data = nullptr;
            DWORD current = 0;
            if (FAILED(contiguous->Lock(&data, nullptr, &current))) {
                if (error != nullptr) {
                    *error = QStringLiteral("无法锁定解码图像");
                }
                return false;
            }
            QImage frame = fromNv12(
                data,
                static_cast<qsizetype>(current),
                width_,
                height_,
                stride_);
            contiguous->Unlock();
            if (frame.isNull()) {
                if (error != nullptr) {
                    *error = QStringLiteral("H.264 解码输出尺寸无效");
                }
                return false;
            }
            *image = std::move(frame);
        }
    }

    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaType> inputType_;
    ComPtr<IMFMediaType> outputType_;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    bool comInitialized_ = false;
    bool mfInitialized_ = false;
    bool open_ = false;
};

H264Decoder::H264Decoder()
    : impl_(std::make_unique<Impl>())
{
}

H264Decoder::~H264Decoder() = default;

bool H264Decoder::open(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    return impl_->open(error);
}

void H264Decoder::close()
{
    impl_->close();
}

bool H264Decoder::decode(
    const QByteArray &data,
    quint32 sequence,
    int fps,
    QImage *image,
    bool *keyframe,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (image != nullptr) {
        image->fill(Qt::transparent);
    }
    if (keyframe != nullptr) {
        *keyframe = false;
    }
    if (!impl_->open_ || data.isEmpty() || image == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("H.264 解码器尚未打开或输入为空");
        }
        return false;
    }

    const int rate = std::clamp(fps, 1, 120);
    const LONGLONG duration =
        std::max<LONGLONG>(1, 10'000'000 / rate);
    const LONGLONG time =
        static_cast<LONGLONG>(sequence) * duration;
    ComPtr<IMFSample> sample = makeSample(data, time, duration);
    if (!sample) {
        if (error != nullptr) {
            *error = QStringLiteral("创建 H.264 解码输入失败");
        }
        return false;
    }

    HRESULT result =
        impl_->transform_->ProcessInput(0, sample.get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        QImage ignored;
        if (!impl_->open_
            || !impl_->collectOutput(
                &ignored,
                error)) {
            return false;
        }
        result = impl_->transform_->ProcessInput(
            0,
            sample.get(),
            0);
    }
    if (failed(result, QStringLiteral("提交 H.264 解码帧"), error)) {
        return false;
    }

    const bool inputKeyframe = isH264Keyframe(data);
    QImage decoded;
    if (!impl_->collectOutput(&decoded, error)) {
        return false;
    }
    if (!decoded.isNull()) {
        *image = std::move(decoded);
        if (keyframe != nullptr) {
            *keyframe = inputKeyframe;
        }
    }
    return true;
}

void H264Decoder::reset()
{
    if (!impl_->open_ || !impl_->transform_) {
        return;
    }
    impl_->transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->transform_->ProcessMessage(
        MFT_MESSAGE_NOTIFY_START_OF_STREAM,
        0);
}

bool H264Decoder::isOpen() const
{
    return impl_->open_;
}

QString H264Decoder::backend() const
{
    return QStringLiteral("media-foundation-h264");
}

} // namespace pd
