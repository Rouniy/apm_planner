#include "comm/ExactLinkTransmitter.h"
#include "comm/ParameterService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"

#include <QtTest>

#include <algorithm>
#include <cstring>

namespace
{
VehicleEndpoint endpoint(int linkId, int systemId, int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    return result;
}

SwarmVehicleInstanceLease instanceLease(
    const VehicleEndpoint &vehicle,
    quint64 linkSessionEpoch = 1,
    quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint = vehicle;
    lease.linkSessionEpoch = linkSessionEpoch;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

void copyParameterId(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    const int count = std::min(16, bytes.size());
    std::memset(id, 0, 16);
    std::memcpy(id, bytes.constData(), static_cast<size_t>(count));
}

mavlink_message_t parameterValue(
    const VehicleEndpoint &source, const QString &name, qint32 value)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, ParameterType::Int32, ParameterEncoding::Bytewise, &encoded);
    Q_ASSERT(encoded);
    copyParameterId(name, payload.param_id);
    payload.param_type = static_cast<quint8>(ParameterType::Int32);
    payload.param_count = 1;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId), &message, &payload);
    return message;
}

ParameterService::ExactOperationReport reportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<ParameterService::ExactOperationReport>(
        spy.at(index).at(0));
}
}

class ParameterServiceSingleVehicleTest final : public QObject
{
    Q_OBJECT

private slots:
    void singlePolicyDoesNotWeakenSwarmRoute();
    void rejectsUnconfiguredMismatchedAndStaleTargets();
    void targetChangeDuringRouteValidationFailsClosed();
    void targetSwitchRetiresWaitingOperationBeforeLateReply();
    void retryUsesCapturedSingleVehiclePolicy();
};

void ParameterServiceSingleVehicleTest::singlePolicyDoesNotWeakenSwarmRoute()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint vehicle = endpoint(7, 42);
    QVERIFY(targets.observeEndpoint(vehicle, true));
    const VehicleTargetLease target = targets.acquireTarget();
    const SwarmVehicleInstanceLease lease = instanceLease(vehicle, 3, 5);
    QVERIFY(service.configureExactTransactions(
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *error) {
            if (error) {
                *error = QStringLiteral("Swarm rejects learned UDP.");
            }
            return false;
        }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, {lease}, &reservation),
             ParameterService::ExactReservationResult::RouteUnavailable);
    QVERIFY(!reservation.isValid());
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(reservation.isValid());

    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("INS_LOG_BAT_CNT");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    service.observeMessage(
        vehicle.linkId, parameterValue(vehicle, read.name, 64));
    QCOMPARE(service.store()->snapshot(vehicle)
                 .value(vehicle.componentId, read.name).value.toInt(), 64);
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterServiceSingleVehicleTest::
rejectsUnconfiguredMismatchedAndStaleTargets()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(8, 43);
    const VehicleEndpoint second = endpoint(9, 44);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease firstTarget = targets.acquireTarget();
    const SwarmVehicleInstanceLease firstLease = instanceLease(first);
    const SwarmVehicleInstanceLease secondLease = instanceLease(second);
    QVERIFY(service.configureExactTransactions(
        [](const SwarmVehicleInstanceLease &candidate) {
            return candidate.isValid();
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, firstTarget, firstLease, &reservation),
             ParameterService::ExactReservationResult::ContextUnavailable);
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, firstTarget, secondLease, &reservation),
             ParameterService::ExactReservationResult::InvalidLease);

    QVERIFY(targets.selectTarget(
        second.linkId, second.systemId, second.componentId));
    QVERIFY(targets.selectTarget(
        first.linkId, first.systemId, first.componentId));
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, firstTarget, firstLease, &reservation),
             ParameterService::ExactReservationResult::StaleLease);
}

void ParameterServiceSingleVehicleTest::
targetChangeDuringRouteValidationFailsClosed()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(10, 45);
    const VehicleEndpoint second = endpoint(11, 46);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease target = targets.acquireTarget();
    const SwarmVehicleInstanceLease lease = instanceLease(first);
    QVERIFY(service.configureExactTransactions(
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [&targets, &second](const SwarmVehicleInstanceLease &, QString *) {
            targets.selectTarget(
                second.linkId, second.systemId, second.componentId);
            return true;
        }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &reservation),
             ParameterService::ExactReservationResult::StaleLease);
    QVERIFY(!reservation.isValid());
}

void ParameterServiceSingleVehicleTest::
targetSwitchRetiresWaitingOperationBeforeLateReply()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(12, 47);
    const VehicleEndpoint second = endpoint(13, 48);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease target = targets.acquireTarget();
    const SwarmVehicleInstanceLease lease = instanceLease(first, 7, 9);
    QVERIFY(service.configureExactTransactions(
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("LOG_BITMASK");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(transmissions, 1);

    QVERIFY(targets.selectTarget(
        second.linkId, second.systemId, second.componentId));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadLeaseRetired);
    QCOMPARE(released.count(), 1);
    service.observeMessage(
        first.linkId, parameterValue(first, read.name, 128));
    QVERIFY(!service.store()->snapshot(first)
                 .contains(first.componentId, read.name));
}

void ParameterServiceSingleVehicleTest::retryUsesCapturedSingleVehiclePolicy()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 2, 10, 2, 100, 100);
    const VehicleEndpoint vehicle = endpoint(14, 49);
    QVERIFY(targets.observeEndpoint(vehicle, true));
    const VehicleTargetLease target = targets.acquireTarget();
    const SwarmVehicleInstanceLease lease = instanceLease(vehicle, 11, 13);
    int singleRouteCalls = 0;
    QVERIFY(service.configureExactTransactions(
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return false; }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [&singleRouteCalls](const SwarmVehicleInstanceLease &,
                            QString *error) {
            ++singleRouteCalls;
            if (singleRouteCalls >= 3) {
                if (error) {
                    *error = QStringLiteral("Single route retired.");
                }
                return false;
            }
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("INS_LOG_BAT_MASK");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 250);
    QCOMPARE(transmissions, 1);
    QCOMPARE(singleRouteCalls, 3);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadTransportFailure);
    QVERIFY(reportAt(finished, 0).description.contains(
        QStringLiteral("Single route retired")));
    QVERIFY(service.releaseExactReservation(reservation));
}

QTEST_GUILESS_MAIN(ParameterServiceSingleVehicleTest)

#include "test_parameterservicesinglevehicle.moc"
