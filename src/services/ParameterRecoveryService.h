#ifndef PARAMETERRECOVERYSERVICE_H
#define PARAMETERRECOVERYSERVICE_H

#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleEndpoint.h"
#include "core/parameters/ParameterFileCodec.h"

#include <QSharedDataPointer>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class VehicleTargetManager;

/**
 * Application-owned, exact-instance recovery writer for Mission Planner
 * parameter files.  A prepared Plan owns the parsed values and the precise
 * vehicle lifetime; confirmation never causes the file to be reopened or the
 * operation to follow a newly selected vehicle.
 */
class ParameterRecoveryService final : public QObject
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
        QString sourcePath() const;
        VehicleTargetLease target() const;
        SwarmVehicleInstanceLease vehicle() const;
        QVector<ConfigRawParamsFileCodec::Entry> entries() const;

    private:
        class Data;
        QSharedDataPointer<Data> d;
        friend class ParameterRecoveryService;
    };

    enum class SubmitResult {
        Started,
        InvalidPlan,
        Busy,
        Unavailable
    };
    Q_ENUM(SubmitResult)

    enum class Outcome {
        Completed,
        Cancelled,
        Rejected,
        OutcomeUncertain
    };
    Q_ENUM(Outcome)

    struct Receipt
    {
        enum class Kind {
            EnableWrite,
            IdentifierReset,
            ParameterWrite
        };

        Kind kind = Kind::ParameterWrite;
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
        QString description;
    };

    struct Report
    {
        quint64 operationId = 0;
        QString sourcePath;
        VehicleEndpoint endpoint;
        Outcome outcome = Outcome::Rejected;
        int totalEntries = 0;
        int setCount = 0;
        int unchangedCount = 0;
        int failedCount = 0;
        int completedEntries = 0;
        int remainingEntries = 0;
        QStringList failedParameters;
        QVector<Receipt> receipts;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && endpoint.isValid();
        }
    };

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;

    static constexpr int MaximumHeartbeatAgeMs = 3000;
    static constexpr qint64 MaximumSourceBytes = 1024 * 1024;
    static constexpr int MaximumEntries = 10000;
    static constexpr int MaximumHistoryEntries = 512;
    static constexpr int DefaultOverallDeadlineMs = 30 * 60 * 1000;

    explicit ParameterRecoveryService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ParameterService *parameterService,
        VehicleCommandService *commandService,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ~ParameterRecoveryService() override;

    bool busy() const noexcept;
    QString status() const;
    QStringList history() const;
    Report lastReport() const;
    quint64 currentOperationId() const noexcept;
    int progressCompleted() const noexcept;
    int progressTotal() const noexcept;

    bool canPrepare(QString *error = nullptr) const;
    bool prepare(const QString &path, Plan *planOut,
                 QString *error = nullptr);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    SubmitResult execute(const Plan &plan, QString *error = nullptr);
    SubmitResult execute(const Plan &plan, quint64 *operationIdOut,
                         QString *error = nullptr);
    bool cancel(quint64 operationId);
    void shutdown();

    void setOverallDeadlineForTesting(int timeoutMs);

signals:
    void stateChanged();
    void operationFinished(ParameterRecoveryService::Report report);

private:
    struct Runtime;

    bool captureVehicle(VehicleTargetLease *target,
                        SwarmVehicleInstanceLease *vehicle,
                        QString *error) const;
    bool validateVehicle(const VehicleTargetLease &target,
                         const SwarmVehicleInstanceLease &vehicle,
                         QString *error) const;
    bool planMatchesPending(const Plan &plan) const;
    bool activeContextValid(QString *error) const;
    bool deadlineReached() const;
    void schedulePump();
    void pump();
    void submitRead(const ConfigRawParamsFileCodec::Entry &entry);
    void submitWrite(const ConfigRawParamsFileCodec::Entry &entry,
                     const QVariant &value, ParameterType type,
                     Receipt::Kind receiptKind);
    void handleParameterFinished(
        const ParameterService::ExactOperationReport &report);
    void scheduleDeferredReports();
    void handleDeadline();
    void recordFailure(const QString &name, const QString &description);
    void advanceMainEntry();
    void finish(Outcome outcome, const QString &description);
    void appendHistory(const QString &line);
    void releaseReservations();

    std::unique_ptr<Runtime> m_runtime;
};

Q_DECLARE_METATYPE(ParameterRecoveryService::Outcome)
Q_DECLARE_METATYPE(ParameterRecoveryService::Receipt)
Q_DECLARE_METATYPE(ParameterRecoveryService::Report)

#endif // PARAMETERRECOVERYSERVICE_H
