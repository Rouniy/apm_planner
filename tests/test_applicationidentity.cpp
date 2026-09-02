#include <QtTest>

#include "configuration.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

class ApplicationIdentityTest final : public QObject
{
    Q_OBJECT

private slots:
    void visibleTitleContainsVersionOnce();
    void settingsUseFreshProductNamespace();
};

void ApplicationIdentityTest::visibleTitleContainsVersionOnce()
{
    const QString title = QStringLiteral(QGC_APPLICATION_DISPLAY_NAME)
        + QLatin1Char(' ') + QStringLiteral(QGC_APPLICATION_VERSION);
    QCOMPARE(title, QStringLiteral("APM Planner 3.0.0"));
    QVERIFY(!title.contains(QStringLiteral("3.0 3.0.0")));
}

void ApplicationIdentityTest::settingsUseFreshProductNamespace()
{
    QTemporaryDir settingsRoot;
    QVERIFY(settingsRoot.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());
    QCoreApplication::setOrganizationName(
        QStringLiteral(QGC_ORGANIZATION_NAME));
    QCoreApplication::setApplicationName(
        QStringLiteral(QGC_APPLICATION_NAME));

    QCOMPARE(QCoreApplication::applicationName(),
             QStringLiteral("APM Planner 3.0"));
    QVERIFY(QCoreApplication::applicationName()
            != QStringLiteral("APM Planner"));

    QSettings oldProfile(QSettings::IniFormat, QSettings::UserScope,
                         QStringLiteral(QGC_ORGANIZATION_NAME),
                         QStringLiteral("APM Planner"));
    oldProfile.setFallbacksEnabled(false);
    oldProfile.setValue(QStringLiteral("oldProfileSentinel"), true);
    oldProfile.sync();

    QSettings currentProfile;
    currentProfile.setFallbacksEnabled(false);
    QVERIFY(!currentProfile.contains(QStringLiteral("oldProfileSentinel")));
}

QTEST_APPLESS_MAIN(ApplicationIdentityTest)
#include "test_applicationidentity.moc"
