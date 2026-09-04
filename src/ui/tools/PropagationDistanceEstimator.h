#ifndef PROPAGATIONDISTANCEESTIMATOR_H
#define PROPAGATIONDISTANCEESTIMATOR_H

#include <QtGlobal>

/**
 * Stateful, transport-independent implementation of Mission Planner's
 * battery_kmleft inputs.
 *
 * Position observations are reduced to one sample per monotonic second, just
 * like CurrentState's one-second distance accumulator. Battery consumption is
 * either taken from BATTERY_STATUS.current_consumed or integrated from current
 * observations. A caller must reset the estimator whenever its exact vehicle
 * target or connection epoch changes.
 */
class PropagationDistanceEstimator
{
public:
    void reset();

    void setConnected(bool connected);
    void setArmed(bool armed) { m_armed = armed; }
    void setGpsFixType(int gpsFixType) { m_gpsFixType = gpsFixType; }

    /**
     * Observes primary-battery state at monotonicMs.
     *
     * Pass NaN for unavailable fields. A non-negative consumedMah is an
     * authoritative BATTERY_STATUS value. Otherwise a finite currentAmps is
     * integrated; -0.01 A is MAVLink's unknown-current sentinel and ignored.
     */
    void observeBattery(qint64 monotonicMs,
                        double batteryRemainingPercent,
                        double currentAmps,
                        double consumedMah);

    /** Observes the latest global position and returns kilometresLeft(). */
    double observePosition(qint64 monotonicMs,
                           double latitude, double longitude);

    bool connected() const { return m_connected; }
    bool usedMahValid() const { return m_usedMahValid; }
    double usedMah() const { return m_usedMah; }
    bool batteryRemainingValid() const;
    double batteryRemainingPercent() const { return m_remainingPercent; }
    double travelledMetres() const { return m_travelledMetres; }
    double kilometresLeft() const;

private:
    static bool validCoordinate(double latitude, double longitude);
    static bool validCurrent(double currentAmps);
    static double distanceMetres(double firstLatitude,
                                 double firstLongitude,
                                 double secondLatitude,
                                 double secondLongitude);

    bool m_connected = false;
    bool m_armed = false;
    int m_gpsFixType = 0;

    bool m_havePosition = false;
    double m_lastLatitude = 0.0;
    double m_lastLongitude = 0.0;
    bool m_haveDistanceTimestamp = false;
    qint64 m_lastDistanceTimestampMs = 0;
    double m_travelledMetres = 0.0;

    double m_remainingPercent = qQNaN();
    bool m_usedMahValid = false;
    double m_usedMah = 0.0;
    bool m_haveCurrentTimestamp = false;
    qint64 m_lastCurrentTimestampMs = 0;
};

#endif // PROPAGATIONDISTANCEESTIMATOR_H
