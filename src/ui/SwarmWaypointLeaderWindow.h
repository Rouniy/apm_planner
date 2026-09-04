#ifndef SWARMWAYPOINTLEADERWINDOW_H
#define SWARMWAYPOINTLEADERWINDOW_H

#include "tools/SwarmWaypointLeaderCore.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QCloseEvent;

/** One presentation snapshot pinned to a complete swarm instance lease. */
struct SwarmWaypointLeaderWindowVehicle
{
    SwarmVehicleInstanceLease lease;
    QString label;
    QString firmware;
    QString liveStatus;
    QString missionPosition;
    QString commandedTarget;
    bool groundEligible = false;
    bool flightEligible = false;
    bool profilePositionValid = false;
    double pathDistanceM = 0.0;
    double relativeAltitudeM = 0.0;
};

/**
 * Fakeable production boundary for discovery, exact mission ownership and the
 * complete Waypoint Leader executor.
 *
 * Implementations must keep every method on the GUI thread.  validatePlan()
 * is read-only and must recheck full exact-instance, mission-generation and
 * executor readiness.  cancelActiveRun() is the synchronous lifecycle seam:
 * after it returns the implementation may not emit another command for the
 * cancelled run.
 */
class SwarmWaypointLeaderWindowInterface : public QObject
{
public:
    using ChangedHandler = std::function<void()>;

    explicit SwarmWaypointLeaderWindowInterface(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~SwarmWaypointLeaderWindowInterface() override = default;

    virtual QVector<SwarmWaypointLeaderWindowVehicle> vehicles() const = 0;
    virtual void refreshVehicles() = 0;

    virtual bool missionForAirMaster(
        const SwarmVehicleInstanceLease &airMaster,
        SwarmWaypointLeaderMissionSnapshot *mission,
        QString *error) const = 0;
    /**
     * Monotonic revision of complete successful mission observations for this
     * exact air-master instance.  It must advance even when a new download has
     * identical content generation and digest, and must be zero when no
     * successful observation exists.
     */
    virtual quint64 missionObservationRevision(
        const SwarmVehicleInstanceLease &airMaster) const noexcept = 0;
    virtual bool refreshMission(
        const SwarmVehicleInstanceLease &airMaster,
        QString *error) = 0;
    /** Synchronously detach/cancel only this exact instance's refresh. */
    virtual void cancelMissionRefresh(
        const SwarmVehicleInstanceLease &airMaster,
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
    virtual void setChangedHandler(ChangedHandler handler) = 0;
};

/** Mission Planner 10 Tools > Swarm Waypoint Leader (Beta). */
class SwarmWaypointLeaderWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirmDangerous;
    };

    static constexpr int WindowWidth = 1320;
    static constexpr int WindowHeight = 790;
    static constexpr int MinimumWindowWidth = 1040;
    static constexpr int MinimumWindowHeight = 650;

    explicit SwarmWaypointLeaderWindow(QWidget *owner = nullptr);
    SwarmWaypointLeaderWindow(
        SwarmWaypointLeaderWindowInterface *interface,
        Dependencies dependencies,
        QWidget *owner = nullptr);
    ~SwarmWaypointLeaderWindow() override;

    static SwarmWaypointLeaderWindow *OpenWindow(QWidget *owner = nullptr);
    static SwarmWaypointLeaderWindow *OpenWindow(
        SwarmWaypointLeaderWindowInterface *interface,
        Dependencies dependencies,
        QWidget *owner = nullptr);

    QString statusText() const;
    bool isRunning() const noexcept;
    int vehicleCount() const noexcept;

public slots:
    void refreshVehicles();
    void refreshMission();

signals:
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    class Implementation;
    std::unique_ptr<Implementation> m_impl;

    static QPointer<SwarmWaypointLeaderWindow> s_current;
};

#endif // SWARMWAYPOINTLEADERWINDOW_H
