#ifndef PROXIMITY_H
#define PROXIMITY_H

#include "QGCMAVLink.h"

#include <QVector>

#include <atomic>
#include <chrono>
#include <mutex>

class Proximity final
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Sample
    {
        quint32 SensorId = 0;
        MAV_SENSOR_ORIENTATION Orientation = MAV_SENSOR_ROTATION_NONE;
        double Angle = 0.0;
        double Size = 45.0;
        double Distance = 0.0;
        TimePoint Received;
        std::chrono::milliseconds Age{1000};

        bool IsCustom() const
        {
            return Orientation == MAV_SENSOR_ROTATION_CUSTOM;
        }

        TimePoint ExpireTime() const { return Received + Age; }
    };

    class DirectionState final
    {
    public:
        void Add(quint32 sensorId, MAV_SENSOR_ORIENTATION orientation,
                 double distanceCm, TimePoint received,
                 std::chrono::milliseconds age =
                     std::chrono::milliseconds(1000));
        void Add(quint32 sensorId, double angleDegrees,
                 double sizeDegrees, double distanceCm,
                 TimePoint received,
                 std::chrono::milliseconds age =
                     std::chrono::milliseconds(1000));

        QVector<Sample> GetRaw(TimePoint now = Clock::now()) const;
        double GetClosest(TimePoint now = Clock::now()) const;
        QVector<MAV_SENSOR_ORIENTATION> GetWarnings(
            double minDistanceCm = 2.0,
            TimePoint now = Clock::now()) const;
        void Clear();

    private:
        void ExpireLocked(TimePoint now) const;

        mutable std::mutex m_mutex;
        mutable QVector<Sample> m_samples;
    };

    explicit Proximity(quint8 systemId = 0);

    DirectionState &directionState() { return m_directionState; }
    const DirectionState &directionState() const { return m_directionState; }
    bool DataAvailable() const { return m_dataAvailable.load(); }
    quint8 systemId() const { return m_systemId; }

    bool observeMessage(const mavlink_message_t &message,
                        double vehicleYawRadians,
                        TimePoint received = Clock::now());
    void Clear();

    static double OrientationAngle(MAV_SENSOR_ORIENTATION orientation);

private:
    quint8 m_systemId = 0;
    DirectionState m_directionState;
    std::atomic_bool m_dataAvailable{false};
};

#endif
