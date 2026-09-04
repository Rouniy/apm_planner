#include "ui/SwarmWaypointLeaderWindowAdapter.h"

#include "comm/SwarmFlightMode.h"

#include <QCoreApplication>
#include <QtTest>

#include <utility>

namespace
{

SwarmVehicleInstanceLease lease(int linkId, int systemId,
                                quint64 instanceEpoch = 1,
                                int componentId = MAV_COMP_ID_AUTOPILOT1)
{
    SwarmVehicleInstanceLease result;
    result.endpoint.linkId = linkId;
    result.endpoint.systemId = systemId;
    result.endpoint.componentId = componentId;
    result.endpoint.linkName = QStringLiteral("Radio %1").arg(linkId);
    result.linkSessionEpoch = 100 + quint64(linkId);
    result.instanceEpoch = instanceEpoch;
    return result;
}

SwarmTelemetrySnapshot telemetry(
    const SwarmVehicleInstanceLease &vehicle,
    int autopilot, int vehicleType, bool heartbeat = true)
{
    SwarmTelemetrySnapshot result;
    result.lease = vehicle;
    result.heartbeatValid = heartbeat;
    result.heartbeatObservedMs = 1000;
    result.autopilot = autopilot;
    result.vehicleType = vehicleType;
    result.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    result.customMode = SwarmFlightMode::isCopter(vehicleType) ? 4 : 10;
    result.positionValid = true;
    result.positionObservedMs = 1000;
    result.latitudeDegrees = 35.0 + vehicle.endpoint.linkId * 0.00001;
    result.longitudeDegrees = 33.0;
    result.relativeAltitudeM = 10.0 + vehicle.endpoint.linkId;
    return result;
}

mavlink_mission_item_int_t missionItem(
    quint16 sequence, qint32 latitudeE7,
    qint32 longitudeE7, float altitude)
{
    mavlink_mission_item_int_t item{};
    item.seq = sequence;
    item.command = MAV_CMD_NAV_WAYPOINT;
    item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    item.x = latitudeE7;
    item.y = longitudeE7;
    item.z = altitude;
    item.mission_type = MAV_MISSION_TYPE_MISSION;
    return item;
}

ExactMissionSnapshot exactMission(
    const SwarmVehicleInstanceLease &airMaster,
    quint64 observationRevision,
    quint64 contentGeneration = 7)
{
    ExactMissionSnapshot result;
    result.key.vehicle = airMaster;
    result.key.missionType = MAV_MISSION_TYPE_MISSION;
    result.contentGeneration = contentGeneration;
    result.observationRevision = observationRevision;
    result.contentDigest = QByteArrayLiteral("same-content");
    result.waypointLeaderSignature = QByteArrayLiteral("signature");
    result.confirmedAtMs = 1000;
    result.items = {
        missionItem(0, 350000000, 330000000, 5.0F),
        missionItem(1, 350010000, 330000000, 20.0F),
        missionItem(2, 350020000, 330000000, 15.0F)};
    return result;
}

class FakeBackend final : public SwarmWaypointLeaderWindowAdapterBackend
{
public:
    void setCallbacks(Callbacks value) override
    {
        callbacks = std::move(value);
    }

    QVector<SwarmTelemetrySnapshot> vehicleSnapshots() const override
    {
        return snapshots;
    }

    void refreshVehicles() override
    {
        ++vehicleRefreshes;
        if (callbacks.vehiclesChanged) {
            callbacks.vehiclesChanged();
        }
    }

    bool acquireMission(const SwarmVehicleInstanceLease &airMaster,
                        ExactMissionSnapshot *snapshot) const override
    {
        if (!missionAvailable
            || !currentMission.key.vehicle.sameInstance(airMaster)) {
            return false;
        }
        if (snapshot) {
            *snapshot = currentMission;
        }
        return snapshot != nullptr;
    }

