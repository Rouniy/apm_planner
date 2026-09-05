#include "Px4FlowService.h"

#include "MavlinkComponentRegistry.h"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{

const QString kVideoOnly = QStringLiteral("VIDEO_ONLY");

QString endpointLabel(const MavlinkComponentInstanceLease &lease)
{
    return lease.isValid() ? lease.endpoint.displayName()
                           : QStringLiteral("PX4Flow source");
}

} // namespace

Px4FlowService::Px4FlowService(
    MavlinkComponentRegistry *registry,
    ParameterService *parameterService,
    QObject *parent)
    : QObject(parent),
      m_registry(registry),
      m_parameterService(parameterService)
{
    qRegisterMetaType<MavlinkComponentInstanceLease>();
    qRegisterMetaType<ParameterService::ExactOperationReport>();

    m_partialFrameTimer.setSingleShot(true);
    m_partialFrameTimer.setInterval(PartialFrameTimeoutMs);
    connect(&m_partialFrameTimer, &QTimer::timeout, this, [this]() {
        if (!m_assembler.active()) {
            return;
        }
        const int received = m_assembler.receivedPackets();
        const int expected = m_assembler.expectedPackets();
        m_assembler.reset();
        if (!m_streamStaleLatched) {
            m_streamStatus = QStringLiteral(
                "Incomplete image timed out after %1 of %2 packets.")
                                     .arg(received).arg(expected);
        }
        emitChanged();
    });

    m_staleFrameTimer.setSingleShot(true);
    m_staleFrameTimer.setInterval(StreamStaleTimeoutMs);
    connect(&m_staleFrameTimer, &QTimer::timeout, this, [this]() {
        if (!m_active) {
            return;
        }
        m_streamStaleLatched = true;
        m_streamStatus = QStringLiteral(
            "Image stream is stale (no complete frame for 3 seconds).");
        emitChanged();
    });

    m_cleanupPollTimer.setSingleShot(true);
    m_cleanupPollTimer.setInterval(QuarantinePollMs);
    connect(&m_cleanupPollTimer, &QTimer::timeout,
            this, &Px4FlowService::advanceCleanup);

    if (m_parameterService) {
        m_exactFinishedConnection = connect(
            m_parameterService,
            &ParameterService::exactOperationFinished,
            this, &Px4FlowService::handleExactReport,
            Qt::QueuedConnection);
        connect(m_parameterService, &QObject::destroyed, this, [this]() {
            m_parameterService.clear();
            m_reservation = {};
            m_operation = {};
            m_operationPurpose = OperationPurpose::None;
            m_boundLease = {};
            m_cleanupRequested = false;
            m_modeKnown = false;
            m_videoOnly = false;
            m_modeUncertain = false;
            m_operationStatus = QStringLiteral(
                "PX4Flow parameter service is unavailable.");
            emitChanged();
        });
    }
    if (m_registry) {
        m_registryChangedConnection = connect(
            m_registry, &MavlinkComponentRegistry::componentsChanged,
            this, &Px4FlowService::emitChanged, Qt::QueuedConnection);
        m_registryRetiredConnection = connect(
            m_registry, &MavlinkComponentRegistry::componentRetired,
            this, &Px4FlowService::retireSource, Qt::QueuedConnection);
        connect(m_registry, &QObject::destroyed, this, [this]() {
            m_registry.clear();
            if (m_operation.isValid() && m_parameterService
                && m_reservation.isValid()) {
                QPointer<Px4FlowService> guard(this);
                m_parameterService->cancelExactOperation(
                    m_reservation, m_operation,
                    QStringLiteral("The component registry was destroyed."));
                if (!guard) {
                    return;
                }
            }
            if (!releaseReservation()) {
                return;
            }
            m_operationStatus = QStringLiteral(
                "PX4Flow component discovery is unavailable.");
            emitChanged();
        });
    }
}

Px4FlowService::~Px4FlowService()
{
    disconnect(m_exactFinishedConnection);
    disconnect(m_registryChangedConnection);
    disconnect(m_registryRetiredConnection);
    m_partialFrameTimer.stop();
    m_staleFrameTimer.stop();
    m_cleanupPollTimer.stop();
    if (m_operation.isValid() && m_parameterService
        && m_reservation.isValid()) {
        m_parameterService->cancelExactOperation(
            m_reservation, m_operation,
            QStringLiteral("The PX4Flow service is being destroyed."));
    }
    releaseReservation();
}

QVariantList Px4FlowService::sources() const
{
    QVariantList rows;
    if (m_replayGeneration != 0 || m_pendingReplayGeneration != 0) {
        const quint64 generation = m_replayGeneration != 0
            ? m_replayGeneration : m_pendingReplayGeneration;
        rows.append(QVariantMap{
            {QStringLiteral("id"), replaySourceId(generation)},
            {QStringLiteral("label"),
             QStringLiteral("Replay %1 (read-only)").arg(generation)}});
    }
    if (!m_registry) {
        return rows;
    }
    const QList<MavlinkComponentInstanceLease> components =
        m_registry->components();
    const int count = std::min(components.size(), MaximumSources);
    rows.reserve(rows.size() + count);
    for (int index = 0; index < count; ++index) {
        const MavlinkComponentInstanceLease &lease = components.at(index);
        rows.append(QVariantMap{
            {QStringLiteral("id"), sourceId(lease)},
            {QStringLiteral("label"), lease.endpoint.displayName()}});
    }
    return rows;
}

