#include <QtTest>

#include "comm/RadioStatusMonitor.h"

#include <QSignalSpy>

#include <cstring>

namespace {

mavlink_message_t radioStatus(quint8 sysid, quint8 compid, quint8 rssi, quint8 remrssi,
                              quint8 noise, quint8 remnoise, quint8 txbuf = 0,
                              quint16 rxerrors = 0, quint16 fixedCount = 0)
{
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_radio_status_pack(sysid, compid, &message, rssi, remrssi, txbuf, noise, remnoise,
                                  rxerrors, fixedCount);
    return message;
}

mavlink_message_t legacyRadio(quint8 sysid, quint8 compid, quint8 rssi, quint8 remrssi,
                              quint8 noise, quint8 remnoise, quint8 txbuf = 0,
                              quint16 rxerrors = 0, quint16 fixedCount = 0)
{
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_radio_pack(sysid, compid, &message, rssi, remrssi, txbuf, noise, remnoise,
                           rxerrors, fixedCount);
    return message;
}

mavlink_message_t heartbeat(quint8 sysid)
{
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_heartbeat_pack(sysid, MAV_COMP_ID_AUTOPILOT1, &message, MAV_TYPE_QUADROTOR,
                               MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
    return message;
}

} // namespace

class RadioStatusMonitorTest final : public QObject
{
    Q_OBJECT

private slots:
    void ignoresNonRadioMessages();
    void decodesBothMessageTypesWithCompleteSnapshot();
    void keepsExactLinkIdentityForDuplicateSysids();
    void readDrivenFilterHoldsForOneSecond();
    void messagesReplaceTheSampleWithoutSteppingTheFilter();
    void remoteSnrUsesRemoteFieldsIndependently();
    void forgetAndClearDropState();
    void backwardClockReanchorsWithoutStepping();
};

void RadioStatusMonitorTest::ignoresNonRadioMessages()
{
    RadioStatusMonitor monitor;
    QSignalSpy samples(&monitor, &RadioStatusMonitor::sampleReceived);
    QVERIFY(!RadioStatusMonitor::IsRadioStatusMessage(MAVLINK_MSG_ID_HEARTBEAT));
    QVERIFY(RadioStatusMonitor::IsRadioStatusMessage(MAVLINK_MSG_ID_RADIO_STATUS));
    QVERIFY(RadioStatusMonitor::IsRadioStatusMessage(MAVLINK_MSG_ID_RADIO));

    QVERIFY(!monitor.observe(1, heartbeat(1), 10));
    QVERIFY(!monitor.observe(-1, radioStatus(51, 68, 150, 93, 55, 55), 10));
    QVERIFY(!monitor.hasSample(1));
    QVERIFY(!monitor.hasSample(-1));
    QVERIFY(!monitor.lastSample(1).isValid());
    QCOMPARE(monitor.localSnrDb(1, 10), 0.0);
    QCOMPARE(monitor.remoteSnrDb(1, 10), 0.0);
    QCOMPARE(samples.count(), 0);
    QVERIFY(monitor.linkIds().isEmpty()); // rejected messages/links create no state
}

void RadioStatusMonitorTest::decodesBothMessageTypesWithCompleteSnapshot()
{
    RadioStatusMonitor monitor;
    QSignalSpy samples(&monitor, &RadioStatusMonitor::sampleReceived);

    // SiK radio identity ('3' / 'D') on link 7.
    QVERIFY(monitor.observe(7, radioStatus(51, 68, 150, 93, 55, 55, 42, 3, 9), 1000));
    QVERIFY(monitor.hasSample(7));
    RadioStatusSample sample = monitor.lastSample(7);
    QVERIFY(sample.isValid());
    QCOMPARE(sample.receivedMs, qint64(1000));
    QCOMPARE(sample.messageId, quint32(MAVLINK_MSG_ID_RADIO_STATUS));
    QCOMPARE(int(sample.systemId), 51);
    QCOMPARE(int(sample.componentId), 68);
    QCOMPARE(int(sample.rssi), 150);
    QCOMPARE(int(sample.remrssi), 93);
    QCOMPARE(int(sample.noise), 55);
    QCOMPARE(int(sample.remnoise), 55);
    QCOMPARE(int(sample.txbuf), 42);
    QCOMPARE(int(sample.rxerrors), 3);
    QCOMPARE(int(sample.fixedCount), 9);
    QCOMPARE(sample.localSnrDbRaw(), 50.0);  // (150 - 55) / 1.9
    QCOMPARE(sample.remoteSnrDbRaw(), 20.0); // (93 - 55) / 1.9
    QCOMPARE(samples.count(), 1);
    QCOMPARE(samples.last().at(0).toInt(), 7);
    QCOMPARE(samples.last().at(1).value<RadioStatusSample>().rssi, quint8(150));

    // Legacy RADIO (#166) from the vehicle's own identity replaces the sample.
    QVERIFY(monitor.observe(7, legacyRadio(1, 1, 120, 100, 60, 50, 7, 1, 2), 2000));
    sample = monitor.lastSample(7);
    QCOMPARE(sample.receivedMs, qint64(2000));
    QCOMPARE(sample.messageId, quint32(MAVLINK_MSG_ID_RADIO));
    QCOMPARE(int(sample.systemId), 1);
    QCOMPARE(int(sample.componentId), 1);
    QCOMPARE(int(sample.rssi), 120);
    QCOMPARE(int(sample.remrssi), 100);
    QCOMPARE(int(sample.noise), 60);
    QCOMPARE(int(sample.remnoise), 50);
    QCOMPARE(int(sample.txbuf), 7);
    QCOMPARE(int(sample.rxerrors), 1);
    QCOMPARE(int(sample.fixedCount), 2);
    QCOMPARE(samples.count(), 2);
    QCOMPARE(monitor.linkIds(), QList<int>{7});
}

void RadioStatusMonitorTest::keepsExactLinkIdentityForDuplicateSysids()
{
    RadioStatusMonitor monitor;
    // The same vehicle sysid 1 is visible on two physical links.
    QVERIFY(monitor.observe(1, radioStatus(1, 1, 150, 93, 55, 55), 0));
    QVERIFY(monitor.observe(2, radioStatus(1, 1, 93, 150, 55, 55), 0));
    QCOMPARE(monitor.linkIds(), (QList<int>{1, 2}));
    QCOMPARE(monitor.localSnrDb(1, 0), 25.0); // 50 * 0.5
    QCOMPARE(monitor.localSnrDb(2, 0), 10.0); // 20 * 0.5
    QCOMPARE(monitor.lastSample(1).rssi, quint8(150));
    QCOMPARE(monitor.lastSample(2).rssi, quint8(93));

    QSignalSpy forgotten(&monitor, &RadioStatusMonitor::linkForgotten);
    monitor.forgetLink(1);
    QCOMPARE(forgotten.count(), 1);
    QCOMPARE(forgotten.last().at(0).toInt(), 1);
    QVERIFY(!monitor.hasSample(1));
    QCOMPARE(monitor.localSnrDb(1, 5000), 0.0);
    QVERIFY(monitor.hasSample(2));
    QCOMPARE(monitor.localSnrDb(2, 500), 10.0); // link 2 untouched, still held
    monitor.forgetLink(1);                         // idempotent, no second signal
    QCOMPARE(forgotten.count(), 1);
}

void RadioStatusMonitorTest::readDrivenFilterHoldsForOneSecond()
{
    RadioStatusMonitor monitor;
    QVERIFY(monitor.observe(3, radioStatus(51, 68, 150, 93, 55, 55), 0));
    // First read steps immediately from 0: 50 * 0.5 + 0 * 0.5.
    QCOMPARE(monitor.localSnrDb(3, 0), 25.0);
    // Within the hold the cached value is returned.
    QCOMPARE(monitor.localSnrDb(3, 1), 25.0);
    QCOMPARE(monitor.localSnrDb(3, 999), 25.0);
    // Exactly 1000 ms after the last step is a new step (>=, MP10 AddSeconds(1) > Now).
    QCOMPARE(monitor.localSnrDb(3, 1000), 37.5);
    QCOMPARE(monitor.localSnrDb(3, 1999), 37.5);
    QCOMPARE(monitor.localSnrDb(3, 2000), 43.75);
    // Reads far apart step once per read, never once per elapsed second.
    QCOMPARE(monitor.localSnrDb(3, 60000), 46.875);
}

void RadioStatusMonitorTest::messagesReplaceTheSampleWithoutSteppingTheFilter()
{
    RadioStatusMonitor monitor;
    QVERIFY(monitor.observe(3, radioStatus(51, 68, 150, 93, 55, 55), 0));
    QCOMPARE(monitor.localSnrDb(3, 0), 25.0);
    // Several packets inside the hold: only the raw sample changes.
    QVERIFY(monitor.observe(3, radioStatus(51, 68, 131, 93, 55, 55), 300)); // raw 40
    QVERIFY(monitor.observe(3, radioStatus(51, 68, 74, 93, 55, 55), 600));  // raw 10
    QCOMPARE(monitor.lastSample(3).rssi, quint8(74));
    QCOMPARE(monitor.localSnrDb(3, 700), 25.0); // still held
    // The next step uses the LATEST raw sample only (no catch-up steps).
    QCOMPARE(monitor.localSnrDb(3, 1000), 17.5); // 10 * 0.5 + 25 * 0.5
    // A packet after a long silence does not step by itself either.
    QVERIFY(monitor.observe(3, radioStatus(51, 68, 150, 93, 55, 55), 30000));
    QCOMPARE(monitor.lastSample(3).receivedMs, qint64(30000));
    QCOMPARE(monitor.localSnrDb(3, 30000), 33.75); // one step: 50 * 0.5 + 17.5 * 0.5
}

void RadioStatusMonitorTest::remoteSnrUsesRemoteFieldsIndependently()
{
    RadioStatusMonitor monitor;
    QVERIFY(monitor.observe(4, radioStatus(51, 68, 150, 93, 55, 55), 0));
    QCOMPARE(monitor.remoteSnrDb(4, 0), 10.0); // (93 - 55) / 1.9 * 0.5
    QCOMPARE(monitor.remoteSnrDb(4, 500), 10.0);
    // The local filter has its own hold clock: its first read happens later.
    QCOMPARE(monitor.localSnrDb(4, 500), 25.0);
    QCOMPARE(monitor.remoteSnrDb(4, 1000), 15.0);
    QCOMPARE(monitor.localSnrDb(4, 1000), 25.0); // local stepped at 500, held until 1500
    QCOMPARE(monitor.localSnrDb(4, 1500), 37.5);
}

void RadioStatusMonitorTest::forgetAndClearDropState()
{
    RadioStatusMonitor monitor;
    QVERIFY(monitor.observe(1, radioStatus(51, 68, 150, 93, 55, 55), 0));
    QVERIFY(monitor.observe(2, radioStatus(51, 68, 150, 93, 55, 55), 0));
    QCOMPARE(monitor.localSnrDb(1, 0), 25.0);
    QCOMPARE(monitor.localSnrDb(1, 1000), 37.5);

    QSignalSpy forgotten(&monitor, &RadioStatusMonitor::linkForgotten);
    monitor.clear();
    QCOMPARE(forgotten.count(), 2);
    QVERIFY(monitor.linkIds().isEmpty());
    QCOMPARE(monitor.localSnrDb(1, 2000), 0.0);
    QCOMPARE(monitor.localSnrDb(2, 2000), 0.0);
    // A fresh sample after clear() restarts the filter from zero.
    QVERIFY(monitor.observe(1, radioStatus(51, 68, 150, 93, 55, 55), 3000));
    QCOMPARE(monitor.localSnrDb(1, 3000), 25.0);
    monitor.clear();
    monitor.clear(); // empty clear emits nothing
    QCOMPARE(forgotten.count(), 3);
}

void RadioStatusMonitorTest::backwardClockReanchorsWithoutStepping()
{
    RadioStatusMonitor monitor;
    QVERIFY(monitor.observe(9, radioStatus(51, 68, 150, 93, 55, 55), 5000));
    QCOMPARE(monitor.localSnrDb(9, 5000), 25.0);
    // The clock jumps back: no step, value kept, hold restarts at 4000.
    QCOMPARE(monitor.localSnrDb(9, 4000), 25.0);
    QCOMPARE(monitor.localSnrDb(9, 4999), 25.0);
    QCOMPARE(monitor.localSnrDb(9, 5000), 37.5); // 1000 ms after the new anchor
    QCOMPARE(monitor.localSnrDb(9, 5999), 37.5);
    // A second jump back followed by a small advance still does not step.
    QCOMPARE(monitor.localSnrDb(9, 100), 37.5);
    QCOMPARE(monitor.localSnrDb(9, 1099), 37.5);
    QCOMPARE(monitor.localSnrDb(9, 1100), 43.75);
    // Negative timestamps are just numbers: validity is a flag, not the sign of receivedMs.
    QVERIFY(monitor.observe(10, radioStatus(51, 68, 150, 93, 55, 55), -5000));
    QCOMPARE(monitor.lastSample(10).receivedMs, qint64(-5000));
    QCOMPARE(monitor.localSnrDb(10, -5000), 25.0);
    QCOMPARE(monitor.localSnrDb(10, -4000), 37.5);
}

QTEST_GUILESS_MAIN(RadioStatusMonitorTest)
#include "test_radiostatusmonitor.moc"
