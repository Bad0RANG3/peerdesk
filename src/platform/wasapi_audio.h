#pragma once

#include "session/media_sources.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace pd {

class WasapiLoopbackSource final : public IAudioSource {
public:
    WasapiLoopbackSource() = default;
    ~WasapiLoopbackSource() override;

    [[nodiscard]] QString label() const override;
    bool start(QString *error = nullptr) override;
    QByteArray readFrame(
        int samples,
        int timeoutMs = 0,
        QString *error = nullptr) override;
    void stop() override;

private:
    void run();
    void finishStartup(const QString &error);
    void appendPcm(const QByteArray &pcm);

    std::thread thread_;
    std::atomic_bool stop_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    QByteArray buffer_;
    QString startupError_;
    QString runtimeError_;
    bool startupDone_ = false;
    bool running_ = false;
};

} // namespace pd
