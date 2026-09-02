#include <QtTest>

#include "comm/DroneCanForwardingBroker.h"

#include <QSignalSpy>
#include <QTimer>

namespace {
DroneCanForwardingBroker::Endpoint endpoint(int uasId = 23)
{
    return {uasId, 255, 190, 1, 7};
}
}

class DroneCanForwardingBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void sameBusMultipleOwnersShareOneSession();
    void rejectsConflictingBusAndStaleRelease();
    void ownerDestructionAndKeepaliveAreReferenceCounted();
    void ackRequiresCurrentSessionAndExactEnvelope();
    void transportLossAndReplacementInvalidateOldTraffic();
    void framesRequireCurrentExactEnvelopeAndValidLength();
    void startupTimeoutStopsAndInvalidatesSession();
    void synchronousSendFailureDoesNotReturnLiveLease();
    void transmitRequiresConfirmedCurrentLease();
    void lateAckDuringDrainCannotConfirmReacquiredLease();
    void releaseBeforeDelayedStartDoesNotSendStop();
};

void DroneCanForwardingBrokerTest::sameBusMultipleOwnersShareOneSession()
{
    DroneCanForwardingBroker broker;
    QObject first;
    QObject second;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    const quint64 session = broker.bindSession(endpoint(), 100);
    QVERIFY(session > 0);

    const auto firstLease = broker.acquire(&first, 0, 100);
    QVERIFY(firstLease.isValid());
    auto *keepaliveTimer = broker.findChild<QTimer *>(
        QStringLiteral("droneCanForwardingKeepaliveTimer"));
    QVERIFY(keepaliveTimer);
    QVERIFY(keepaliveTimer->isActive());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.first().at(0).toULongLong(), session);
    QCOMPARE(commands.first().at(1).toInt(), 1);
    QCOMPARE(commands.first().at(2).toInt(), 1);

    const auto duplicate = broker.acquire(&first, 0, 200);
    QCOMPARE(duplicate.leaseGeneration, firstLease.leaseGeneration);
    QCOMPARE(commands.count(), 1);
    const auto secondLease = broker.acquire(&second, 0, 200);
    QVERIFY(secondLease.isValid());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(broker.leaseCount(), 2);

    QVERIFY(broker.release(firstLease, 200));
    QCOMPARE(commands.count(), 1);
    QVERIFY(broker.release(secondLease, 200));
    QCOMPARE(commands.count(), 2);
    QCOMPARE(commands.last().at(2).toInt(), 0);
    QVERIFY(!broker.isForwarding());
    QVERIFY(!keepaliveTimer->isActive());
}

void DroneCanForwardingBrokerTest::rejectsConflictingBusAndStaleRelease()
{
    DroneCanForwardingBroker broker;
    QObject first;
    QObject second;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    broker.bindSession(endpoint(), 0);
    const auto oldLease = broker.acquire(&first, 0, 0);
    QVERIFY(oldLease.isValid());
    QVERIFY(!broker.acquire(&second, 1, 0).isValid());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(broker.activeBusIndex(), 0);

    QVERIFY(broker.release(oldLease, 10));
    const auto replacement = broker.acquire(&first, 1, 10);
    QVERIFY(replacement.isValid());
    QVERIFY(replacement.leaseGeneration > oldLease.leaseGeneration);
    QCOMPARE(broker.activeBusIndex(), 1);
    QVERIFY(!broker.release(oldLease, 10));
    QVERIFY(broker.owns(replacement));
    QCOMPARE(commands.count(), 2);
    QCOMPARE(commands.last().at(2).toInt(), 0);
    broker.tick(1009);
    QCOMPARE(commands.count(), 2);
    broker.tick(1010);
    QCOMPARE(commands.count(), 3);
    QCOMPARE(commands.last().at(2).toInt(), 2);
}

void DroneCanForwardingBrokerTest::ownerDestructionAndKeepaliveAreReferenceCounted()
{
    DroneCanForwardingBroker broker;
    auto *first = new QObject;
    auto *second = new QObject;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    broker.bindSession(endpoint(), 100);
    QVERIFY(broker.acquire(first, 0, 100).isValid());
    QVERIFY(broker.acquire(second, 0, 100).isValid());
    broker.handleAck(broker.sessionGeneration(), 23, 1, 32000,
                     0, 255, 190);
    broker.tick(1099);
    QCOMPARE(commands.count(), 1);
    broker.tick(1100);
    QCOMPARE(commands.count(), 2);
    broker.tick(5100);
    QCOMPARE(commands.count(), 3);

    delete first;
    QCOMPARE(broker.leaseCount(), 1);
    QCOMPARE(commands.count(), 3);
    delete second;
    QCOMPARE(broker.leaseCount(), 0);
    QCOMPARE(commands.count(), 4);
    QCOMPARE(commands.last().at(2).toInt(), 0);
    broker.tick(6100);
    QCOMPARE(commands.count(), 4);
}

