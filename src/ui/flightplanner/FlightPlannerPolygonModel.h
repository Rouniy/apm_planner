#ifndef FLIGHTPLANNERPOLYGONMODEL_H
#define FLIGHTPLANNERPOLYGONMODEL_H

#include "SurveyGridGenerator.h"
#include "WpRow.h"

#include <QObject>
#include <QString>
#include <QVector>

class FlightPlannerPolygonModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int Count READ Count NOTIFY DrawnPolygonChanged)
    Q_PROPERTY(double AreaSquareMeters READ PolygonArea
               NOTIFY DrawnPolygonChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(QString LastError READ LastError NOTIFY lastErrorChanged)

public:
    explicit FlightPlannerPolygonModel(QObject *parent = nullptr);

    const QVector<SurveyGridCoordinate> &DrawnPolygon() const;
    int Count() const;
    QString Status() const;
    QString LastError() const;

    bool IsValid(QString *error = nullptr) const;

    // Drawing is incremental, so one or two valid points are accepted.  Once
    // three points exist, every further point must keep the polygon simple.
    bool AddDrawnPolygonPoint(double latitude, double longitude,
                              double altitude = 0.0);
    bool AddDrawnPolygonPoint(const SurveyGridCoordinate &point);
    bool ReplaceDrawnPolygon(
            const QVector<SurveyGridCoordinate> &points);
    void ClearDrawnPolygon();

    // Uses only commands that contribute to the aircraft flight path and use
    // a global MAVLink coordinate frame.  The old polygon is retained if the
    // selected waypoint set does not form a valid polygon.
    bool BuildPolygonFromWaypoints(const QVector<WpRowData> &waypoints);

    double PolygonArea() const;
    bool OffsetDrawnPolygon(double meters);

    bool LoadPolygon(const QString &path, bool append = false);
    bool SavePolygon(const QString &path);

signals:
    void DrawnPolygonChanged();
    void changed();
    void statusChanged(const QString &status);
    void lastErrorChanged(const QString &error);
    void errorOccurred(const QString &error);

private:
    bool replaceValidated(QVector<SurveyGridCoordinate> points,
                          const QString &successStatus);
    bool fail(const QString &error);
    void succeed(const QString &status);
    void publishChange();

    QVector<SurveyGridCoordinate> m_drawnPolygon;
    QString m_status;
    QString m_lastError;

    Q_DISABLE_COPY(FlightPlannerPolygonModel)
};

#endif // FLIGHTPLANNERPOLYGONMODEL_H