    ExactMissionSnapshotService::StartResult requestMission(
        QObject *, const SwarmVehicleInstanceLease &airMaster,
        ExactMissionTransferToken *token, QString *error) override
    {
        ++missionRequests;
        requestedAir = airMaster;
        if (requestResult
            != ExactMissionSnapshotService::StartResult::Started) {
            if (token) {
                *token = {};
            }
            if (error) {
                *error = requestError;
            }
            return requestResult;
        }
        activeToken.id = ++nextToken;
        if (duringRequest) {
            duringRequest();
        }
        if (synchronousForeign && callbacks.missionFinished) {
            ExactMissionTransferResult foreign;
            foreign.token.id = activeToken.id + 1000;
            foreign.key.vehicle = lease(99, airMaster.endpoint.systemId);
            foreign.key.missionType = MAV_MISSION_TYPE_MISSION;
            callbacks.missionFinished(foreign);
        }
        if (synchronousFinish && callbacks.missionFinished) {
            currentMission = exactMission(
                airMaster, currentMission.observationRevision + 1,
                currentMission.contentGeneration == 0
                    ? 7 : currentMission.contentGeneration);
            missionAvailable = true;
            ExactMissionTransferResult finished;
            finished.token = activeToken;
            finished.key = currentMission.key;
            finished.state = MissionTransferService::State::Complete;
            finished.missionResult = MAV_MISSION_ACCEPTED;
            finished.snapshot = currentMission;
            callbacks.missionFinished(finished);
        }
        if (token) {
            *token = activeToken;
        }
        return requestResult;
    }

    bool cancelMission(const ExactMissionTransferToken &token,
                       const QString &reason) override
    {
        ++missionCancellations;
        cancelledTokens.append(token.id);
        cancellationReasons.append(reason);
        if (emitCancelledSynchronously && callbacks.missionFinished) {
            ExactMissionTransferResult result;
            result.token = token;
            result.key.vehicle = requestedAir;
            result.key.missionType = MAV_MISSION_TYPE_MISSION;
            result.state = MissionTransferService::State::Cancelled;
            result.errorString = reason;
            callbacks.missionFinished(result);
        }
        return token.id == activeToken.id;
    }

    bool executorReady(QString *error) const override
    {
        ++readyCalls;
        if (!ready && error) {
            *error = QStringLiteral("executor unavailable");
        }
        return ready;
    }

    bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                      QString *) const override
    {
        ++validateCalls;
        lastPlan = plan;
        return acceptsPlan;
    }

    bool start(const SwarmWaypointLeaderPlan &plan,
               QString *) override
    {
        ++startCalls;
        lastPlan = plan;
        running = startAccepted;
        return startAccepted;
    }

    void cancelActiveRun(const QString &reason) override
    {
        ++runCancellations;
        lastRunCancellation = reason;
        running = false;
    }

    bool requestMode(SwarmWaypointLeaderMode next,
                     QString *) override
    {
        ++modeRequests;
        currentMode = next;
        return modeAccepted;
    }

    bool isRunning() const noexcept override { return running; }
    SwarmWaypointLeaderMode mode() const noexcept override
    {
        return currentMode;
    }
    QString statusText() const override { return status; }

    void finish(const ExactMissionTransferResult &result)
    {
        if (callbacks.missionFinished) {
            callbacks.missionFinished(result);
        }
    }

