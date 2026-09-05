#include "comm/LogDownloadTracker.h"

#include <QtTest/QTest>

#include <limits>

class LogDownloadTrackerTest final : public QObject
{
    Q_OBJECT

private slots:
    void ignoresStaleShortPacketBelowHighestOffset();
    void corruptFarOffsetDoesNotPoisonEndInference();
    void defersFarShortEndUntilStreamIsQuiet();
    void countsOverlapsAndDuplicatesOnlyOnce();
    void supportsStreamAndFiftyPacketRepairRequests();
    void acceptsAlignedZeroLengthTerminator();
    void rejectsOversizeAndOverflowWithoutMutation();
    void enforcesRangeCapacityTransactionally();
    void explicitTrustedLengthAndResetAreDeterministic();
};

using Result = LogDownloadTracker::AddResult;

void LogDownloadTrackerTest::ignoresStaleShortPacketBelowHighestOffset()
{
    LogDownloadTracker tracker;

    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(0, 40, true), Result::Accepted);
    QVERIFY(!tracker.totalLength());

    QCOMPARE(tracker.add(180, 30, true), Result::Accepted);
    QVERIFY(tracker.totalLength());
    QCOMPARE(*tracker.totalLength(), quint32(210));
}

void LogDownloadTrackerTest::corruptFarOffsetDoesNotPoisonEndInference()
{
    LogDownloadTracker tracker;

    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(0x40000000U, 90, true), Result::Accepted);
    QVERIFY(!tracker.totalLength());

    QCOMPARE(tracker.add(180, 30, true), Result::Accepted);
    QVERIFY(tracker.totalLength());
    QCOMPARE(*tracker.totalLength(), quint32(210));
    QCOMPARE(tracker.coveredBytes(), quint64(210));
}

void LogDownloadTrackerTest::defersFarShortEndUntilStreamIsQuiet()
{
    LogDownloadTracker tracker;
    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);

    QCOMPARE(tracker.add(1000000, 40, true), Result::Accepted);
    QCOMPARE(tracker.add(1000200, 20, true), Result::Accepted);
    QVERIFY(!tracker.totalLength());
    QVERIFY(tracker.acceptPendingTotalLength());
    QVERIFY(tracker.totalLength());
    QCOMPARE(*tracker.totalLength(), quint32(1000220));

    tracker.reset();
    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(1000000, 40, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QVERIFY(!tracker.acceptPendingTotalLength());
    QVERIFY(!tracker.totalLength());
}

void LogDownloadTrackerTest::countsOverlapsAndDuplicatesOnlyOnce()
{
    LogDownloadTracker tracker;

    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(30, 90, false), Result::Accepted);

    QCOMPARE(tracker.coveredBytes(), quint64(180));
    QCOMPARE(tracker.rangeCount(), 1);
    QCOMPARE(tracker.frontierEnd(), quint32(180));
    const LogDownloadRequest next = tracker.nextRequest(50 * 90);
    QCOMPARE(next.offset, quint32(180));
    QCOMPARE(next.count, std::numeric_limits<quint32>::max());
}

void LogDownloadTrackerTest::supportsStreamAndFiftyPacketRepairRequests()
{
    LogDownloadTracker tracker;
    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(10000, 20, true), Result::Accepted);

    const LogDownloadRequest stream = tracker.nextRequest(50 * 90);
    QCOMPARE(stream.offset, quint32(90));
    QCOMPARE(stream.count, std::numeric_limits<quint32>::max());

    QVERIFY(tracker.acceptPendingTotalLength());
    const LogDownloadRequest firstRepair = tracker.nextRequest(50 * 90);
    QCOMPARE(firstRepair.offset, quint32(90));
    QCOMPARE(firstRepair.count, quint32(4500));

    QCOMPARE(tracker.add(90, 90, false), Result::Accepted);
    const LogDownloadRequest nextRepair = tracker.nextRequest(50 * 90);
    QCOMPARE(nextRepair.offset, quint32(180));
    QCOMPARE(nextRepair.count, quint32(4500));
}

void LogDownloadTrackerTest::acceptsAlignedZeroLengthTerminator()
{
    LogDownloadTracker tracker;
    QCOMPARE(tracker.add(0, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(90, 90, true), Result::Accepted);
    QCOMPARE(tracker.add(180, 0, true), Result::Accepted);

    QVERIFY(tracker.totalLength());
    QCOMPARE(*tracker.totalLength(), quint32(180));
    QCOMPARE(tracker.coveredBytes(), quint64(180));
    QVERIFY(tracker.complete());
}

void LogDownloadTrackerTest::rejectsOversizeAndOverflowWithoutMutation()
{
    LogDownloadTracker tracker;

    QCOMPARE(tracker.add(0, quint8(91), true), Result::InvalidRange);
    QCOMPARE(tracker.add(std::numeric_limits<quint32>::max() - 10,
                         quint8(90), true),
             Result::InvalidRange);
    QCOMPARE(tracker.coveredBytes(), quint64(0));
    QCOMPARE(tracker.rangeCount(), 0);
    QVERIFY(!tracker.totalLength());
    QVERIFY(!tracker.acceptPendingTotalLength());
}

void LogDownloadTrackerTest::enforcesRangeCapacityTransactionally()
{
    LogDownloadTracker tracker;
    for (std::size_t index = 0;
         index < LogDownloadTracker::MaximumRanges; ++index) {
        QCOMPARE(tracker.add(static_cast<quint32>(index * 2), 1, false),
                 Result::Accepted);
    }

    QCOMPARE(tracker.rangeCount(),
             static_cast<int>(LogDownloadTracker::MaximumRanges));
    const quint64 coveredBefore = tracker.coveredBytes();
    QCOMPARE(tracker.add(
                     static_cast<quint32>(
                             LogDownloadTracker::MaximumRanges * 2),
                     1, false),
             Result::RangeCapacityExceeded);
    QCOMPARE(tracker.coveredBytes(), coveredBefore);
    QCOMPARE(tracker.rangeCount(),
             static_cast<int>(LogDownloadTracker::MaximumRanges));

    // Existing and bridging data can still be represented without exceeding
    // the cap; a service normally treats the earlier result as fatal.
    QCOMPARE(tracker.add(0, 1, false), Result::Accepted);
    QCOMPARE(tracker.add(1, 1, false), Result::Accepted);
    QCOMPARE(tracker.rangeCount(),
             static_cast<int>(LogDownloadTracker::MaximumRanges - 1));
}

void LogDownloadTrackerTest::explicitTrustedLengthAndResetAreDeterministic()
{
    LogDownloadTracker tracker(std::optional<quint32>(100));
    QCOMPARE(tracker.add(0, 90, false), Result::Accepted);
    QCOMPARE(tracker.add(90, 10, false), Result::Accepted);
    QCOMPARE(tracker.coveredBytes(), quint64(100));
    QVERIFY(tracker.complete());

    tracker.reset();
    QVERIFY(!tracker.totalLength());
    QCOMPARE(tracker.coveredBytes(), quint64(0));
    QCOMPARE(tracker.rangeCount(), 0);
    QVERIFY(!tracker.complete());
}

QTEST_APPLESS_MAIN(LogDownloadTrackerTest)

#include "test_logdownloadtracker.moc"
