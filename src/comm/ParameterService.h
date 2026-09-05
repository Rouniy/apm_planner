#ifndef PARAMETERSERVICE_H
#define PARAMETERSERVICE_H

#include "VehicleEndpoint.h"
#include "MavlinkComponentInstanceLease.h"
#include "SwarmTelemetryRegistry.h"
#include "core/parameters/ParameterCodec.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QVariant>
#include <QVariantList>

#include <functional>

#include <mavlink.h>

class ExactLinkTransmitter;
class ParameterStore;
class VehicleTargetManager;

/**
 * Exact-endpoint implementation of the classic MAVLink parameter protocol.
 *
 * Legacy requests are authorized by a current VehicleTargetLease. Production
 * exact requests use either a reserved SwarmVehicleInstanceLease or a
 * deliberately separate peripheral component lease with its own injected
 * registry/route policy. All traffic stays on the specified physical link and
 * the single PARAM_VALUE consumer correlates the complete endpoint, lifetime,
 * name, type and normalized-value envelope. Cache ownership is independent of
 * UI widgets and remains split per endpoint.
 */
class ParameterService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *store READ storeObject CONSTANT)

public:
    enum class SendResult {
        Sent,
        InvalidTarget,
        StaleTarget,
        InvalidParameter,
        TransportUnavailable,
        Busy
    };
    Q_ENUM(SendResult)

    enum class WriteFailureReason {
        InvalidTarget,
        StaleTarget,
        InvalidParameter,
        TransportUnavailable,
        Timeout,
        TypeMismatch,
        ValueRejected
    };
    Q_ENUM(WriteFailureReason)

    using ExactLeaseValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease)>;
    using ExactRouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;
    using ComponentLeaseValidator = std::function<bool(
        const MavlinkComponentInstanceLease &lease)>;
    using ComponentRouteValidator = std::function<bool(
        const MavlinkComponentInstanceLease &lease, QString *error)>;

    struct ExactReservationToken
    {
        QPointer<QObject> owner;
        quint64 reservationId = 0;
        QList<SwarmVehicleInstanceLease> leases;
        QList<MavlinkComponentInstanceLease> componentLeases;

        bool isValid() const noexcept
        {
            return !owner.isNull() && reservationId != 0
                && ((!leases.isEmpty() && componentLeases.isEmpty())
                    || (leases.isEmpty()
                        && componentLeases.size() == 1));
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

    enum class ExactOperationKind {
        Read,
        Write
    };
    Q_ENUM(ExactOperationKind)

    struct ExactReadRequest
    {
        QString name;
    };

    struct ExactWriteRequest
    {
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
        bool force = false;
    };

    struct ExactOperationToken
    {
        quint64 operationId = 0;
        quint64 reservationId = 0;
        SwarmVehicleInstanceLease lease;
        MavlinkComponentInstanceLease componentLease;
        ExactOperationKind kind = ExactOperationKind::Read;
        QString name;
        ParameterType type = ParameterType::Unknown;
        QVariant normalizedValue;

        bool isValid() const noexcept
        {
            return operationId != 0 && reservationId != 0
                && (lease.isValid() != componentLease.isValid())
                && !name.isEmpty();
        }

        bool isComponentOperation() const noexcept
        {
            return componentLease.isValid() && !lease.isValid();
        }
    };

    enum class ExactSubmitResult {
        Started,
        InvalidOwner,
        InvalidReservation,
        InvalidLease,
        InvalidParameter,
        StaleLease,
        RouteUnavailable,
        Busy,
        Quarantined,
        ContextUnavailable,
        TransportUnavailable,
        TransportOutcomeUncertain
    };
    Q_ENUM(ExactSubmitResult)

    enum class ExactTerminalResult {
        ReadSucceeded,
        WriteSucceeded,
        WriteSkipped,
        Rejected,
        ReadCancelled,
        WriteCancelled,
        WriteCancelledOutcomeUncertain,
        ReadTimedOut,
        ReadTransportFailure,
        ReadLeaseRetired,
        ReadLinkForgotten,
        WriteTimedOutOutcomeUncertain,
        WriteTransportOutcomeUncertain,
        WriteLeaseRetiredOutcomeUncertain,
        WriteLinkForgottenOutcomeUncertain,
        WriteTimedOut
    };
    Q_ENUM(ExactTerminalResult)

    struct ExactOperationReport
    {
        ExactOperationToken token;
        ExactTerminalResult terminalResult =
            ExactTerminalResult::ReadTransportFailure;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
        int attempts = 0;
        bool frameAttempted = false;
        bool ownerDetached = false;
        QString description;
    };

    static constexpr int DefaultExactReadRetryIntervalMs = 700;
    static constexpr int DefaultExactReadMaximumRetries = 3;
    static constexpr int DefaultExactWriteRetryIntervalMs = 700;
    static constexpr int DefaultExactWriteMaximumRetries = 3;
    static constexpr int DefaultExactWriteMaximumLifetimeMs = 4000;
    static constexpr int DefaultExactWriteQuarantineMs = 6000;

    explicit ParameterService(VehicleTargetManager *targetManager,
                              ExactLinkTransmitter *transmitter,
                              QObject *parent = nullptr);

    ParameterStore *store() const { return m_store; }
    QObject *storeObject() const;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    bool configureExactTransactions(
        ExactLeaseValidator leaseValidator,
        ExactRouteValidator routeValidator);
    bool configureSingleVehicleExactRoute(
        ExactRouteValidator routeValidator);
    bool configureComponentExactTransactions(
        ComponentLeaseValidator leaseValidator,
        ComponentRouteValidator routeValidator);
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
    ExactReservationResult reserveComponentEndpoint(
        QObject *owner,
        const MavlinkComponentInstanceLease &lease,
        ExactReservationToken *reservationOut,
        QString *error = nullptr);
    bool releaseExactReservation(
        const ExactReservationToken &reservation);
    bool cancelExactOperation(
        const ExactReservationToken &reservation,
        const ExactOperationToken &operation,
        const QString &reason = QString());
    ExactSubmitResult submitExactRead(
        const ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ExactReadRequest &request,
        ExactOperationToken *operationOut = nullptr,
        QString *error = nullptr);
    ExactSubmitResult submitExactWrite(
        const ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ExactWriteRequest &request,
        ExactOperationToken *operationOut = nullptr,
        QString *error = nullptr);
    ExactSubmitResult submitComponentRead(
        const ExactReservationToken &reservation,
        const MavlinkComponentInstanceLease &lease,
        const ExactReadRequest &request,
        ExactOperationToken *operationOut = nullptr,
        QString *error = nullptr);
    ExactSubmitResult submitComponentWrite(
        const ExactReservationToken &reservation,
        const MavlinkComponentInstanceLease &lease,
        const ExactWriteRequest &request,
        ExactOperationToken *operationOut = nullptr,
        QString *error = nullptr);
    void retireExactVehicle(const SwarmVehicleInstanceLease &lease);
    void retireComponent(const MavlinkComponentInstanceLease &lease);
    bool isExactWriteQuarantined(
        const SwarmVehicleInstanceLease &lease,
        const QString &name,
        const QVariant &value,
        ParameterType type);
    bool isComponentWriteQuarantined(
        const MavlinkComponentInstanceLease &lease,
        const QString &name,
        const QVariant &value,
        ParameterType type);
    Q_INVOKABLE int requestCurrentParameterList();
    Q_INVOKABLE int requestCurrentParameterRead(const QString &name);
    Q_INVOKABLE int requestCurrentParameterReadByIndex(int index);
    Q_INVOKABLE bool cancelCurrentParameterList();
    Q_INVOKABLE int setCurrentParameter(const QString &name,
                                        const QVariant &value, int type);
    Q_INVOKABLE qulonglong writeCurrentParameter(
        const QString &name, const QVariant &value, bool force = false);
    Q_INVOKABLE qulonglong writeCurrentParameters(
        const QVariantList &changes, bool force = false);
    Q_INVOKABLE bool cancelParameterWrite(qulonglong transactionId);
    Q_INVOKABLE QVariant currentParameterValue(const QString &name) const;
    Q_INVOKABLE QVariantList currentParameters() const;

    SendResult requestParameterList(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId);
    SendResult requestParameterRead(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        const QString &name);
    SendResult requestParameterReadByIndex(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        int index);
    SendResult setParameter(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        const QString &name, const QVariant &value, ParameterType type,
        bool force = false);

    void observeMessage(int linkId, const mavlink_message_t &message);
    void observePhysicalMessage(int linkId, quint64 linkSessionEpoch,
                                const mavlink_message_t &message);
    void observeComponentMessage(int linkId, quint64 linkSessionEpoch,
                                 const mavlink_message_t &message);
    void setEncoding(const VehicleEndpoint &endpoint,
                     ParameterEncoding encoding);
    void forgetLink(int linkId);

    // Keeps production timing identical to Mission Planner while allowing
    // deterministic, fast unit tests of the retry state machine.
    void setRetryPolicyForTesting(int inactivityMs,
                                  int missingBurstIntervalMs,
                                  int maximumWholeListRetries = 2,
                                  int maximumIndexAttempts = 3);
    void setWriteRetryPolicyForTesting(int acknowledgementTimeoutMs,
                                       int maximumRetries = 3);
    void setExactRetryPolicyForTesting(
        int readRetryIntervalMs,
        int readMaximumRetries,
        int writeRetryIntervalMs,
        int writeMaximumRetries,
        int writeMaximumLifetimeMs,
        int quarantineMs = DefaultExactWriteQuarantineMs);

signals:
    void listStarted(qulonglong targetGeneration,
                     int linkId, int systemId, int componentId);
    void listCompleted(qulonglong targetGeneration,
                       int linkId, int systemId, int componentId);
    void listFailed(qulonglong targetGeneration,
                    int linkId, int systemId, int componentId,
                    const QString &reason);
    void listCancelled(qulonglong targetGeneration,
                       int linkId, int systemId, int componentId);
    void parameterValueReceived(
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        int parameterCount, int parameterIndex,
        const QString &name, const QVariant &value, int type);
    void parameterRead(qulonglong targetGeneration,
                       int linkId, int systemId, int componentId,
                       const QString &name, const QVariant &value, int type);
    void parameterWriteAcknowledged(
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name, const QVariant &value, int type);
    void parameterWriteStarted(
        qulonglong transactionId, qulonglong batchId,
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name, const QVariant &value, int type);
    void parameterWriteRetried(qulonglong transactionId, int attempt);
    void parameterWriteSkipped(
        qulonglong transactionId, qulonglong batchId,
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name, const QVariant &value, int type);
    void parameterWriteCompleted(
        qulonglong transactionId, qulonglong batchId,
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name, const QVariant &value, int type);
    void parameterWriteFailed(
        qulonglong transactionId, qulonglong batchId,
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name, int reason, const QString &message);
    void parameterWriteCancelled(
        qulonglong transactionId, qulonglong batchId,
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        const QString &name);
    void parameterBatchStarted(
        qulonglong batchId, qulonglong targetGeneration,
        int linkId, int systemId, int componentId, int total);
    void parameterBatchProgress(qulonglong batchId,
                                int completed, int total,
                                int succeeded, int failed);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void transactionsCancelled(qulonglong targetGeneration);
    void exactOperationRetried(
        ParameterService::ExactOperationToken token, int attempt);
    void exactOperationFinished(
        ParameterService::ExactOperationReport report);
    void exactReservationReleased(qulonglong reservationId);

private:
    struct PendingRead
    {
        VehicleTargetLease target;
    };

    struct PendingWrite
    {
        quint64 transactionId = 0;
        quint64 batchId = 0;
        VehicleTargetLease target;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
        QString name;
        QVariant requestedValue;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
        ParameterEncoding encoding = ParameterEncoding::Bytewise;
        float wireValue = 0.0F;
        bool force = false;
        bool schemaBound = false;
        bool skip = false;
        int attempts = 0;
    };

    struct PendingBatch
    {
        int total = 0;
        int completed = 0;
        int succeeded = 0;
        int failed = 0;
    };

    struct QueuedListRequest
    {
        bool active = false;
        VehicleTargetLease target;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    };

    enum class ExactReservationPolicy
    {
        Swarm,
        SingleVehicle,
        Component
    };

    enum class ExactLeaseDomain
    {
        Swarm,
        Component
    };

    struct ExactInstanceLease
    {
        VehicleEndpoint endpoint;
        quint64 linkSessionEpoch = 0;
        quint64 instanceEpoch = 0;
        ExactLeaseDomain domain = ExactLeaseDomain::Swarm;

        bool isValid() const noexcept
        {
            return endpoint.isValid() && linkSessionEpoch != 0
                && instanceEpoch != 0;
        }

        bool sameInstance(const ExactInstanceLease &other) const noexcept
        {
            return domain == other.domain
                && endpoint.sameIdentity(other.endpoint)
                && linkSessionEpoch == other.linkSessionEpoch
                && instanceEpoch == other.instanceEpoch;
        }

        bool operator==(const ExactInstanceLease &other) const noexcept
        {
            return sameInstance(other);
        }
    };

    struct ExactReservationRecord
    {
        QPointer<QObject> owner;
        QList<ExactInstanceLease> leases;
        VehicleTargetLease target;
        ExactReservationPolicy policy = ExactReservationPolicy::Swarm;
        bool closing = false;
        QMetaObject::Connection ownerDestroyedConnection;
    };

    struct PendingExactOperation
    {
        ExactOperationToken token;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
        ParameterEncoding encoding = ParameterEncoding::Bytewise;
        float wireValue = 0.0F;
        int attempts = 0;
        int retryIntervalMs = DefaultExactReadRetryIntervalMs;
        int maximumAttempts = 1 + DefaultExactReadMaximumRetries;
        qint64 absoluteDeadlineMs = 0;
        bool frameAttempted = false;
    };

    struct ExactCachedValue
    {
        ExactInstanceLease lease;
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
    };

    struct ExactWriteQuarantine
    {
        VehicleEndpoint endpoint;
        QString name;
        QVariant normalizedValue;
        ParameterType type = ParameterType::Unknown;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
        qint64 expiresAtMs = 0;
    };

    bool targetIsCurrent(const VehicleTargetLease &target) const;
    static ExactInstanceLease exactInstance(
        const SwarmVehicleInstanceLease &lease);
    static ExactInstanceLease exactInstance(
        const MavlinkComponentInstanceLease &lease);
    static ExactInstanceLease exactInstance(
        const ExactOperationToken &token);
    static bool reservationTokenMatches(
        const ExactReservationToken &token,
        const ExactReservationRecord &record);
    bool exactLeaseIsCurrent(const ExactInstanceLease &lease) const;
    bool exactRouteIsEligible(
        const ExactInstanceLease &lease,
        ExactReservationPolicy policy,
        QString *error) const;
    bool exactReservationTargetIsCurrent(
        const ExactReservationRecord &reservation) const;
    bool exactReservationContains(
        const ExactReservationRecord &reservation,
        const ExactInstanceLease &lease) const;
    bool legacyOperationTouches(const VehicleEndpoint &endpoint) const;
    bool exactEndpointReserved(const VehicleEndpoint &endpoint) const;
    static bool parameterNameBytes(const QString &name, QByteArray *bytes);
    static ParameterType parameterType(quint8 mavlinkType);
    static bool sameEnvelope(const VehicleTargetLease &target,
                             int linkId,
                             const mavlink_message_t &message);
    VehicleEndpoint endpointFor(int linkId,
                                const mavlink_message_t &message) const;
    ParameterEncoding encodingFor(const VehicleEndpoint &endpoint) const;
    SendResult send(const VehicleTargetLease &target,
                    quint8 localSystemId, quint8 localComponentId,
                    mavlink_message_t message);
    SendResult startParameterList(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId);
    SendResult transmitParameterListRequest();
    SendResult transmitParameterIndexRequest(int index);
    void completeParameterList(const VehicleEndpoint &source);
    void failParameterList(const QString &reason);
    void cancelParameterList(bool notify);
    void clearParameterListState();
    void restartListTimer(int intervalMs);
    void handleListRetryTimeout();
    SendResult enqueueParameterWrite(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        const QString &name, const QVariant &value, ParameterType type,
        bool force, bool schemaBound, quint64 batchId,
        quint64 *transactionId);
    bool validateParameterWrite(
        const VehicleTargetLease &target,
        const QString &name, const QVariant &value, ParameterType type,
        QByteArray *nameBytes = nullptr) const;
    bool normalizeParameterWrite(
        const VehicleTargetLease &target,
        const QVariant &value, ParameterType type,
        QVariant *normalizedValue, float *wireValue,
        ParameterEncoding *encoding) const;
    void pumpTransactions();
    void startActiveWrite();
    SendResult transmitActiveWrite();
    void scheduleActiveWriteTimeout();
    void handleWriteRetryTimeout(quint64 transactionId, int attempt);
    void acknowledgeActiveWrite(const QVariant &value, ParameterType type);
    void skipActiveWrite();
    void failActiveWrite(WriteFailureReason reason, const QString &message);
    void cancelActiveWrite();
    void cancelQueuedWritesForGeneration(quint64 generation,
                                         int linkId = -1);
    void finishBatchItem(quint64 batchId, bool succeeded);
    void cancelQueuedParameterList(bool notify);
    void handleTargetGenerationChanged(qulonglong generation);
    void handleTargetGenerationSettled(qulonglong generation);
    void syncSelectedEndpoint();
    void cancelTransactions(quint64 currentGeneration);
    ExactReservationResult reserveExactEndpointsWithPolicy(
        QObject *owner,
        const QList<ExactInstanceLease> &leases,
        ExactReservationPolicy policy,
        const VehicleTargetLease &target,
        ExactReservationToken *reservationOut,
        QString *error);
    ExactSubmitResult submitExactOperation(
        const ExactReservationToken &reservation,
        const ExactInstanceLease &lease,
        ExactOperationKind kind,
        const QString &name,
        const QVariant &value,
        ParameterType type,
        bool force,
        ExactOperationToken *operationOut,
        QString *error);
    ExactSubmitResult transmitExactOperation();
    void scheduleExactRetry();
    void handleExactRetryTimeout();
    void finishExactOperation(
        ExactTerminalResult result,
        const QVariant &value,
        ParameterType type,
        const QString &description,
        bool quarantine);
    bool observeExactParameterValue(
        int linkId,
        const mavlink_message_t &message,
        const VehicleEndpoint &source,
        const QString &name,
        ParameterType type,
        const QVariant &value,
        int parameterCount,
        int parameterIndex);
    void rememberExactValue(
        const ExactInstanceLease &lease,
        const QString &name,
        const QVariant &value,
        ParameterType type);
    bool exactCacheMatches(
        const ExactInstanceLease &lease,
        const QString &name,
        const QVariant &value,
        ParameterType type) const;
    bool matchesExactWriteQuarantine(
        const VehicleEndpoint &endpoint,
        const QString &name,
        int *index = nullptr) const;
    bool exactEndpointQuarantined(
        const VehicleEndpoint &endpoint) const;
    void addExactWriteQuarantine(
        const PendingExactOperation &operation);
    void cleanupExpiredExactQuarantines();
    void scheduleExactQuarantineExpiry();
    void touchLegacyTrafficFence(const VehicleEndpoint &endpoint);
    bool legacyTrafficFenced(const VehicleEndpoint &endpoint) const;
    void handleExactOwnerDestroyed(quint64 reservationId);
    bool exactReservationHasPending(quint64 reservationId) const;
    void maybeReleaseExactReservation(quint64 reservationId);
    void removeExactReservation(quint64 reservationId);
    void retireExactInstance(const ExactInstanceLease &lease);
    quint64 nextExactReservationId();
    quint64 nextExactOperationId();

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    ParameterStore *const m_store;
    QHash<VehicleEndpoint, ParameterEncoding> m_encodings;
    bool m_listActive = false;
    VehicleTargetLease m_listTarget;
    quint8 m_listLocalSystemId = 255;
    quint8 m_listLocalComponentId = MAV_COMP_ID_MISSIONPLANNER;
    int m_listReportedCount = 0;
    QSet<int> m_listReceivedIndices;
    QHash<int, int> m_listIndexAttempts;
    int m_listNextMissingIndex = 0;
    int m_listWholeRetries = 0;
    int m_listInactivityMs = 4000;
    int m_listMissingBurstIntervalMs = 1000;
    int m_listMaximumWholeRetries = 2;
    int m_listMaximumIndexAttempts = 3;
    QTimer m_listRetryTimer;
    QueuedListRequest m_queuedList;
    QSet<quint64> m_writesBeforeQueuedList;
    QHash<QString, PendingRead> m_pendingReads;
    QQueue<PendingWrite> m_writeQueue;
    PendingWrite m_activeWrite;
    bool m_writeActive = false;
    QHash<quint64, PendingBatch> m_pendingBatches;
    quint64 m_nextWriteTransactionId = 1;
    quint64 m_nextBatchId = 1;
    int m_writeAcknowledgementTimeoutMs = 700;
    int m_writeMaximumRetries = 3;
    bool m_pumpingTransactions = false;
    bool m_pumpAgain = false;
    bool m_cancellingTransactions = false;
    quint64 m_lastHandledTargetGeneration = 0;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    ExactLeaseValidator m_exactLeaseValidator;
    ExactRouteValidator m_exactRouteValidator;
    ExactRouteValidator m_singleVehicleExactRouteValidator;
    ComponentLeaseValidator m_componentLeaseValidator;
    ComponentRouteValidator m_componentRouteValidator;
    bool m_exactApiInFlight = false;
    QHash<quint64, ExactReservationRecord> m_exactReservations;
    QHash<VehicleEndpoint, quint64> m_exactEndpointReservations;
    PendingExactOperation m_exactOperation;
    bool m_exactOperationActive = false;
    QHash<VehicleEndpoint, QHash<QString, ExactCachedValue>>
        m_exactCachedValues;
    QList<ExactWriteQuarantine> m_exactWriteQuarantines;
    QHash<VehicleEndpoint, qint64> m_legacyTrafficFenceExpiries;
    QElapsedTimer m_exactClock;
    QTimer m_exactRetryTimer;
    QTimer m_exactQuarantineTimer;
    quint64 m_nextExactReservationId = 0;
    quint64 m_nextExactOperationId = 0;
    int m_exactReadRetryIntervalMs = DefaultExactReadRetryIntervalMs;
    int m_exactReadMaximumRetries = DefaultExactReadMaximumRetries;
    int m_exactWriteRetryIntervalMs = DefaultExactWriteRetryIntervalMs;
    int m_exactWriteMaximumRetries = DefaultExactWriteMaximumRetries;
    int m_exactWriteMaximumLifetimeMs =
        DefaultExactWriteMaximumLifetimeMs;
    int m_exactWriteQuarantineMs = DefaultExactWriteQuarantineMs;
};

Q_DECLARE_METATYPE(ParameterService::ExactReservationToken)
Q_DECLARE_METATYPE(ParameterService::ExactReservationResult)
Q_DECLARE_METATYPE(ParameterService::ExactOperationKind)
Q_DECLARE_METATYPE(ParameterService::ExactReadRequest)
Q_DECLARE_METATYPE(ParameterService::ExactWriteRequest)
Q_DECLARE_METATYPE(ParameterService::ExactOperationToken)
Q_DECLARE_METATYPE(ParameterService::ExactSubmitResult)
Q_DECLARE_METATYPE(ParameterService::ExactTerminalResult)
Q_DECLARE_METATYPE(ParameterService::ExactOperationReport)

#endif // PARAMETERSERVICE_H
