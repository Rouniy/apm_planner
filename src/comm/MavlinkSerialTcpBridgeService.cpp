#include "MavlinkSerialTcpBridgeService.h"

#include "ExactLinkTransmitter.h"
#include "SerialBridgeTcpServer.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QSharedData>
#include <QSignalBlocker>
#include <QTimer>

#include <atomic>
#include <memory>
#include <utility>

namespace {
constexpr int MaximumHeartbeatAgeMs = 3000;
constexpr int TargetCheckAndPollMs = 50;
constexpr int SerialWritePaceMs = 10;
constexpr qint64 NanosecondsPerMillisecond = 1000000;
constexpr qint64 MaximumTcpPullBytes = 280;
constexpr int SerialPayloadBytes = MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN;

std::atomic<quint64> NextServiceIdentity{1};

quint64 nextNonZero(std::atomic<quint64> &counter)
{
    quint64 value = counter.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = counter.fetch_add(1, std::memory_order_relaxed);
    return value;
}

QString exactTargetText(const VehicleEndpoint &endpoint)
{
    return QStringLiteral("%1 (Link %2, system/component %3/%4)")
        .arg(endpoint.displayName())
        .arg(endpoint.linkId)
        .arg(endpoint.systemId)
        .arg(endpoint.componentId);
}

int remainingDataPaceMs(const QElapsedTimer &lastDataWrite)
{
    if (!lastDataWrite.isValid()) return 0;
    const qint64 remainingNs =
        qint64(SerialWritePaceMs) * NanosecondsPerMillisecond
        - lastDataWrite.nsecsElapsed();
    if (remainingNs <= 0) return 0;
    return static_cast<int>(
        (remainingNs + NanosecondsPerMillisecond - 1)
        / NanosecondsPerMillisecond);
}
}

class MavlinkSerialTcpBridgeService::Plan::Data final : public QSharedData
{
public:
    quint64 serviceIdentity = 0;
    quint64 planId = 0;
    Options options;
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    QString description;
};

MavlinkSerialTcpBridgeService::Plan::Plan() = default;
MavlinkSerialTcpBridgeService::Plan::Plan(const Plan &) = default;
MavlinkSerialTcpBridgeService::Plan &
MavlinkSerialTcpBridgeService::Plan::operator=(const Plan &) = default;
MavlinkSerialTcpBridgeService::Plan::~Plan() = default;

bool MavlinkSerialTcpBridgeService::Plan::isValid() const noexcept
{
    return d && d->serviceIdentity != 0 && d->planId != 0
        && d->target.isValid() && d->vehicle.isValid();
}

MavlinkSerialTcpBridgeService::Options
MavlinkSerialTcpBridgeService::Plan::options() const
{
    return d ? d->options : Options{};
}

VehicleTargetLease MavlinkSerialTcpBridgeService::Plan::target() const
{
    return d ? d->target : VehicleTargetLease{};
}

SwarmVehicleInstanceLease
MavlinkSerialTcpBridgeService::Plan::vehicle() const
{
    return d ? d->vehicle : SwarmVehicleInstanceLease{};
}

QString MavlinkSerialTcpBridgeService::Plan::description() const
{
    return d ? d->description : QString();
}

struct MavlinkSerialTcpBridgeService::Runtime
{
    QPointer<VehicleTargetManager> targetManager;
    QPointer<SwarmTelemetryRegistry> telemetryRegistry;
    QPointer<ExactLinkTransmitter> transmitter;
    QPointer<SerialBridgeTcpServer> server;
    RouteValidator routeValidator;
    QTimer *serviceTimer = nullptr;
    QTimer *transmitTimer = nullptr;

    quint64 serviceIdentity = nextNonZero(NextServiceIdentity);
    quint64 nextPlanId = 1;
    quint64 nextOperationId = 1;
    quint64 operationId = 0;
    quint8 localSystemId = 0;
    quint8 localComponentId = 0;
    Plan activePlan;
    QString status = QStringLiteral("Stopped.");
    QString targetDescription;
    QString lastReleaseOutcome;
    QByteArray transmitBatch;
    QElapsedTimer lastDataWrite;
    quint64 bytesFromTcp = 0;
    quint64 bytesToTcp = 0;
    quint64 droppedBytes = 0;
    bool starting = false;
    bool finishing = false;
    bool shuttingDown = false;
    bool clientSession = false;
    bool uartClaimAttempted = false;
    bool releaseAttempted = false;
    bool sending = false;
    quint64 clientGeneration = 0;
    std::shared_ptr<bool> claimAttemptInFlight;
    std::shared_ptr<bool> releaseAttemptInFlight;

    bool claimWasAttempted() const noexcept
    {
        return uartClaimAttempted
            || (claimAttemptInFlight && *claimAttemptInFlight);
    }
};

