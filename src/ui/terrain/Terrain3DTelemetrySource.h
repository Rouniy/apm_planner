#ifndef TERRAIN3DTELEMETRYSOURCE_H
#define TERRAIN3DTELEMETRYSOURCE_H

#include "comm/VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <functional>

#include <mavlink.h>

class VehicleTargetManager;

struct Terrain3DTelemetrySnapshot
{
    VehicleTargetLease lease;
    bool heartbeatValid = false;
    bool armed = false;
    QString mode;
    bool positionValid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeAmslM = 0.0;
    double altitudeRelativeM = 0.0;
    double velocityNorthMps = 0.0;
    double velocityEastMps = 0.0;
    double velocityUpMps = 0.0;
    qint64 positionObservedMs = -1;
    bool attitudeValid = false;
    double rollDeg = 0.0;
    double pitchDeg = 0.0;
    double yawDeg = 0.0;

    bool isValid() const noexcept { return lease.isValid(); }
};

/** Exact-target, read-only telemetry feed for the software terrain viewer. */
class Terrain3DTelemetrySource final : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>;
    using ModeFormatter =
        std::function<QString(int, int, quint32, quint8)>;

    explicit Terrain3DTelemetrySource(
        VehicleTargetManager *targets,
        Clock clock = Clock(),
        ModeFormatter modeFormatter = ModeFormatter(),
        QObject *parent = nullptr);

    Terrain3DTelemetrySnapshot snapshot() const { return m_snapshot; }
    qint64 monotonicTimeMs() const;
    bool isCurrentLease(const VehicleTargetLease &lease) const;

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);

signals:
    void snapshotChanged();
    void targetEpochChanged();

private slots:
    void invalidateTargetEpoch();
    void resetTargetEpoch();

private:
    bool accepts(int linkId, const mavlink_message_t &message) const;
    static QString fallbackModeText(int autopilot, int vehicleType,
                                    quint32 customMode, quint8 baseMode);

    VehicleTargetManager *const m_targets;
    Clock m_clock;
    ModeFormatter m_modeFormatter;
    QElapsedTimer m_elapsedClock;
    Terrain3DTelemetrySnapshot m_snapshot;
};

#endif // TERRAIN3DTELEMETRYSOURCE_H
