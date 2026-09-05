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

VehicleEndpoint endpoint(int linkId, int systemId, int componentId)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    return result;
}

MavlinkComponentInstanceLease componentLease(
    const VehicleEndpoint &source,
    quint64 linkSessionEpoch = 1,
    quint64 instanceEpoch = 1)
{
    MavlinkComponentInstanceLease lease;
    lease.endpoint = source;
    lease.linkSessionEpoch = linkSessionEpoch;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

SwarmVehicleInstanceLease swarmLease(
    const VehicleEndpoint &source,
    quint64 linkSessionEpoch = 1,
    quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint = source;
    lease.linkSessionEpoch = linkSessionEpoch;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

void copyParameterId(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    const int count = std::min(16, static_cast<int>(bytes.size()));
    std::memset(id, 0, 16);
    if (count > 0) {
        std::memcpy(id, bytes.constData(), static_cast<size_t>(count));
    }
}

QString parameterId(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(id, length);
}

mavlink_message_t parameterValue(
    const VehicleEndpoint &source,
    const QString &name,
    const QVariant &value,
    ParameterType type = ParameterType::Real32,
    ParameterEncoding encoding = ParameterEncoding::Bytewise)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, type, encoding, &encoded);
    Q_ASSERT(encoded);
    copyParameterId(name, payload.param_id);
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = 1;
    payload.param_index = 0;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId),
        &message, &payload);
    return message;
}

ParameterService::ExactOperationReport reportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<ParameterService::ExactOperationReport>(
        spy.at(index).at(0));
}

} // namespace

class ParameterComponentTransactionsTest final : public QObject
{
    Q_OBJECT

private slots:
    void componentPolicyIsIndependentAndSharesEndpointArbitration();
    void onlyEpochPinnedRawIngressCompletesComponentRead();
    void componentWriteUsesExactTargetAndRequiresExactEcho();
    void retryRevalidatesTheCapturedComponentPolicy();
    void storeSignalCannotBlessAStaleComponentResponse();
    void delayedOldRetirementCannotEraseSuccessorCache();
    void successorAdmissionPreservesSelectionAndEncoding();
    void swarmRetirementStillClearsOrdinaryCache();
    void uncertainComponentWriteIsQuarantined();
};