QString Px4FlowService::status() const
{
    if (!m_operationStatus.isEmpty()) {
        return m_operationStatus;
    }
    if (!m_persistentWarning.isEmpty()) {
        return m_persistentWarning;
    }
    if (!m_streamStatus.isEmpty()) {
        return m_streamStatus;
    }
    if (m_shuttingDown) {
        return QStringLiteral("PX4Flow service is shutting down.");
    }
    if (m_replay) {
        return QStringLiteral("Replay source is read-only.");
    }
    if (m_selectedSourceId.isEmpty()) {
        return QStringLiteral("Select a PX4Flow source.");
    }
    if (!m_active) {
        return QStringLiteral("PX4Flow view is inactive.");
    }
    if (!m_boundLease.isValid()) {
        return QStringLiteral("The selected PX4Flow source is unavailable.");
    }
    return m_videoOnly
        ? QStringLiteral("Focus mode is active. Press Video to restore streaming.")
        : QStringLiteral("Video mode is active. Press Focus to enter focus mode.");
}

bool Px4FlowService::canToggle() const
{
    return m_active && !m_replay && !m_shuttingDown && !busy()
        && m_modeKnown && m_boundLease.isValid()
        && m_reservation.isValid() && selectedLeaseIsCurrent();
}

bool Px4FlowService::busy() const
{
    return m_operation.isValid() || m_cleanupRequested
        || m_cleanupPollTimer.isActive() || m_apiCallInFlight;
}

void Px4FlowService::activate()
{
    if (m_shuttingDown) {
        return;
    }
    if (m_replay) {
        if (!m_active) {
            m_active = true;
            m_activeBeforeReplay = true;
            m_staleFrameTimer.start();
            emitChanged();
        }
        return;
    }
    if (m_active) {
        return;
    }
    m_active = true;
    if (m_cleanupRequested || m_apiCallInFlight) {
        emitChanged();
        return;
    }
    m_operationStatus.clear();
    if (!m_selectedSourceId.isEmpty()) {
        bindSelectedSource();
    } else {
        emitChanged();
    }
}

void Px4FlowService::deactivate()
{
    if (!m_active && !m_replay) {
        return;
    }
    m_active = false;
    if (m_replay) {
        m_activeBeforeReplay = false;
        resetStreamState(false);
        emitChanged();
        return;
    }
    resetStreamState(false);
    if (m_reservation.isValid() || m_operation.isValid()
        || m_apiCallInFlight) {
        beginCleanup(QStringLiteral(
            "Restoring VIDEO_ONLY=0 while the PX4Flow view is inactive."));
    } else {
        emitChanged();
    }
}

void Px4FlowService::selectSource(const QString &sourceIdValue)
{
    if (m_shuttingDown) {
        return;
    }
    const QString requested = sourceIdValue.trimmed();
    if (requested == m_selectedSourceId && !m_sourceTransitionPending
        && m_pendingReplayGeneration == 0) {
        if (m_active && !m_replay && !busy()) {
            m_operationStatus.clear();
            if (m_reservation.isValid() && !m_modeKnown) {
                m_operationStatus = QStringLiteral(
                    "Reading VIDEO_ONLY from %1...")
                        .arg(endpointLabel(m_boundLease));
                submitRead(OperationPurpose::DiscoverMode);
            } else if (!m_reservation.isValid()) {
                bindSelectedSource();
            }
        }
        return;
    }
    const MavlinkComponentInstanceLease requestedLease = findSource(requested);
    if (!requested.isEmpty() && !requestedLease.isValid()) {
        m_operationStatus = QStringLiteral(
            "The selected PX4Flow source is no longer available.");
        emitChanged();
        return;
    }

    if (m_replay) {
        m_replay = false;
        m_replayGeneration = 0;
        m_replayEndpointPinned = false;
        m_replaySystemId = 0;
        m_replayComponentId = 0;
        m_liveSourceBeforeReplay.clear();
    }
    m_pendingReplayGeneration = 0;
    if (m_reservation.isValid() || m_operation.isValid()
        || m_cleanupRequested || m_apiCallInFlight) {
        m_pendingSourceId = requested;
        m_sourceTransitionPending = true;
        beginCleanup(QStringLiteral(
            "Restoring VIDEO_ONLY=0 before changing PX4Flow source."));
        return;
    }

    m_selectedSourceId = requested;
    m_pendingSourceId.clear();
    m_sourceTransitionPending = false;
    m_operationStatus.clear();
    resetStreamState(true);
    if (m_active && !requested.isEmpty()) {
        bindSelectedSource();
    } else {
        emitChanged();
    }
}

