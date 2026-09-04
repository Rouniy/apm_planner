#include "comm/MovingBaseService.h"
#include "comm/MovingBasePositionStore.h"
#include "comm/VehicleTargetManager.h"

#include <QSignalSpy>
#include <QtTest>

#include <limits>

namespace
{

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Link %1").arg(linkId);
    result.componentName = QStringLiteral("Component %1").arg(componentId);
    return result;
}

NmeaGgaFix ggaFix(double latitude, double longitude,
                  double altitude = 42.5)
{
    NmeaGgaFix result;
    result.latitude = latitude;
    result.longitude = longitude;
    result.altitudeM = altitude;
    result.satellites = 14;
    result.hdop = 0.7;
    result.fixQuality = 4;
    result.hasGeoidSeparation = true;
    result.geoidSeparationM = 18.25;
    return result;
}

class Fixture
{
public:
    Fixture()
        : store(&targets)
        , service(
              &targets, &store,
              MovingBaseService::TimingSeams{
                  [this]() { return nowMs; },
                  [this](int delayMs) {
                      armedDelayMs = delayMs;
                      ++armCount;
                  },
                  [this]() {
                      armedDelayMs = -1;
                      ++disarmCount;
                  }})
    {
    }

    VehicleTargetLease select(const VehicleEndpoint &value)
    {
        targets.observeEndpoint(value, true);
        if (!targets.acquireTarget().endpoint.sameIdentity(value)) {
            targets.selectTarget(value.linkId, value.systemId,
                                 value.componentId);
        }
        return targets.acquireTarget();
    }

    qint64 nowMs = 0;
    int armedDelayMs = -1;
    int armCount = 0;
    int disarmCount = 0;
    VehicleTargetManager targets;
    MovingBasePositionStore store;
    MovingBaseService service;
};

MovingBasePositionSnapshot publishedSnapshot(const QSignalSpy &spy,
                                             int index)
{
    return qvariant_cast<MovingBasePositionSnapshot>(
        spy.at(index).at(1));
}

MovingBaseService::RequestResult endedResult(const QSignalSpy &spy,
                                             int index)
{
    return qvariant_cast<MovingBaseService::RequestResult>(
        spy.at(index).at(1));
}

} // namespace

class MovingBaseServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void validatesRateOwnerAndExactTarget();
    void unsettledTargetCannotStart();
    void firstValidFixPublishesImmediatelyAsAmsl();
    void rateCapPublishesOnlyNewestQueuedFix();
    void staleTimeoutMatchesRate_data();
    void staleTimeoutMatchesRate();
    void staleFixClearsButSessionStaysActive();
    void noFixClearsPendingAndPublishedPosition();
    void targetLinkAndOwnerLifecycleStopAndClear();
    void staleTokenCannotAffectNewSession();
    void reentrantTargetChangeDuringPublishStopsOldSession();
    void synchronousObserversMayDeleteTheService();
};

void MovingBaseServiceTest::validatesRateOwnerAndExactTarget()
{
    QCOMPARE(MovingBaseService::supportedRates(),
             QList<double>({0.25, 0.5, 1.0, 2.0}));
    QVERIFY(MovingBaseService::isSupportedRate(0.25));
    QVERIFY(MovingBaseService::isSupportedRate(2.0));
    QVERIFY(!MovingBaseService::isSupportedRate(0.0));
    QVERIFY(!MovingBaseService::isSupportedRate(0.75));

    Fixture fixture;
    const VehicleTargetLease first = fixture.select(endpoint(3));
    fixture.targets.observeEndpoint(endpoint(9));
    QObject owner;
    QObject competingOwner;

    QCOMPARE(fixture.service.start(nullptr, first, 0.5),
             MovingBaseService::RequestResult::InvalidOwner);
    QCOMPARE(fixture.service.start(&owner, first, 0.75),
             MovingBaseService::RequestResult::InvalidRate);

    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    QCOMPARE(fixture.service.start(&owner, first, 0.5),
             MovingBaseService::RequestResult::StaleTarget);
    const VehicleTargetLease current = fixture.targets.acquireTarget();
    MovingBaseService::SessionToken session;
    QCOMPARE(fixture.service.start(&owner, current, 2.0, &session),
             MovingBaseService::RequestResult::Started);
    QVERIFY(session.isValid());
    QCOMPARE(fixture.service.rateHz(), 2.0);
    QCOMPARE(fixture.service.staleTimeoutMs(), 5000);
    QCOMPARE(fixture.service.start(&competingOwner, current, 1.0),
             MovingBaseService::RequestResult::Busy);
    QCOMPARE(fixture.service.stop(session),
             MovingBaseService::RequestResult::Stopped);
    QCOMPARE(fixture.service.state(), MovingBaseService::State::Idle);
}