void DroneCanForwardingBrokerTest::ackRequiresCurrentSessionAndExactEnvelope()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    const auto ep = endpoint(42);
    const quint64 session = broker.bindSession(ep, 0);
    const auto lease = broker.acquire(&owner, 0, 0);
    QVERIFY(!broker.confirmed(lease));

    const auto ack = [&broker, session](quint64 generation, int uas,
                                        int source, int command,
                                        int result, int targetSystem,
                                        int targetComponent) {
        broker.handleAck(generation, uas, source, command, result,
                         targetSystem, targetComponent);
    };
    ack(session + 1, 42, 1, 32000, 0, 255, 190);
    ack(session, 41, 1, 32000, 0, 255, 190);
    ack(session, 42, 2, 32000, 0, 255, 190);
    ack(session, 42, 1, 32001, 0, 255, 190);
    ack(session, 42, 1, 32000, 0, 254, 190);
    ack(session, 42, 1, 32000, 0, 255, 191);
    QVERIFY(!broker.confirmed(lease));
    ack(session, 42, 1, 32000, 5, 255, 190);
    QVERIFY(!broker.confirmed(lease));
    ack(session, 42, 1, 32000, 0, 255, 190);
    QVERIFY(broker.confirmed(lease));
}

void DroneCanForwardingBrokerTest::transportLossAndReplacementInvalidateOldTraffic()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    QSignalSpy invalidated(
        &broker, &DroneCanForwardingBroker::sessionInvalidated);
    const quint64 firstSession = broker.bindSession(endpoint(20), 0);
    const auto oldLease = broker.acquire(&owner, 0, 0);
    broker.transportLost(firstSession + 1);
    QVERIFY(broker.owns(oldLease));
    broker.transportLost(firstSession);
    QVERIFY(!broker.owns(oldLease));
    QVERIFY(!broker.isBound());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(invalidated.count(), 1);

    const quint64 secondSession = broker.bindSession(endpoint(21), 10);
    QVERIFY(secondSession > firstSession);
    QVERIFY(!broker.owns(oldLease));
    const auto replacement = broker.acquire(&owner, 1, 10);
    QVERIFY(replacement.isValid());
    QCOMPARE(commands.last().at(2).toInt(), 2);

    // A different live endpoint cannot preempt an existing physical session.
    QCOMPARE(broker.bindSession(endpoint(22), 20), quint64(0));
    QVERIFY(broker.owns(replacement));
    QVERIFY(broker.release(replacement, 20));
    QCOMPARE(commands.last().at(0).toULongLong(), secondSession);
    QCOMPARE(commands.last().at(2).toInt(), 0);
    const quint64 thirdSession = broker.bindSession(endpoint(22), 20);
    QVERIFY(thirdSession > secondSession);
}

void DroneCanForwardingBrokerTest::framesRequireCurrentExactEnvelopeAndValidLength()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    QSignalSpy frames(&broker,
                      &DroneCanForwardingBroker::frameAccepted);
    const quint64 session = broker.bindSession(endpoint(77), 0);
    const auto lease = broker.acquire(&owner, 1, 0);
    QVERIFY(lease.isValid());
    const QByteArray classic(8, '\x55');

    broker.handleFrame(session + 1, 77, 1, 255, 190,
                       1, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 76, 1, 255, 190,
                       1, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 77, 2, 255, 190,
                       1, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 254, 190,
                       1, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 255, 191,
                       1, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       0, 0x80000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, classic + QByteArray(1, 0), false, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, QByteArray(65, 0), true, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, QByteArray(9, 0), true, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x00000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0xC0000123U, classic, false, 1);
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, QByteArray(), false, 1);
    QCOMPARE(frames.count(), 0);

    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, classic, false, 2);
    QCOMPARE(frames.count(), 1);
    broker.handleFrame(session, 77, 1, 0, 0,
                       1, 0x80000124U, QByteArray(12, 0), true, 3);
    QCOMPARE(frames.count(), 2);
    QVERIFY(broker.release(lease, 4));
    broker.handleFrame(session, 77, 1, 255, 190,
                       1, 0x80000123U, classic, false, 4);
    QCOMPARE(frames.count(), 2);
}

void DroneCanForwardingBrokerTest::startupTimeoutStopsAndInvalidatesSession()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    QSignalSpy failures(&broker,
                        &DroneCanForwardingBroker::sessionFailed);
    const quint64 session = broker.bindSession(endpoint(), 100);
    const auto lease = broker.acquire(&owner, 0, 100);
    broker.tick(3099);
    QVERIFY(broker.owns(lease));
    broker.tick(3100);
    QVERIFY(!broker.owns(lease));
    QVERIFY(!broker.isBound());
    QCOMPARE(commands.last().at(0).toULongLong(), session);
    QCOMPARE(commands.last().at(2).toInt(), 0);
    QCOMPARE(failures.count(), 1);
}

void DroneCanForwardingBrokerTest::synchronousSendFailureDoesNotReturnLiveLease()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    const quint64 session = broker.bindSession(endpoint(), 100);
    connect(&broker,
            &DroneCanForwardingBroker::forwardingCommandRequested,
            &broker, [&broker](quint64 generation, int, int bus) {
        if (bus != 0) {
            broker.transportLost(generation);
        }
    });

    const auto lease = broker.acquire(&owner, 0, 100);
    QVERIFY(!lease.isValid());
    QVERIFY(!broker.isBound());
    QVERIFY(!broker.isForwarding());
    QCOMPARE(broker.leaseCount(), 0);
    QVERIFY(broker.lastError().contains(QStringLiteral("lost"),
                                        Qt::CaseInsensitive));
    broker.transportLost(session);
}