    Callbacks callbacks;
    QVector<SwarmTelemetrySnapshot> snapshots;
    ExactMissionSnapshot currentMission;
    SwarmVehicleInstanceLease requestedAir;
    ExactMissionTransferToken activeToken;
    ExactMissionSnapshotService::StartResult requestResult =
        ExactMissionSnapshotService::StartResult::Started;
    QString requestError;
    QString status = QStringLiteral("executor idle");
    mutable SwarmWaypointLeaderPlan lastPlan;
    QVector<quint64> cancelledTokens;
    QStringList cancellationReasons;
    QString lastRunCancellation;
    quint64 nextToken = 10;
    int vehicleRefreshes = 0;
    int missionRequests = 0;
    int missionCancellations = 0;
    mutable int readyCalls = 0;
    mutable int validateCalls = 0;
    int startCalls = 0;
    int runCancellations = 0;
    int modeRequests = 0;
    bool missionAvailable = false;
    bool synchronousFinish = false;
    bool synchronousForeign = false;
    bool emitCancelledSynchronously = true;
    bool ready = true;
    bool acceptsPlan = true;
    bool startAccepted = true;
    bool running = false;
    bool modeAccepted = true;
    SwarmWaypointLeaderMode currentMode = SwarmWaypointLeaderMode::Idle;
    std::function<void()> duringRequest;
};

ExactMissionTransferResult completion(
    quint64 token,
    const ExactMissionSnapshot &snapshot,
    bool succeeded = true)
{
    ExactMissionTransferResult result;
    result.token.id = token;
    result.key = snapshot.key;
    result.state = succeeded ? MissionTransferService::State::Complete
                             : MissionTransferService::State::Error;
    result.missionResult = succeeded ? MAV_MISSION_ACCEPTED
                                     : MAV_MISSION_ERROR;
    result.snapshot = succeeded ? snapshot : ExactMissionSnapshot();
    result.errorString = succeeded ? QString()
                                   : QStringLiteral("download failed");
    return result;
}

} // namespace

class SwarmWaypointLeaderWindowAdapterTest final : public QObject
{
    Q_OBJECT

private slots:
    void duplicateSysidsKeepExactIdentityAndEligibility();
    void convertsExactMissionAndAdvancesIdenticalObservation();
    void ownsOnlyMatchingMissionTokenAndIgnoresLateForeignResults();
    void synchronousCompletionIsCorrelatedAfterStartReturns();
    void synchronousCancellationUsesReturnedToken();
    void changedNotificationsAreCoalesced();
    void delegatesCompleteExecutorState();
};

void SwarmWaypointLeaderWindowAdapterTest::
duplicateSysidsKeepExactIdentityAndEligibility()
{
    FakeBackend backend;
    const auto rover = lease(1, 42);
    const auto copter = lease(2, 42);
    const auto px4 = lease(3, 43);
    const auto stale = lease(4, 44);
    backend.snapshots = {
        telemetry(copter, MAV_AUTOPILOT_ARDUPILOTMEGA,
                  MAV_TYPE_QUADROTOR),
        telemetry(rover, MAV_AUTOPILOT_ARDUPILOTMEGA,
                  MAV_TYPE_GROUND_ROVER),
        telemetry(px4, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR),
        telemetry(stale, MAV_AUTOPILOT_ARDUPILOTMEGA,
                  MAV_TYPE_QUADROTOR, false),
        telemetry(copter, MAV_AUTOPILOT_ARDUPILOTMEGA,
                  MAV_TYPE_QUADROTOR)};

    SwarmWaypointLeaderWindowAdapter adapter(&backend);
    const auto vehicles = adapter.vehicles();
    QCOMPARE(vehicles.size(), 4);
    QVERIFY(vehicles.at(0).lease.sameInstance(rover));
    QVERIFY(vehicles.at(1).lease.sameInstance(copter));
    QVERIFY(vehicles.at(0).label.contains(QStringLiteral("Radio 1")));
    QVERIFY(vehicles.at(1).label.contains(QStringLiteral("Radio 2")));
    QCOMPARE(vehicles.at(0).lease.endpoint.systemId,
             vehicles.at(1).lease.endpoint.systemId);
    QVERIFY(vehicles.at(0).groundEligible);
    QVERIFY(!vehicles.at(0).flightEligible);
    QVERIFY(vehicles.at(1).groundEligible);
    QVERIFY(vehicles.at(1).flightEligible);
    QVERIFY(vehicles.at(2).groundEligible);
    QVERIFY(!vehicles.at(2).flightEligible);
    QVERIFY(!vehicles.at(3).groundEligible);
    QVERIFY(!vehicles.at(3).flightEligible);
}