void ParameterComponentTransactionsTest::
componentPolicyIsIndependentAndSharesEndpointArbitration()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint sensor = endpoint(70, 81, 50);
    const MavlinkComponentInstanceLease component =
        componentLease(sensor, 11, 13);
    const SwarmVehicleInstanceLease swarm = swarmLease(sensor, 11, 17);

    QVERIFY(service.configureExactTransactions(
        [](const SwarmVehicleInstanceLease &lease) {
            return lease.isValid();
        },
        [](const SwarmVehicleInstanceLease &, QString *error) {
            if (error) {
                *error = QStringLiteral("Swarm route remains restricted.");
            }
            return false;
        }));
    QVERIFY(service.configureComponentExactTransactions(
        [&component](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(component);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    ParameterService::ExactReservationToken rejected;
    QCOMPARE(service.reserveExactEndpoints(&owner, {swarm}, &rejected),
             ParameterService::ExactReservationResult::RouteUnavailable);
    QVERIFY(!rejected.isValid());

    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, component, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(reservation.isValid());
    QVERIFY(reservation.leases.isEmpty());
    QCOMPARE(reservation.componentLeases.size(), 1);
    QVERIFY(reservation.componentLeases.first().sameInstance(component));

    QCOMPARE(service.reserveExactEndpoints(&owner, {swarm}, &rejected),
             ParameterService::ExactReservationResult::Busy);
    QVERIFY(service.releaseExactReservation(reservation));

    MavlinkComponentInstanceLease stale = component;
    ++stale.instanceEpoch;
    QCOMPARE(service.reserveComponentEndpoint(&owner, stale, &rejected),
             ParameterService::ExactReservationResult::StaleLease);
}

void ParameterComponentTransactionsTest::
onlyEpochPinnedRawIngressCompletesComponentRead()
{
    VehicleTargetManager targets;
    QList<mavlink_message_t> submitted;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    const VehicleEndpoint sensor = endpoint(71, 81, 50);
    const MavlinkComponentInstanceLease lease =
        componentLease(sensor, 19, 23);
    transmitter.setLinkSessionEpoch(sensor.linkId, lease.linkSessionEpoch);
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this, [&submitted](int, quint64, mavlink_message_t message) {
                submitted.append(message);
            });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease successor =
        swarmLease(sensor, lease.linkSessionEpoch, 101);
    QVERIFY(service.configureExactTransactions(
        [&successor](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(successor);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(service.configureComponentExactTransactions(
        [&lease](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("VIDEO_ONLY");
    ParameterService::ExactOperationToken operation;
    QCOMPARE(service.submitComponentRead(
                 reservation, lease, read, &operation),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(operation.isValid());
    QVERIFY(operation.isComponentOperation());
    QCOMPARE(submitted.size(), 1);
    QCOMPARE(submitted.first().msgid,
             static_cast<quint32>(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    mavlink_param_request_read_t request{};
    mavlink_msg_param_request_read_decode(&submitted.first(), &request);
    QCOMPARE(request.target_system, static_cast<quint8>(81));
    QCOMPARE(request.target_component, static_cast<quint8>(50));
    QCOMPARE(parameterId(request.param_id), read.name);

    const mavlink_message_t response = parameterValue(
        sensor, read.name, 0.0F);
    service.observeMessage(sensor.linkId, response);
    QCOMPARE(finished.count(), 0);
    QVERIFY(!service.store()->snapshot(sensor)
                 .contains(sensor.componentId, read.name));
    service.observeComponentMessage(
        sensor.linkId, lease.linkSessionEpoch + 1, response);
    service.observeComponentMessage(
        sensor.linkId + 1, lease.linkSessionEpoch, response);
    QCOMPARE(finished.count(), 0);

    ParameterService::ExactReservationToken successorReservation;
    ParameterService::ExactReservationResult successorReserveResult =
        ParameterService::ExactReservationResult::ContextUnavailable;
    ParameterService::ExactSubmitResult successorSubmitResult =
        ParameterService::ExactSubmitResult::ContextUnavailable;
    connect(&service, &ParameterService::exactOperationFinished,
            this,
            [&](const ParameterService::ExactOperationReport &report) {
                if (!report.token.isComponentOperation()) {
                    return;
                }
                service.releaseExactReservation(reservation);
                successorReserveResult = service.reserveExactEndpoints(
                    &owner, {successor}, &successorReservation);
                if (successorReserveResult
                    == ParameterService::ExactReservationResult::Reserved) {
                    successorSubmitResult = service.submitExactRead(
                        successorReservation, successor, read);
                }
            });
    service.observePhysicalMessage(
        sensor.linkId, lease.linkSessionEpoch, response);
    QCOMPARE(finished.count(), 1);
    const ParameterService::ExactOperationReport report =
        reportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::ReadSucceeded);
    QVERIFY(report.token.isComponentOperation());
    QVERIFY(report.token.componentLease.sameInstance(lease));
    QCOMPARE(report.value.toFloat(), 0.0F);
    // The synchronous successor reservation intentionally invalidates the
    // preceding domain's cache before it sends its own read.
    QVERIFY(!service.store()->snapshot(sensor)
                .contains(sensor.componentId, read.name));
    QCOMPARE(successorReserveResult,
             ParameterService::ExactReservationResult::Reserved);
    QCOMPARE(successorSubmitResult,
             ParameterService::ExactSubmitResult::Started);

    // The wrapper chose the component consumer before the terminal signal.
    // The callback installed a same-endpoint/name successor synchronously, but
    // the original physical frame was not reconsidered under its domain.
    QCOMPARE(finished.count(), 1);
    const mavlink_message_t successorResponse = parameterValue(
        sensor, read.name, 1.0F);
    service.observePhysicalMessage(
        sensor.linkId, successor.linkSessionEpoch, successorResponse);
    QCOMPARE(finished.count(), 2);
    QVERIFY(!reportAt(finished, 1).token.isComponentOperation());
    QCOMPARE(reportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::ReadSucceeded);
    QVERIFY(service.store()->snapshot(sensor)
                .contains(sensor.componentId, read.name));
    QVERIFY(service.releaseExactReservation(successorReservation));
}

void ParameterComponentTransactionsTest::
componentWriteUsesExactTargetAndRequiresExactEcho()
{
    VehicleTargetManager targets;
    QList<mavlink_message_t> submitted;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    const VehicleEndpoint sensor = endpoint(72, 81, 50);
    const MavlinkComponentInstanceLease lease =
        componentLease(sensor, 29, 31);
    transmitter.setLinkSessionEpoch(sensor.linkId, lease.linkSessionEpoch);
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this, [&submitted](int, quint64, mavlink_message_t message) {
                submitted.append(message);
            });
    ParameterService service(&targets, &transmitter);
    QVERIFY(service.configureComponentExactTransactions(
        [&lease](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);

    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("VIDEO_ONLY");
    write.value = 1.0F;
    write.type = ParameterType::Real32;
    write.force = true;
    QCOMPARE(service.submitComponentWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(submitted.size(), 1);
    QCOMPARE(submitted.first().msgid,
             static_cast<quint32>(MAVLINK_MSG_ID_PARAM_SET));
    mavlink_param_set_t request{};
    mavlink_msg_param_set_decode(&submitted.first(), &request);
    QCOMPARE(request.target_system, static_cast<quint8>(81));
    QCOMPARE(request.target_component, static_cast<quint8>(50));
    QCOMPARE(parameterId(request.param_id), write.name);
    QCOMPARE(request.param_type,
             static_cast<quint8>(ParameterType::Real32));

    const VehicleEndpoint wrongComponent = endpoint(72, 81, 100);
    service.observeComponentMessage(
        sensor.linkId, lease.linkSessionEpoch,
        parameterValue(wrongComponent, write.name, 1.0F));
    service.observeComponentMessage(
        sensor.linkId, lease.linkSessionEpoch,
        parameterValue(sensor, QStringLiteral("OTHER"), 1.0F));
    QCOMPARE(finished.count(), 0);
    service.observeComponentMessage(
        sensor.linkId, lease.linkSessionEpoch,
        parameterValue(sensor, write.name, 1.0F));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::WriteSucceeded);
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterComponentTransactionsTest::
retryRevalidatesTheCapturedComponentPolicy()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    int routeCalls = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 2, 10, 2, 100, 100);
    const MavlinkComponentInstanceLease lease =
        componentLease(endpoint(73, 81, 50), 37, 41);
    QVERIFY(service.configureComponentExactTransactions(
        [&lease](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [&routeCalls](const MavlinkComponentInstanceLease &, QString *error) {
            ++routeCalls;
            if (routeCalls >= 3) {
                if (error) {
                    *error = QStringLiteral("Component route retired.");
                }
                return false;
            }
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("VIDEO_ONLY");
    QCOMPARE(service.submitComponentRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 250);
    QCOMPARE(transmissions, 1);
    QCOMPARE(routeCalls, 3);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadTransportFailure);
    QVERIFY(reportAt(finished, 0).description.contains(
        QStringLiteral("Component route retired")));
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterComponentTransactionsTest::
storeSignalCannotBlessAStaleComponentResponse()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const MavlinkComponentInstanceLease lease =
        componentLease(endpoint(77, 81, 50), 71, 73);
    bool leaseCurrent = true;
    QVERIFY(service.configureComponentExactTransactions(
        [&lease, &leaseCurrent](
            const MavlinkComponentInstanceLease &candidate) {
            return leaseCurrent && candidate.sameInstance(lease);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("VIDEO_ONLY");
    QCOMPARE(service.submitComponentRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    connect(service.store(), &ParameterStore::endpointParameterChanged,
            this, [&leaseCurrent](int, int, int, const QString &) {
                leaseCurrent = false;
            });

    service.observePhysicalMessage(
        lease.endpoint.linkId, lease.linkSessionEpoch,
        parameterValue(lease.endpoint, read.name, 1.0F));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadLeaseRetired);
    QCOMPARE(released.count(), 1);
    QVERIFY(!service.store()->snapshot(lease.endpoint)
                 .contains(lease.endpoint.componentId, read.name));
}

void ParameterComponentTransactionsTest::
delayedOldRetirementCannotEraseSuccessorCache()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint sensor = endpoint(74, 81, 50);
    MavlinkComponentInstanceLease current =
        componentLease(sensor, 43, 47);
    const MavlinkComponentInstanceLease old = current;
    QVERIFY(service.configureComponentExactTransactions(
        [&current](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(current);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReadRequest oldRead;
    oldRead.name = QStringLiteral("OLD_ONLY");
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, current, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitComponentRead(reservation, current, oldRead),
             ParameterService::ExactSubmitResult::Started);
    service.observeComponentMessage(
        sensor.linkId, current.linkSessionEpoch,
        parameterValue(sensor, oldRead.name, 0.0F));
    QVERIFY(service.releaseExactReservation(reservation));

    ++current.instanceEpoch;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, current, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(!service.store()->snapshot(sensor)
                 .contains(sensor.componentId, oldRead.name));
    ParameterService::ExactReadRequest newRead;
    newRead.name = QStringLiteral("NEW_ONLY");
    QCOMPARE(service.submitComponentRead(reservation, current, newRead),
             ParameterService::ExactSubmitResult::Started);
    service.observeComponentMessage(
        sensor.linkId, current.linkSessionEpoch,
        parameterValue(sensor, newRead.name, 1.0F));
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(service.store()->snapshot(sensor)
                 .value(sensor.componentId, newRead.name).value.toFloat(),
             1.0F);

    service.retireComponent(old);
    QVERIFY(!service.store()->snapshot(sensor)
                 .contains(sensor.componentId, oldRead.name));
    QCOMPARE(service.store()->snapshot(sensor)
                 .value(sensor.componentId, newRead.name).value.toFloat(),
             1.0F);
}

void ParameterComponentTransactionsTest::
successorAdmissionPreservesSelectionAndEncoding()
{
    VehicleTargetManager targets;
    QList<mavlink_message_t> submitted;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    const VehicleEndpoint vehicle = endpoint(78, 42, 1);
    transmitter.setLinkSessionEpoch(vehicle.linkId, 79);
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this, [&submitted](int, quint64, mavlink_message_t message) {
                submitted.append(message);
            });
    ParameterService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(vehicle, true));
    SwarmVehicleInstanceLease current = swarmLease(vehicle, 79, 83);
    QVERIFY(service.configureExactTransactions(
        [&current](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(current);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    service.setEncoding(vehicle, ParameterEncoding::CStyleCast);

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(&owner, {current}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("OLD_ONLY");
    QCOMPARE(service.submitExactRead(reservation, current, read),
             ParameterService::ExactSubmitResult::Started);
    service.observeMessage(
        vehicle.linkId,
        parameterValue(vehicle, read.name, qint32(3), ParameterType::Int32,
                       ParameterEncoding::CStyleCast));
    QVERIFY(service.releaseExactReservation(reservation));

    ++current.instanceEpoch;
    QCOMPARE(service.reserveExactEndpoints(&owner, {current}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(service.store()->endpoint().sameIdentity(vehicle));
    QVERIFY(!service.store()->snapshot()
                 .contains(vehicle.componentId, read.name));

    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("NEW_INT");
    write.value = qint32(7);
    write.type = ParameterType::Int32;
    write.force = true;
    QCOMPARE(service.submitExactWrite(reservation, current, write),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(!submitted.isEmpty());
    mavlink_param_set_t payload{};
    mavlink_msg_param_set_decode(&submitted.last(), &payload);
    QCOMPARE(payload.target_system, static_cast<quint8>(42));
    QCOMPARE(payload.target_component, static_cast<quint8>(1));
    QCOMPARE(payload.param_value, 7.0F);
    service.observeMessage(
        vehicle.linkId,
        parameterValue(vehicle, write.name, write.value, write.type,
                       ParameterEncoding::CStyleCast));
    QCOMPARE(service.store()->snapshot()
                 .value(vehicle.componentId, write.name).value.toInt(), 7);
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterComponentTransactionsTest::
swarmRetirementStillClearsOrdinaryCache()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint vehicle = endpoint(76, 42, 1);
    QVERIFY(targets.observeEndpoint(vehicle, true));
    service.observeMessage(
        vehicle.linkId,
        parameterValue(vehicle, QStringLiteral("LOG_BITMASK"), 7.0F));
    QVERIFY(service.store()->snapshot(vehicle)
                .contains(vehicle.componentId,
                          QStringLiteral("LOG_BITMASK")));

    service.retireExactVehicle(swarmLease(vehicle, 61, 67));
    QVERIFY(!service.store()->snapshot(vehicle)
                 .contains(vehicle.componentId,
                           QStringLiteral("LOG_BITMASK")));
}

void ParameterComponentTransactionsTest::
uncertainComponentWriteIsQuarantined()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return false; });
    ParameterService service(&targets, &transmitter);
    const MavlinkComponentInstanceLease lease =
        componentLease(endpoint(75, 81, 50), 53, 59);
    QVERIFY(service.configureComponentExactTransactions(
        [&lease](const MavlinkComponentInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveComponentEndpoint(
                 &owner, lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("VIDEO_ONLY");
    write.value = 1.0F;
    write.type = ParameterType::Real32;
    write.force = true;
    QCOMPARE(service.submitComponentWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::TransportOutcomeUncertain);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteTransportOutcomeUncertain);
    QVERIFY(service.isComponentWriteQuarantined(
        lease, write.name, write.value, write.type));
    QCOMPARE(service.submitComponentWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Quarantined);
    QVERIFY(service.releaseExactReservation(reservation));
}

QTEST_GUILESS_MAIN(ParameterComponentTransactionsTest)

#include "test_parametercomponenttransactions.moc"
