#ifndef PARAMETERSERVICE_H
#define PARAMETERSERVICE_H

#include "VehicleEndpoint.h"
#include "core/parameters/ParameterCodec.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QVariant>
#include <QVariantList>

#include <mavlink.h>

class ExactLinkTransmitter;
class ParameterStore;
class VehicleTargetManager;

/**
 * Exact-endpoint implementation of the classic MAVLink parameter protocol.
 *
 * Requests are authorized by a current VehicleTargetLease and written only to
 * its physical link.  PARAM_VALUE responses are correlated with the complete
 * (link, system, component, generation, name/type/value) envelope.  Cache
 * ownership is independent of UI widgets and remains split per endpoint.
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
        TransportUnavailable
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

    explicit ParameterService(VehicleTargetManager *targetManager,
                              ExactLinkTransmitter *transmitter,
                              QObject *parent = nullptr);

    ParameterStore *store() const { return m_store; }
    QObject *storeObject() const;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
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

    bool targetIsCurrent(const VehicleTargetLease &target) const;
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
};

#endif // PARAMETERSERVICE_H