void MovingBaseServiceTest::unsettledTargetCannotStart()
{
    Fixture fixture;
    fixture.select(endpoint(3));
    fixture.targets.observeEndpoint(endpoint(9));
    QObject owner;
    MovingBaseService::RequestResult nestedResult =
        MovingBaseService::RequestResult::Started;
    const QMetaObject::Connection connection = connect(
        &fixture.targets, &VehicleTargetManager::targetGenerationChanged,
        &fixture.targets, [&](qulonglong) {
            nestedResult = fixture.service.start(
                &owner, fixture.targets.acquireTarget(), 0.5);
        });

    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    disconnect(connection);
    QCOMPARE(nestedResult,
             MovingBaseService::RequestResult::TargetUnsettled);
    QVERIFY(!fixture.service.hasActiveSession());
}

void MovingBaseServiceTest::firstValidFixPublishesImmediatelyAsAmsl()
{
    Fixture fixture;
    fixture.nowMs = 100;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject owner;
    MovingBaseService::SessionToken session;
    QSignalSpy published(&fixture.service,
                         &MovingBaseService::fixPublished);

    QCOMPARE(fixture.service.start(&owner, lease, 0.5, &session),
             MovingBaseService::RequestResult::Started);
    NmeaGgaFix invalid = ggaFix(35.1, 33.2);
    invalid.fixQuality = 0;
    QCOMPARE(fixture.service.submitFix(session, invalid),
             MovingBaseService::RequestResult::InvalidFix);
    QCOMPARE(published.count(), 0);

    // Exact Null Island is valid Moving Base data. Map presentation may choose
    // not to render it, but the transport-independent registry preserves it.
    const NmeaGgaFix fix = ggaFix(0.0, 0.0, 123.5);
    QCOMPARE(fixture.service.submitFix(session, fix),
             MovingBaseService::RequestResult::Published);
    QCOMPARE(published.count(), 1);
    const MovingBasePositionSnapshot snapshot =
        publishedSnapshot(published, 0);
    QVERIFY(snapshot.isValid());
    QCOMPARE(snapshot.target.generation, lease.generation);
    QCOMPARE(snapshot.fix.latitudeDegrees, 0.0);
    QCOMPARE(snapshot.fix.longitudeDegrees, 0.0);
    QCOMPARE(snapshot.fix.altitudeAmslMetres, 123.5);
    QCOMPARE(snapshot.fix.observedMonotonicMs, qint64(100));
    QCOMPARE(snapshot.fix.satellites, 14);
    QCOMPARE(snapshot.fix.hdop, 0.7);
    // GGA geoid separation must not be added to its already-AMSL field 9.
    QVERIFY(snapshot.fix.altitudeAmslMetres != fix.geodeticAltitudeM());
    QCOMPARE(fixture.armedDelayMs, 6000);
}