void Px4FlowService::toggleFocus()
{
    if (!canToggle()) {
        if (m_replay) {
            m_operationStatus = QStringLiteral(
                "Focus mode cannot be changed for a replay source.");
        } else if (!busy()) {
            m_operationStatus = QStringLiteral(
                "VIDEO_ONLY has not been read from the selected source.");
        }
        emitChanged();
        return;
    }
    m_operationStatus = m_videoOnly
        ? QStringLiteral("Restoring PX4Flow video mode...")
        : QStringLiteral("Enabling PX4Flow focus mode...");
    submitWrite(!m_videoOnly, OperationPurpose::ToggleMode);
}

void Px4FlowService::observeMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    if (!m_active || m_replay || m_cleanupRequested
        || !m_boundLease.isValid() || !selectedLeaseIsCurrent()
        || linkId != m_boundLease.endpoint.linkId
        || linkSessionEpoch != m_boundLease.linkSessionEpoch
        || message.sysid != m_boundLease.endpoint.systemId
        || message.compid != m_boundLease.endpoint.componentId) {
        return;
    }
    observeImageMessage(message);
}

void Px4FlowService::beginReplay(quint64 generation)
{
    if (generation == 0 || m_shuttingDown) {
        return;
    }
    if (m_replay && m_replayGeneration == generation) {
        return;
    }
    if (!m_replay && m_pendingReplayGeneration == 0) {
        m_liveSourceBeforeReplay = m_selectedSourceId;
        m_activeBeforeReplay = m_active;
    }
    if (m_reservation.isValid() || m_operation.isValid()
        || m_cleanupRequested || m_apiCallInFlight) {
        m_pendingReplayGeneration = generation;
        m_pendingSourceId.clear();
        m_sourceTransitionPending = false;
        beginCleanup(QStringLiteral(
            "Restoring VIDEO_ONLY=0 before opening replay traffic."));
        return;
    }
    startReplayNow(generation);
}

void Px4FlowService::observeReplay(
    quint64 generation, const mavlink_message_t &message)
{
    if (!m_replay || !m_active || generation == 0
        || generation != m_replayGeneration) {
        return;
    }
    if (!m_replayEndpointPinned) {
        if (message.msgid != MAVLINK_MSG_ID_DATA_TRANSMISSION_HANDSHAKE) {
            return;
        }
        if (message.sysid == 0 || message.compid == 0
            || message.compid == MAV_COMP_ID_MISSIONPLANNER) {
            return;
        }
        beginFrame(message);
        if (m_assembler.active()) {
            m_replaySystemId = message.sysid;
            m_replayComponentId = message.compid;
            m_replayEndpointPinned = true;
        }
        return;
    }
    if (message.sysid != m_replaySystemId
        || message.compid != m_replayComponentId) {
        return;
    }
    observeImageMessage(message);
}

void Px4FlowService::endReplay(quint64 generation)
{
    if (generation == 0) {
        return;
    }
    if (m_pendingReplayGeneration == generation) {
        m_pendingReplayGeneration = 0;
        m_liveSourceBeforeReplay.clear();
        m_activeBeforeReplay = false;
    }
    if (!m_replay || generation != m_replayGeneration) {
        return;
    }
    resetStreamState(true);
    m_replay = false;
    m_replayGeneration = 0;
    m_replayEndpointPinned = false;
    m_replaySystemId = 0;
    m_replayComponentId = 0;
    m_selectedSourceId = m_liveSourceBeforeReplay;
    m_liveSourceBeforeReplay.clear();
    m_active = m_activeBeforeReplay;
    m_activeBeforeReplay = false;
    m_operationStatus.clear();
    if (m_active && !m_selectedSourceId.isEmpty()) {
        bindSelectedSource();
    } else {
        emitChanged();
    }
}

void Px4FlowService::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    m_active = false;
    m_pendingSourceId.clear();
    m_sourceTransitionPending = false;
    m_pendingReplayGeneration = 0;
    m_replay = false;
    m_replayGeneration = 0;
    m_replayEndpointPinned = false;
    m_replaySystemId = 0;
    m_replayComponentId = 0;
    resetStreamState(false);
    if (m_reservation.isValid() || m_operation.isValid()
        || m_apiCallInFlight) {
        beginCleanup(QStringLiteral(
            "Restoring VIDEO_ONLY=0 before PX4Flow shutdown."));
    } else {
        emitChanged();
    }
}

QString Px4FlowService::sourceId(
    const MavlinkComponentInstanceLease &lease)
{
    if (!lease.isValid()) {
        return {};
    }
    return QStringLiteral("live:%1:%2:%3:%4:%5")
        .arg(lease.endpoint.linkId)
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId)
        .arg(lease.linkSessionEpoch)
        .arg(lease.instanceEpoch);
}

QString Px4FlowService::replaySourceId(quint64 generation)
{
    return generation == 0
        ? QString() : QStringLiteral("replay:%1").arg(generation);
}

