#include "PlannerMeasurementOverlay.h"

#include <QBrush>
#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QPen>
#include <QtGlobal>

namespace
{
bool validPoint(const QPointF &point)
{
    return qIsFinite(point.x()) && qIsFinite(point.y());
}

QPainterPath markerPath(const QPointF &point)
{
    // Compact red map pin, matching the two red GMarkerGoogle markers used by
    // Mission Planner's ContextMeasure interaction.
    QPainterPath path;
    path.addEllipse(point + QPointF(0.0, -5.0), 5.0, 5.0);
    path.moveTo(point + QPointF(-3.5, -1.5));
    path.lineTo(point + QPointF(0.0, 5.0));
    path.lineTo(point + QPointF(3.5, -1.5));
    path.closeSubpath();
    return path;
}
}

namespace MissionPlanner
{

void PlannerMeasurementOverlay::Rebuild(
        QGraphicsItemGroup *group,
        const QVector<QPointF> &projectedPoints)
{
    if (!group) return;

    const QList<QGraphicsItem *> oldItems = group->childItems();
    for (QGraphicsItem *item : oldItems) delete item;

    QVector<QPointF> points;
    points.reserve(qMin(projectedPoints.size(), 2));
    for (const QPointF &point : projectedPoints) {
        if (validPoint(point)) points.append(point);
        if (points.size() == 2) break;
    }

    if (points.size() == 2) {
        QPainterPath linePath(points.first());
        linePath.lineTo(points.last());
        auto *line = new QGraphicsPathItem(linePath, group);
        QPen linePen(QColor(0, 160, 0));
        linePen.setWidth(3);
        linePen.setCosmetic(true);
        line->setPen(linePen);
        line->setZValue(0.0);
    }

    for (const QPointF &point : points) {
        auto *marker = new QGraphicsPathItem(markerPath(point), group);
        QPen markerPen(Qt::white);
        markerPen.setWidth(1);
        markerPen.setCosmetic(true);
        marker->setPen(markerPen);
        marker->setBrush(QColor(220, 32, 32));
        marker->setZValue(1.0);
    }
}

} // namespace MissionPlanner
