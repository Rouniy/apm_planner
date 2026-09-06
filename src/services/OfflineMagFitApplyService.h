#ifndef OFFLINEMAGFITAPPLYSERVICE_H
#define OFFLINEMAGFITAPPLYSERVICE_H

#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleEndpoint.h"
#include "services/OfflineMagFitService.h"

#include <QSharedDataPointer>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class VehicleTargetManager;

class OfflineMagFitApplyService final : public QObject
{
    Q_OBJECT

public:
    struct Write
    {
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
    };

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
        QVector<OfflineMagFitResult> results() const;
        QVector<Write> writes() const;

    private:
        class Data;
        QSharedDataPointer<Data> d;
        friend class OfflineMagFitApplyService;
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

    struct Report
    {
        quint64 operationId = 0;
        QString sourcePath;
        VehicleEndpoint endpoint;
        Outcome outcome = Outcome::Rejected;
        int totalWrites = 0;
        int confirmedWrites = 0;
        int remainingWrites = 0;
        QVector<Write> receipts;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && endpoint.isValid();
        }
    };

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;

    static constexpr int MaximumHeartbeatAgeMs = 3000;
    static constexpr int MaximumHistoryEntries = 512;
    static constexpr int DefaultOverallDeadlineMs = 10 * 60 * 1000;

    explicit OfflineMagFitApplyService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ParameterService *parameterService,
        VehicleCommandService *commandService,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ~OfflineMagFitApplyService() override;

    bool busy() const noexcept;
    QString status() const;
    QStringList history() const;
    Report lastReport() const;
    quint64 currentOperationId() const noexcept;
    int progressCompleted() const noexcept;
    int progressTotal() const noexcept;

    bool canPrepare(QString *error = nullptr) const;
    bool prepare(const OfflineMagFitReport &analysis, Plan *planOut,
                 QString *error = nullptr);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    SubmitResult execute(const Plan &plan, quint64 *operationIdOut,
                         QString *error = nullptr);
    bool cancel(quint64 operationId);
    void shutdown();

    void setOverallDeadlineForTesting(int timeoutMs);

signals:
    void stateChanged();
    void operationFinished(OfflineMagFitApplyService::Report report);

private:
    struct Runtime;

    bool captureVehicle(VehicleTargetLease *target,
                        SwarmVehicleInstanceLease *vehicle,
                        QString *error) const;
    bool validateVehicle(const VehicleTargetLease &target,
                         const SwarmVehicleInstanceLease &vehicle,
                         QString *error) const;
    bool planMatchesPending(const Plan &plan) const;
    bool validatePlanSchema(const Plan &plan, bool requireInitialValues,
                            QString *error) const;
    bool deadlineReached() const;
    void scheduleNext();
    void submitCurrentWrite();
    void handleParameterFinished(
        const ParameterService::ExactOperationReport &report);
    void scheduleDeferredReport();
    void handleDeadline();
    void finish(Outcome outcome, const QString &description);
    void appendHistory(const QString &line);
    void releaseReservations();

    std::unique_ptr<Runtime> m_runtime;
};

Q_DECLARE_METATYPE(OfflineMagFitApplyService::Outcome)
Q_DECLARE_METATYPE(OfflineMagFitApplyService::Write)
Q_DECLARE_METATYPE(OfflineMagFitApplyService::Report)

#endif // OFFLINEMAGFITAPPLYSERVICE_H
