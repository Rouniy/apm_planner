#include <QtTest>

#include "ui/MAVLinkReplaySource.h"

class MAVLinkReplaySourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesActiveLeaseAndPublishesMatchingGeneration();
    void rejectsPacketsFromEndedGenerationAfterReload();
    void invalidatesLeaseBeforeEndNotification();
    void endObserverMayInstallSuccessorLease();
    void refusesOverlappingLease();
};

void MAVLinkReplaySourceTest::
exposesActiveLeaseAndPublishesMatchingGeneration()
{
    MAVLinkReplaySource source;
    QSignalSpy observed(&source,
                        &MAVLinkReplaySource::replayMessageObserved);

    const MAVLinkReplayLease lease = source.beginSource(
        QStringLiteral("flight-one.tlog"));
    QVERIFY(lease.isValid());
    QCOMPARE(source.activeLease().generation, lease.generation);
    QCOMPARE(source.activeLease().displayName,
             QStringLiteral("flight-one.tlog"));

    mavlink_message_t message{};
    message.msgid = 330;
    QVERIFY(source.publish(lease.generation, message));
    QCOMPARE(observed.count(), 1);
    QCOMPARE(observed.at(0).at(0).toULongLong(), lease.generation);
    QCOMPARE(observed.at(0).at(1).value<mavlink_message_t>().msgid,
             quint32(330));
}

void MAVLinkReplaySourceTest::rejectsPacketsFromEndedGenerationAfterReload()
{
    MAVLinkReplaySource source;
    QSignalSpy observed(&source,
                        &MAVLinkReplaySource::replayMessageObserved);
    QSignalSpy ended(&source, &MAVLinkReplaySource::replaySourceEnded);

    const MAVLinkReplayLease first = source.beginSource(
        QStringLiteral("first.tlog"));
    QVERIFY(source.endSource(first.generation));
    const MAVLinkReplayLease second = source.beginSource(
        QStringLiteral("second.tlog"));
    QVERIFY(second.isValid());
    QVERIFY(second.generation != first.generation);

    mavlink_message_t staleMessage{};
    staleMessage.msgid = 1;
    QVERIFY(!source.publish(first.generation, staleMessage));
    QVERIFY(!source.endSource(first.generation));
    QCOMPARE(source.activeLease().generation, second.generation);
    QCOMPARE(observed.count(), 0);

    mavlink_message_t currentMessage{};
    currentMessage.msgid = 2;
    QVERIFY(source.publish(second.generation, currentMessage));
    QCOMPARE(observed.count(), 1);
    QCOMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(0).toULongLong(), first.generation);
}

void MAVLinkReplaySourceTest::invalidatesLeaseBeforeEndNotification()
{
    MAVLinkReplaySource source;
    const MAVLinkReplayLease lease = source.beginSource(
        QStringLiteral("ending.tlog"));
    bool observedInvalidLease = false;
    bool stalePublishRejected = false;
    mavlink_message_t message{};

    connect(&source, &MAVLinkReplaySource::replaySourceEnded,
            &source,
            [&source, &observedInvalidLease, &stalePublishRejected,
             &message](quint64 generation) {
                observedInvalidLease = !source.activeLease().isValid();
                stalePublishRejected = !source.publish(generation, message);
            });

    QVERIFY(source.endSource(lease.generation));
    QVERIFY(observedInvalidLease);
    QVERIFY(stalePublishRejected);
}

void MAVLinkReplaySourceTest::endObserverMayInstallSuccessorLease()
{
    MAVLinkReplaySource source;
    const MAVLinkReplayLease first = source.beginSource(
        QStringLiteral("first.tlog"));
    MAVLinkReplayLease successor;

    connect(&source, &MAVLinkReplaySource::replaySourceEnded,
            &source,
            [&source, &successor](quint64) {
                successor = source.beginSource(
                    QStringLiteral("successor.tlog"));
            });

    QVERIFY(source.endSource(first.generation));
    QVERIFY(successor.isValid());
    QCOMPARE(source.activeLease().generation, successor.generation);
    QCOMPARE(source.activeLease().displayName,
             QStringLiteral("successor.tlog"));

    mavlink_message_t message{};
    QVERIFY(!source.publish(first.generation, message));
    QVERIFY(source.publish(successor.generation, message));
}

void MAVLinkReplaySourceTest::refusesOverlappingLease()
{
    MAVLinkReplaySource source;
    QSignalSpy ended(&source, &MAVLinkReplaySource::replaySourceEnded);
    const MAVLinkReplayLease first = source.beginSource(
        QStringLiteral("active.tlog"));

    const MAVLinkReplayLease overlapping = source.beginSource(
        QStringLiteral("must-not-replace.tlog"));
    QVERIFY(!overlapping.isValid());
    QCOMPARE(source.activeLease().generation, first.generation);
    QCOMPARE(source.activeLease().displayName,
             QStringLiteral("active.tlog"));
    QCOMPARE(ended.count(), 0);
}

QTEST_MAIN(MAVLinkReplaySourceTest)
#include "test_mavlinkreplaysource.moc"
