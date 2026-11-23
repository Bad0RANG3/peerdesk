#include "app/audio_output.h"
#include "app/host_controller.h"
#include "app/preferences.h"
#include "app/single_instance.h"
#include "app/viewer_controller.h"
#include "diagnostics/logging.h"
#include "platform/startup_registration.h"
#include "session/media_sources.h"
#include "ui/app_window.h"
#include "ui/host_panel.h"
#include "ui/settings_dialog.h"
#include "ui/ui_theme.h"
#include "ui/viewer_page.h"

#ifdef Q_OS_WIN
#include "platform/wasapi_audio.h"
#include "platform/windows_clipboard.h"
#include "platform/windows_input.h"
#include "platform/windows_screen_source.h"
#include "platform/windows_startup_registration.h"
#endif

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

void printError(const QString &message)
{
    std::fprintf(stderr, "%s\n", message.toUtf8().constData());
}

void installPlatformBackends(
    pd::HostController &host,
    pd::ViewerController &viewer,
    bool syntheticCapture,
    bool syntheticAudio)
{
    if (syntheticCapture) {
        host.setScreenSource(
            std::make_shared<pd::SyntheticScreenSource>(
                1280,
                720,
                true));
    } else {
#ifdef Q_OS_WIN
        host.setScreenSource(std::make_shared<pd::WindowsScreenSource>());
#else
        host.setScreenSource(
            std::make_shared<pd::SyntheticScreenSource>(
                1280,
                720,
                true));
#endif
    }

    if (syntheticAudio) {
        host.setAudioSource(std::make_shared<pd::ToneAudioSource>(440.0));
    } else {
#ifdef Q_OS_WIN
        host.setAudioSource(std::make_shared<pd::WasapiLoopbackSource>());
#endif
    }

#ifdef Q_OS_WIN
    host.setInputSink(std::make_shared<pd::WindowsInputSink>());
    host.setClipboard(std::make_shared<pd::WindowsClipboard>());
#endif

    auto playback = std::make_shared<pd::PcmAudioPlayer>();
    viewer.setAudioPlayback(playback);
}

void applyOverrides(
    const std::shared_ptr<pd::Preferences> &preferences,
    const QCommandLineParser &parser,
    const QCommandLineOption &serverOption,
    const QCommandLineOption &tokenOption,
    const QCommandLineOption &codeOption)
{
    pd::HostPreferences host = preferences->host();
    pd::ViewerPreferences viewer = preferences->viewer();
    bool changed = false;

    if (parser.isSet(serverOption)) {
        host.server = parser.value(serverOption).trimmed();
        viewer.server = host.server;
        changed = true;
    }
    if (parser.isSet(tokenOption)) {
        host.token = parser.value(tokenOption).trimmed();
        viewer.token = host.token;
        changed = true;
    }
    if (parser.isSet(codeOption)) {
        host.code = parser.value(codeOption).trimmed();
        viewer.code = host.code;
        changed = true;
    }

    if (changed) {
        preferences->setHost(host);
        preferences->setViewer(viewer);
    }
}

void registerLoggingSecrets(
    const std::shared_ptr<pd::Preferences> &preferences)
{
    if (!preferences) {
        return;
    }
    const pd::HostPreferences host = preferences->host();
    const pd::ViewerPreferences viewer = preferences->viewer();
    auto &logging = pd::diagnostics::LogManager::instance();
    logging.addSecret(host.token);
    logging.addSecret(viewer.token);
    logging.addSecret(host.code);
    logging.addSecret(viewer.code);
}

