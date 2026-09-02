#ifndef FENCERALLYCONTROLLER_H
#define FENCERALLYCONTROLLER_H

#include "FenceRallyModel.h"
#include "FlightPlannerMissionModel.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>

class FlightPlannerViewModel;

namespace MissionPlanner
{

/**
 * Typed facade for the Fence and Rally stores owned by FlightPlannerViewModel.
 *
 * Mission transfers deliberately remain in FlightPlannerViewModel: that class
 * owns the binding to MissionTransferController and already protects transfer
 * identity, mission type and stale terminal results.  This facade selects the
 * corresponding store before forwarding a read or write, so Fence and Rally
 * use MAV_MISSION_TYPE_FENCE/RALLY through the same protocol implementation as
 * the normal mission store.
 */
class FenceRallyController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(FlightPlannerViewModel *Planner READ Planner WRITE setPlanner
               NOTIFY plannerChanged)
    Q_PROPERTY(QString LastError READ LastError NOTIFY lastErrorChanged)

public:
    explicit FenceRallyController(QObject *parent = nullptr);
    explicit FenceRallyController(FlightPlannerViewModel *planner,
                                  QObject *parent = nullptr);
    ~FenceRallyController() override;

    FlightPlannerViewModel *Planner() const;
    QString LastError() const;

    Fence::DecodeResult CurrentFence() const;
    RallyPoints::DecodeResult CurrentRally() const;

    bool SetFence(const Fence &fence, bool append = false);
    bool SetRally(const RallyPoints &rally, bool append = false);

public slots:
    void setPlanner(FlightPlannerViewModel *planner);

    bool LoadLegacyFence(const QString &path, bool append = false);
    bool SaveLegacyFence(const QString &path);
    bool LoadLegacyRally(const QString &path, bool append = false);
    bool SaveLegacyRally(const QString &path);

    bool DownloadFence();
    bool UploadFence();
    bool DownloadRally();
    bool UploadRally();

signals:
    void plannerChanged(FlightPlannerViewModel *planner);
    void lastErrorChanged(const QString &error);
    void errorOccurred(const QString &error);

private:
    bool applyFence(const Fence &fence, bool append, QString *error);
    bool applyRally(const RallyPoints &rally, bool append, QString *error);
    bool startTransfer(FlightPlannerMissionModel::MissionStore store,
                       bool upload);
    bool requireIdlePlanner(const QString &operation);
    bool fail(const QString &error);
    void clearError();
    void setSuccessStatus(const QString &status);

    QPointer<FlightPlannerViewModel> m_planner;
    QMetaObject::Connection m_plannerDestroyedConnection;
    QString m_lastError;

    Q_DISABLE_COPY(FenceRallyController)
};

} // namespace MissionPlanner

#endif // FENCERALLYCONTROLLER_H
