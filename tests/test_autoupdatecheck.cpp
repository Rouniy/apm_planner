#include "ui/AutoUpdateCheck.h"

#include "configuration.h"
#include "logging.h"

#include <QLoggingCategory>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

Q_LOGGING_CATEGORY(apmGeneral, "apm.tests.autoupdate")

#define APM_TEST_STRINGIFY_DETAIL(value) #value
#define APM_TEST_STRINGIFY(value) APM_TEST_STRINGIFY_DETAIL(value)

namespace {
QByteArray manifest(const QByteArray &releases)
{
    return QByteArrayLiteral("{\"releases\":[") + releases
        + QByteArrayLiteral("]}");
}

QByteArray release(const char *platform, const char *type,
                   const char *version, const char *name)
{
    return QByteArrayLiteral("{\"platform\":\"") + platform
        + QByteArrayLiteral("\",\"type\":\"") + type
        + QByteArrayLiteral("\",\"version\":\"") + version
        + QByteArrayLiteral("\",\"url\":\"https://example.invalid/") + name
        + QByteArrayLiteral("\",\"name\":\"") + name
        + QByteArrayLiteral("\"}");
}
}

class AutoUpdateCheckTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void comparesStableAndReleaseCandidateVersions();
    void selectsNewestReleaseForExactChannel();
    void rejectsInvalidManifestAndSkippedVersion();
    void completesInlineChecksAndRejectsConcurrency();
    void reportsLocalNetworkFailure();
    void keepsStableAndBetaSkipVersionsSeparate();
    void hasPackagedLinuxPlatformId();

private:
    QTemporaryDir m_settingsDirectory;
};

void AutoUpdateCheckTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlanner3Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("AutoUpdateCheckTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory.path());
}

void AutoUpdateCheckTest::comparesStableAndReleaseCandidateVersions()
{
    QVERIFY(!AutoUpdateCheck::isVersionNewer(QStringLiteral("3.0.0"),
                                             QStringLiteral("3.0.0")));
    QVERIFY(AutoUpdateCheck::isVersionNewer(QStringLiteral("3.0.0"),
                                            QStringLiteral("3.0.0-rc3")));
    QVERIFY(AutoUpdateCheck::isVersionNewer(QStringLiteral("3.0.0-rc3"),
                                            QStringLiteral("3.0.0-rc2")));
    QVERIFY(!AutoUpdateCheck::isVersionNewer(QStringLiteral("3.0.0-rc4"),
                                             QStringLiteral("3.0.0")));
}

void AutoUpdateCheckTest::selectsNewestReleaseForExactChannel()
{
    const QByteArray json = manifest(
        release("ubuntu64", "stable", "3.1.0", "stable-310") + ','
        + release("ubuntu64", "beta", "9.0.0-rc1", "beta-900") + ','
        + release("ubuntu64", "stable", "4.0.0", "stable-400") + ','
        + release("win", "stable", "8.0.0", "wrong-platform"));

    const AutoUpdateCheck::UpdateSelection stable = AutoUpdateCheck::selectUpdate(
        json, QStringLiteral("ubuntu64"), AutoUpdateCheck::Stable,
        QStringLiteral("3.0.0"));
    QCOMPARE(stable.status, AutoUpdateCheck::UpdateSelection::Available);
    QCOMPARE(stable.version, QStringLiteral("4.0.0"));
    QCOMPARE(stable.releaseType, QStringLiteral("stable"));

    const AutoUpdateCheck::UpdateSelection beta = AutoUpdateCheck::selectUpdate(
        json, QStringLiteral("ubuntu64"), AutoUpdateCheck::Beta,
        QStringLiteral("3.0.0"));
    QCOMPARE(beta.status, AutoUpdateCheck::UpdateSelection::Available);
    QCOMPARE(beta.version, QStringLiteral("9.0.0-rc1"));
    QCOMPARE(beta.releaseType, QStringLiteral("beta"));
}

