#ifndef WARNINGTELEMETRYSOURCE_H
#define WARNINGTELEMETRYSOURCE_H

#include "comm/VehicleEndpoint.h"
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <mavlink.h>

class VehicleTargetManager;

/** Read-only MP CurrentState numeric fields for one exact transport lease.
 * Values use the documented canonical units, never global display preferences.
 * Unknown, invalid and older-than-5-second samples are absent, not zero.
 * Ingress must be parser-validated and physical-lifecycle fenced by its owner.
 */
class WarningTelemetrySource final : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 FreshnessMs = 5000;
    explicit WarningTelemetrySource(VehicleTargetManager *targets,
                                    Clock clock = Clock(), QObject *parent = nullptr);
    static QStringList fieldNames();
    static QString fieldUnits(const QString &field);
    QHash<QString, double> values() const;
    quint64 epoch() const { return m_epoch; }
    VehicleTargetLease lease() const { return m_lease; }
    bool isCurrentLease(const VehicleTargetLease &lease) const;

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);
    // Owner invokes this at a physical/replay ingress epoch boundary, before
    // any fresh frame is offered. Selected-target generation need not change.
    void invalidateSourceEpoch();

signals:
    void epochChanged();

private:
    struct Sample { double value = 0; qint64 at = -1; };
    qint64 nowMs() const;
    bool current() const;
    void clearEpoch(bool acquire);
    void put(const QString &name, double value, qint64 at, bool valid = true);
    QPointer<VehicleTargetManager> m_targets;
    Clock m_clock;
    QElapsedTimer m_elapsed;
    VehicleTargetLease m_lease;
    QHash<QString, Sample> m_samples;
    quint64 m_epoch = 0;
    bool m_homeValid = false;
    double m_homeLatitude = 0, m_homeLongitude = 0, m_homeAltitude = 0;
};

#endif