MavlinkSerialTcpBridgeService::MavlinkSerialTcpBridgeService(
    VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ExactLinkTransmitter *transmitter,
    quint8 localSystemId,
    quint8 localComponentId,
    RouteValidator routeValidator,
    QObject *parent)
    : QObject(parent)
    , m_runtime(new Runtime)
{
    m_runtime->targetManager = targetManager;
    m_runtime->telemetryRegistry = telemetryRegistry;
    m_runtime->transmitter = transmitter;
    m_runtime->localSystemId = localSystemId;
    m_runtime->localComponentId = localComponentId;
    m_runtime->routeValidator = std::move(routeValidator);

    auto *server = new SerialBridgeTcpServer(this);
    m_runtime->server = server;
    connect(server, &SerialBridgeTcpServer::clientConnected,
            this, &MavlinkSerialTcpBridgeService::handleClientConnected);
    connect(server, &SerialBridgeTcpServer::clientDisconnected,
            this, &MavlinkSerialTcpBridgeService::handleClientDisconnected);
    connect(server, &SerialBridgeTcpServer::readyRead,
            this, &MavlinkSerialTcpBridgeService::handleTcpReadyRead);
    connect(server, &SerialBridgeTcpServer::bytesWritten,
            this, [this](qint64 bytes) {
        if (bytes > 0 && m_runtime->operationId) {
            m_runtime->bytesToTcp += static_cast<quint64>(bytes);
            emit stateChanged();
        }
    });
    connect(server, &SerialBridgeTcpServer::failure,
            this, &MavlinkSerialTcpBridgeService::handleTcpFailure);

    auto *serviceTimer = new QTimer(this);
    serviceTimer->setInterval(TargetCheckAndPollMs);
    m_runtime->serviceTimer = serviceTimer;
    connect(serviceTimer, &QTimer::timeout,
            this, &MavlinkSerialTcpBridgeService::serviceTimers);

    auto *transmitTimer = new QTimer(this);
    transmitTimer->setSingleShot(true);
    transmitTimer->setTimerType(Qt::PreciseTimer);
    m_runtime->transmitTimer = transmitTimer;
    connect(transmitTimer, &QTimer::timeout,
            this, &MavlinkSerialTcpBridgeService::transmitNext);

    if (targetManager) {
        connect(targetManager, &VehicleTargetManager::currentTargetChanged,
                this, [this]() {
            if (m_runtime->operationId) {
                serviceTimers();
            } else {
                m_runtime->targetDescription.clear();
                emit stateChanged();
            }
        });
    }
    if (telemetryRegistry) {
        connect(telemetryRegistry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](const SwarmVehicleInstanceLease &lease,
                             SwarmTelemetryRegistry::RetirementReason) {
            if (m_runtime->activePlan.isValid()
                && m_runtime->activePlan.vehicle().sameInstance(lease)) {
                serviceTimers();
            }
        });
    }

    qRegisterMetaType<Options>();
    qRegisterMetaType<Plan>();
}

MavlinkSerialTcpBridgeService::~MavlinkSerialTcpBridgeService()
{
    if (m_runtime->serviceTimer) m_runtime->serviceTimer->stop();
    if (m_runtime->transmitTimer) m_runtime->transmitTimer->stop();
    m_runtime->shuttingDown = true;
    m_runtime->finishing = true;
    m_runtime->clientSession = false;
    if (m_runtime->server) {
        const QSignalBlocker blockServerCallbacks(m_runtime->server);
        disconnect(m_runtime->server, nullptr, this, nullptr);
        m_runtime->server->stop();
    }
    // The external exact-route validator cannot safely run from a partially
    // destroyed QObject. Normal window close calls tokened stop(), and normal
    // application teardown calls shutdown(); abrupt destruction closes only
    // the local TCP endpoint and leaves remote UART state explicitly unknown.
    delete m_runtime;
}

bool MavlinkSerialTcpBridgeService::validDevice(quint8 device) noexcept
{
    return device <= SERIAL_CONTROL_DEV_GPS2
        || device == SERIAL_CONTROL_DEV_SHELL
        || (device >= SERIAL_CONTROL_SERIAL0
            && device <= SERIAL_CONTROL_SERIAL9);
}

QString MavlinkSerialTcpBridgeService::deviceName(quint8 device)
{
    switch (device) {
    case SERIAL_CONTROL_DEV_TELEM1: return QStringLiteral("TELEM1");
    case SERIAL_CONTROL_DEV_TELEM2: return QStringLiteral("TELEM2");
    case SERIAL_CONTROL_DEV_GPS1: return QStringLiteral("GPS1");
    case SERIAL_CONTROL_DEV_GPS2: return QStringLiteral("GPS2");
    case SERIAL_CONTROL_DEV_SHELL: return QStringLiteral("SHELL");
    default:
        if (device >= SERIAL_CONTROL_SERIAL0
            && device <= SERIAL_CONTROL_SERIAL9) {
            return QStringLiteral("SERIAL%1")
                .arg(device - SERIAL_CONTROL_SERIAL0);
        }
        return QStringLiteral("Unknown (%1)").arg(device);
    }
}

void MavlinkSerialTcpBridgeService::assignError(
    QString *error, const QString &message)
{
    if (error) *error = message;
}

bool MavlinkSerialTcpBridgeService::busy() const
{
    return m_runtime->operationId != 0 || m_runtime->starting
        || m_runtime->finishing;
}

quint64 MavlinkSerialTcpBridgeService::operationId() const
{
    return m_runtime->operationId;
}

MavlinkSerialTcpBridgeService::Plan
MavlinkSerialTcpBridgeService::activePlan() const
{
    return m_runtime->activePlan;
}