void SwarmWaypointLeaderWindowAdapterTest::
convertsExactMissionAndAdvancesIdenticalObservation()
{
    FakeBackend backend;
    const auto air = lease(2, 42);
    backend.snapshots = {telemetry(
        air, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR)};
    backend.currentMission = exactMission(air, 50);
    backend.missionAvailable = true;
    SwarmWaypointLeaderWindowAdapter adapter(&backend);

    QCOMPARE(adapter.missionObservationRevision(air), quint64(50));
    SwarmWaypointLeaderMissionSnapshot converted;
    QString error;
    QVERIFY2(adapter.missionForAirMaster(air, &converted, &error),
             qPrintable(error));
    QVERIFY(converted.airMaster.sameInstance(air));
    QCOMPARE(converted.contentGeneration, quint64(7));
    QCOMPARE(converted.contentDigest, QByteArrayLiteral("same-content"));
    QCOMPARE(converted.items.size(), 3);
    QCOMPARE(converted.items.at(1).sequence, 1);
    QCOMPARE(converted.items.at(1).latitudeE7, qint32(350010000));
    QCOMPARE(converted.items.at(1).relativeAltitudeM, 20.0F);

    QVERIFY(adapter.refreshMission(air, &error));
    const quint64 token = backend.activeToken.id;
    backend.currentMission = exactMission(air, 51); // identical contents
    backend.finish(completion(token, backend.currentMission));
    QCOMPARE(adapter.missionObservationRevision(air), quint64(51));
}

void SwarmWaypointLeaderWindowAdapterTest::
ownsOnlyMatchingMissionTokenAndIgnoresLateForeignResults()
{
    FakeBackend backend;
    const auto air = lease(2, 42);
    const auto other = lease(3, 42);
    backend.snapshots = {
        telemetry(air, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR),
        telemetry(other, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR)};
    SwarmWaypointLeaderWindowAdapter adapter(&backend);
    int changed = 0;
    adapter.setChangedHandler([&changed]() { ++changed; });

    QString error;
    QVERIFY(adapter.refreshMission(air, &error));
    const quint64 ownedToken = backend.activeToken.id;
    backend.currentMission = exactMission(air, 4);
    backend.finish(completion(ownedToken + 999, backend.currentMission));
    QVERIFY(!adapter.refreshMission(air, &error));
    QCOMPARE(backend.missionRequests, 1);
    adapter.cancelMissionRefresh(other, QStringLiteral("foreign"));
    QCOMPARE(backend.missionCancellations, 0);
    adapter.cancelMissionRefresh(air, QStringLiteral("owned cancel"));
    QCOMPARE(backend.missionCancellations, 1);
    QCOMPARE(backend.cancelledTokens, QVector<quint64>{ownedToken});

    QCoreApplication::processEvents();
    QCOMPARE(changed, 1);
    backend.currentMission = exactMission(air, 5);
    backend.missionAvailable = true;
    backend.finish(completion(ownedToken, backend.currentMission));
    backend.finish(completion(ownedToken + 999, backend.currentMission));
    QCoreApplication::processEvents();
    QCOMPARE(changed, 1); // cancelled/foreign completions are detached

    backend.requestResult = ExactMissionSnapshotService::StartResult::Busy;
    backend.requestError = QStringLiteral("global mission owner busy");
    QVERIFY(!adapter.refreshMission(air, &error));
    QCOMPARE(error, QStringLiteral("global mission owner busy"));
}