void AutoUpdateCheckTest::rejectsInvalidManifestAndSkippedVersion()
{
    QCOMPARE(AutoUpdateCheck::selectUpdate(
                 QByteArrayLiteral("not json"), QStringLiteral("ubuntu64"),
                 AutoUpdateCheck::Stable, QStringLiteral("3.0.0")).status,
             AutoUpdateCheck::UpdateSelection::InvalidManifest);
    QCOMPARE(AutoUpdateCheck::selectUpdate(
                 QByteArrayLiteral("{}"), QStringLiteral("ubuntu64"),
                 AutoUpdateCheck::Stable, QStringLiteral("3.0.0")).status,
             AutoUpdateCheck::UpdateSelection::InvalidManifest);

    const QByteArray json = manifest(
        release("ubuntu64", "stable", "4.0.0", "stable-400"));
    QCOMPARE(AutoUpdateCheck::selectUpdate(
                 json, QStringLiteral("ubuntu64"), AutoUpdateCheck::Stable,
                 QStringLiteral("3.0.0"), QStringLiteral("4.0.0")).status,
             AutoUpdateCheck::UpdateSelection::NoUpdate);
}

void AutoUpdateCheckTest::completesInlineChecksAndRejectsConcurrency()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const QByteArray json = manifest(
        release(APM_TEST_STRINGIFY(APP_PLATFORM), "stable", "4.0.0",
                "stable-400"));
    QCOMPARE(file.write(json), qint64(json.size()));
    file.flush();

    AutoUpdateCheck checker;
    QSignalSpy started(&checker, &AutoUpdateCheck::checkStarted);
    QSignalSpy available(&checker, &AutoUpdateCheck::checkAvailable);
    QSignalSpy legacyAvailable(&checker, &AutoUpdateCheck::updateAvailable);
    bool nestedCheckAccepted = true;
    connect(&checker, &AutoUpdateCheck::checkStarted, &checker,
            [&checker, &file, &nestedCheckAccepted]() {
                nestedCheckAccepted = checker.checkForUpdates(
                    AutoUpdateCheck::Beta, AutoUpdateCheck::Inline,
                    QUrl::fromLocalFile(file.fileName()));
            });
    QVERIFY(checker.checkForUpdates(AutoUpdateCheck::Stable,
                                    AutoUpdateCheck::Inline,
                                    QUrl::fromLocalFile(file.fileName())));
    QVERIFY(!nestedCheckAccepted);
    QVERIFY(!checker.checkForUpdates(AutoUpdateCheck::Beta,
                                     AutoUpdateCheck::Inline,
                                     QUrl::fromLocalFile(file.fileName())));
    QCOMPARE(started.count(), 1);
    QTRY_COMPARE(available.count(), 1);
    QCOMPARE(legacyAvailable.count(), 0);
    QVERIFY(!checker.isChecking());
}

void AutoUpdateCheckTest::reportsLocalNetworkFailure()
{
    AutoUpdateCheck checker;
    QSignalSpy failed(&checker, &AutoUpdateCheck::checkFailed);
    QVERIFY(checker.checkForUpdates(
        AutoUpdateCheck::Stable, AutoUpdateCheck::Inline,
        QUrl::fromLocalFile(m_settingsDirectory.filePath(
            QStringLiteral("missing-manifest.json")))));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!checker.isChecking());
}

void AutoUpdateCheckTest::keepsStableAndBetaSkipVersionsSeparate()
{
    AutoUpdateCheck checker;
    checker.setSkippedVersion(AutoUpdateCheck::Stable,
                              QStringLiteral("4.0.0"));
    checker.setSkippedVersion(AutoUpdateCheck::Beta,
                              QStringLiteral("5.0.0-rc1"));

    QCOMPARE(checker.skippedVersion(AutoUpdateCheck::Stable),
             QStringLiteral("4.0.0"));
    QCOMPARE(checker.skippedVersion(AutoUpdateCheck::Beta),
             QStringLiteral("5.0.0-rc1"));
}

void AutoUpdateCheckTest::hasPackagedLinuxPlatformId()
{
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    QCOMPARE(QStringLiteral(APM_TEST_STRINGIFY(APP_PLATFORM)),
             QStringLiteral("ubuntu64"));
#else
    QVERIFY(!QStringLiteral(APM_TEST_STRINGIFY(APP_PLATFORM)).isEmpty());
#endif
}

QTEST_GUILESS_MAIN(AutoUpdateCheckTest)
#include "test_autoupdatecheck.moc"
