#ifndef PLANNERMEASUREMENTOVERLAY_H
#define PLANNERMEASUREMENTOVERLAY_H

#include <QPointF>
#include <QVector>

class QGraphicsItemGroup;

namespace MissionPlanner
{

class PlannerMeasurementOverlay final
{
public:
    static void Rebuild(QGraphicsItemGroup *group,
                        const QVector<QPointF> &projectedPoints);
};

} // namespace MissionPlanner

#endif // PLANNERMEASUREMENTOVERLAY_H
