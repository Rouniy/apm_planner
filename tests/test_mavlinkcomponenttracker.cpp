#include "uas/MAVLinkComponentTracker.h"

#include <QtTest/QTest>

class MAVLinkComponentTrackerTest final : public QObject
{
    Q_OBJECT

private slots:
    void tracksExtendedMavlink2MessageIds();
    void keepsPreferredComponentAsPrimary();
};

void MAVLinkComponentTrackerTest::tracksExtendedMavlink2MessageIds()
{
    MAVLinkComponentTracker tracker;

    auto result = tracker.observe(11030, 1);
    QVERIFY(!result.multiComponentSourceDetected);
    QVERIFY(!result.wrongComponent);

    result = tracker.observe(11030, 42);
    QVERIFY(result.multiComponentSourceDetected);
    QVERIFY(result.wrongComponent);

    result = tracker.observe(11030, 1);
    QVERIFY(result.multiComponentSourceDetected);
    QVERIFY(!result.wrongComponent);

    result = tracker.observe(0x00ffffffU, 1);
    QVERIFY(!result.multiComponentSourceDetected);
    QVERIFY(!result.wrongComponent);
}

void MAVLinkComponentTrackerTest::keepsPreferredComponentAsPrimary()
{
    MAVLinkComponentTracker tracker;

    tracker.observe(30, 1);
    auto result = tracker.observe(30, 201, true);
    QVERIFY(!result.multiComponentSourceDetected);
    QVERIFY(!result.wrongComponent);

    result = tracker.observe(30, 1);
    QVERIFY(result.multiComponentSourceDetected);
    QVERIFY(result.wrongComponent);
}

QTEST_APPLESS_MAIN(MAVLinkComponentTrackerTest)
#include "test_mavlinkcomponenttracker.moc"