bool Px4FlowService::terminalIsUncertain(
    ParameterService::ExactTerminalResult terminal)
{
    using Result = ParameterService::ExactTerminalResult;
    switch (terminal) {
    case Result::WriteCancelledOutcomeUncertain:
    case Result::WriteTimedOutOutcomeUncertain:
    case Result::WriteTransportOutcomeUncertain:
    case Result::WriteLeaseRetiredOutcomeUncertain:
    case Result::WriteLinkForgottenOutcomeUncertain:
        return true;
    default:
        return false;
    }
}

bool Px4FlowService::modeFromValue(const QVariant &value, bool *ok)
{
    bool converted = false;
    const double numeric = value.toDouble(&converted);
    converted = converted && std::isfinite(numeric);
    if (ok) {
        *ok = converted;
    }
    return converted && numeric != 0.0;
}

MavlinkComponentInstanceLease Px4FlowService::findSource(
    const QString &id) const
{
    if (!m_registry || id.isEmpty()) {
        return {};
    }
    const QList<MavlinkComponentInstanceLease> components =
        m_registry->components();
    for (const MavlinkComponentInstanceLease &lease : components) {
        if (sourceId(lease) == id) {
            return lease;
        }
    }
    return {};
}

bool Px4FlowService::selectedLeaseIsCurrent() const
{
    return m_registry && m_boundLease.isValid()
        && m_registry->validateLease(m_boundLease)
        && sourceId(m_boundLease) == m_selectedSourceId;
}

void Px4FlowService::bindSelectedSource()
{
    if (!m_active || m_replay || m_shuttingDown
        || m_selectedSourceId.isEmpty()) {
        emitChanged();
        return;
    }
    const MavlinkComponentInstanceLease lease =
        findSource(m_selectedSourceId);
    if (!lease.isValid() || !m_registry
        || !m_registry->validateLease(lease)) {
        m_operationStatus = QStringLiteral(
            "The selected PX4Flow source is no longer available.");
        emitChanged();
        return;
    }
    // Pin imaging before attempting the independent parameter reservation.
    // A busy parameter route must not turn a valid sensor stream into an
    // empty page, and it must never make us follow another component.
    const bool newImagingLease = !m_boundLease.sameInstance(lease);
    m_boundLease = lease;
    if (newImagingLease) {
        m_streamStaleLatched = false;
        m_staleFrameTimer.start();
    } else if (!m_staleFrameTimer.isActive()
               && !m_streamStaleLatched) {
        m_staleFrameTimer.start();
    }
    if (!m_parameterService) {
        m_operationStatus = QStringLiteral(
            "PX4Flow parameter service is unavailable.");
        emitChanged();
        return;
    }

    ParameterService::ExactReservationToken reservation;
    QString error;
    m_apiCallInFlight = true;
    QPointer<Px4FlowService> guard(this);
    const auto result = m_parameterService->reserveComponentEndpoint(
        this, lease, &reservation, &error);
    if (!guard) {
        return;
    }
    m_apiCallInFlight = false;
    if (result != ParameterService::ExactReservationResult::Reserved) {
        if (m_cleanupRequested) {
            m_cleanupRequested = false;
            m_boundLease = {};
            applyPendingTransition();
            return;
        }
        if (m_shuttingDown || !m_active) {
            m_boundLease = {};
            emitChanged();
            return;
        }
        m_operationStatus = error.isEmpty()
            ? QStringLiteral("The selected PX4Flow parameter route is busy.")
            : error;
        emitChanged();
        return;
    }
    m_reservation = reservation;
    m_modeKnown = false;
    m_videoOnly = false;
    m_modeUncertain = false;
    m_modeType = ParameterType::Unknown;
    m_operationStatus = QStringLiteral("Reading VIDEO_ONLY from %1...")
        .arg(endpointLabel(lease));
    if (m_cleanupRequested || !m_active) {
        beginCleanup(QStringLiteral(
            "Restoring VIDEO_ONLY=0 after PX4Flow activation was interrupted."));
        return;
    }
    submitRead(OperationPurpose::DiscoverMode);
}

bool Px4FlowService::releaseReservation()
{
    const ParameterService::ExactReservationToken reservation = m_reservation;
    m_reservation = {};
    m_operation = {};
    m_operationPurpose = OperationPurpose::None;
    m_boundLease = {};
    m_modeKnown = false;
    m_videoOnly = false;
    m_modeUncertain = false;
    m_modeType = ParameterType::Unknown;
    m_uncertainValue.clear();
    if (reservation.isValid() && m_parameterService) {
        QPointer<Px4FlowService> guard(this);
        m_parameterService->releaseExactReservation(reservation);
        return !guard.isNull();
    }
    return true;
}

