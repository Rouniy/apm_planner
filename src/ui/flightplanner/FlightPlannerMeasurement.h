#ifndef FLIGHTPLANNERMEASUREMENT_H
#define FLIGHTPLANNERMEASUREMENT_H

#include <QString>

class FlightPlannerMeasurement final
{
public:
    struct Point
    {
        double latitude = 0.0;
        double longitude = 0.0;
    };

    struct Result
    {
        Point start;
        Point end;
        double distanceMeters = 0.0;
        double bearingDegrees = 0.0;
        bool valid = false;
    };

    enum class Step
    {
        Rejected,
        Started,
        Completed
    };

    Step AddPoint(double latitude, double longitude,
                  Result *completedResult = nullptr);
    void Reset();

    bool IsActive() const;
    Point Start() const;

    static bool IsValidPoint(const Point &point);
    static Result Calculate(const Point &start, const Point &end);
    static QString FormatDistance(double distanceMeters,
                                  double displayMultiplier,
                                  const QString &unit);

private:
    bool m_active = false;
    Point m_start;
};

#endif // FLIGHTPLANNERMEASUREMENT_H
