#include "comm/MAVLinkSigningClock.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QList>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

namespace {

qint64 timeForTicks(quint64 ticks)
{
    return MAVLinkSigningClock::EpochUnixMs
        + static_cast<qint64>(ticks / 100);
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

class MAVLinkSigningClockTest final : public QObject
{
    Q_OBJECT

private slots:
    void monotonicAcrossWallClockRollback();
    void restartSkipsCompleteReservedBlock();
    void exclusiveLockLivesWithOpenClock();
    void rejectsInvalidWallTimesWithoutState();
    void rejectsCorruptAndTruncatedStateWithoutOverwrite();
    void verifiedFutureAdvancesDurably();
    void exhaustionNeverWrapsOrReuses();
    void failedReservationDoesNotIssueOrAdvance();
    void activeStateReplacementFailsTransactionally();
    void rejectsPathAliasesAndLockSymlink();
    void publishedStateIsOwnerOnly();
};

void MAVLinkSigningClockTest::monotonicAcrossWallClockRollback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningClock clock(directory.filePath(QStringLiteral("clock.bin")));
    QVERIFY(clock.open(timeForTicks(100000)));
    QCOMPARE(clock.current(timeForTicks(100000)), quint64(100000));

    quint64 first = 0;
    quint64 second = 0;
    quint64 afterRollback = 0;
    QVERIFY(clock.next(timeForTicks(100000), &first));
    QVERIFY(clock.next(timeForTicks(100000), &second));
    QVERIFY(clock.next(MAVLinkSigningClock::EpochUnixMs, &afterRollback));
    QCOMPARE(first, quint64(100000));
    QCOMPARE(second, quint64(100001));
    QCOMPARE(afterRollback, quint64(100002));
}

void MAVLinkSigningClockTest::restartSkipsCompleteReservedBlock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    {
        MAVLinkSigningClock first(path);
        QVERIFY(first.open(timeForTicks(100)));
        quint64 timestamp = 0;
        QVERIFY(first.next(timeForTicks(100), &timestamp));
        QCOMPARE(timestamp, quint64(100));
    }
    {
        MAVLinkSigningClock restarted(path);
        QVERIFY(restarted.open(timeForTicks(100)));
        quint64 timestamp = 0;
        QVERIFY(restarted.next(timeForTicks(100), &timestamp));
        QCOMPARE(timestamp, quint64(100100));
    }
}

void MAVLinkSigningClockTest::exclusiveLockLivesWithOpenClock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    MAVLinkSigningClock owner(path);
    QVERIFY(owner.open(timeForTicks(100)));
    const QByteArray before = readAll(path);

    MAVLinkSigningClock contender(path);
    QString error;
    QVERIFY(!contender.open(timeForTicks(100), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!contender.isOpen());
    QCOMPARE(readAll(path), before);
}

void MAVLinkSigningClockTest::rejectsInvalidWallTimesWithoutState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString earlyPath = directory.filePath(QStringLiteral("early.bin"));
    MAVLinkSigningClock early(earlyPath);
    QVERIFY(!early.open(MAVLinkSigningClock::EpochUnixMs - 1));
    QVERIFY(!QFileInfo::exists(earlyPath));
    QCOMPARE(early.current(MAVLinkSigningClock::EpochUnixMs), quint64(0));

    const QString overflowPath = directory.filePath(QStringLiteral("overflow.bin"));
    MAVLinkSigningClock overflow(overflowPath);
    QVERIFY(!overflow.open(std::numeric_limits<qint64>::max()));
    QVERIFY(!QFileInfo::exists(overflowPath));
}

