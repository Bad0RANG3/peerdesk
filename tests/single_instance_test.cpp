#include "app/single_instance.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTest>
#include <QUuid>

#include <memory>

class SingleInstanceTest final : public QObject {
    Q_OBJECT

private slots:
    void deliversActivationToPrimaryInstance();
    void notifiesAnInstanceThatIsStillStarting();
    void reportsWhenNoInstanceIsRunning();
    void rejectsDuplicatePrimaryInstance();
    void rejectsSecondPrimaryProcess();
    void allowsReacquireAfterCleanShutdown();
    void allowsReacquireAfterAbruptShutdown();
};

namespace {

QString uniqueServerName()
{
    return QStringLiteral("PeerDesk.test.")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString helperPath()
{
    return QCoreApplication::applicationDirPath()
        + QStringLiteral("/peerdesk-single-instance-host.exe");
}

} // namespace

void SingleInstanceTest::deliversActivationToPrimaryInstance()
{
    const QString name = uniqueServerName();
    QProcess primary;
    primary.setProgram(helperPath());
    primary.setArguments({name});
    primary.setProcessChannelMode(QProcess::SeparateChannels);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));

    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 5000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("READY"));

    QVERIFY(pd::SingleInstance::notifyRunningInstance(name));
    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 3000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("ACTIVATED"));

    QVERIFY(pd::SingleInstance::notifyRunningInstance(name));
    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 3000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("ACTIVATED"));

    primary.kill();
    QVERIFY(primary.waitForFinished(3000));
}

void SingleInstanceTest::notifiesAnInstanceThatIsStillStarting()
{
    const QString name = uniqueServerName();
    QProcess primary;
    primary.setProgram(helperPath());
    primary.setArguments({name});
    primary.setProcessChannelMode(QProcess::SeparateChannels);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));

    QVERIFY(pd::SingleInstance::notifyRunningInstance(name, 3000));
    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 3000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("READY"));
    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 3000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("ACTIVATED"));

    primary.kill();
    QVERIFY(primary.waitForFinished(3000));
}

void SingleInstanceTest::reportsWhenNoInstanceIsRunning()
{
    QVERIFY(!pd::SingleInstance::notifyRunningInstance(
        uniqueServerName(),
        100));
}

void SingleInstanceTest::rejectsDuplicatePrimaryInstance()
{
    const QString name = uniqueServerName();
    pd::SingleInstance primary(name);
    QVERIFY(primary.listen());

    pd::SingleInstance duplicate(name);
    QString error;
    QVERIFY(!duplicate.listen(&error));
    QVERIFY(!duplicate.isListening());
    QVERIFY(error.contains(QStringLiteral("正在运行")));
    QVERIFY(primary.isListening());
}

void SingleInstanceTest::rejectsSecondPrimaryProcess()
{
    const QString name = uniqueServerName();
    QProcess primary;
    primary.setProgram(helperPath());
    primary.setArguments({name});
    primary.setProcessChannelMode(QProcess::SeparateChannels);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(primary.canReadLine(), 5000);
    QCOMPARE(
        QString::fromUtf8(primary.readLine()).trimmed(),
        QStringLiteral("READY"));

    QProcess duplicate;
    duplicate.setProgram(helperPath());
    duplicate.setArguments({name});
    duplicate.setProcessChannelMode(QProcess::SeparateChannels);
    duplicate.start();
    QVERIFY(duplicate.waitForStarted(3000));
    QVERIFY(duplicate.waitForFinished(3000));
    QCOMPARE(duplicate.exitStatus(), QProcess::NormalExit);
    QCOMPARE(duplicate.exitCode(), 1);
    QVERIFY(
        duplicate.readAllStandardError()
            .contains("正在运行"));
    QCOMPARE(primary.state(), QProcess::Running);

    primary.kill();
    QVERIFY(primary.waitForFinished(3000));
}

void SingleInstanceTest::allowsReacquireAfterCleanShutdown()
{
    const QString name = uniqueServerName();
    {
        pd::SingleInstance primary(name);
        QVERIFY(primary.listen());
    }

    pd::SingleInstance replacement(name);
    QVERIFY2(
        replacement.listen(),
        "a cleanly released instance name should be reusable");
    QVERIFY(replacement.isListening());
}

void SingleInstanceTest::allowsReacquireAfterAbruptShutdown()
{
    const QString name = uniqueServerName();
    const auto startPrimary = [&name]() {
        auto process = std::make_unique<QProcess>();
        process->setProgram(helperPath());
        process->setArguments({name});
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->start();
        return process;
    };

    std::unique_ptr<QProcess> primary = startPrimary();
    QVERIFY(primary->waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(primary->canReadLine(), 5000);
    QCOMPARE(
        QString::fromUtf8(primary->readLine()).trimmed(),
        QStringLiteral("READY"));
    primary->kill();
    QVERIFY(primary->waitForFinished(3000));

    primary = startPrimary();
    QVERIFY(primary->waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(primary->canReadLine(), 5000);
    QCOMPARE(
        QString::fromUtf8(primary->readLine()).trimmed(),
        QStringLiteral("READY"));
    primary->kill();
    QVERIFY(primary->waitForFinished(3000));
}

QTEST_GUILESS_MAIN(SingleInstanceTest)

#include "single_instance_test.moc"
