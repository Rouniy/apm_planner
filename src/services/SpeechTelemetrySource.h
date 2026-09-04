#ifndef SPEECHTELEMETRYSOURCE_H
#define SPEECHTELEMETRYSOURCE_H

#include "comm/VehicleEndpoint.h"
#include "uas/APMFirmwareVersion.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include <functional>

#include <mavlink.h>

class VehicleTargetManager;

/** One complete logical STATUSTEXT tied to its immutable source epoch. */
struct ExactStatusText
{
    VehicleTargetLease lease;
    quint8 rawSeverity = MAV_SEVERITY_INFO;
    quint8 severity = MAV_SEVERITY_INFO;
    quint16 messageId = 0;
    QString text;
    qint64 completedAtMs = -1;
};

Q_DECLARE_METATYPE(ExactStatusText)

/** Exact-target event for the non-STATUSTEXT speech inputs. */
struct ExactSpeechTelemetryEvent
{
    enum Kind : quint8 {
        Armed,
        FlightMode,
        Waypoint,
        Battery
    };

    VehicleTargetLease lease;
    Kind kind = Armed;
    bool armed = false;
    QString mode;
    int waypoint = 0;
    double batteryVoltage = 0.0;
    double batteryRemainingPercent = 0.0;
};

Q_DECLARE_METATYPE(ExactSpeechTelemetryEvent)

/**
 * Exact-target telemetry used by the Mission Planner speech policy.
 *
 * The legacy UAS graph is keyed by system id, so it cannot distinguish equal
 * MAVLink ids arriving on different physical links.  This source follows the
 * immutable VehicleTargetManager lease and accepts only packets belonging to
 * that exact link/system/component/generation epoch.
 */
class SpeechTelemetrySource final : public QObject
{
    Q_OBJECT

public:
    struct Snapshot
    {
        VehicleTargetLease lease;
        // Start of this source's selected-target epoch. If the source is
        // constructed after an existing connection, this conservatively
        // starts later and therefore delays (never advances) No Data speech.
        qint64 connectedSinceMs = -1;
        qint64 lastPacketMs = -1;
        bool heartbeatValid = false;
        bool armed = false;
        bool modeValid = false;
        quint32 customMode = 0;
        QString mode;
        bool altitudeValid = false;
        double altitudeMeters = 0.0;
        bool airspeedValid = false;
        double airspeedMps = 0.0;
        bool groundSpeedValid = false;
        double groundSpeedMps = 0.0;
        bool waypointValid = false;
        int waypointNumber = 0;
        bool batteryVoltageValid = false;
        double batteryVoltage = 0.0;
        bool batteryRemainingValid = false;
        double batteryRemainingPercent = 0.0;

        bool isValid() const noexcept { return lease.isValid(); }
    };

    using Clock = std::function<qint64()>;

    explicit SpeechTelemetrySource(VehicleTargetManager *targets,
                                   Clock clock = Clock(),
                                   QObject *parent = nullptr);

    Snapshot snapshot() const { return m_snapshot; }
    qint64 monotonicTimeMs() const { return nowMs(); }
    bool isCurrentLease(const VehicleTargetLease &lease) const;
    static QString modeText(int autopilot, int vehicleType,
                            quint32 customMode, quint8 baseMode);

public slots:
    // LinkManager invokes this only for frames accepted by MAVLinkProtocol.
    // MAVLink 2 may legitimately truncate trailing zero payload bytes, so
    // callers must not pass arbitrary, parser-unvalidated message structs.
    void observeMessage(int linkId, const mavlink_message_t &message);

signals:
    void snapshotChanged();
    void targetEpochChanged();
    void armedChanged(bool armed);
    void flightModeChanged(const QString &mode);
    void waypointChanged(int sequence);
    void batteryTelemetryChanged(double voltage,
                                 double remainingPercent);
    void exactSpeechTelemetry(ExactSpeechTelemetryEvent event);
    void statusTextCompleted(ExactStatusText event);

private slots:
    void invalidateTargetEpoch();
    void resetTargetEpoch();

private:
    struct StatusAssembly
    {
        quint8 rawSeverity = MAV_SEVERITY_INFO;
        quint8 nextChunk = 0;
        QByteArray utf8;
        qint64 updatedAtMs = -1;
    };

    bool accepts(int linkId, const mavlink_message_t &message) const;
    bool observeStatusText(const VehicleTargetLease &lease,
                           const mavlink_message_t &message,
                           qint64 observedMs, ExactStatusText *completed);
    void updateFirmwareCompatibility();
    void clearStatusTextState();
    qint64 nowMs() const;

    VehicleTargetManager *const m_targets;
    Clock m_clock;
    QElapsedTimer m_elapsedClock;
    Snapshot m_snapshot;
    QHash<quint16, StatusAssembly> m_statusAssemblies;
    APMFirmwareVersion m_firmwareVersion;
    QByteArray m_flightCustomVersion;
    quint32 m_flightSwVersion = 0;
    int m_autopilot = MAV_AUTOPILOT_GENERIC;
    int m_vehicleType = MAV_TYPE_GENERIC;
    bool m_flightVersionSeen = false;
    bool m_legacySeverityCompatibility = false;
};

#endif // SPEECHTELEMETRYSOURCE_H
