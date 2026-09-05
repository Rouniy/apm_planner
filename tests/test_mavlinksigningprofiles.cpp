#include "services/MavlinkSigningProfiles.h"

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

const QString Profile = QStringLiteral(
    "12345678-9abc-4def-8123-456789abcdef");

QByteArray fingerprint(char value)
{
    return QByteArray(32, value);
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

} // namespace

class MavlinkSigningProfilesTest final : public QObject
{
    Q_OBJECT

private slots:
    void profileIdsAreCanonicalAndBounded();
    void absentLoadIsSideEffectFreeUnlessRequired();
    void saveAndRestartAreStrictAndIdempotent();
    void deletedAndMalformedMetadataStayRequired();
    void mismatchAndCorruptSettingsAreNeverOverwritten();
    void callerGroupIsRejectedAndPreserved();
};

void MavlinkSigningProfilesTest::profileIdsAreCanonicalAndBounded()
{
    QVERIFY(MavlinkSigningProfiles::validProfileId(Profile));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(
        QStringLiteral("{12345678-9abc-4def-8123-456789abcdef}")));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(Profile.toUpper()));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(
        QStringLiteral("12345678-9abc-4def-8123-456789abcdeg")));
    QVERIFY(MavlinkSigningProfiles::validProfileId(
        QStringLiteral("startup-udp-1")));
    QVERIFY(MavlinkSigningProfiles::validProfileId(
        QStringLiteral("startup-udp-65535")));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(
        QStringLiteral("startup-udp-0")));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(
        QStringLiteral("startup-udp-0001")));
    QVERIFY(!MavlinkSigningProfiles::validProfileId(
        QStringLiteral("startup-udp-65536")));
    QVERIFY(MavlinkSigningProfiles::validProfileId(
        MavlinkSigningProfiles::newProfileId()));
}

void MavlinkSigningProfilesTest::
absentLoadIsSideEffectFreeUnlessRequired()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        const auto ordinary = MavlinkSigningProfiles::load(settings, Profile);
        QVERIFY(!ordinary.required);
        QVERIFY(ordinary.fingerprint.isEmpty());
        QVERIFY(ordinary.error.isEmpty());

        const auto required = MavlinkSigningProfiles::load(
            settings, Profile, true);
        QVERIFY(required.required);
        QVERIFY(required.fingerprint.isEmpty());
        QVERIFY(!required.error.isEmpty());

        const auto invalid = MavlinkSigningProfiles::load(
            settings, QStringLiteral("not-a-profile"));
        QVERIFY(invalid.required);
        QVERIFY(!invalid.error.isEmpty());
    }
    QVERIFY(!QFile::exists(path));
}

void MavlinkSigningProfilesTest::saveAndRestartAreStrictAndIdempotent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    const QByteArray expected = fingerprint('A');
    QString error;
    {
        QSettings settings(path, QSettings::IniFormat);
        QVERIFY(MavlinkSigningProfiles::saveRequired(
            settings, Profile, expected, &error));
        QVERIFY(error.isEmpty());
        const QByteArray first = readAll(path);
        QVERIFY(!first.contains(expected));
        QVERIFY(first.contains(expected.toHex()));
        QVERIFY(MavlinkSigningProfiles::saveRequired(
            settings, Profile, expected, &error));
        QCOMPARE(readAll(path), first);
    }
    {
        QSettings restarted(path, QSettings::IniFormat);
        const auto policy = MavlinkSigningProfiles::load(restarted, Profile);
        QVERIFY(policy.required);
        QCOMPARE(policy.fingerprint, expected);
        QVERIFY(policy.error.isEmpty());
    }
}

void MavlinkSigningProfilesTest::
deletedAndMalformedMetadataStayRequired()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString base = QStringLiteral("MAVLinkSigning/Profiles/") + Profile;

    settings.setValue(base + QStringLiteral("/marker"), 1);
    settings.sync();
    auto policy = MavlinkSigningProfiles::load(settings, Profile);
    QVERIFY(policy.required);
    QVERIFY(!policy.error.isEmpty());

    for (const QString &malformed : {
             QString(), QStringLiteral("00"), QString(64, QLatin1Char('g')),
             QString(64, QLatin1Char('A'))}) {
        settings.setValue(base + QStringLiteral("/fingerprint"), malformed);
        settings.sync();
        policy = MavlinkSigningProfiles::load(settings, Profile);
        QVERIFY(policy.required);
        QVERIFY(policy.fingerprint.isEmpty());
        QVERIFY(!policy.error.isEmpty());
    }

    QString error;
    const QByteArray malformedBefore = readAll(path);
    QVERIFY(!MavlinkSigningProfiles::saveRequired(
        settings, Profile, fingerprint('E'), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), malformedBefore);

    settings.remove(base + QStringLiteral("/fingerprint"));
    settings.sync();
    policy = MavlinkSigningProfiles::load(settings, Profile, true);
    QVERIFY(policy.required);
    QVERIFY(!policy.error.isEmpty());
    const QByteArray deletedBefore = readAll(path);
    QVERIFY(!MavlinkSigningProfiles::saveRequired(
        settings, Profile, fingerprint('E'), &error));
    QCOMPARE(readAll(path), deletedBefore);
}

void MavlinkSigningProfilesTest::
mismatchAndCorruptSettingsAreNeverOverwritten()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    const QByteArray first = fingerprint('B');
    QString error;
    {
        QSettings settings(path, QSettings::IniFormat);
        QVERIFY(MavlinkSigningProfiles::saveRequired(
            settings, Profile, first, &error));
        const QByteArray before = readAll(path);
        QVERIFY(!MavlinkSigningProfiles::saveRequired(
            settings, Profile, fingerprint('C'), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAll(path), before);
    }

    const QByteArray corrupt("[MAVLinkSigning/Profiles\ninvalid=value\n");
    QVERIFY(writeAll(path, corrupt));
    QSettings malformed(path, QSettings::IniFormat);
    const auto policy = MavlinkSigningProfiles::load(malformed, Profile);
    QVERIFY(policy.required);
    QVERIFY(!policy.error.isEmpty());
    QVERIFY(!MavlinkSigningProfiles::saveRequired(
        malformed, Profile, first, &error));
    QCOMPARE(readAll(path), corrupt);
}

void MavlinkSigningProfilesTest::callerGroupIsRejectedAndPreserved()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Connections/One"));
    const QString before = settings.group();
    const auto policy = MavlinkSigningProfiles::load(settings, Profile);
    QVERIFY(policy.required);
    QVERIFY(!policy.error.isEmpty());
    QCOMPARE(settings.group(), before);
    QString error;
    QVERIFY(!MavlinkSigningProfiles::saveRequired(
        settings, Profile, fingerprint('D'), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(settings.group(), before);
    settings.endGroup();
    QVERIFY(!QFile::exists(path));
}

QTEST_GUILESS_MAIN(MavlinkSigningProfilesTest)

#include "test_mavlinksigningprofiles.moc"
