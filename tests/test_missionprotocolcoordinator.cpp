#include "comm/MissionProtocolCoordinator.h"

#include <QtTest>

class MissionProtocolCoordinatorTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsCollidingLease();
    void sameOwnerAndTypeIsIdempotent();
    void staleTokenCannotReleaseNewLease();
    void destroyedOwnerReleasesLease();
};

void MissionProtocolCoordinatorTest::rejectsCollidingLease()
{
    MissionProtocolCoordinator coordinator;
    QObject missionOwner;
    QObject fenceOwner;

    const auto mission = coordinator.tryAcquire(
        &missionOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(mission.isValid());
    QCOMPARE(coordinator.owner(), &missionOwner);
    QCOMPARE(coordinator.missionType(),
             MissionProtocolCoordinator::MissionType::Mission);
    QCOMPARE(coordinator.generation(), mission.generation);
    QVERIFY(coordinator.owns(mission));

    const auto collision = coordinator.tryAcquire(
        &fenceOwner, MissionProtocolCoordinator::MissionType::Fence);
    QVERIFY(!collision.isValid());
    QVERIFY(!coordinator.owns(collision));
    QVERIFY(!coordinator.release(collision));
    QCOMPARE(coordinator.owner(), &missionOwner);
    QVERIFY(coordinator.owns(mission));

    QVERIFY(coordinator.release(mission));
    QVERIFY(coordinator.owner() == nullptr);

    const auto fence = coordinator.tryAcquire(
        &fenceOwner, MissionProtocolCoordinator::MissionType::Fence);
    QVERIFY(fence.isValid());
    QVERIFY(fence.generation > mission.generation);
    QCOMPARE(coordinator.owner(), &fenceOwner);
    QCOMPARE(coordinator.missionType(),
             MissionProtocolCoordinator::MissionType::Fence);
}

void MissionProtocolCoordinatorTest::sameOwnerAndTypeIsIdempotent()
{
    MissionProtocolCoordinator coordinator;
    QObject leaseOwner;

    const auto first = coordinator.tryAcquire(
        &leaseOwner, MissionProtocolCoordinator::MissionType::Rally);
    const auto repeated = coordinator.tryAcquire(
        &leaseOwner, MissionProtocolCoordinator::MissionType::Rally);
    QVERIFY(first.isValid());
    QVERIFY(repeated.isValid());
    QCOMPARE(repeated.owner.data(), first.owner.data());
    QCOMPARE(repeated.missionType, first.missionType);
    QCOMPARE(repeated.generation, first.generation);
    QVERIFY(coordinator.owns(first));
    QVERIFY(coordinator.owns(repeated));

    const auto differentType = coordinator.tryAcquire(
        &leaseOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(!differentType.isValid());
    QCOMPARE(coordinator.missionType(),
             MissionProtocolCoordinator::MissionType::Rally);

    QVERIFY(coordinator.release(repeated));
    QVERIFY(!coordinator.release(first));
}

void MissionProtocolCoordinatorTest::staleTokenCannotReleaseNewLease()
{
    MissionProtocolCoordinator coordinator;
    QObject leaseOwner;

    const auto oldLease = coordinator.tryAcquire(
        &leaseOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(coordinator.release(oldLease));

    const auto newLease = coordinator.tryAcquire(
        &leaseOwner, MissionProtocolCoordinator::MissionType::Fence);
    QVERIFY(newLease.isValid());
    QVERIFY(newLease.generation > oldLease.generation);
    QVERIFY(!coordinator.owns(oldLease));
    QVERIFY(!coordinator.release(oldLease));
    QCOMPARE(coordinator.owner(), &leaseOwner);
    QVERIFY(coordinator.owns(newLease));
}

void MissionProtocolCoordinatorTest::destroyedOwnerReleasesLease()
{
    MissionProtocolCoordinator coordinator;
    auto *destroyedOwner = new QObject;
    const auto destroyedLease = coordinator.tryAcquire(
        destroyedOwner, MissionProtocolCoordinator::MissionType::Rally);
    QVERIFY(destroyedLease.isValid());
    const quint64 destroyedGeneration = destroyedLease.generation;

    delete destroyedOwner;

    QVERIFY(coordinator.owner() == nullptr);
    QVERIFY(!destroyedLease.isValid());
    QVERIFY(!coordinator.owns(destroyedLease));
    QVERIFY(!coordinator.release(destroyedLease));
    QCOMPARE(coordinator.generation(), destroyedGeneration);

    QObject nextOwner;
    const auto nextLease = coordinator.tryAcquire(
        &nextOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(nextLease.isValid());
    QVERIFY(nextLease.generation > destroyedGeneration);
    QVERIFY(coordinator.owns(nextLease));
}

QTEST_MAIN(MissionProtocolCoordinatorTest)
#include "test_missionprotocolcoordinator.moc"