QString MavlinkSerialTcpBridgeService::status() const
{
    return m_runtime->status;
}

QString MavlinkSerialTcpBridgeService::targetDescription() const
{
    if (!m_runtime->targetDescription.isEmpty()) {
        return m_runtime->targetDescription;
    }
    if (!m_runtime->targetManager
        || !m_runtime->targetManager->isTargetGenerationSettled()) {
        return QString();
    }
    const VehicleTargetLease selected =
        m_runtime->targetManager->acquireTarget();
    return selected.isValid() ? exactTargetText(selected.endpoint) : QString();
}

quint16 MavlinkSerialTcpBridgeService::boundPort() const
{
    return m_runtime->server ? m_runtime->server->boundPort() : 0;
}

bool MavlinkSerialTcpBridgeService::hasClient() const
{
    return m_runtime->server && m_runtime->server->hasClient();
}

quint64 MavlinkSerialTcpBridgeService::bytesFromTcp() const
{
    return m_runtime->bytesFromTcp;
}

quint64 MavlinkSerialTcpBridgeService::bytesToTcp() const
{
    return m_runtime->bytesToTcp;
}

quint64 MavlinkSerialTcpBridgeService::droppedBytes() const
{
    return m_runtime->droppedBytes;
}

bool MavlinkSerialTcpBridgeService::planBelongsHere(
    const Plan &plan) const noexcept
{
    return plan.isValid()
        && plan.d.constData()->serviceIdentity == m_runtime->serviceIdentity;
}

bool MavlinkSerialTcpBridgeService::hasOneKnownSystem(
    const Plan &plan, QString *error) const
{
    if (!m_runtime->targetManager || !m_runtime->telemetryRegistry
        || !plan.isValid()) {
        assignError(error, QStringLiteral(
            "Vehicle discovery is unavailable."));
        return false;
    }
    const auto *data = plan.d.constData();
    QSet<int> systems;
    for (const VehicleEndpoint &endpoint
         : m_runtime->targetManager->endpoints()) {
        if (endpoint.linkId == data->vehicle.endpoint.linkId
            && endpoint.systemId > 0) {
            systems.insert(endpoint.systemId);
        }
    }
    for (const VehicleEndpoint &endpoint
         : m_runtime->telemetryRegistry->endpoints()) {
        if (endpoint.linkId == data->vehicle.endpoint.linkId
            && endpoint.systemId > 0) {
            systems.insert(endpoint.systemId);
        }
    }
    if (systems.size() != 1
        || !systems.contains(data->vehicle.endpoint.systemId)) {
        assignError(error, QStringLiteral(
            "SERIAL_CONTROL is untargeted and requires exactly one known MAVLink system on the selected physical link."));
        return false;
    }
    return true;
}

bool MavlinkSerialTcpBridgeService::stateIsSafe(
    const Plan &plan, bool requireSelected, bool requireDisarmed,
    QString *error) const
{
    if ((m_runtime->shuttingDown && requireSelected) || !planBelongsHere(plan)
        || !m_runtime->targetManager || !m_runtime->telemetryRegistry
        || !m_runtime->transmitter || !m_runtime->routeValidator
        || !m_runtime->localSystemId || !m_runtime->localComponentId) {
        assignError(error, QStringLiteral(
            "MAVLink serial bridge services are unavailable."));
        return false;
    }
    const auto *data = plan.d.constData();
    if (!data->target.endpoint.sameIdentity(data->vehicle.endpoint)
        || data->vehicle.endpoint.componentId != MAV_COMP_ID_AUTOPILOT1
        || !hasOneKnownSystem(plan, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "The exact vehicle instance or its single-system physical link is no longer live.");
        }
        return false;
    }
    SwarmTelemetrySnapshot snapshot;
    const bool requireFreshHeartbeat = requireSelected || requireDisarmed;
    if (!m_runtime->telemetryRegistry->snapshotForLease(
            data->vehicle, &snapshot)
        || !snapshot.heartbeatValid
        || (requireFreshHeartbeat
            && !m_runtime->telemetryRegistry->observationIsFresh(
                snapshot.heartbeatObservedMs, MaximumHeartbeatAgeMs))
        || snapshot.autopilot == MAV_AUTOPILOT_INVALID
        || (requireDisarmed && snapshot.armed)) {
        assignError(error, QStringLiteral(
            "A fresh disarmed autopilot is required for SERIAL_CONTROL traffic."));
        return false;
    }
    if (requireSelected) {
        if (!m_runtime->targetManager->isTargetGenerationSettled()
            || !m_runtime->targetManager->isCurrentTarget(
                data->target.endpoint.linkId,
                data->target.endpoint.systemId,
                data->target.endpoint.componentId,
                data->target.generation)
            || !m_runtime->targetManager->hasFreshHeartbeat(
                data->target, MaximumHeartbeatAgeMs)
            || m_runtime->targetManager->heartbeatArmed(data->target)
                   != snapshot.armed
            || m_runtime->targetManager->heartbeatAutopilot(data->target)
                   != snapshot.autopilot) {
            assignError(error, QStringLiteral(
                "The selected vehicle changed or its heartbeat is stale."));
            return false;
        }
    }
    return true;
}

