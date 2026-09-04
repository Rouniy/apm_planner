#include <QtTest>

#include "services/SpeechSettings.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
QSettings settingsFor(const QTemporaryDir &directory, const QString &name)
{
    return QSettings(directory.filePath(name), QSettings::IniFormat);
}
}

class SpeechSettingsTest final : public QObject
{
    Q_OBJECT

private slots:
    void missingSettingDefaultsDisabledWithoutWriting();
    void initialValueUsesCanonicalKey();
    void setEnabledPersistsAndSignalsOnlyLiveChanges();
    void reloadObservesExternalChanges();
};

void SpeechSettingsTest::missingSettingDefaultsDisabledWithoutWriting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("missing.ini"));

    SpeechSettings speech(&settings);

    QVERIFY(!speech.isEnabled());
    QVERIFY(!settings.contains(SpeechSettings::settingsKey()));
}

void SpeechSettingsTest::initialValueUsesCanonicalKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("initial.ini"));
    settings.setValue(QStringLiteral("speechenable"), true);

    SpeechSettings speech(&settings);

    QCOMPARE(SpeechSettings::settingsKey(), QStringLiteral("speechenable"));
    QVERIFY(speech.isEnabled());
}

void SpeechSettingsTest::setEnabledPersistsAndSignalsOnlyLiveChanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("persist.ini"));
    QSettings settings(path, QSettings::IniFormat);
    SpeechSettings speech(&settings);
    QSignalSpy changed(&speech, &SpeechSettings::enabledChanged);

    speech.setEnabled(false);
    QCOMPARE(changed.count(), 0);
    QVERIFY(settings.contains(QStringLiteral("speechenable")));
    QVERIFY(!settings.value(QStringLiteral("speechenable")).toBool());

    speech.setEnabled(true);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.takeFirst().at(0).toBool(), true);
    QVERIFY(speech.isEnabled());

    speech.setEnabled(true);
    QCOMPARE(changed.count(), 0);

    QSettings persisted(path, QSettings::IniFormat);
    QVERIFY(persisted.value(QStringLiteral("speechenable")).toBool());
}

void SpeechSettingsTest::reloadObservesExternalChanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("reload.ini"));
    QSettings settings(path, QSettings::IniFormat);
    SpeechSettings speech(&settings);
    QSignalSpy changed(&speech, &SpeechSettings::enabledChanged);

    {
        QSettings external(path, QSettings::IniFormat);
        external.setValue(QStringLiteral("speechenable"), true);
        external.sync();
    }
    speech.reload();
    QVERIFY(speech.isEnabled());
    QCOMPARE(changed.count(), 1);

    speech.reload();
    QCOMPARE(changed.count(), 1);

    {
        QSettings external(path, QSettings::IniFormat);
        external.remove(QStringLiteral("speechenable"));
        external.sync();
    }
    speech.reload();
    QVERIFY(!speech.isEnabled());
    QCOMPARE(changed.count(), 2);
    QCOMPARE(changed.at(1).at(0).toBool(), false);
}

QTEST_GUILESS_MAIN(SpeechSettingsTest)

#include "test_speechsettings.moc"
