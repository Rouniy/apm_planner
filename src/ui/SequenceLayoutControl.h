#ifndef SEQUENCELAYOUTCONTROL_H
#define SEQUENCELAYOUTCONTROL_H

#include <QImage>
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

struct SequenceLayoutOffset
{
    int systemId = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline bool operator==(const SequenceLayoutOffset &left,
                       const SequenceLayoutOffset &right)
{
    return left.systemId == right.systemId
        && left.x == right.x
        && left.y == right.y
        && left.z == right.z;
}

inline bool operator!=(const SequenceLayoutOffset &left,
                       const SequenceLayoutOffset &right)
{
    return !(left == right);
}

Q_DECLARE_METATYPE(SequenceLayoutOffset)

class SequenceLayoutControl final : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double halfSpanMeters READ halfSpanMeters
               WRITE setHalfSpanMeters NOTIFY halfSpanMetersChanged)
    Q_PROPERTY(double backgroundStep READ backgroundStep
               WRITE setBackgroundStep NOTIFY backgroundTransformChanged)

public:
    struct BackgroundTransform
    {
        double x = 0.0;
        double y = 0.0;
        double width = 1.0;
        double height = 1.0;
        double step = 1.0;
    };

    explicit SequenceLayoutControl(QWidget *parent = nullptr);

    QVector<SequenceLayoutOffset> offsets() const { return m_offsets; }
    void setOffsets(const QVector<SequenceLayoutOffset> &offsets);

    double halfSpanMeters() const { return m_halfSpanMeters; }
    void setHalfSpanMeters(double halfSpanMeters);

    bool loadBackground(const QString &path, QString *errorMessage = nullptr);
    void clearBackground();
    bool hasBackground() const { return !m_background.isNull(); }

    BackgroundTransform backgroundTransform() const;
    double backgroundStep() const { return m_backgroundStep; }
    void moveBackground(double deltaX, double deltaY);
    void resizeBackground(double deltaWidth, double deltaHeight);
    void setBackgroundStep(double step);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

signals:
    void offsetsChanged();
    void offsetDragged(int systemId, double x, double y);
    void halfSpanMetersChanged(double halfSpanMeters);
    void backgroundChanged(bool loaded);
    void backgroundTransformChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QRectF plotBounds() const;
    double pixelsPerMeter(const QRectF &plot) const;
    QPointF screenPoint(const SequenceLayoutOffset &offset,
                        const QRectF &plot, double scale) const;
    QPointF clampedMarkerPoint(const QPointF &point,
                               const QRectF &plot) const;
    void drawGrid(QPainter &painter, const QRectF &plot, double scale) const;
    void drawMarkers(QPainter &painter, const QRectF &plot,
                     double scale) const;

    QVector<SequenceLayoutOffset> m_offsets;
    int m_draggedIndex = -1;
    QImage m_background;
    double m_halfSpanMeters = 20.0;
    double m_backgroundX = 0.0;
    double m_backgroundY = 0.0;
    double m_backgroundWidth = 1.0;
    double m_backgroundHeight = 1.0;
    double m_backgroundStep = 1.0;
};

#endif