void Px4FlowService::submitRead(OperationPurpose purpose)
{
    if (!m_parameterService || !m_reservation.isValid()
        || !m_boundLease.isValid()) {
        if (m_cleanupRequested) {
            finishCleanup(false,
                QStringLiteral("Cannot verify VIDEO_ONLY during cleanup."));
        } else {
            m_operationStatus = QStringLiteral(
                "PX4Flow parameter route is unavailable.");
            emitChanged();
        }
        return;
    }
    ParameterService::ExactReadRequest request;
    request.name = kVideoOnly;
    ParameterService::ExactOperationToken operation;
    QString error;
    m_operationPurpose = purpose;
    m_apiCallInFlight = true;
    QPointer<Px4FlowService> guard(this);
    const auto result = m_parameterService->submitComponentRead(
        m_reservation, m_boundLease, request, &operation, &error);
    if (!guard) {
        return;
    }
    m_apiCallInFlight = false;
    if (operation.isValid()) {
        // The writer may have synchronously completed the operation even
        // when the submit return describes transport failure.  Its immutable
        // terminal report is queued to us, so install the token first.
        m_operation = operation;
        if (m_cleanupRequested && purpose == OperationPurpose::DiscoverMode) {
            beginCleanup(m_operationStatus);
        } else {
            emitChanged();
        }
        return;
    }
    if (result != ParameterService::ExactSubmitResult::Started) {
        m_operationPurpose = OperationPurpose::None;
        if (m_cleanupRequested) {
            if (purpose == OperationPurpose::DiscoverMode) {
                advanceCleanup();
            } else {
                finishCleanup(false, error.isEmpty()
                    ? QStringLiteral("VIDEO_ONLY cleanup read could not start.")
                    : error);
            }
        } else {
            m_operationStatus = error.isEmpty()
                ? QStringLiteral("VIDEO_ONLY could not be read.") : error;
            emitChanged();
        }
        return;
    }
    m_operationStatus = QStringLiteral(
        "VIDEO_ONLY read started without an operation identity.");
    emitChanged();
}

void Px4FlowService::submitWrite(
    bool enabled, OperationPurpose purpose)
{
    if (!m_parameterService || !m_reservation.isValid()
        || !m_boundLease.isValid()
        || m_modeType == ParameterType::Unknown) {
        if (m_cleanupRequested) {
            finishCleanup(false,
                QStringLiteral("VIDEO_ONLY type is unknown during cleanup."));
        } else {
            m_operationStatus = QStringLiteral(
                "VIDEO_ONLY type is unknown; no value was written.");
            emitChanged();
        }
        return;
    }
    ParameterService::ExactWriteRequest request;
    request.name = kVideoOnly;
    request.value = enabled ? QVariant(1.0) : QVariant(0.0);
    request.type = m_modeType;
    request.force = false;
    ParameterService::ExactOperationToken operation;
    QString error;
    m_operationPurpose = purpose;
    m_apiCallInFlight = true;
    QPointer<Px4FlowService> guard(this);
    const auto result = m_parameterService->submitComponentWrite(
        m_reservation, m_boundLease, request, &operation, &error);
    if (!guard) {
        return;
    }
    m_apiCallInFlight = false;
    if (operation.isValid()) {
        m_operation = operation;
        if (m_cleanupRequested && purpose == OperationPurpose::ToggleMode) {
            beginCleanup(m_operationStatus);
        } else {
            emitChanged();
        }
        return;
    }
    if (result != ParameterService::ExactSubmitResult::Started) {
        m_operationPurpose = OperationPurpose::None;
        if (m_cleanupRequested) {
            if (purpose == OperationPurpose::ToggleMode) {
                advanceCleanup();
            } else {
                finishCleanup(false, error.isEmpty()
                    ? QStringLiteral("VIDEO_ONLY=0 cleanup could not start.")
                    : error);
            }
        } else {
            m_operationStatus = error.isEmpty()
                ? QStringLiteral("VIDEO_ONLY could not be written.") : error;
            emitChanged();
        }
        return;
    }
    m_operationStatus = QStringLiteral(
        "VIDEO_ONLY write started without an operation identity.");
    emitChanged();
}

