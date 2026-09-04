#ifndef WAYPOINTLEADERPROFILECONTROL_H
#define WAYPOINTLEADERPROFILECONTROL_H

#include "tools/SwarmWaypointLeaderCore.h"

#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class QPaintEvent;
class QPainter;

/** The role controls only the MP10 marker colour and legend membership. */
enum class WaypointLeaderVehicleRole
{
    GroundMaster,
    AirMaster,
    Follower
};

/**
 * Presentation data for one exact vehicle instance.
 *
 * The owning window derives label and both coordinates from the same immutable
 * SwarmVehicleInstanceLease/telemetry snapshot.  Keeping the role explicit
 * avoids inferring safety-relevant identity from a display label.
 */
struct WaypointLeaderVehicleMarker
{
    WaypointLeaderVehicleRole role = WaypointLeaderVehicleRole::Follower;
    QString label;
    double pathDistanceM = 0.0;
    double altitudeM = 0.0;
};

inline bool operator==(const WaypointLeaderVehicleMarker &left,
                       const WaypointLeaderVehicleMarker &right)
{
    return left.role == right.role && left.label == right.label
        && left.pathDistanceM == right.pathDistanceM
        && left.altitudeM == right.altitudeM;
}

inline bool operator!=(const WaypointLeaderVehicleMarker &left,
                       const WaypointLeaderVehicleMarker &right)
{
    return !(left == right);
}

Q_DECLARE_METATYPE(WaypointLeaderVehicleRole)
Q_DECLARE_METATYPE(WaypointLeaderVehicleMarker)

/**
 * Native MP10-recognizable altitude-over-mission plot for Waypoint Leader.
 *
 * setProfile() directly accepts SwarmWaypointLeaderMissionPath::profile().
 * Both input collections are copied, finite-filtered and capped, so a window
 * can safely replace telemetry snapshots without leaving borrowed state in the
 * paint path.  The control has no timers or transport ownership.
 */
class WaypointLeaderProfileControl final : public QWidget
{
    Q_OBJECT

public:
    static constexpr int MaximumProfilePoints = 4096;
    static constexpr int MaximumVehicleMarkers = 24;

    explicit WaypointLeaderProfileControl(QWidget *parent = nullptr);

    QVector<SwarmWaypointLeaderProfilePoint> profile() const
    {
        return m_profile;
    }
    void setProfile(
        const QVector<SwarmWaypointLeaderProfilePoint> &profile);

    QVector<WaypointLeaderVehicleMarker> vehicleMarkers() const
    {
        return m_vehicleMarkers;
    }
    void setVehicleMarkers(
        const QVector<WaypointLeaderVehicleMarker> &markers);

    bool hasDrawableProfile() const noexcept;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    /** Deterministic geometry seams shared by painting and widget tests. */
    QRectF plotBounds() const;
    QPointF profilePointPosition(int profileIndex) const;
    QPointF vehicleMarkerPosition(int markerIndex) const;

signals:
    void profileChanged();
    void vehicleMarkersChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawGrid(QPainter &painter, const QRectF &plot) const;
    void drawProfile(QPainter &painter, const QRectF &plot) const;
    void drawVehicleMarkers(QPainter &painter, const QRectF &plot) const;

    QVector<SwarmWaypointLeaderProfilePoint> m_profile;
    QVector<WaypointLeaderVehicleMarker> m_vehicleMarkers;
};

#endif // WAYPOINTLEADERPROFILECONTROL_H
