#ifndef REMOTEDATAFLASHLOGSERVICE_H
#define REMOTEDATAFLASHLOGSERVICE_H

#include "SwarmTelemetryRegistry.h"
#include "RemoteDataFlashLogWriter.h"
#include "VehicleEndpoint.h"

#include <QSharedDataPointer>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class ExactLinkTransmitter;
class ParameterService;
class VehicleTargetManager;

/**
 * Application-owned receiver for ArduPilot's REMOTE_LOG_* streaming protocol.
 *
 * There is no protocol acknowledgement for START or STOP and no remote end of
 * file.  A successful report therefore means only that the explicitly stopped
 * local capture was published; it never claims that the autopilot stopped or
 * that the final DataFlash block was received.
 */
class RemoteDataFlashLogService final : public QObject
{
    Q_OBJECT

public:
    class Plan
    {
    public:
        Plan();
        Plan(const Plan &other);
        Plan &operator=(const Plan &other);
        ~Plan();

        bool isValid() const noexcept;
        QString directoryPath() const;
        QString fileStem() const;
        QString destinationDescription() const;
        VehicleTargetLease target() const;
        SwarmVehicleInstanceLease vehicle() const;
        quint8 localSystemId() const;
        quint8 localComponentId() const;

    private:
        class Data;
        QSharedDataPointer<Data> d;
        friend class RemoteDataFlashLogService;
    };

    struct MissingRange
    {
        quint32 first = 0;
        quint32 last = 0;
    };

    enum class Phase {
        Idle,
        Opening,
        AwaitingSequenceZero,
        Receiving,
        Stopping,
        Publishing,
        Discarding
    };
    Q_ENUM(Phase)

    enum class StartResult {
        Started,
        InvalidPlan,
        Busy,
        Unavailable,
        IoError,
        TransportUnavailable
    };
    Q_ENUM(StartResult)

    enum class Outcome {
        SavedUnverified,
        Cancelled,
        StartUnconfirmed,
        TimedOut,
        LeaseRetired,
        TransportFailed,
        IoError,
        ProtocolError,
        ShuttingDown
    };
    Q_ENUM(Outcome)

    struct Report
    {
        quint64 operationId = 0;
        QString destinationPath;
        VehicleEndpoint endpoint;
        Outcome outcome = Outcome::ProtocolError;
        qint64 bytes = 0;
        qint64 blocks = 0;
        qint64 duplicateBlocks = 0;
        qint64 missingBlocks = 0;
        quint32 highestSequence = 0;
        QVector<MissingRange> missingRanges;
        bool sequenceZeroObserved = false;
        bool startAttempted = false;
        bool stopAttempted = false;
        bool stopSubmitted = false;
        QStringList warnings;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && endpoint.isValid();
        }
        bool published() const noexcept
        {
            return outcome == Outcome::SavedUnverified
                && !destinationPath.isEmpty();
        }
    };

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &vehicle, QString *error)>;
    using Clock = std::function<qint64()>;

    static constexpr int MaximumHeartbeatAgeMs = 3000;
    static constexpr int DefaultFirstBlockTimeoutMs = 10000;
    static constexpr int DefaultStreamSilenceMs = 15000;
    static constexpr int DefaultMaximumSessionMs = 8 * 60 * 60 * 1000;
    static constexpr int DefaultStopQuietMs = 250;
    static constexpr int MaximumHistoryEntries = 512;
    static constexpr int MaximumPreSequenceZeroBlocks = 256;

    explicit RemoteDataFlashLogService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ParameterService *parameterService,
        ExactLinkTransmitter *transmitter,
        quint8 localSystemId,
        quint8 localComponentId,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    RemoteDataFlashLogService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ParameterService *parameterService,
        ExactLinkTransmitter *transmitter,
        quint8 localSystemId,
        quint8 localComponentId,
        RouteValidator routeValidator,
        Clock clock,
        int firstBlockTimeoutMs,
        int streamSilenceMs,
        int maximumSessionMs,
        int stopQuietMs,
        QObject *parent = nullptr);
    ~RemoteDataFlashLogService() override;

    bool busy() const noexcept;
    Phase phase() const noexcept;
    quint64 currentOperationId() const noexcept;
    Plan activePlan() const;
    QString status() const;
    QStringList history() const;
    Report lastReport() const;
    qint64 blocksStored() const noexcept;
    qint64 bytesStored() const noexcept;

    bool canPrepare(QString *error = nullptr) const;
    bool prepare(const QString &directoryPath, Plan *planOut,
                 QString *error = nullptr);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    StartResult start(const Plan &plan, quint64 *operationIdOut,
                      QString *error = nullptr);
    bool stopAndSave(quint64 operationId, QString *error = nullptr);
    bool cancel(quint64 operationId, QString *error = nullptr);
    void shutdown();

    /** Feed one centrally accepted packet with its immutable physical epoch. */
    void observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);

signals:
    void stateChanged();
    void operationFinished(RemoteDataFlashLogService::Report report);

private slots:
    void checkTimeouts();

private:
    struct Runtime;

    bool captureVehicle(VehicleTargetLease *target,
                        SwarmVehicleInstanceLease *vehicle,
                        QString *error) const;
    bool validateVehicle(const VehicleTargetLease &target,
                         const SwarmVehicleInstanceLease &vehicle,
                         bool requireDisarmed,
                         QString *error) const;
    bool validatePlanInternal(const Plan &plan, bool requireDisarmed,
                              QString *error) const;
    bool operationIsCurrent(quint64 operationId) const noexcept;
    bool deadlineReached() const;
    void handleWriterOpened(quint64 operationId, const QString &path);
    void handleBlockStored(quint64 operationId, quint32 sequence,
                           bool duplicate);
    void handleWriterFailed(quint64 operationId, const QString &reason);
    void handleWriterFinished(quint64 operationId,
                              const RemoteDataFlashLogWriter::Result &result);
    bool sendStatus(quint64 operationId, quint32 sequence, quint8 status,
                    bool *attempted = nullptr);
    void beginDiscard(Outcome outcome, const QString &description,
                      bool sendStopIfOwned, bool preservePartial = true);
    void finishReport(Outcome outcome, const QString &description);
    void appendHistory(const QString &line);

    std::unique_ptr<Runtime> m_runtime;
};

Q_DECLARE_METATYPE(RemoteDataFlashLogService::MissingRange)
Q_DECLARE_METATYPE(RemoteDataFlashLogService::Outcome)
Q_DECLARE_METATYPE(RemoteDataFlashLogService::Report)

#endif // REMOTEDATAFLASHLOGSERVICE_H
