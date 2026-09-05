#ifndef VEHICLECOMMANDSERVICE_H
#define VEHICLECOMMANDSERVICE_H

#include "SwarmTelemetryRegistry.h"
#include "VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <array>
#include <functional>

#include <mavlink.h>

class VehicleTargetManager;
class ExactLinkTransmitter;

/**
 * Exact-link MAVLink COMMAND_LONG/COMMAND_INT transport.
 *
 * Legacy sends are authorized by VehicleTargetLease. Group operations reserve
 * immutable SwarmVehicleInstanceLease endpoints and use the exact transaction
 * API below. A stale lease or unavailable route fails closed; this service
 * never fans a command out over links collected by the legacy sysid-only UAS
 * object and remains the single consumer of COMMAND_ACK for both paths.
 */
class VehicleCommandService final : public QObject
{
    Q_OBJECT

public:
    using ExactLeaseValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease)>;
    using ExactRouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;

    enum class SendResult {
        Sent,
        InvalidTarget,
        StaleTarget,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    /**
     * A reservation pins a set of exact vehicle instances to one owner.  The
     * owner may disappear after a command reaches the writer; in that case the
     * application-owned service keeps draining the ACK/timeout while refusing
     * further submissions through the detached reservation.
     */
    struct ExactReservationToken
    {
        QPointer<QObject> owner;
        quint64 reservationId = 0;
        QList<SwarmVehicleInstanceLease> leases;

        bool isValid() const noexcept
        {
            return !owner.isNull() && reservationId != 0
                && !leases.isEmpty();
        }
    };

    struct ExactCommandRequest
    {
        MAV_CMD command = static_cast<MAV_CMD>(0);
        quint8 confirmation = 0;
        std::array<float, 7> params{};
        // Values <= 0 select DefaultExactCommandTimeoutMs.
        int acknowledgementTimeoutMs = 0;
        // Values <= 0 select DefaultExactCommandMaximumLifetimeMs. This
        // absolute lifetime is captured at submission and is never extended
        // by MAV_RESULT_IN_PROGRESS acknowledgements.
        int maximumLifetimeMs = 0;
        // Optional operation-specific safety gate, run once immediately
        // before the command waiter and frame are created. Returning false
        // rejects the command without transmission.
        std::function<bool(QString *)> validateBeforeWrite;
    };

    struct ExactCommandToken
    {
        quint64 transactionId = 0;
        quint64 reservationId = 0;
        SwarmVehicleInstanceLease lease;
        MAV_CMD command = static_cast<MAV_CMD>(0);

        bool isValid() const noexcept
        {
            return transactionId != 0 && reservationId != 0
                && lease.isValid();
        }
    };

    enum class ExactReservationResult {
        Reserved,
        InvalidOwner,
        InvalidLease,
        StaleLease,
        RouteUnavailable,
        Busy,
        ContextUnavailable
    };
    Q_ENUM(ExactReservationResult)

    enum class ExactSubmitResult {
        Started,
        InvalidOwner,
        InvalidReservation,
        InvalidLease,
        InvalidCommand,
        StaleLease,
        RouteUnavailable,
        Busy,
        Quarantined,
        ContextUnavailable,
        TransportOutcomeUncertain
    };
    Q_ENUM(ExactSubmitResult)

    enum class ExactTerminalResult {
        AcknowledgedAccepted,
        AcknowledgedRejected,
        TimedOutOutcomeUncertain,
        TransportOutcomeUncertain,
        LeaseRetiredOutcomeUncertain,
        LinkForgottenOutcomeUncertain,
        RejectedBeforeTransmission
    };
    Q_ENUM(ExactTerminalResult)

    struct ExactCommandReport
    {
        ExactCommandToken token;
        ExactTerminalResult terminalResult =
            ExactTerminalResult::TransportOutcomeUncertain;
        int mavResult = -1;
        int progress = 255;
        int resultParam2 = 0;
        int acknowledgementTargetSystem = 0;
        int acknowledgementTargetComponent = 0;
        // The shared transmitter reached its frame writer; this is not proof
        // that the underlying socket accepted any bytes.
        bool frameAttempted = false;
        bool ownerDetached = false;
        QString description;
    };

    static constexpr int DefaultExactCommandTimeoutMs = 2000;
    static constexpr int DefaultExactCommandMaximumLifetimeMs =
        10 * 60 * 1000;
    static constexpr int DefaultExactQuarantineMs = 6000;

    explicit VehicleCommandService(VehicleTargetManager *targetManager,
                                   ExactLinkTransmitter *transmitter,
                                   QObject *parent = nullptr);

    void setLocalIdentity(quint8 systemId, quint8 componentId);

    /**
     * Installs the application-owned registry and route validation seams.
     * LinkManager supplies validators backed by SwarmTelemetryRegistry and by
     * its exact-link route policy, and forwards endpointRetired() to
     * retireExactVehicle().  Reconfiguration is rejected while reservations
     * or exact transactions exist.
     */
    bool configureExactTransactions(
        ExactLeaseValidator leaseValidator,
        ExactRouteValidator routeValidator);
    bool configureSingleVehicleExactRoute(
        ExactRouteValidator routeValidator);
    void setExactCommandTimeoutForTesting(int timeoutMs);
    void setExactQuarantineForTesting(int timeoutMs);

    ExactReservationResult reserveExactEndpoints(
        QObject *owner,
        const QList<SwarmVehicleInstanceLease> &leases,
        ExactReservationToken *reservationOut,
        QString *error = nullptr);
    ExactReservationResult reserveSingleVehicleEndpoint(
        QObject *owner,
        const VehicleTargetLease &target,
        const SwarmVehicleInstanceLease &lease,
        ExactReservationToken *reservationOut,
        QString *error = nullptr);
    bool releaseExactReservation(const ExactReservationToken &reservation);
    /**
     * Starts one ACK-gated COMMAND_LONG. exactCommandFinished() may be emitted
     * synchronously from this call, so application orchestrators must connect
     * before submitting and correlate exclusively through commandOut/token.
     */
    ExactSubmitResult submitExactCommandLong(
        const ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ExactCommandRequest &request,
        ExactCommandToken *commandOut = nullptr,
        QString *error = nullptr);
    bool isExactCommandQuarantined(
        const SwarmVehicleInstanceLease &lease, MAV_CMD command);

    /** Called by LinkManager for SwarmTelemetryRegistry::endpointRetired(). */
    void retireExactVehicle(const SwarmVehicleInstanceLease &lease);

    Q_INVOKABLE int sendCurrentCommandLong(
        int command, int confirmation,
        float param1, float param2, float param3, float param4,
        float param5, float param6, float param7);

    SendResult sendCommandLong(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        MAV_CMD command, quint8 confirmation,
        float param1, float param2, float param3, float param4,
        float param5, float param6, float param7);

    SendResult sendCommandInt(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        MAV_CMD command, MAV_FRAME frame,
        float param1, float param2, float param3, float param4,
        qint32 x, qint32 y, float z,
        quint8 current = 0, quint8 autocontinue = 0);

    void forgetLink(int linkId);
    void observeMessage(int linkId, const mavlink_message_t &message);

signals:
    void commandAckReceived(
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        int command, int result, int progress, int resultParam2,
        int targetSystem, int targetComponent);
    void exactCommandProgress(
        VehicleCommandService::ExactCommandToken token,
        int mavResult, int progress, int resultParam2);
    void exactCommandFinished(
        VehicleCommandService::ExactCommandReport report);
    void exactReservationReleased(qulonglong reservationId);

private:
    enum class ExactReservationPolicy
    {
        Swarm,
        SingleVehicle
    };

    struct SenderIdentity
    {
        VehicleEndpoint endpoint;
        quint8 systemId = 0;
        quint8 componentId = 0;
    };

    struct ExactReservationRecord
    {
        QPointer<QObject> owner;
        QList<SwarmVehicleInstanceLease> leases;
        VehicleTargetLease target;
        ExactReservationPolicy policy = ExactReservationPolicy::Swarm;
        bool closing = false;
        QMetaObject::Connection ownerDestroyedConnection;
    };

    struct PendingExactCommand
    {
        ExactCommandToken token;
        quint8 localSystemId = 0;
        quint8 localComponentId = 0;
        qint64 deadlineMs = 0;
        qint64 absoluteDeadlineMs = 0;
        int timeoutMs = DefaultExactCommandTimeoutMs;
        bool frameAttempted = false;
    };

    struct QuarantinedExactCommand
    {
        VehicleEndpoint endpoint;
        quint16 command = 0;
        quint8 localSystemId = 0;
        quint8 localComponentId = 0;
        qint64 expiresAtMs = 0;
    };

    bool targetIsCurrent(const VehicleTargetLease &target) const;
    bool leaseIsCurrent(const SwarmVehicleInstanceLease &lease) const;
    bool reservationTargetIsCurrent(
        const ExactReservationRecord &reservation) const;
    ExactReservationResult reserveExactEndpointsWithPolicy(
        QObject *owner,
        const QList<SwarmVehicleInstanceLease> &leases,
        ExactReservationPolicy policy,
        const VehicleTargetLease &target,
        ExactReservationToken *reservationOut,
        QString *error);
    bool reservationContains(
        const ExactReservationRecord &reservation,
        const SwarmVehicleInstanceLease &lease) const;
    bool legacyCommandPendingFor(const VehicleEndpoint &endpoint) const;
    bool exactEndpointBlocksLegacy(const VehicleEndpoint &endpoint,
                                   quint16 command);
    bool acknowledgementTargets(
        quint8 targetSystem, quint8 targetComponent,
        quint8 localSystemId, quint8 localComponentId) const noexcept;
    bool observeExactAcknowledgement(
        int linkId, const mavlink_message_t &message,
        const mavlink_command_ack_t &acknowledgement);
    bool matchesQuarantine(
        const VehicleEndpoint &endpoint, quint16 command,
        quint8 targetSystem, quint8 targetComponent,
        int *index = nullptr) const;
    void addQuarantine(const PendingExactCommand &pending);
    void cleanupExpiredQuarantines();
    void scheduleExactDeadline();
    void scheduleQuarantineExpiry();
    void handleExactDeadline();
    void handleQuarantineExpiry();
    void handleTargetGenerationChanged(qulonglong generation);
    void handleExactOwnerDestroyed(quint64 reservationId);
    void finishExactCommand(
        quint64 transactionId, ExactTerminalResult result,
        int mavResult, int progress, int resultParam2,
        int acknowledgementTargetSystem,
        int acknowledgementTargetComponent,
        const QString &description, bool quarantine);
    bool reservationHasPending(quint64 reservationId) const;
    void maybeReleaseClosingReservation(quint64 reservationId);
    void removeReservation(quint64 reservationId);
    quint64 nextExactReservationId();
    quint64 nextExactTransactionId();
    SendResult finalizeAndWrite(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        quint16 command, mavlink_message_t message);
    void clearPendingCommands();

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    QHash<quint64, QHash<quint16, SenderIdentity>> m_pendingCommands;
    ExactLeaseValidator m_exactLeaseValidator;
    ExactRouteValidator m_exactRouteValidator;
    ExactRouteValidator m_singleVehicleExactRouteValidator;
    QHash<quint64, ExactReservationRecord> m_exactReservations;
    QHash<VehicleEndpoint, quint64> m_exactEndpointReservations;
    QHash<quint64, PendingExactCommand> m_pendingExactCommands;
    QHash<VehicleEndpoint, quint64> m_pendingExactByEndpoint;
    QList<QuarantinedExactCommand> m_exactQuarantines;
    QElapsedTimer m_exactClock;
    QTimer m_exactDeadlineTimer;
    QTimer m_exactQuarantineTimer;
    quint64 m_nextExactReservationId = 0;
    quint64 m_nextExactTransactionId = 0;
    int m_exactCommandTimeoutMs = DefaultExactCommandTimeoutMs;
    int m_exactQuarantineMs = DefaultExactQuarantineMs;
    bool m_exactApiInFlight = false;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
};

Q_DECLARE_METATYPE(VehicleCommandService::ExactReservationToken)
Q_DECLARE_METATYPE(VehicleCommandService::ExactCommandRequest)
Q_DECLARE_METATYPE(VehicleCommandService::ExactCommandToken)
Q_DECLARE_METATYPE(VehicleCommandService::ExactReservationResult)
Q_DECLARE_METATYPE(VehicleCommandService::ExactSubmitResult)
Q_DECLARE_METATYPE(VehicleCommandService::ExactTerminalResult)
Q_DECLARE_METATYPE(VehicleCommandService::ExactCommandReport)

#endif // VEHICLECOMMANDSERVICE_H
