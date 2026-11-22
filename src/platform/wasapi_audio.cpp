#include "platform/wasapi_audio.h"

#include "platform/pcm_resampler.h"

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <QString>

#include <algorithm>
#include <chrono>
#include <memory>

namespace pd {

namespace {

constexpr qsizetype kMaxBufferedPcm = 16000 * 2 * 2;

QString hresultText(const char *operation, HRESULT result)
{
    return QStringLiteral("%1 失败 hr=0x%2")
        .arg(QString::fromLatin1(operation))
        .arg(
            static_cast<quint32>(result),
            8,
            16,
            QLatin1Char('0'))
        .toUpper();
}

template <typename T>
void release(T *&pointer)
{
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

bool parseFormat(
    const WAVEFORMATEX *format,
    PcmMonoResampler::SampleFormat *sampleFormat,
    QString *error)
{
    if (format == nullptr || format->nChannels == 0
        || format->nSamplesPerSec == 0 || format->nBlockAlign == 0) {
        *error = QStringLiteral("WASAPI 返回了无效的混音格式");
        return false;
    }

    bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm16 = format->wFormatTag == WAVE_FORMAT_PCM
        && format->wBitsPerSample == 16;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *extended =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        isFloat = IsEqualGUID(
                      extended->SubFormat,
                      KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            != 0;
        isPcm16 = IsEqualGUID(
                      extended->SubFormat,
                      KSDATAFORMAT_SUBTYPE_PCM)
            != 0
            && format->wBitsPerSample == 16;
    }
    if (isFloat && format->wBitsPerSample == 32) {
        *sampleFormat = PcmMonoResampler::SampleFormat::Float32;
        return true;
    }
    if (isPcm16) {
        *sampleFormat = PcmMonoResampler::SampleFormat::Int16;
        return true;
    }

    *error = QStringLiteral("不支持的 WASAPI 混音格式：%1 Hz / %2 ch / %3 bit")
                 .arg(format->nSamplesPerSec)
                 .arg(format->nChannels)
                 .arg(format->wBitsPerSample);
    return false;
}

} // namespace

WasapiLoopbackSource::~WasapiLoopbackSource()
{
    stop();
}

QString WasapiLoopbackSource::label() const
{
    return QStringLiteral("WASAPI loopback");
}

void WasapiLoopbackSource::finishStartup(const QString &error)
{
    {
        const std::lock_guard lock(mutex_);
        startupError_ = error;
        startupDone_ = true;
        running_ = error.isEmpty();
    }
    wake_.notify_all();
}

void WasapiLoopbackSource::appendPcm(const QByteArray &pcm)
{
    if (pcm.isEmpty()) {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        buffer_.append(pcm);
        if (buffer_.size() > kMaxBufferedPcm) {
            buffer_.remove(0, buffer_.size() - kMaxBufferedPcm);
        }
    }
    wake_.notify_all();
}

bool WasapiLoopbackSource::start(QString *error)
{
    if (thread_.joinable()) {
        stop();
    }
    {
        const std::lock_guard lock(mutex_);
        buffer_.clear();
        startupError_.clear();
        runtimeError_.clear();
        startupDone_ = false;
        running_ = false;
    }
    stop_.store(false);
    thread_ = std::thread(&WasapiLoopbackSource::run, this);

    bool finished = false;
    QString failure;
    {
        std::unique_lock lock(mutex_);
        finished = wake_.wait_for(
            lock,
            std::chrono::seconds(3),
            [this] { return startupDone_; });
        failure = startupError_;
    }
    if (!finished) {
        failure = QStringLiteral("WASAPI 启动超时");
        stop_.store(true);
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        const std::lock_guard lock(mutex_);
        running_ = false;
    }
    if (error != nullptr) {
        *error = failure;
    }
    return failure.isEmpty();
}

void WasapiLoopbackSource::run()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized) {
        finishStartup(hresultText("CoInitializeEx", comResult));
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    WAVEFORMATEX *mixFormat = nullptr;
    bool clientStarted = false;
    QString failure;

    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void **>(&enumerator));
    if (FAILED(result)) {
        failure = hresultText("CoCreateInstance(MMDeviceEnumerator)", result);
    }

