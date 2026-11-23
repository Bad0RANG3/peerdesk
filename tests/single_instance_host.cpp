#include "app/single_instance.h"

#include <QCoreApplication>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("peerdesk-single-instance-host"));

    const QStringList arguments = application.arguments();
    if (arguments.size() != 2) {
        std::fprintf(stderr, "usage: host <instance-name>\n");
        return 2;
    }

    pd::SingleInstance instance(arguments.at(1));
    QString error;
    if (!instance.listen(&error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }

    QObject::connect(
        &instance,
        &pd::SingleInstance::activationRequested,
        &application,
        []() {
            std::printf("ACTIVATED\n");
            std::fflush(stdout);
        });
    std::printf("READY\n");
    std::fflush(stdout);
    return application.exec();
}