bool MavlinkSerialTcpBridgeService::validateVehicle(
    const Plan &plan, bool requireSelected, bool requireDisarmed,
    QString *error) const
{
    const Plan pinned = plan;
    const QPointer<const MavlinkSerialTcpBridgeService> guard(this);
    if (!stateIsSafe(pinned, requireSelected, requireDisarmed, error)) {
        return false;
    }
    const RouteValidator route = m_runtime->routeValidator;
    if (!route || !route(pinned.d.constData()->vehicle, error)) return false;
    if (!guard) return false;
    return stateIsSafe(pinned, requireSelected, requireDisarmed, error);
}

bool MavlinkSerialTcpBridgeService::capturePlan(
    const Options &options, Plan *planOut, QString *error) const
{
    if (!validDevice(options.device)) {
        assignError(error, QStringLiteral(
            "Select one of the 15 defined SERIAL_CONTROL devices."));
        return false;
    }
    if (!m_runtime->targetManager || !m_runtime->telemetryRegistry) {
        assignError(error, QStringLiteral(
            "Vehicle discovery is unavailable."));
        return false;
    }
    const VehicleTargetLease target =
        m_runtime->targetManager->acquireTarget();
    if (!target.isValid()
        || !m_runtime->targetManager->isTargetGenerationSettled()) {
        assignError(error, QStringLiteral(
            "Select one settled connected vehicle first."));
        return false;
    }
    const SwarmVehicleInstanceLease vehicle =
        m_runtime->telemetryRegistry->acquireVehicle(
            target.endpoint, MaximumHeartbeatAgeMs);
    if (!vehicle.isValid()) {
        assignError(error, QStringLiteral(
            "The selected vehicle has no fresh exact heartbeat."));
        return false;
    }

    Plan plan;
    plan.d = new Plan::Data;
    plan.d->serviceIdentity = m_runtime->serviceIdentity;
    plan.d->planId = m_runtime->nextPlanId;
    plan.d->options = options;
    plan.d->target = target;
    plan.d->vehicle = vehicle;
    const QString exposure = options.allowRemoteClients
        ? QStringLiteral("all IPv4 interfaces")
        : QStringLiteral("127.0.0.1 only");
    const QString baud = options.baudRate == 0
        ? QStringLiteral("keep current baud")
        : QStringLiteral("%1 baud").arg(options.baudRate);
    plan.d->description = QStringLiteral(
        "%1 / %2; TCP %3:%4; %5")
        .arg(exactTargetText(target.endpoint), deviceName(options.device),
             exposure)
        .arg(options.listenPort)
        .arg(baud);

    if (!validateVehicle(plan, true, true, error)) return false;
    if (planOut) *planOut = plan;
    return true;
}

bool MavlinkSerialTcpBridgeService::prepare(
    const Options &options, Plan *planOut, QString *error)
{
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (planOut) *planOut = {};
    if (error) error->clear();
    if (!planOut) {
        assignError(error, QStringLiteral("A plan output is required."));
        return false;
    }
    if (busy() || m_runtime->shuttingDown) {
        assignError(error, QStringLiteral(
            "A MAVLink serial TCP bridge is already active."));
        return false;
    }
    Plan plan;
    if (!capturePlan(options, &plan, error) || !guard) return false;
    ++m_runtime->nextPlanId;
    if (m_runtime->nextPlanId == 0) ++m_runtime->nextPlanId;
    *planOut = plan;
    return true;
}

bool MavlinkSerialTcpBridgeService::validate(
    const Plan &plan, QString *error) const
{
    if (error) error->clear();
    if (!planBelongsHere(plan)) {
        assignError(error, QStringLiteral(
            "The MAVLink serial bridge plan is invalid or belongs to another service."));
        return false;
    }
    return validateVehicle(plan, true, true, error);
}

bool MavlinkSerialTcpBridgeService::operationIsCurrent(
    quint64 id, const Plan::Data *identity) const
{
    return id != 0 && m_runtime->operationId == id
        && m_runtime->activePlan.isValid()
        && (!identity
            || m_runtime->activePlan.d.constData() == identity);
}