void forwardControllerLogs(
    pd::HostController &host,
    pd::ViewerController &viewer)
{
    QObject::connect(
        &host,
        &pd::HostController::logMessage,
        [](const QString &message) {
            qInfo().noquote() << message;
        });
    QObject::connect(
        &viewer,
        &pd::ViewerController::logMessage,
        [](const QString &message) {
            qInfo().noquote() << message;
        });
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("peerdesk-client"));
    QApplication::setOrganizationName(QStringLiteral("PeerDesk"));
    QApplication::setApplicationVersion(QStringLiteral(PEERDESK_VERSION));
    QApplication::setApplicationDisplayName(QStringLiteral("PeerDesk"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PeerDesk remote desktop client"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption backgroundOption(
        QStringList{QStringLiteral("background")},
        QStringLiteral("登录启动模式：驻留系统托盘并保持本机可被控"));
    const QCommandLineOption serverOption(
        QStringList{QStringLiteral("server")},
        QStringLiteral("信令服务器地址，例如 192.168.1.2:7900"),
        QStringLiteral("address"));
    const QCommandLineOption tokenOption(
        QStringList{QStringLiteral("token")},
        QStringLiteral("服务器令牌"),
        QStringLiteral("token"));
    const QCommandLineOption codeOption(
        QStringList{QStringLiteral("code")},
        QStringLiteral("要连接或本机使用的 9 位连接码"),
        QStringLiteral("code"));
    const QCommandLineOption pageOption(
        QStringList{QStringLiteral("page")},
        QStringLiteral("启动页面：remote、host、viewer 或 settings"),
        QStringLiteral("page"),
        QStringLiteral("remote"));
    const QCommandLineOption viewerDemoOption(
        QStringList{QStringLiteral("viewer-demo")},
        QStringLiteral("用合成画面启动原生 Viewer 会话页"));
    const QCommandLineOption captureOption(
        QStringList{QStringLiteral("capture")},
        QStringLiteral("采集后端：native 或 test"),
        QStringLiteral("mode"),
        QStringLiteral("native"));
    const QCommandLineOption audioTestOption(
        QStringList{QStringLiteral("audio-test")},
        QStringLiteral("使用合成音源，不采集系统声音"));
    const QCommandLineOption screenshotOption(
        QStringList{QStringLiteral("screenshot")},
        QStringLiteral("保存窗口截图后退出"),
        QStringLiteral("path"));
    const QCommandLineOption exitAfterOption(
        QStringList{QStringLiteral("exit-after")},
        QStringLiteral("指定毫秒后自动退出"),
        QStringLiteral("milliseconds"));
    const QCommandLineOption logDirectoryOption(
        QStringList{QStringLiteral("log-dir")},
        QStringLiteral("日志目录（默认 %APPDATA%\\PeerDesk\\logs）"),
        QStringLiteral("path"));

    parser.addOptions({
        backgroundOption,
        serverOption,
        tokenOption,
        codeOption,
        pageOption,
        viewerDemoOption,
        captureOption,
        audioTestOption,
        screenshotOption,
        exitAfterOption,
        logDirectoryOption,
    });
    parser.process(application);

    const QString page = parser.value(pageOption).trimmed().toLower();
    if (page != QStringLiteral("remote")
        && page != QStringLiteral("host")
        && page != QStringLiteral("viewer")
        && page != QStringLiteral("settings")) {
        printError(QStringLiteral("无效的启动页面：%1").arg(page));
        return 2;
    }

    const bool viewerDemo = parser.isSet(viewerDemoOption);
    if (viewerDemo && page != QStringLiteral("viewer")) {
        printError(QStringLiteral("--viewer-demo 需要配合 --page viewer"));
        return 2;
    }

    const QString capture = parser.value(captureOption).trimmed().toLower();
    if (capture != QStringLiteral("native")
        && capture != QStringLiteral("test")) {
        printError(QStringLiteral("无效的采集后端：%1").arg(capture));
        return 2;
    }

    const bool syntheticCapture = capture == QStringLiteral("test");
    const bool syntheticAudio = parser.isSet(audioTestOption);
    const bool hasScreenshot = parser.isSet(screenshotOption);
    const bool isolated =
        hasScreenshot || syntheticCapture || syntheticAudio || viewerDemo;
    const bool background =
        parser.isSet(backgroundOption) && !isolated;

    bool delayOk = true;
    const int exitAfter = parser.isSet(exitAfterOption)
        ? parser.value(exitAfterOption).toInt(&delayOk)
        : 0;
    if (!delayOk || exitAfter < 0) {
        printError(QStringLiteral("无效的自动退出时间"));
        return 2;
    }

    QString screenshotPath;
    if (hasScreenshot) {
        screenshotPath = parser.value(screenshotOption).trimmed();
        if (screenshotPath.isEmpty()) {
            printError(QStringLiteral("截图路径不能为空"));
            return 2;
        }
        screenshotPath =
            QDir::current().absoluteFilePath(screenshotPath);
        const QFileInfo info(screenshotPath);
        if (!QDir().mkpath(info.absolutePath())) {
            printError(QStringLiteral("无法创建截图目录：%1")
                           .arg(info.absolutePath()));
            return 2;
        }
    }

    std::unique_ptr<pd::SingleInstance> instanceGuard;
    if (!isolated) {
        const QString instanceName = pd::SingleInstance::defaultServerName();
        instanceGuard =
            std::make_unique<pd::SingleInstance>(instanceName);
        QString instanceError;
        if (!instanceGuard->listen(&instanceError)) {
            if (pd::SingleInstance::notifyRunningInstance(instanceName)) {
                return 0;
            }
            if (!instanceGuard->listen(&instanceError)) {
                printError(QStringLiteral("无法启动客户端：%1")
                               .arg(instanceError));
                return 1;
            }
        }
    }

    std::unique_ptr<QTemporaryDir> isolatedLogDirectory;
    QString logDirectory;
    if (parser.isSet(logDirectoryOption)) {
        logDirectory = QDir::current().absoluteFilePath(
            parser.value(logDirectoryOption).trimmed());
    } else if (isolated) {
        isolatedLogDirectory = std::make_unique<QTemporaryDir>();
        if (isolatedLogDirectory->isValid()) {
            logDirectory = isolatedLogDirectory->filePath(
                QStringLiteral("logs"));
        }
    }

    pd::diagnostics::LoggingOptions loggingOptions;
    loggingOptions.directory = logDirectory;
    loggingOptions.applicationId = isolated
        ? QStringLiteral("peerdesk-client-test")
        : QStringLiteral("peerdesk-client");
    loggingOptions.trackSession = !isolated;
    QString loggingError;
    const bool loggingEnabled =
        pd::diagnostics::LogManager::instance().start(
            loggingOptions,
            &loggingError);
    if (!loggingEnabled) {
        printError(QStringLiteral("无法启用文件日志：%1")
                       .arg(loggingError));
    }
    const bool previousSessionCrashed =
        loggingEnabled
        && pd::diagnostics::LogManager::instance()
               .previousSessionCrashed();

    pd::ui::applyTheme(application);

    const auto preferences = std::make_shared<pd::Preferences>(
        isolated ? pd::Preferences::Mode::Ephemeral
                 : pd::Preferences::Mode::Persistent);
    applyOverrides(
        preferences,
        parser,
        serverOption,
        tokenOption,
        codeOption);
    registerLoggingSecrets(preferences);

    pd::HostController host;
    pd::ViewerController viewer;
    host.setPreferences(preferences);
    viewer.setPreferences(preferences);
    forwardControllerLogs(host, viewer);
    installPlatformBackends(
        host,
        viewer,
        syntheticCapture || viewerDemo,
        syntheticAudio || viewerDemo);

    std::shared_ptr<pd::StartupRegistration> startupRegistration;
#ifdef Q_OS_WIN
    startupRegistration =
        std::make_shared<pd::WindowsStartupRegistration>();
#endif
    pd::ui::AppWindow window(&host, &viewer, startupRegistration);
    if (instanceGuard) {
        QObject::connect(
            instanceGuard.get(),
            &pd::SingleInstance::activationRequested,
            &window,
            [&window]() {
                window.showMainWindow();
            });
    }
    QObject::connect(
        &window,
        &pd::ui::AppWindow::quitRequested,
        &application,
        &QCoreApplication::quit);
    if (isolated) {
        window.hostPanel()->setAutoStart(false);
    }
    bool startHidden = false;
    if (background) {
        startHidden = window.startInBackground();
    } else if (viewerDemo) {
        pd::SyntheticScreenSource demoSource(1600, 900, true);
        QTimer demoTimer;
        window.resize(1600, 900);

        const auto renderDemoFrame = [&window, &demoSource]() {
            pd::CaptureRequest request;
            request.monitor = 0;
            request.maxWidth = 1600;
            request.pixelFormat = QStringLiteral("rgb");
            const pd::CaptureResult capture = demoSource.capture(request);
            if (!capture.ok()) {
                return;
            }

            QJsonArray monitors;
            for (const pd::MonitorInfo &monitor : demoSource.monitors()) {
                monitors.append(QJsonObject{
                    {QStringLiteral("index"), monitor.index},
                    {QStringLiteral("x"), monitor.left},
                    {QStringLiteral("y"), monitor.top},
                    {QStringLiteral("w"), monitor.width},
                    {QStringLiteral("h"), monitor.height},
                    {QStringLiteral("primary"), monitor.primary},
                    {QStringLiteral("name"), monitor.name},
                });
            }
            window.viewerPage()->showDemoFrame(
                capture.image,
                QStringLiteral("STUDIO-PC"),
                monitors);
        };

        renderDemoFrame();
        window.showViewerPage();
        QObject::connect(
            &demoTimer,
            &QTimer::timeout,
            &window,
            renderDemoFrame);
        demoTimer.start(66);
    } else if (page == QStringLiteral("host")) {
        window.showHostPage();
    } else if (page == QStringLiteral("viewer")) {
        window.showViewerPage();
    } else if (page == QStringLiteral("settings")) {
        window.showSettings();
    }

    QObject::connect(
        &application,
        &QCoreApplication::aboutToQuit,
        &application,
        [&host, &viewer]() {
            viewer.hangUp();
            host.stop();
            pd::diagnostics::LogManager::instance().stop();
        });

    if (!startHidden) {
        window.show();
        window.raise();
        window.activateWindow();
        if (previousSessionCrashed) {
            QTimer::singleShot(
                700,
                &window,
                [&window]() {
                    window.showToast(
                        QStringLiteral(
                            "上次运行未正常退出，可在设置中导出诊断包"));
                });
        }
    }

    if (hasScreenshot) {
        QTimer::singleShot(450, &window, [&window, page, screenshotPath]() {
            QWidget *target = &window;
            if (page == QStringLiteral("settings")
                && window.settingsDialog()
                && window.settingsDialog()->isVisible()) {
                target = window.settingsDialog();
            }
            const QPixmap screenshot = target->grab();
            if (screenshot.isNull()
                || !screenshot.save(screenshotPath, "PNG")) {
                printError(QStringLiteral("无法保存截图：%1")
                               .arg(screenshotPath));
                QCoreApplication::exit(1);
                return;
            }
            QCoreApplication::quit();
        });
    }
    if (exitAfter > 0) {
        QTimer::singleShot(exitAfter, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