void MovingBaseServiceTest::rateCapPublishesOnlyNewestQueuedFix()
{
    Fixture fixture;
    fixture.nowMs = 100;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject owner;
    MovingBaseService::SessionToken session;
    QSignalSpy published(&fixture.service,
                         &MovingBaseService::fixPublished);
    QCOMPARE(fixture.service.start(&owner, lease, 2.0, &session),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(session, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::Published);

    fixture.nowMs = 200;
    QCOMPARE(fixture.service.submitFix(session, ggaFix(3.0, 4.0)),
             MovingBaseService::RequestResult::Queued);
    QCOMPARE(fixture.armedDelayMs, 400);
    fixture.nowMs = 300;
    QCOMPARE(fixture.service.submitFix(session, ggaFix(5.0, 6.0)),
             MovingBaseService::RequestResult::Queued);
    QCOMPARE(fixture.armedDelayMs, 300);
    QCOMPARE(published.count(), 1);

    fixture.nowMs = 599;
    fixture.service.processTimersForTesting();
    QCOMPARE(published.count(), 1);
    QCOMPARE(fixture.armedDelayMs, 1);
    fixture.nowMs = 600;
    fixture.service.processTimersForTesting();
    QCOMPARE(published.count(), 2);
    const MovingBasePositionSnapshot latest =
        publishedSnapshot(published, 1);
    QCOMPARE(latest.fix.latitudeDegrees, 5.0);
    QCOMPARE(latest.fix.longitudeDegrees, 6.0);
    QCOMPARE(latest.fix.observedMonotonicMs, qint64(300));
    QCOMPARE(fixture.store.currentSnapshot().fix.latitudeDegrees, 5.0);
}

void MovingBaseServiceTest::staleTimeoutMatchesRate_data()
{
    QTest::addColumn<double>("rateHz");
    QTest::addColumn<int>("timeoutMs");
    QTest::newRow("quarter-hz") << 0.25 << 12000;
    QTest::newRow("half-hz") << 0.5 << 6000;
    QTest::newRow("one-hz") << 1.0 << 5000;
    QTest::newRow("two-hz") << 2.0 << 5000;
}

void MovingBaseServiceTest::staleTimeoutMatchesRate()
{
    QFETCH(double, rateHz);
    QFETCH(int, timeoutMs);
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject owner;
    MovingBaseService::SessionToken session;
    QCOMPARE(fixture.service.start(&owner, lease, rateHz, &session),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.staleTimeoutMs(), timeoutMs);
    QCOMPARE(fixture.service.submitFix(session, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::Published);
    QCOMPARE(fixture.armedDelayMs, timeoutMs);
}

void MovingBaseServiceTest::staleFixClearsButSessionStaysActive()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject owner;
    MovingBaseService::SessionToken session;
    QSignalSpy cleared(&fixture.service,
                       &MovingBaseService::positionCleared);
    QSignalSpy ended(&fixture.service,
                     &MovingBaseService::sessionEnded);
    QSignalSpy published(&fixture.service,
                         &MovingBaseService::fixPublished);
    QCOMPARE(fixture.service.start(&owner, lease, 0.25, &session),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(session, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::Published);

    fixture.nowMs = 11999;
    fixture.service.processTimersForTesting();
    QVERIFY(fixture.store.currentSnapshot().isValid());
    fixture.nowMs = 12000;
    fixture.service.processTimersForTesting();
    QVERIFY(!fixture.store.currentSnapshot().isValid());
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(ended.count(), 0);
    QVERIFY(fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.state(), MovingBaseService::State::Active);
    QCOMPARE(fixture.armedDelayMs, -1);

    fixture.nowMs = 12001;
    QCOMPARE(fixture.service.submitFix(session, ggaFix(3.0, 4.0)),
             MovingBaseService::RequestResult::Published);
    QCOMPARE(published.count(), 2);
}

void MovingBaseServiceTest::noFixClearsPendingAndPublishedPosition()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject owner;
    MovingBaseService::SessionToken session;
    QSignalSpy published(&fixture.service,
                         &MovingBaseService::fixPublished);
    QCOMPARE(fixture.service.start(&owner, lease, 1.0, &session),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(session, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::Published);
    fixture.nowMs = 100;
    QCOMPARE(fixture.service.submitFix(session, ggaFix(3.0, 4.0)),
             MovingBaseService::RequestResult::Queued);

    QCOMPARE(fixture.service.reportNoFix(
                 session, QStringLiteral("Receiver reports quality 0.")),
             MovingBaseService::RequestResult::Cleared);
    QVERIFY(!fixture.store.currentSnapshot().isValid());
    QVERIFY(fixture.service.hasActiveSession());
    QCOMPARE(fixture.armedDelayMs, -1);
    fixture.nowMs = 1000;
    fixture.service.processTimersForTesting();
    QCOMPARE(published.count(), 1);

    fixture.nowMs = 1001;
    QCOMPARE(fixture.service.submitFix(session, ggaFix(5.0, 6.0)),
             MovingBaseService::RequestResult::Published);
    QCOMPARE(published.count(), 2);
}

void MovingBaseServiceTest::targetLinkAndOwnerLifecycleStopAndClear()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.select(endpoint(3));
    fixture.targets.observeEndpoint(endpoint(9));
    QObject firstOwner;
    MovingBaseService::SessionToken firstSession;
    QSignalSpy ended(&fixture.service,
                     &MovingBaseService::sessionEnded);
    QCOMPARE(fixture.service.start(&firstOwner, first, 0.5,
                                   &firstSession),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(firstSession, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::Published);
    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(ended.count(), 1);
    QCOMPARE(endedResult(ended, 0),
             MovingBaseService::RequestResult::StaleTarget);
    QVERIFY(!fixture.store.currentSnapshot().isValid());

    const VehicleTargetLease second = fixture.targets.acquireTarget();
    QObject secondOwner;
    MovingBaseService::SessionToken secondSession;
    QCOMPARE(fixture.service.start(&secondOwner, second, 0.5,
                                   &secondSession),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(secondSession, ggaFix(3.0, 4.0)),
             MovingBaseService::RequestResult::Published);
    fixture.service.forgetLink(9);
    QVERIFY(!fixture.service.hasActiveSession());
    QVERIFY(!fixture.store.currentSnapshot().isValid());
    QCOMPARE(ended.count(), 2);
    QCOMPARE(endedResult(ended, 1),
             MovingBaseService::RequestResult::Stopped);

    MovingBaseService::SessionToken thirdSession;
    QObject *thirdOwner = new QObject;
    QCOMPARE(fixture.service.start(thirdOwner, second, 1.0,
                                   &thirdSession),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.submitFix(thirdSession, ggaFix(5.0, 6.0)),
             MovingBaseService::RequestResult::Published);
    delete thirdOwner;
    QVERIFY(!fixture.service.hasActiveSession());
    QVERIFY(!fixture.store.currentSnapshot().isValid());
    QCOMPARE(ended.count(), 3);
    QCOMPARE(endedResult(ended, 2),
             MovingBaseService::RequestResult::Stopped);
}

void MovingBaseServiceTest::staleTokenCannotAffectNewSession()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select(endpoint(3));
    QObject firstOwner;
    QObject secondOwner;
    MovingBaseService::SessionToken firstSession;
    MovingBaseService::SessionToken secondSession;
    QCOMPARE(fixture.service.start(&firstOwner, lease, 0.5,
                                   &firstSession),
             MovingBaseService::RequestResult::Started);
    QCOMPARE(fixture.service.stop(firstSession),
             MovingBaseService::RequestResult::Stopped);
    QCOMPARE(fixture.service.start(&secondOwner, lease, 0.5,
                                   &secondSession),
             MovingBaseService::RequestResult::Started);
    QVERIFY(secondSession.generation > firstSession.generation);

    QCOMPARE(fixture.service.submitFix(firstSession, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::InvalidSession);
    QCOMPARE(fixture.service.reportNoFix(firstSession),
             MovingBaseService::RequestResult::InvalidSession);
    QCOMPARE(fixture.service.stop(firstSession),
             MovingBaseService::RequestResult::InvalidSession);
    QVERIFY(fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.submitFix(secondSession, ggaFix(3.0, 4.0)),
             MovingBaseService::RequestResult::Published);
}

void MovingBaseServiceTest::
reentrantTargetChangeDuringPublishStopsOldSession()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.select(endpoint(3));
    fixture.targets.observeEndpoint(endpoint(9));
    QObject owner;
    MovingBaseService::SessionToken session;
    QSignalSpy published(&fixture.service,
                         &MovingBaseService::fixPublished);
    QSignalSpy ended(&fixture.service,
                     &MovingBaseService::sessionEnded);
    QCOMPARE(fixture.service.start(&owner, first, 0.5, &session),
             MovingBaseService::RequestResult::Started);
    connect(&fixture.store, &MovingBasePositionStore::positionUpdated,
            &fixture.store,
            [&fixture](const MovingBasePositionSnapshot &) {
                fixture.targets.selectTarget(9, 42, 1);
            });

    QCOMPARE(fixture.service.submitFix(session, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::StaleTarget);
    QCOMPARE(published.count(), 0);
    QCOMPARE(ended.count(), 1);
    QCOMPARE(endedResult(ended, 0),
             MovingBaseService::RequestResult::StaleTarget);
    QVERIFY(!fixture.service.hasActiveSession());
    QVERIFY(!fixture.store.currentSnapshot().isValid());
}

void MovingBaseServiceTest::synchronousObserversMayDeleteTheService()
{
    VehicleTargetManager targets;
    MovingBasePositionStore store(&targets);
    targets.observeEndpoint(endpoint(3), true);
    const VehicleTargetLease lease = targets.acquireTarget();
    QObject owner;

    auto *duringStart = new MovingBaseService(&targets, &store);
    QPointer<MovingBaseService> startGuard(duringStart);
    connect(duringStart, &MovingBaseService::stateChanged, &owner,
            [duringStart](MovingBaseService::State) {
                delete duringStart;
            });
    QCOMPARE(duringStart->start(&owner, lease, 0.5),
             MovingBaseService::RequestResult::InvalidSession);
    QVERIFY(startGuard.isNull());

    auto *duringFix = new MovingBaseService(&targets, &store);
    QPointer<MovingBaseService> fixGuard(duringFix);
    MovingBaseService::SessionToken fixSession;
    QCOMPARE(duringFix->start(&owner, lease, 0.5, &fixSession),
             MovingBaseService::RequestResult::Started);
    connect(duringFix, &MovingBaseService::fixPublished, &owner,
            [duringFix](const MovingBaseService::SessionToken &,
                        const MovingBasePositionSnapshot &) {
                delete duringFix;
            });
    QCOMPARE(duringFix->submitFix(fixSession, ggaFix(1.0, 2.0)),
             MovingBaseService::RequestResult::InvalidSession);
    QVERIFY(fixGuard.isNull());

    auto *duringStop = new MovingBaseService(&targets, &store);
    QPointer<MovingBaseService> stopGuard(duringStop);
    MovingBaseService::SessionToken stopSession;
    QCOMPARE(duringStop->start(&owner, lease, 0.5, &stopSession),
             MovingBaseService::RequestResult::Started);
    connect(duringStop, &MovingBaseService::sessionEnded, &owner,
            [duringStop](const MovingBaseService::SessionToken &,
                         MovingBaseService::RequestResult,
                         const QString &) {
                delete duringStop;
            });
    QCOMPARE(duringStop->stop(stopSession),
             MovingBaseService::RequestResult::Stopped);
    QVERIFY(stopGuard.isNull());
}

QTEST_GUILESS_MAIN(MovingBaseServiceTest)
#include "test_movingbaseservice.moc"