void DroneCanForwardingBrokerTest::transmitRequiresConfirmedCurrentLease()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    QSignalSpy transmitted(
        &broker, &DroneCanForwardingBroker::frameTransmitRequested);
    const quint64 session = broker.bindSession(endpoint(), 0);
    const auto lease = broker.acquire(&owner, 1, 0);
    QVERIFY(!broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(1, char(0xC5)), false));
    broker.handleAck(session, 23, 1, 32000, 0, 255, 190);

    QVERIFY(broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(1, char(0xC5)), false));
    QCOMPARE(transmitted.count(), 1);
    QCOMPARE(transmitted.first().at(0).toULongLong(), session);
    QCOMPARE(transmitted.first().at(2).toInt(), 1);
    QCOMPARE(transmitted.first().at(3).toUInt(), quint32(0x9E01AAFFU));
    QVERIFY(!broker.transmitFrame(
        lease, 0x1E01AAFFU, QByteArray(1, char(0xC5)), false));
    QVERIFY(!broker.transmitFrame(
        lease, 0xDE01AAFFU, QByteArray(1, char(0xC5)), false));
    QVERIFY(!broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(), false));
    QVERIFY(!broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(9, 0), false));
    QVERIFY(!broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(9, 0), true));
    QVERIFY(broker.transmitFrame(
        lease, 0x9E01AAFFU, QByteArray(12, 0), true));
    QCOMPARE(transmitted.count(), 2);
}

void DroneCanForwardingBrokerTest::lateAckDuringDrainCannotConfirmReacquiredLease()
{
    DroneCanForwardingBroker broker;
    QObject owner;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    const quint64 session = broker.bindSession(endpoint(), 0);
    const auto firstLease = broker.acquire(&owner, 0, 0);
    broker.handleAck(session, 23, 1, 32000, 0, 255, 190);
    QVERIFY(broker.confirmed(firstLease));
    QVERIFY(broker.release(firstLease, 100));
    QCOMPARE(commands.count(), 2);
    QCOMPARE(commands.last().at(2).toInt(), 0);

    const auto replacement = broker.acquire(&owner, 0, 100);
    QVERIFY(replacement.isValid());
    QVERIFY(!broker.confirmed(replacement));
    QCOMPARE(commands.count(), 2);

    // Neither a delayed successful start/keepalive ACK nor a delayed
    // rejection may affect the desired lease while its start is held back.
    broker.handleAck(session, 23, 1, 32000, 0, 255, 190);
    broker.handleAck(session, 23, 1, 32000, 4, 255, 190);
    QVERIFY(broker.isBound());
    QVERIFY(broker.owns(replacement));
    QVERIFY(!broker.confirmed(replacement));

    broker.tick(1099);
    QCOMPARE(commands.count(), 2);
    broker.tick(1100);
    QCOMPARE(commands.count(), 3);
    QCOMPARE(commands.last().at(2).toInt(), 1);
    QVERIFY(!broker.confirmed(replacement));
    broker.handleAck(session, 23, 1, 32000, 0, 255, 190);
    QVERIFY(broker.confirmed(replacement));
}

void DroneCanForwardingBrokerTest::releaseBeforeDelayedStartDoesNotSendStop()
{
    DroneCanForwardingBroker broker;
    QObject firstOwner;
    QSignalSpy commands(
        &broker, &DroneCanForwardingBroker::forwardingCommandRequested);
    broker.bindSession(endpoint(), 0);
    const auto firstLease = broker.acquire(&firstOwner, 0, 0);
    QVERIFY(firstLease.isValid());
    QVERIFY(broker.release(firstLease, 100));
    QCOMPARE(commands.count(), 2);

    auto *destroyedOwner = new QObject;
    const auto destroyedLease = broker.acquire(destroyedOwner, 0, 100);
    QVERIFY(destroyedLease.isValid());
    delete destroyedOwner;
    QCOMPARE(broker.leaseCount(), 0);
    QCOMPARE(commands.count(), 2);

    QObject releasedOwner;
    const auto releasedLease = broker.acquire(&releasedOwner, 0, 200);
    QVERIFY(releasedLease.isValid());
    QVERIFY(broker.release(releasedLease, 300));
    QCOMPARE(commands.count(), 2);

    QObject finalOwner;
    const auto finalLease = broker.acquire(&finalOwner, 0, 1099);
    QVERIFY(finalLease.isValid());
    broker.tick(1099);
    QCOMPARE(commands.count(), 2);
    broker.tick(1100);
    QCOMPARE(commands.count(), 3);
    QCOMPARE(commands.last().at(2).toInt(), 1);
}

QTEST_MAIN(DroneCanForwardingBrokerTest)
#include "test_dronecanforwardingbroker.moc"