bool MavlinkSerialTcpBridgeService::start(
    const Plan &submittedPlan, quint64 *operationIdOut, QString *error)
{
    const Plan plan = submittedPlan;
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    if (!operationIdOut || !planBelongsHere(plan)) {
        assignError(error, QStringLiteral(
            "A valid plan and operation output are required."));
        return false;
    }
    if (busy() || m_runtime->shuttingDown || !m_runtime->server) {
        assignError(error, QStringLiteral(
            "A MAVLink serial TCP bridge is already active or unavailable."));
        return false;
    }

    quint64 id = m_runtime->nextOperationId++;
    if (id == 0) id = m_runtime->nextOperationId++;
    m_runtime->operationId = id;
    m_runtime->activePlan = plan;
    m_runtime->starting = true;
    m_runtime->targetDescription = exactTargetText(
        plan.d.constData()->target.endpoint);
    m_runtime->status = QStringLiteral("Validating the frozen bridge target…");
    m_runtime->bytesFromTcp = 0;
    m_runtime->bytesToTcp = 0;
    m_runtime->droppedBytes = 0;
    m_runtime->lastReleaseOutcome.clear();
    m_runtime->transmitBatch.clear();
    m_runtime->lastDataWrite.invalidate();
    m_runtime->clientSession = false;
    m_runtime->uartClaimAttempted = false;
    m_runtime->releaseAttempted = false;
    m_runtime->claimAttemptInFlight.reset();
    m_runtime->releaseAttemptInFlight.reset();
    *operationIdOut = id; // Publish ownership before the first callback.
    const auto *const identity = plan.d.constData();
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    emit stateChanged();
    if (!guard) return false;
    if (!operationIsCurrent(id, identity)) {
        assignError(error, QStringLiteral(
            "The bridge start was cancelled during admission."));
        return false;
    }
    QString reason;
    if (!validateVehicle(plan, true, true, &reason)) {
        if (!guard) return false;
        if (operationIsCurrent(id, identity)) complete(id, reason);
        assignError(error, reason);
        return false;
    }
    if (!guard || !operationIsCurrent(id, identity)) return false;
    const bool listening = m_runtime->server->start(
            plan.d.constData()->options.listenPort,
            plan.d.constData()->options.allowRemoteClients, &reason);
    if (!guard) return false;
    if (!listening) {
        if (!operationIsCurrent(id, identity)) return false;
        complete(id, QStringLiteral("Unable to start the TCP listener: %1")
                     .arg(reason));
        assignError(error, reason);
        return false;
    }
    if (!guard || !operationIsCurrent(id, identity)) return false;
    m_runtime->starting = false;
    m_runtime->status = QStringLiteral(
        "Listening on TCP %1; the vehicle UART is untouched until one client connects.")
        .arg(m_runtime->server->boundPort());
    m_runtime->serviceTimer->start();
    emit stateChanged();
    return bool(guard) && operationIsCurrent(id, identity);
}

void MavlinkSerialTcpBridgeService::updateStatus(const QString &status)
{
    m_runtime->status = status;
    emit stateChanged();
}

bool MavlinkSerialTcpBridgeService::sendSerialControl(
    quint64 id, const Plan &plan, quint8 flags, quint16 timeout,
    quint32 baudRate, const QByteArray &data, bool cleanup,
    quint64 expectedClientGeneration, bool *attempted)
{
    if (attempted) *attempted = false;
    const Plan pinned = plan;
    const auto *const identity = pinned.d.constData();
    if (!operationIsCurrent(id, identity)
        || (expectedClientGeneration
            && m_runtime->clientGeneration
                != expectedClientGeneration)) return false;
    QString reason;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!validateVehicle(pinned, !cleanup, !cleanup, &reason)) return false;
    if (!guard || !operationIsCurrent(id, identity)
        || (expectedClientGeneration
            && m_runtime->clientGeneration
                != expectedClientGeneration)) return false;
    auto frameAttempted = std::make_shared<bool>(false);
    if (cleanup) {
        m_runtime->releaseAttemptInFlight = frameAttempted;
    } else if (!m_runtime->claimWasAttempted()) {
        m_runtime->claimAttemptInFlight = frameAttempted;
    }
    const ExactLinkTransmitter::SendResult result =
        m_runtime->transmitter->sendSerialControl(
            pinned.d.constData()->vehicle.endpoint.linkId,
            pinned.d.constData()->vehicle.linkSessionEpoch,
            m_runtime->localSystemId, m_runtime->localComponentId,
            pinned.d.constData()->options.device, flags, timeout, baudRate,
            data, frameAttempted.get());
    if (attempted) *attempted = *frameAttempted;
    if (!guard || !operationIsCurrent(id, identity)
        || (expectedClientGeneration
            && m_runtime->clientGeneration
                != expectedClientGeneration)) return false;
    if (cleanup) {
        m_runtime->releaseAttempted =
            m_runtime->releaseAttempted || *frameAttempted;
        if (m_runtime->releaseAttemptInFlight == frameAttempted) {
            m_runtime->releaseAttemptInFlight.reset();
        }
    } else if (m_runtime->claimAttemptInFlight == frameAttempted) {
        m_runtime->uartClaimAttempted =
            m_runtime->uartClaimAttempted || *frameAttempted;
        m_runtime->claimAttemptInFlight.reset();
    }
    return result == ExactLinkTransmitter::SendResult::Sent;
}

void MavlinkSerialTcpBridgeService::handleClientConnected()
{
    if (!m_runtime->operationId || m_runtime->finishing
        || m_runtime->clientSession || !m_runtime->server
        || !m_runtime->server->hasClient()) return;
    const quint64 id = m_runtime->operationId;
    const Plan plan = m_runtime->activePlan;
    const auto *const identity = plan.d.constData();
    const quint64 clientGeneration = ++m_runtime->clientGeneration;
    m_runtime->clientSession = true;
    m_runtime->lastDataWrite.invalidate();
    m_runtime->lastReleaseOutcome.clear();
    m_runtime->uartClaimAttempted = false;
    m_runtime->releaseAttempted = false;
    m_runtime->claimAttemptInFlight.reset();
    m_runtime->releaseAttemptInFlight.reset();
    QString reason;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!validateVehicle(plan, true, true, &reason)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == clientGeneration) {
            terminate(id, reason, false);
        }
        return;
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    const quint8 flags = SERIAL_CONTROL_FLAG_EXCLUSIVE
        | SERIAL_CONTROL_FLAG_RESPOND | SERIAL_CONTROL_FLAG_MULTI;
    bool attempted = false;
    const bool sent = sendSerialControl(
        id, plan, flags, 100, plan.d.constData()->options.baudRate,
        QByteArray(), false, clientGeneration, &attempted);
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    if (!sent) {
        terminate(id, QStringLiteral(
            "The SERIAL_CONTROL open request was not submitted; the bridge stopped."),
            attempted || m_runtime->claimWasAttempted());
        return;
    }
    m_runtime->status = QStringLiteral(
        "TCP client connected. The exclusive UART request was submitted but has no protocol acknowledgement.");
    emit stateChanged();
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    scheduleTransmit();
}

