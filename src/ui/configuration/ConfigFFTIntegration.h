#ifndef CONFIGFFTINTEGRATION_H
#define CONFIGFFTINTEGRATION_H

#include "ParamField.h"
#include "comm/ParameterService.h"
#include "comm/VehicleTargetManager.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

class ConfigFFTView;
class QTimer;

/**
 * Exact-parameter bridge for one ConfigFFTView lifetime.
 *
 * The selected target generation and vehicle-instance lease are immutable.
 * A target/link replacement retires only this page's work; offline FFT
 * analysis remains owned by the transport-neutral view model.
 */
class ConfigFFTIntegration final : public QObject
{
public:
    using ExactLeaseProvider = std::function<SwarmVehicleInstanceLease(
        const VehicleTargetLease &target)>;

    ConfigFFTIntegration(
        ConfigFFTView *view,
        ParameterService *parameters,
        VehicleTargetManager *targets,
        ExactLeaseProvider exactLeaseProvider,
        QObject *parent = nullptr);
    ~ConfigFFTIntegration() override;

    VehicleTargetLease pinnedTarget() const { return m_target; }
    SwarmVehicleInstanceLease pinnedVehicle() const { return m_vehicle; }
    bool parameterTargetUsable() const;
    bool operationActive() const noexcept
    {
        return m_activity != Activity::None;
    }

private:
    enum class Activity
    {
        None,
        Refresh,
        Write
    };

    static QStringList parameterNames();
    static QString submitError(
        ParameterService::ExactSubmitResult result,
        const QString &detail);
    static QString reservationError(
        ParameterService::ExactReservationResult result,
        const QString &detail);

    bool exactLeaseStillCurrent();
    bool beginReservation(Activity activity, QString *error);
    void releaseReservation();
    void syncParameterContext();
    void publishInitialSnapshot();
    void retirePinnedTarget(const QString &reason);

    void beginRefresh(int componentId);
    void submitNextRead();
    void beginWrite(quint64 requestId, int componentId,
                    const QString &name, const QVariant &value);
    void receiveReport(const ParameterService::ExactOperationReport &report);
    void processReport(const ParameterService::ExactOperationReport &report);
    void finishRefreshFailure(const QString &reason);
    void finishWriteFailure(const QString &reason, bool cancelled);
    void clearActivity();

    QPointer<ConfigFFTView> m_view;
    QPointer<ParameterService> m_parameters;
    QPointer<VehicleTargetManager> m_targets;
    QTimer *m_freshnessTimer = nullptr;
    ExactLeaseProvider m_exactLeaseProvider;
    VehicleTargetLease m_target;
    SwarmVehicleInstanceLease m_vehicle;
    ParameterService::ExactReservationToken m_reservation;
    ParameterService::ExactOperationToken m_operation;
    Activity m_activity = Activity::None;
    QStringList m_refreshNames;
    int m_refreshIndex = 0;
    QList<ConfigFriendlyParameterValue> m_refreshValues;
    QHash<QString, ParameterType> m_types;
    QVector<ParameterService::ExactOperationReport> m_deferredReports;
    quint64 m_writeRequestId = 0;
    qulonglong m_writeBatchId = 0;
    QString m_writeName;
    QVariant m_writeValue;
    bool m_submitting = false;
    bool m_retired = false;
    bool m_destroying = false;
};

ConfigFFTIntegration *BindConfigFFTViewToExactParameters(
    ConfigFFTView *view,
    ParameterService *parameters,
    VehicleTargetManager *targets,
    ConfigFFTIntegration::ExactLeaseProvider exactLeaseProvider);

#endif // CONFIGFFTINTEGRATION_H
