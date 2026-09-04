#ifndef FORMATIONGRIDCONTROL_H
#define FORMATIONGRIDCONTROL_H

#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QPainter;
class QWheelEvent;

/**
 * Presentation-only description of one marker in the formation editor.
 *
 * instanceKey is deliberately opaque to this control.  The owner can encode a
 * full SwarmVehicleInstanceLease without coupling the reusable canvas to the
 * telemetry or command services.
 */
struct FormationGridItem
{
    QString instanceKey;
    int systemId = 0;
    int componentId = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool included = false;
    bool eligible = false;
    bool leader = false;
};

inline bool operator==(const FormationGridItem &left,
                       const FormationGridItem &right)
{
    return left.instanceKey == right.instanceKey
        && left.systemId == right.systemId
        && left.componentId == right.componentId
        && left.x == right.x && left.y == right.y && left.z == right.z
        && left.included == right.included
        && left.eligible == right.eligible
        && left.leader == right.leader;
}

inline bool operator!=(const FormationGridItem &left,
                       const FormationGridItem &right)
{
    return !(left == right);
}

Q_DECLARE_METATYPE(FormationGridItem)

/** Mission Planner 10 FormationGridControl-compatible formation canvas. */
class FormationGridControl final : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double halfSpanMeters READ halfSpanMeters
               WRITE setHalfSpanMeters NOTIFY halfSpanMetersChanged)

public:
    explicit FormationGridControl(QWidget *parent = nullptr);

    QVector<FormationGridItem> items() const { return m_items; }
    void setItems(const QVector<FormationGridItem> &items);

    double halfSpanMeters() const { return m_halfSpanMeters; }
    void setHalfSpanMeters(double halfSpanMeters);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    /** Deterministic geometry seams shared by rendering and widget tests. */
    QRectF plotBounds() const;
    QPointF markerPosition(int itemIndex) const;

signals:
    void itemsChanged();
    void itemDragged(const QString &instanceKey, double x, double y);
    void halfSpanMetersChanged(double halfSpanMeters);

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    double pixelsPerMeter(const QRectF &plot) const;
    QPointF screenPoint(const FormationGridItem &item,
                        const QRectF &plot, double scale) const;
    QPointF clampedMarkerPoint(const QPointF &point,
                               const QRectF &plot) const;
    int draggableItemAt(const QPointF &position) const;
    void drawGrid(QPainter &painter, const QRectF &plot,
                  double scale) const;
    void drawMarkers(QPainter &painter, const QRectF &plot,
                     double scale) const;

    QVector<FormationGridItem> m_items;
    int m_draggedIndex = -1;
    double m_halfSpanMeters = 25.0;
};

#endif // FORMATIONGRIDCONTROL_H
