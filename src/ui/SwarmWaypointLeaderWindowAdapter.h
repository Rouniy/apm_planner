#ifndef SWARMWAYPOINTLEADERWINDOWADAPTER_H
#define SWARMWAYPOINTLEADERWINDOWADAPTER_H

#include "SwarmWaypointLeaderWindow.h"

#include "comm/ExactMissionSnapshotService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "services/SwarmWaypointLeaderExecutor.h"

#include <QObject>
#include <QVector>

#include <functional>
#include <memory>

/**
 * Deterministic boundary used by the production Waypoint Leader window
 * adapter.  The ordinary constructor below installs the application-service
 * implementation; the injectable constructor exists for exact lifecycle
 * tests and does not transfer ownership.
 */
class SwarmWaypointLeaderWindowAdapterBackend : public QObject
{
public:
    struct Callbacks
    {
        std::function<void()> vehiclesChanged;
        std::function<void()> missionCacheChanged;
        std::function<void(ExactMissionTransferResult)> missionFinished;
        std::function<void()> executorChanged;
        std::function<void()> availabilityChanged;
    };

    explicit SwarmWaypointLeaderWindowAdapterBackend(
        QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~SwarmWaypointLeaderWindowAdapterBackend() override = default;

    virtual void setCallbacks(Callbacks callbacks) = 0;

    virtual QVector<SwarmTelemetrySnapshot> vehicleSnapshots() const = 0;
    virtual void refreshVehicles() = 0;

    virtual bool acquireMission(
        const SwarmVehicleInstanceLease &airMaster,
        ExactMissionSnapshot *snapshot) const = 0;
    virtual ExactMissionSnapshotService::StartResult requestMission(
        QObject *owner,
        const SwarmVehicleInstanceLease &airMaster,
        ExactMissionTransferToken *token,
        QString *error) = 0;
    virtual bool cancelMission(
        const ExactMissionTransferToken &token,
        const QString &reason) = 0;

    virtual bool executorReady(QString *error) const = 0;
    virtual bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                              QString *error) const = 0;
    virtual bool start(const SwarmWaypointLeaderPlan &plan,
                       QString *error) = 0;
    virtual void cancelActiveRun(const QString &reason) = 0;
    virtual bool requestMode(SwarmWaypointLeaderMode mode,
                             QString *error) = 0;
    virtual bool isRunning() const noexcept = 0;
    virtual SwarmWaypointLeaderMode mode() const noexcept = 0;
    virtual QString statusText() const = 0;
};

/**
 * Production bridge between the bounded QWidget and the three application-
 * owned exact-instance services.
 *
 * The adapter never identifies a vehicle by sysid alone.  Its mission request
 * ownership is the complete (adapter, transfer token, exact air instance)
 * tuple, so cancellation and late/foreign completions cannot affect another
 * window or another physical link carrying the same MAVLink ids.
 */
class SwarmWaypointLeaderWindowAdapter final
    : public SwarmWaypointLeaderWindowInterface
{
public:
    SwarmWaypointLeaderWindowAdapter(
        SwarmTelemetryRegistry *registry,
        ExactMissionSnapshotService *missions,
        SwarmWaypointLeaderExecutor *executor,
        QObject *parent = nullptr);

    /** Test seam. The backend must outlive the adapter or may be destroyed. */
    explicit SwarmWaypointLeaderWindowAdapter(
        SwarmWaypointLeaderWindowAdapterBackend *backend,
        QObject *parent = nullptr);
    ~SwarmWaypointLeaderWindowAdapter() override;

    QVector<SwarmWaypointLeaderWindowVehicle> vehicles() const override;
    void refreshVehicles() override;

    bool missionForAirMaster(
        const SwarmVehicleInstanceLease &airMaster,
        SwarmWaypointLeaderMissionSnapshot *mission,
        QString *error) const override;
    quint64 missionObservationRevision(
        const SwarmVehicleInstanceLease &airMaster) const noexcept override;
    bool refreshMission(const SwarmVehicleInstanceLease &airMaster,
                        QString *error) override;
    void cancelMissionRefresh(
        const SwarmVehicleInstanceLease &airMaster,
        const QString &reason) override;

    bool executorReady(QString *error) const override;
    bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                      QString *error) const override;
    bool start(const SwarmWaypointLeaderPlan &plan,
               QString *error) override;
    void cancelActiveRun(const QString &reason) override;
    bool requestMode(SwarmWaypointLeaderMode mode,
                     QString *error) override;

    bool isRunning() const noexcept override;
    SwarmWaypointLeaderMode mode() const noexcept override;
    QString statusText() const override;
    void setChangedHandler(ChangedHandler handler) override;

private:
    class Implementation;
    std::unique_ptr<Implementation> m_impl;
};

#endif // SWARMWAYPOINTLEADERWINDOWADAPTER_H
