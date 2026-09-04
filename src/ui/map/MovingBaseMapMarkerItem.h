#ifndef MOVINGBASEMAPMARKERITEM_H
#define MOVINGBASEMAPMARKERITEM_H

#include <QFont>
#include <QGraphicsItem>
#include <QString>

/** MP10-compatible second-line text for the Flight Data BASE marker. */
QString FormatMovingBaseMapDetail(double altitudeAmslMetres,
                                  const QString &tag);

/** Marker visibility contract. The NMEA store may still retain (0,0). */
bool IsMovingBaseMapPositionRenderable(double latitude, double longitude,
                                       double altitudeAmslMetres) noexcept;

/** Fixed-screen-size, non-interactive Moving Base marker graphics. */
class MovingBaseMapMarkerItem final : public QGraphicsItem
{
public:
    explicit MovingBaseMapMarkerItem(QGraphicsItem *parent = nullptr);

    void setFix(double altitudeAmslMetres, const QString &tag);
    QString detailText() const { return m_detailText; }

    QRectF boundingRect() const override;
    void paint(QPainter *painter,
               const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

private:
    static QFont titleFont();
    static QFont detailFont();
    void updateGeometry();

    QString m_detailText;
    QRectF m_markerRect;
    QRectF m_titleRect;
    QRectF m_detailTextRect;
    QRectF m_detailBackground;
    QRectF m_bounds;
};

#endif // MOVINGBASEMAPMARKERITEM_H