void MavlinkSerialTcpBridgeService::releaseClientUart(
    quint64 id, bool keepListening, const QString &description)
{
    if (!operationIsCurrent(id)) return;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    const Plan plan = m_runtime->activePlan;
    const auto *const identity = plan.d.constData();
    m_runtime->clientSession = false;
    const quint64 retiredClientGeneration = ++m_runtime->clientGeneration;
    // Retire the old client's send state before any validator/transmitter
    // callback can admit and drive a replacement client.
    m_runtime->sending = false;
    if (m_runtime->transmitTimer) m_runtime->transmitTimer->stop();
    m_runtime->transmitBatch.clear();
    const bool shouldRelease = m_runtime->claimWasAttempted()
        && !m_runtime->releaseAttempted
        && !(m_runtime->releaseAttemptInFlight
             && *m_runtime->releaseAttemptInFlight);
    bool submitted = false;
    bool frameAttempted = false;
    if (shouldRelease) {
        // Zero flags release the UART. It is safe after arming/selection change,
        // but never after replacement of the physical instance or peer.
        submitted = sendSerialControl(
            id, plan, 0, 0, 0, QByteArray(), true,
            retiredClientGeneration, &frameAttempted);
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != retiredClientGeneration) return;
    m_runtime->uartClaimAttempted = false;
    m_runtime->claimAttemptInFlight.reset();
    m_runtime->lastDataWrite.invalidate();
    if (!shouldRelease) {
        m_runtime->lastReleaseOutcome = QStringLiteral(
            "No UART release was needed because no claim frame was attempted.");
    } else if (submitted) {
        m_runtime->lastReleaseOutcome = QStringLiteral(
            "A zero-flag UART release was submitted without protocol acknowledgement.");
    } else if (frameAttempted) {
        m_runtime->lastReleaseOutcome = QStringLiteral(
            "A zero-flag UART release reached the transmitter, but submission was not confirmed; UART state is uncertain.");
    } else {
        m_runtime->lastReleaseOutcome = QStringLiteral(
            "The zero-flag UART release was not submitted; verify the vehicle before reconnecting.");
    }
    if (!keepListening) return;

    QString reason;
    if (!validateVehicle(plan, true, true, &reason)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == retiredClientGeneration) {
            terminate(id, reason, false);
        }
        return;
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != retiredClientGeneration) return;
    m_runtime->releaseAttempted = false;
    m_runtime->releaseAttemptInFlight.reset();
    m_runtime->status = description + QLatin1Char(' ')
        + m_runtime->lastReleaseOutcome;
    emit stateChanged();
}

void MavlinkSerialTcpBridgeService::handleClientDisconnected()
{
    if (!m_runtime->operationId || m_runtime->finishing
        || !m_runtime->clientSession) return;
    releaseClientUart(
        m_runtime->operationId, true,
        QStringLiteral("TCP client disconnected; listening for one client."));
}

void MavlinkSerialTcpBridgeService::handleTcpReadyRead()
{
    scheduleTransmit();
}

void MavlinkSerialTcpBridgeService::scheduleTransmit(int delayMs)
{
    if (!m_runtime->operationId || m_runtime->finishing
        || !m_runtime->clientSession
        || !m_runtime->server || !m_runtime->server->hasClient()
        || !m_runtime->transmitTimer) return;
    int effectiveDelayMs = qMax(0, delayMs);
    effectiveDelayMs = qMax(
        effectiveDelayMs, remainingDataPaceMs(m_runtime->lastDataWrite));
    if (m_runtime->transmitTimer->isActive()
        && m_runtime->transmitTimer->remainingTime() >= effectiveDelayMs) {
        return;
    }
    m_runtime->transmitTimer->start(effectiveDelayMs);
}

