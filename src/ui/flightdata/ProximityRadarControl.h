#ifndef PROXIMITYRADARCONTROL_H
#define PROXIMITYRADARCONTROL_H

#include <QPointer>
#include <QMetaObject>
#include <QWidget>

class Proximity;
class QColor;
class QHideEvent;
class QKeyEvent;
class QPaintEvent;
class QPainter;
class QPen;
class QShowEvent;
class QTimer;

class ProximityRadarControl final : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double RadiusCm READ radiusCm WRITE setRadiusCm
               NOTIFY scaleChanged)
    Q_PROPERTY(double VehicleSizeCm READ vehicleSizeCm WRITE setVehicleSizeCm
               NOTIFY scaleChanged)

public:
    explicit ProximityRadarControl(QWidget *parent = nullptr);
    ProximityRadarControl(Proximity *proximity, QWidget *parent);

    Proximity *ProximityState() const;
    void setProximity(Proximity *proximity);

    double radiusCm() const { return m_radiusCm; }
    double vehicleSizeCm() const { return m_vehicleSizeCm; }
    QSize sizeHint() const override;

public slots:
    void setRadiusCm(double radiusCm);
    void setVehicleSizeCm(double vehicleSizeCm);

signals:
    void scaleChanged();

protected:
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    static QPointF Polar(const QPointF &center, double radius,
                         double angleDegrees);
    static void DrawArc(QPainter &painter, const QPointF &center,
                        double radius, double startAngle, double endAngle,
                        const QPen &pen);
    static void DrawCenteredText(QPainter &painter, const QString &text,
                                 const QPointF &center, const QColor &color,
                                 double pointSize);
    static void DrawText(QPainter &painter, const QString &text,
                         const QPointF &point, const QColor &color,
                         double pointSize);

    Proximity *m_proximity = nullptr;
    QTimer *m_timer = nullptr;
    double m_radiusCm = 500.0;
    double m_vehicleSizeCm = 80.0;
};

#endif