void Px4FlowService::handleExactReport(
    const ParameterService::ExactOperationReport &report)
{
    if (!m_operation.isValid() || !report.token.isValid()
        || report.token.operationId != m_operation.operationId
        || report.token.reservationId != m_operation.reservationId
        || !report.token.isComponentOperation()
        || !report.token.componentLease.sameInstance(m_boundLease)) {
        return;
    }

    const OperationPurpose purpose = m_operationPurpose;
    const ParameterService::ExactOperationToken completed = m_operation;
    m_operation = {};
    m_operationPurpose = OperationPurpose::None;
    using Terminal = ParameterService::ExactTerminalResult;
    const bool readSucceeded =
        report.terminalResult == Terminal::ReadSucceeded;
    const bool writeSucceeded =
        report.terminalResult == Terminal::WriteSucceeded
        || report.terminalResult == Terminal::WriteSkipped;

    if (purpose == OperationPurpose::DiscoverMode) {
        if (readSucceeded) {
            bool valid = false;
            const bool enabled = modeFromValue(report.value, &valid);
            if (valid && report.type != ParameterType::Unknown) {
                m_modeKnown = true;
                m_videoOnly = enabled;
                m_modeType = report.type;
                m_modeUncertain = false;
                m_operationStatus.clear();
            } else {
                m_modeKnown = false;
                m_operationStatus = QStringLiteral(
                    "VIDEO_ONLY returned an unsupported value or type.");
            }
        } else {
            m_modeKnown = false;
            m_operationStatus = report.description.isEmpty()
                ? QStringLiteral("VIDEO_ONLY could not be read.")
                : report.description;
        }
        if (m_cleanupRequested) {
            advanceCleanup();
            return;
        }
        emitChanged();
        return;
    }

    if (purpose == OperationPurpose::ToggleMode) {
        if (writeSucceeded) {
            bool valid = false;
            const bool enabled = modeFromValue(
                completed.normalizedValue, &valid);
            m_modeKnown = valid;
            m_videoOnly = valid && enabled;
            m_modeType = completed.type;
            m_modeUncertain = false;
            m_operationStatus.clear();
            if (m_cleanupRequested) {
                advanceCleanup();
                return;
            }
            emitChanged();
            return;
        }
        if (terminalIsUncertain(report.terminalResult)) {
            m_modeKnown = false;
            m_modeUncertain = true;
            m_uncertainValue = completed.normalizedValue;
            m_modeType = completed.type;
            m_operationStatus = QStringLiteral(
                "VIDEO_ONLY write outcome is uncertain; verifying and restoring zero.");
            beginCleanup(m_operationStatus);
            return;
        }
        if (m_cleanupRequested) {
            bool valid = false;
            const bool enabled = modeFromValue(report.value, &valid);
            if (report.terminalResult == Terminal::Rejected && valid
                && report.type != ParameterType::Unknown) {
                m_modeKnown = true;
                m_videoOnly = enabled;
                m_modeType = report.type;
            }
            advanceCleanup();
            return;
        }
        m_operationStatus = report.description.isEmpty()
            ? QStringLiteral("VIDEO_ONLY was not changed.")
            : report.description;
        emitChanged();
        return;
    }

    if (purpose == OperationPurpose::CleanupRead) {
        if (!readSucceeded) {
            finishCleanup(false, report.description.isEmpty()
                ? QStringLiteral("Could not verify VIDEO_ONLY during cleanup.")
                : report.description);
            return;
        }
        bool valid = false;
        const bool enabled = modeFromValue(report.value, &valid);
        if (!valid || report.type == ParameterType::Unknown) {
            finishCleanup(false, QStringLiteral(
                "VIDEO_ONLY returned an unsupported value during cleanup."));
            return;
        }
        m_modeKnown = true;
        m_videoOnly = enabled;
        m_modeType = report.type;
        m_modeUncertain = false;
        if (enabled) {
            m_operationStatus = QStringLiteral(
                "Restoring VIDEO_ONLY=0 on %1...")
                    .arg(endpointLabel(m_boundLease));
            submitWrite(false, OperationPurpose::CleanupWrite);
        } else {
            finishCleanup(true);
        }
        return;
    }

    if (purpose == OperationPurpose::CleanupWrite) {
        if (writeSucceeded) {
            m_modeKnown = true;
            m_videoOnly = false;
            m_modeUncertain = false;
            finishCleanup(true);
        } else {
            const QString message = terminalIsUncertain(report.terminalResult)
                ? QStringLiteral(
                    "VIDEO_ONLY=0 was transmitted but not confirmed; the sensor may remain in focus mode.")
                : (report.description.isEmpty()
                       ? QStringLiteral("VIDEO_ONLY=0 cleanup failed.")
                       : report.description);
            finishCleanup(false, message);
        }
        return;
    }

    if (m_cleanupRequested) {
        advanceCleanup();
    }
}

void Px4FlowService::beginCleanup(const QString &reason)
{
    if (!m_cleanupRequested) {
        m_cleanupRequested = true;
        m_cleanupAttempted = false;
        m_cleanupElapsed.restart();
    }
    m_operationStatus = reason;
    resetStreamState(false);
    if (m_apiCallInFlight) {
        emitChanged();
        return;
    }
    if (m_operation.isValid() && m_parameterService
        && m_reservation.isValid()) {
        if (m_operationPurpose == OperationPurpose::CleanupRead
            || m_operationPurpose == OperationPurpose::CleanupWrite) {
            // A second source/replay/close intent changes the destination
            // after cleanup, not the already-running reset on the old sensor.
            m_cleanupPollTimer.start();
            emitChanged();
            return;
        }
        QPointer<Px4FlowService> guard(this);
        const bool cancelled = m_parameterService->cancelExactOperation(
            m_reservation, m_operation, reason);
        if (!guard) {
            return;
        }
        // false may mean that the exact operation already completed and its
        // queued immutable report has not reached us yet.  Preserve the token
        // and wait instead of trusting the pre-write mode snapshot.
        Q_UNUSED(cancelled);
        m_cleanupPollTimer.start();
        emitChanged();
        return;
    }
    advanceCleanup();
}