void MavlinkSerialTcpBridgeService::transmitNext()
{
    if (m_runtime->sending || m_runtime->finishing || !m_runtime->operationId
        || !m_runtime->clientSession || !m_runtime->server
        || !m_runtime->server->hasClient()) return;
    const quint64 id = m_runtime->operationId;
    const Plan plan = m_runtime->activePlan;
    const auto *const identity = plan.d.constData();
    const quint64 clientGeneration = m_runtime->clientGeneration;
    // Cover the whole validation/read/send path. A RouteValidator is an
    // external callback and may pump events; nested transmitNext calls must
    // not enter before this invocation either sends or retires its client.
    m_runtime->sending = true;
    int paceDelayMs = remainingDataPaceMs(m_runtime->lastDataWrite);
    if (paceDelayMs > 0) {
        m_runtime->sending = false;
        scheduleTransmit(paceDelayMs);
        return;
    }
    QString reason;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!validateVehicle(plan, true, true, &reason)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == clientGeneration) {
            m_runtime->sending = false;
            terminate(id, reason, true);
        }
        return;
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    if (m_runtime->transmitBatch.isEmpty()) {
        const QByteArray input = m_runtime->server->takeInput(MaximumTcpPullBytes);
        if (!guard || !operationIsCurrent(id, identity)
            || m_runtime->clientGeneration != clientGeneration
            || !m_runtime->clientSession || m_runtime->finishing) return;
        m_runtime->transmitBatch = input;
    }
    // Keep this independent guard even though sending blocks nested data
    // writes: it also protects future callbacks added to the input facade.
    paceDelayMs = remainingDataPaceMs(m_runtime->lastDataWrite);
    if (paceDelayMs > 0) {
        m_runtime->sending = false;
        scheduleTransmit(paceDelayMs);
        return;
    }
    if (m_runtime->transmitBatch.isEmpty()) {
        // A graceful FIN retains the facade's bounded input. Retire the
        // logical client only after the paced MAVLink path drains it all.
        m_runtime->sending = false;
        if (m_runtime->server->inputEnded()) {
            m_runtime->server->finishInput();
        }
        return;
    }

    const int count = qMin(SerialPayloadBytes,
                           m_runtime->transmitBatch.size());
    const QByteArray chunk = m_runtime->transmitBatch.left(count);
    const bool finalChunk = count == m_runtime->transmitBatch.size();
    const quint8 flags = SERIAL_CONTROL_FLAG_EXCLUSIVE
        | (finalChunk ? SERIAL_CONTROL_FLAG_RESPOND : 0);
    bool attempted = false;
    const bool sent = sendSerialControl(
        id, plan, flags, 0, 0, chunk, false,
        clientGeneration, &attempted);
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession || m_runtime->finishing) return;
    m_runtime->sending = false;
    if (!sent) {
        terminate(id, QStringLiteral(
            "A TCP payload could not be submitted to the frozen SERIAL_CONTROL route; the bridge stopped."),
            attempted || m_runtime->claimWasAttempted());
        return;
    }
    m_runtime->transmitBatch.remove(0, count);
    m_runtime->bytesFromTcp += static_cast<quint64>(count);
    m_runtime->lastDataWrite.restart();
    emit stateChanged();
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    scheduleTransmit(SerialWritePaceMs);
}

void MavlinkSerialTcpBridgeService::serviceTimers()
{
    if (!m_runtime->operationId || m_runtime->finishing
        || m_runtime->sending) return;
    const quint64 id = m_runtime->operationId;
    const Plan plan = m_runtime->activePlan;
    const auto *const identity = plan.d.constData();
    const quint64 clientGeneration = m_runtime->clientGeneration;
    QString reason;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!validateVehicle(plan, true, true, &reason)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == clientGeneration) {
            terminate(id, reason, m_runtime->claimWasAttempted());
        }
        return;
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    if (m_runtime->server->inputEnded()) {
        scheduleTransmit();
        return;
    }
    const quint8 flags = SERIAL_CONTROL_FLAG_EXCLUSIVE
        | SERIAL_CONTROL_FLAG_RESPOND | SERIAL_CONTROL_FLAG_MULTI;
    bool attempted = false;
    if (!sendSerialControl(id, plan, flags, 100, 0,
                           QByteArray(), false, clientGeneration,
                           &attempted)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == clientGeneration) {
            terminate(id, QStringLiteral(
                "The periodic SERIAL_CONTROL read request failed; the bridge stopped."),
                attempted || m_runtime->claimWasAttempted());
        }
    }
}