void SwarmWaypointLeaderWindowAdapterTest::
synchronousCompletionIsCorrelatedAfterStartReturns()
{
    FakeBackend backend;
    const auto air = lease(7, 77);
    backend.snapshots = {telemetry(
        air, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_HEXAROTOR)};
    backend.currentMission = exactMission(air, 10);
    backend.missionAvailable = true;
    backend.synchronousForeign = true;
    backend.synchronousFinish = true;
    SwarmWaypointLeaderWindowAdapter adapter(&backend);

    QString error;
    QVERIFY2(adapter.refreshMission(air, &error), qPrintable(error));
    QCOMPARE(adapter.missionObservationRevision(air), quint64(11));
    // The synchronous terminal completion cleared only the matching token.
    QVERIFY2(adapter.refreshMission(air, &error), qPrintable(error));
    QCOMPARE(backend.missionRequests, 2);
    QCOMPARE(adapter.missionObservationRevision(air), quint64(12));
}

void SwarmWaypointLeaderWindowAdapterTest::
synchronousCancellationUsesReturnedToken()
{
    FakeBackend backend;
    const auto air = lease(8, 88);
    backend.snapshots = {telemetry(
        air, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR)};
    SwarmWaypointLeaderWindowAdapter adapter(&backend);
    backend.duringRequest = [&adapter, air]() {
        adapter.cancelMissionRefresh(
            air, QStringLiteral("cancel during request"));
    };

    QString error;
    QVERIFY2(adapter.refreshMission(air, &error), qPrintable(error));
    QCOMPARE(backend.missionCancellations, 1);
    QCOMPARE(backend.cancelledTokens,
             QVector<quint64>{backend.activeToken.id});
    QCOMPARE(backend.cancellationReasons,
             QStringList{QStringLiteral("cancel during request")});

    backend.duringRequest = {};
    QVERIFY2(adapter.refreshMission(air, &error), qPrintable(error));
}

void SwarmWaypointLeaderWindowAdapterTest::
changedNotificationsAreCoalesced()
{
    FakeBackend backend;
    SwarmWaypointLeaderWindowAdapter adapter(&backend);
    int changed = 0;
    adapter.setChangedHandler([&changed]() { ++changed; });
    QVERIFY(backend.callbacks.vehiclesChanged);
    QVERIFY(backend.callbacks.missionCacheChanged);
    QVERIFY(backend.callbacks.executorChanged);
    backend.callbacks.vehiclesChanged();
    backend.callbacks.missionCacheChanged();
    backend.callbacks.executorChanged();
    QCOMPARE(changed, 0);
    QTRY_COMPARE(changed, 1);

    adapter.refreshVehicles();
    QCOMPARE(backend.vehicleRefreshes, 1);
    QTRY_COMPARE(changed, 2);
}

void SwarmWaypointLeaderWindowAdapterTest::delegatesCompleteExecutorState()
{
    FakeBackend backend;
    SwarmWaypointLeaderWindowAdapter adapter(&backend);
    SwarmWaypointLeaderPlan plan;
    plan.groundMaster = lease(1, 11);
    plan.airMaster = lease(2, 12);
    QString error;

    QVERIFY(adapter.executorReady(&error));
    QVERIFY(adapter.validatePlan(plan, &error));
    QCOMPARE(backend.validateCalls, 1);
    QVERIFY(adapter.start(plan, &error));
    QVERIFY(adapter.isRunning());
    QCOMPARE(backend.startCalls, 1);
    QVERIFY(backend.lastPlan.airMaster.sameInstance(plan.airMaster));
    QVERIFY(adapter.requestMode(
        SwarmWaypointLeaderMode::ReturnAlongMission, &error));
    QCOMPARE(adapter.mode(),
             SwarmWaypointLeaderMode::ReturnAlongMission);
    adapter.cancelActiveRun(QStringLiteral("operator stop"));
    QVERIFY(!adapter.isRunning());
    QCOMPARE(backend.runCancellations, 1);
    QCOMPARE(backend.lastRunCancellation,
             QStringLiteral("operator stop"));
    QCOMPARE(adapter.statusText(), QStringLiteral("executor idle"));
}

QTEST_GUILESS_MAIN(SwarmWaypointLeaderWindowAdapterTest)

#include "test_swarmwaypointleaderwindowadapter.moc"