void Px4FlowService::advanceCleanup()
{
    m_cleanupPollTimer.stop();
    if (!m_cleanupRequested) {
        return;
    }
    if (m_operation.isValid()) {
        if (m_cleanupElapsed.isValid()
            && m_cleanupElapsed.elapsed() < CleanupDeadlineMs) {
            m_cleanupPollTimer.start();
            return;
        }
        finishCleanup(false, QStringLiteral(
            "The pending VIDEO_ONLY operation did not retire before the cleanup deadline."));
        return;
    }
    if (!m_parameterService || !m_registry || !m_reservation.isValid()
        || !m_boundLease.isValid()
        || !m_registry->validateLease(m_boundLease)) {
        const bool alreadySafe = m_modeKnown && !m_videoOnly
            && !m_modeUncertain;
        finishCleanup(alreadySafe, alreadySafe ? QString() : QStringLiteral(
            "The original PX4Flow instance disappeared before VIDEO_ONLY=0 was confirmed."));
        return;
    }
    if (!m_cleanupElapsed.isValid()
        || m_cleanupElapsed.elapsed() >= CleanupDeadlineMs) {
        finishCleanup(false, QStringLiteral(
            "VIDEO_ONLY cleanup timed out; the sensor may remain in focus mode."));
        return;
    }
    if (m_modeUncertain && m_modeType != ParameterType::Unknown
        && m_uncertainValue.isValid()
        && m_parameterService->isComponentWriteQuarantined(
            m_boundLease, kVideoOnly, m_uncertainValue, m_modeType)) {
        m_operationStatus = QStringLiteral(
            "Waiting for the uncertain VIDEO_ONLY write fence before cleanup...");
        m_cleanupPollTimer.start();
        emitChanged();
        return;
    }
    if (!m_modeKnown || m_modeUncertain) {
        if (m_cleanupAttempted) {
            finishCleanup(false, QStringLiteral(
                "VIDEO_ONLY could not be verified during cleanup."));
            return;
        }
        m_cleanupAttempted = true;
        m_operationStatus = QStringLiteral(
            "Verifying VIDEO_ONLY on %1 before cleanup...")
                .arg(endpointLabel(m_boundLease));
        submitRead(OperationPurpose::CleanupRead);
        return;
    }
    if (m_videoOnly) {
        m_cleanupAttempted = true;
        m_operationStatus = QStringLiteral(
            "Restoring VIDEO_ONLY=0 on %1...")
                .arg(endpointLabel(m_boundLease));
        submitWrite(false, OperationPurpose::CleanupWrite);
        return;
    }
    finishCleanup(true);
}

void Px4FlowService::finishCleanup(bool safe, const QString &message)
{
    m_cleanupPollTimer.stop();
    m_cleanupRequested = false;
    m_cleanupAttempted = false;
    const bool unsafeState = !m_modeKnown || m_videoOnly || m_modeUncertain
        || (m_operation.isValid()
            && m_operation.kind
                == ParameterService::ExactOperationKind::Write);
    if (!safe && unsafeState) {
        m_persistentWarning = message.isEmpty()
            ? QStringLiteral(
                "VIDEO_ONLY=0 was not confirmed on the original PX4Flow source.")
            : message;
    }
    m_operationStatus.clear();
    if (!releaseReservation()) {
        return;
    }
    applyPendingTransition();
}

void Px4FlowService::applyPendingTransition()
{
    if (m_pendingReplayGeneration != 0 && !m_shuttingDown) {
        const quint64 generation = m_pendingReplayGeneration;
        m_pendingReplayGeneration = 0;
        startReplayNow(generation);
        return;
    }
    if (m_sourceTransitionPending) {
        m_selectedSourceId = m_pendingSourceId;
        m_pendingSourceId.clear();
        m_sourceTransitionPending = false;
        resetStreamState(true);
    }
    if (!m_shuttingDown && m_active && !m_replay
        && !m_selectedSourceId.isEmpty()) {
        bindSelectedSource();
    } else {
        emitChanged();
    }
}

void Px4FlowService::retireSource(
    const MavlinkComponentInstanceLease &lease)
{
    if (!lease.isValid() || !m_boundLease.sameInstance(lease)) {
        if (sourceId(lease) == m_selectedSourceId) {
            m_operationStatus = QStringLiteral(
                "The selected PX4Flow source retired.");
            resetStreamState(true);
            emitChanged();
        }
        return;
    }
    const bool unsafe = m_videoOnly || m_modeUncertain
        || (m_operation.isValid()
            && m_operation.kind
                == ParameterService::ExactOperationKind::Write);
    m_cleanupRequested = false;
    m_cleanupPollTimer.stop();
    if (!releaseReservation()) {
        return;
    }
    resetStreamState(true);
    m_operationStatus = QStringLiteral(
        "The selected PX4Flow source retired; it was not retargeted.");
    if (unsafe) {
        m_persistentWarning = QStringLiteral(
            "The PX4Flow instance retired before VIDEO_ONLY=0 was confirmed.");
    }
    emitChanged();
}