void MavlinkSerialTcpBridgeService::observeMessage(
    int linkId, quint64 linkSessionEpoch, const mavlink_message_t &message)
{
    if (!m_runtime->operationId || m_runtime->finishing || !m_runtime->clientSession
        || !m_runtime->server || !m_runtime->server->hasClient()
        || message.msgid != MAVLINK_MSG_ID_SERIAL_CONTROL
        || message.len < 9
        || message.len > MAVLINK_MSG_ID_SERIAL_CONTROL_LEN) return;
    const quint64 id = m_runtime->operationId;
    const Plan plan = m_runtime->activePlan;
    const auto *const data = plan.d.constData();
    const auto *const identity = data;
    const quint64 clientGeneration = m_runtime->clientGeneration;
    if (linkId != data->vehicle.endpoint.linkId
        || linkSessionEpoch != data->vehicle.linkSessionEpoch
        || message.sysid != data->vehicle.endpoint.systemId
        || message.compid != data->vehicle.endpoint.componentId) return;
    mavlink_serial_control_t packet{};
    mavlink_msg_serial_control_decode(&message, &packet);
    if (packet.device != data->options.device
        || !(packet.flags & SERIAL_CONTROL_FLAG_REPLY)) return;
    if (packet.count > SerialPayloadBytes) {
        terminate(id, QStringLiteral(
            "A malformed SERIAL_CONTROL reply exceeded 70 bytes; the bridge stopped."),
            true);
        return;
    }
    if (packet.count == 0) return;
    QString reason;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!validateVehicle(plan, true, true, &reason)) {
        if (guard && operationIsCurrent(id, identity)
            && m_runtime->clientGeneration == clientGeneration) {
            terminate(id, reason, true);
        }
        return;
    }
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    const QByteArray bytes(
        reinterpret_cast<const char *>(packet.data), packet.count);
    if (m_runtime->server->inputEnded()) {
        m_runtime->droppedBytes += static_cast<quint64>(bytes.size());
        m_runtime->status = QStringLiteral(
            "The TCP peer closed its input; a late UART reply had no TCP destination and was counted as dropped.");
        emit stateChanged();
        return;
    }
    const bool accepted = m_runtime->server->sendBytes(bytes);
    if (!guard || !operationIsCurrent(id, identity)
        || m_runtime->clientGeneration != clientGeneration
        || !m_runtime->clientSession) return;
    if (!accepted) {
        m_runtime->droppedBytes += static_cast<quint64>(bytes.size());
        terminate(id, QStringLiteral(
            "The bounded TCP output queue overflowed; the client and UART session were stopped rather than dropping bytes silently."),
            true);
        return;
    }
    m_runtime->status = QStringLiteral(
        "Bridging one TCP client to %1. UART ownership and baud restoration are not acknowledged by SERIAL_CONTROL.")
        .arg(deviceName(data->options.device));
    emit stateChanged();
}

void MavlinkSerialTcpBridgeService::handleTcpFailure(
    const QString &reason)
{
    if (!m_runtime->operationId || m_runtime->finishing) return;
    terminate(m_runtime->operationId,
              QStringLiteral("TCP bridge failed: %1").arg(reason),
              m_runtime->claimWasAttempted());
}

void MavlinkSerialTcpBridgeService::terminate(
    quint64 id, const QString &description, bool attemptRelease)
{
    if (!operationIsCurrent(id) || m_runtime->finishing) return;
    m_runtime->finishing = true;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (m_runtime->serviceTimer) m_runtime->serviceTimer->stop();
    if (m_runtime->transmitTimer) m_runtime->transmitTimer->stop();
    if (attemptRelease && m_runtime->claimWasAttempted()) {
        releaseClientUart(id, false, description);
    }
    if (!guard || !operationIsCurrent(id)) return;
    if (m_runtime->server) m_runtime->server->stop();
    if (!guard || !operationIsCurrent(id)) return;
    const QString releaseOutcome = m_runtime->lastReleaseOutcome.isEmpty()
        ? QStringLiteral(
            "No UART release was needed because no claim frame was attempted.")
        : m_runtime->lastReleaseOutcome;
    complete(id, description + QLatin1Char(' ') + releaseOutcome);
}

void MavlinkSerialTcpBridgeService::complete(
    quint64 id, const QString &description)
{
    if (!operationIsCurrent(id)) return;
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    m_runtime->finishing = true;
    if (m_runtime->serviceTimer) m_runtime->serviceTimer->stop();
    if (m_runtime->transmitTimer) m_runtime->transmitTimer->stop();
    if (m_runtime->server && m_runtime->server->isListening()) {
        m_runtime->server->stop();
    }
    if (!guard || !operationIsCurrent(id)) return;
    m_runtime->operationId = 0;
    m_runtime->activePlan = {};
    m_runtime->starting = false;
    m_runtime->clientSession = false;
    m_runtime->sending = false;
    m_runtime->transmitBatch.clear();
    m_runtime->lastDataWrite.invalidate();
    m_runtime->claimAttemptInFlight.reset();
    m_runtime->releaseAttemptInFlight.reset();
    m_runtime->status = description;
    m_runtime->targetDescription.clear();
    // Keep a terminal barrier through both operation-specific notifications:
    // callbacks must not admit a successor which an old finished() observer
    // could then mistake for the completed operation.
    emit stateChanged();
    if (!guard) return;
    emit finished(id, description);
    if (!guard) return;
    m_runtime->finishing = false;
    emit stateChanged();
}

bool MavlinkSerialTcpBridgeService::stop(
    quint64 id, QString *error)
{
    if (error) error->clear();
    const QPointer<MavlinkSerialTcpBridgeService> guard(this);
    if (!operationIsCurrent(id)) {
        assignError(error, QStringLiteral(
            "The selected MAVLink serial bridge operation is no longer active."));
        return false;
    }
    terminate(id, QStringLiteral(
        "Bridge stopped. A zero-flag UART release is best-effort and the previous baud rate is not restored automatically."),
        m_runtime->claimWasAttempted());
    return bool(guard);
}

void MavlinkSerialTcpBridgeService::shutdown()
{
    if (m_runtime->shuttingDown) return;
    m_runtime->shuttingDown = true;
    if (m_runtime->operationId) {
        terminate(m_runtime->operationId, QStringLiteral(
            "Application shutdown stopped the MAVLink serial bridge; UART release could not be acknowledged."),
            m_runtime->claimWasAttempted());
    } else if (m_runtime->server) {
        m_runtime->server->stop();
    }
}