    if (failure.isEmpty()) {
        result = enumerator->GetDefaultAudioEndpoint(
            eRender,
            eConsole,
            &device);
        if (FAILED(result)) {
            failure = hresultText("GetDefaultAudioEndpoint", result);
        }
    }

    if (failure.isEmpty()) {
        result = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void **>(&client));
        if (FAILED(result)) {
            failure = hresultText("IMMDevice::Activate", result);
        }
    }

    if (failure.isEmpty()) {
        result = client->GetMixFormat(&mixFormat);
        if (FAILED(result)) {
            failure = hresultText("IAudioClient::GetMixFormat", result);
        }
    }

    std::unique_ptr<PcmMonoResampler> resampler;
    quint16 blockAlign = 0;
    if (failure.isEmpty()) {
        PcmMonoResampler::SampleFormat sampleFormat =
            PcmMonoResampler::SampleFormat::Float32;
        failure = QString();
        if (!parseFormat(mixFormat, &sampleFormat, &failure)) {
            // parseFormat filled the error.
        } else {
            blockAlign = mixFormat->nBlockAlign;
            resampler = std::make_unique<PcmMonoResampler>(
                static_cast<int>(mixFormat->nSamplesPerSec),
                mixFormat->nChannels,
                sampleFormat);
        }
    }

    if (failure.isEmpty()) {
        result = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            2000000,
            0,
            mixFormat,
            nullptr);
        if (FAILED(result)) {
            failure = hresultText("IAudioClient::Initialize(loopback)", result);
        }
    }

    if (failure.isEmpty()) {
        result = client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void **>(&capture));
        if (FAILED(result)) {
            failure = hresultText("IAudioClient::GetService", result);
        }
    }

    if (failure.isEmpty()) {
        result = client->Start();
        if (FAILED(result)) {
            failure = hresultText("IAudioClient::Start", result);
        } else {
            clientStarted = true;
        }
    }

    if (mixFormat != nullptr) {
        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;
    }
    release(enumerator);
    release(device);

    if (failure.isEmpty()) {
        finishStartup({});
    } else {
        finishStartup(failure);
    }

    while (failure.isEmpty() && !stop_.load()) {
        UINT32 packetFrames = 0;
        result = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(result)) {
            failure = hresultText("IAudioClient::GetNextPacketSize", result);
            break;
        }
        if (packetFrames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }

        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        result = capture->GetBuffer(
            &data,
            &frames,
            &flags,
            nullptr,
            nullptr);
        if (FAILED(result)) {
            failure = hresultText("IAudioCaptureClient::GetBuffer", result);
            break;
        }

        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            resampler->reset();
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            const QByteArray silence(
                static_cast<qsizetype>(frames) * blockAlign,
                '\0');
            appendPcm(resampler->feed(silence.constData(), silence.size()));
        } else {
            appendPcm(resampler->feed(
                data,
                static_cast<qsizetype>(frames) * blockAlign));
        }
        capture->ReleaseBuffer(frames);
    }

    if (!failure.isEmpty()) {
        const std::lock_guard lock(mutex_);
        runtimeError_ = failure;
        running_ = false;
    }
    wake_.notify_all();

    if (clientStarted) {
        client->Stop();
    }
    release(capture);
    release(client);
    CoUninitialize();
}

QByteArray WasapiLoopbackSource::readFrame(
    int samples,
    int timeoutMs,
    QString *error)
{
    const qsizetype wanted =
        std::max(0, samples) * static_cast<qsizetype>(2);
    std::unique_lock lock(mutex_);
    if (timeoutMs > 0 && buffer_.isEmpty()) {
        wake_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this] { return !buffer_.isEmpty() || !runtimeError_.isEmpty(); });
    }
    if (!runtimeError_.isEmpty()) {
        if (error != nullptr) {
            *error = runtimeError_;
        }
        runtimeError_.clear();
        return {};
    }
    if (buffer_.isEmpty() || wanted == 0) {
        return {};
    }
    const qsizetype count = std::min(wanted, buffer_.size());
    QByteArray result = buffer_.left(count);
    buffer_.remove(0, count);
    return result;
}

void WasapiLoopbackSource::stop()
{
    stop_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    const std::lock_guard lock(mutex_);
    running_ = false;
    buffer_.clear();
}

} // namespace pd