void MAVLinkSigningClockTest::rejectsCorruptAndTruncatedStateWithoutOverwrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString validPath = directory.filePath(QStringLiteral("valid.bin"));
    {
        MAVLinkSigningClock clock(validPath);
        QVERIFY(clock.open(timeForTicks(100)));
    }
    const QByteArray valid = readAll(validPath);
    QVERIFY(valid.size() > 16);

    const QList<QByteArray> invalidStates = {
        valid.left(valid.size() - 1),
        QByteArray(valid).replace(0, 1, QByteArray(1, 'X')),
        QByteArray(valid).replace(valid.size() - 1, 1, QByteArray(1, 'X')),
        QByteArray(4096, 'X')
    };
    for (int index = 0; index < invalidStates.size(); ++index) {
        const QString path = directory.filePath(
            QStringLiteral("invalid-%1.bin").arg(index));
        QVERIFY(writeAll(path, invalidStates.at(index)));
        MAVLinkSigningClock clock(path);
        QVERIFY(!clock.open(timeForTicks(100)));
        QCOMPARE(readAll(path), invalidStates.at(index));
    }
}

void MAVLinkSigningClockTest::verifiedFutureAdvancesDurably()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    {
        MAVLinkSigningClock clock(path);
        QVERIFY(clock.open(timeForTicks(100)));
        QVERIFY(clock.observeVerified(1000000, timeForTicks(100)));
        QCOMPARE(clock.current(timeForTicks(100)), quint64(1000001));
        QVERIFY(clock.observeVerified(500, timeForTicks(100)));
        QCOMPARE(clock.current(timeForTicks(100)), quint64(1000001));
        quint64 timestamp = 0;
        QVERIFY(clock.next(timeForTicks(100), &timestamp));
        QCOMPARE(timestamp, quint64(1000001));
    }
    MAVLinkSigningClock restarted(path);
    QVERIFY(restarted.open(timeForTicks(100)));
    quint64 timestamp = 0;
    QVERIFY(restarted.next(timeForTicks(100), &timestamp));
    QCOMPARE(timestamp, quint64(1100001));
}

void MAVLinkSigningClockTest::exhaustionNeverWrapsOrReuses()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    {
        MAVLinkSigningClock clock(path);
        QVERIFY(clock.open(MAVLinkSigningClock::EpochUnixMs));
        QVERIFY(clock.observeVerified(
            MAVLinkSigningClock::MaxTimestamp - 1,
            MAVLinkSigningClock::EpochUnixMs));
        quint64 timestamp = 0;
        QVERIFY(clock.next(MAVLinkSigningClock::EpochUnixMs, &timestamp));
        QCOMPARE(timestamp, MAVLinkSigningClock::MaxTimestamp);
        QVERIFY(!clock.next(MAVLinkSigningClock::EpochUnixMs, &timestamp));
        QCOMPARE(timestamp, quint64(0));
        QCOMPARE(clock.current(MAVLinkSigningClock::EpochUnixMs), quint64(0));
        QVERIFY(!clock.observeVerified(
            MAVLinkSigningClock::MaxTimestamp,
            MAVLinkSigningClock::EpochUnixMs));
    }
    MAVLinkSigningClock restarted(path);
    QVERIFY(!restarted.open(MAVLinkSigningClock::EpochUnixMs));
}

void MAVLinkSigningClockTest::failedReservationDoesNotIssueOrAdvance()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString liveDirectory = directory.filePath(QStringLiteral("live"));
    const QString movedDirectory = directory.filePath(QStringLiteral("moved"));
    QVERIFY(QDir().mkpath(liveDirectory));
    const QString path = liveDirectory + QStringLiteral("/clock.bin");
    MAVLinkSigningClock clock(path);
    QVERIFY(clock.open(MAVLinkSigningClock::EpochUnixMs));

    QVERIFY(QDir().rename(liveDirectory, movedDirectory));
    quint64 timestamp = 123;
    QVERIFY(!clock.next(timeForTicks(200000), &timestamp));
    QCOMPARE(timestamp, quint64(0));

    QVERIFY(QDir().rename(movedDirectory, liveDirectory));
    QVERIFY(clock.next(MAVLinkSigningClock::EpochUnixMs, &timestamp));
    QCOMPARE(timestamp, quint64(1));
}