void Px4FlowService::observeImageMessage(
    const mavlink_message_t &message)
{
    if (message.msgid == MAVLINK_MSG_ID_DATA_TRANSMISSION_HANDSHAKE) {
        beginFrame(message);
    } else if (message.msgid == MAVLINK_MSG_ID_ENCAPSULATED_DATA) {
        addFramePacket(message);
    }
}

void Px4FlowService::beginFrame(const mavlink_message_t &message)
{
    mavlink_data_transmission_handshake_t data{};
    mavlink_msg_data_transmission_handshake_decode(&message, &data);
    QString error;
    const auto result = m_assembler.begin(
        {data.type, data.size, data.width, data.height, data.packets,
         data.payload, data.jpg_quality}, &error);
    if (result != Px4FlowFrameAssembler::BeginResult::Started) {
        m_partialFrameTimer.stop();
        if (!m_streamStaleLatched) {
            m_streamStatus = error.isEmpty()
                ? QStringLiteral("Unsupported PX4Flow image descriptor.")
                : error;
        }
        emitChanged();
        return;
    }
    m_partialFrameTimer.start();
    if (!m_streamStaleLatched) {
        m_streamStatus = QStringLiteral(
            "Receiving PX4Flow image (%1 packets)...")
                .arg(m_assembler.expectedPackets());
    }
    emitChanged();
}

void Px4FlowService::addFramePacket(const mavlink_message_t &message)
{
    mavlink_encapsulated_data_t packet{};
    mavlink_msg_encapsulated_data_decode(&message, &packet);
    const auto outcome = m_assembler.add(
        packet.seqnr, packet.data,
        MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN);
    using PacketResult = Px4FlowFrameAssembler::PacketResult;
    switch (outcome.result) {
    case PacketResult::Accepted:
    case PacketResult::Duplicate:
        return;
    case PacketResult::Completed:
        m_partialFrameTimer.stop();
        if (outcome.completedFrame) {
            publishFrame(*outcome.completedFrame);
        }
        return;
    case PacketResult::IgnoredNoFrame:
        return;
    case PacketResult::InvalidSequence:
        if (!m_streamStaleLatched) {
            m_streamStatus = QStringLiteral(
                "PX4Flow sent an invalid image packet sequence.");
        }
        break;
    case PacketResult::InvalidPayload:
        if (!m_streamStaleLatched) {
            m_streamStatus = QStringLiteral(
                "PX4Flow sent an invalid image packet payload.");
        }
        break;
    case PacketResult::ConflictingDuplicate:
        if (!m_streamStaleLatched) {
            m_streamStatus = QStringLiteral(
                "PX4Flow sent conflicting duplicate image data.");
        }
        break;
    }
    m_partialFrameTimer.stop();
    emitChanged();
}

void Px4FlowService::publishFrame(
    const Px4FlowFrameAssembler::Frame &completed)
{
    if (completed.width == 0 || completed.height == 0
        || completed.grayscale.size()
            != int(completed.width) * int(completed.height)) {
        m_streamStatus = QStringLiteral(
            "PX4Flow completed an invalid grayscale image.");
        emitChanged();
        return;
    }
    QImage image(completed.width, completed.height,
                 QImage::Format_Grayscale8);
    if (image.isNull()) {
        m_streamStatus = QStringLiteral(
            "PX4Flow image could not be allocated.");
        emitChanged();
        return;
    }
    const int width = completed.width;
    for (int row = 0; row < completed.height; ++row) {
        uchar *destination = image.scanLine(row);
        std::memcpy(destination,
                    completed.grayscale.constData() + row * width,
                    static_cast<std::size_t>(width));
        if (image.bytesPerLine() > width) {
            std::memset(destination + width, 0,
                        static_cast<std::size_t>(
                            image.bytesPerLine() - width));
        }
    }
    m_frame = image;
    m_streamStaleLatched = false;
    m_streamStatus = QStringLiteral("PX4Flow image %1 x %2.")
        .arg(completed.width).arg(completed.height);
    m_staleFrameTimer.start();
    emitChanged();
}

void Px4FlowService::resetStreamState(bool clearImage)
{
    m_partialFrameTimer.stop();
    m_staleFrameTimer.stop();
    m_assembler.reset();
    m_streamStatus.clear();
    m_streamStaleLatched = false;
    if (clearImage) {
        m_frame = QImage();
    }
}

void Px4FlowService::startReplayNow(quint64 generation)
{
    resetStreamState(true);
    m_replay = true;
    m_replayGeneration = generation;
    m_replayEndpointPinned = false;
    m_replaySystemId = 0;
    m_replayComponentId = 0;
    m_pendingReplayGeneration = 0;
    m_active = true;
    m_selectedSourceId = replaySourceId(generation);
    m_operationStatus.clear();
    m_staleFrameTimer.start();
    emitChanged();
}

void Px4FlowService::emitChanged()
{
    emit changed();
}