void MAVLinkSigningClockTest::activeStateReplacementFailsTransactionally()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    const QString otherPath = directory.filePath(QStringLiteral("other.bin"));
    QByteArray replacement;
    {
        MAVLinkSigningClock other(otherPath);
        QVERIFY(other.open(timeForTicks(500000)));
        replacement = readAll(otherPath);
    }

    MAVLinkSigningClock clock(path);
    QVERIFY(clock.open(MAVLinkSigningClock::EpochUnixMs));
    const QByteArray original = readAll(path);
    QVERIFY(original != replacement);

    QVERIFY(writeAll(path, replacement));
    quint64 timestamp = 123;
    QVERIFY(!clock.next(timeForTicks(200000), &timestamp));
    QCOMPARE(timestamp, quint64(0));
    QCOMPARE(readAll(path), replacement);

    const QByteArray corrupt(original.size(), 'X');
    QVERIFY(writeAll(path, corrupt));
    QVERIFY(!clock.next(timeForTicks(200000), &timestamp));
    QCOMPARE(timestamp, quint64(0));
    QCOMPARE(readAll(path), corrupt);

    // Restoring the exact revision proves both failed reservations left the
    // in-memory issuance floor untouched.
    QVERIFY(writeAll(path, original));
    QVERIFY(clock.next(MAVLinkSigningClock::EpochUnixMs, &timestamp));
    QCOMPARE(timestamp, quint64(1));
}

void MAVLinkSigningClockTest::rejectsPathAliasesAndLockSymlink()
{
    MAVLinkSigningClock relative(QStringLiteral("relative-clock.bin"));
    QVERIFY(!relative.open(MAVLinkSigningClock::EpochUnixMs));

#ifndef Q_OS_WIN
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString realParent = directory.filePath(QStringLiteral("real"));
    const QString aliasParent = directory.filePath(QStringLiteral("alias"));
    QVERIFY(QDir().mkpath(realParent));
    QVERIFY(QFile::link(realParent, aliasParent));
    MAVLinkSigningClock alias(
        aliasParent + QStringLiteral("/aliased-clock.bin"));
    QVERIFY(!alias.open(MAVLinkSigningClock::EpochUnixMs));
    QVERIFY(!QFileInfo::exists(
        realParent + QStringLiteral("/aliased-clock.bin")));

    const QString path = realParent + QStringLiteral("/clock.bin");
    const QString lockTarget = realParent + QStringLiteral("/lock-target");
    QVERIFY(writeAll(lockTarget, QByteArray("do-not-replace")));
    QVERIFY(QFile::link(lockTarget, path + QStringLiteral(".lock")));
    MAVLinkSigningClock symlinkedLock(path);
    QVERIFY(!symlinkedLock.open(MAVLinkSigningClock::EpochUnixMs));
    QCOMPARE(readAll(lockTarget), QByteArray("do-not-replace"));
    QVERIFY(!QFileInfo::exists(path));
#endif
}

void MAVLinkSigningClockTest::publishedStateIsOwnerOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("clock.bin"));
    MAVLinkSigningClock clock(path);
    QVERIFY(clock.open(timeForTicks(100)));

    const QFileDevice::Permissions permissions = QFileInfo(path).permissions();
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
#ifndef Q_OS_WIN
    QVERIFY(!(permissions & (QFileDevice::ExeOwner | QFileDevice::ReadGroup
                             | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                             | QFileDevice::ReadOther | QFileDevice::WriteOther
                             | QFileDevice::ExeOther)));
    const QFileDevice::Permissions lockPermissions =
        QFileInfo(path + QStringLiteral(".lock")).permissions();
    QVERIFY(lockPermissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(lockPermissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!(lockPermissions
              & (QFileDevice::ExeOwner | QFileDevice::ReadGroup
                 | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                 | QFileDevice::ReadOther | QFileDevice::WriteOther
                 | QFileDevice::ExeOther)));
#endif
}

QTEST_APPLESS_MAIN(MAVLinkSigningClockTest)

#include "test_mavlinksigningclock.moc"
