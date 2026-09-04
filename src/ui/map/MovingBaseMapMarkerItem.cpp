#include "MovingBaseMapMarkerItem.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

QString FormatMovingBaseMapDetail(double altitudeAmslMetres,
                                  const QString &tag)
{
    if (!qIsFinite(altitudeAmslMetres)) {
        return {};
    }
    const double displayedAltitude = qFuzzyIsNull(altitudeAmslMetres)
        ? 0.0 : altitudeAmslMetres;
    QString text = QStringLiteral("%1 m AMSL")
        .arg(displayedAltitude, 0, 'f', 1);
    const QString normalizedTag = tag.simplified();
    if (!normalizedTag.isEmpty()) {
        text += QStringLiteral("  ") + normalizedTag;
    }
    return text;
}

bool IsMovingBaseMapPositionRenderable(
    double latitude, double longitude,
    double altitudeAmslMetres) noexcept
{
    return qIsFinite(latitude) && qIsFinite(longitude)
        && qIsFinite(altitudeAmslMetres)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (latitude != 0.0 || longitude != 0.0);
}

MovingBaseMapMarkerItem::MovingBaseMapMarkerItem(QGraphicsItem *parent)
    : QGraphicsItem(parent)
{
    setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    setFlag(QGraphicsItem::ItemIsMovable, false);
    setFlag(QGraphicsItem::ItemIsSelectable, false);
    setZValue(5.0);
    setData(0, QStringLiteral("MovingBaseMarker"));
    setFix(0.0, QString());
}

void MovingBaseMapMarkerItem::setFix(
    double altitudeAmslMetres, const QString &tag)
{
    const QString text = FormatMovingBaseMapDetail(
        altitudeAmslMetres, tag);
    if (m_detailText == text) {
        return;
    }
    prepareGeometryChange();
    m_detailText = text;
    setData(1, text);
    updateGeometry();
    update();
}

QRectF MovingBaseMapMarkerItem::boundingRect() const
{
    return m_bounds;
}

void MovingBaseMapMarkerItem::paint(
    QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    painter->setRenderHint(QPainter::Antialiasing, true);

    QPen outline(Qt::black);
    outline.setWidthF(1.0);
    painter->setPen(outline);
    painter->setBrush(QColor(0, 210, 210));
    painter->drawEllipse(m_markerRect);

    painter->setFont(titleFont());
    painter->setPen(Qt::black);
    painter->setBrush(Qt::NoBrush);
    painter->drawText(m_titleRect, Qt::AlignCenter,
                      QStringLiteral("BASE"));

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 160));
    painter->drawRoundedRect(m_detailBackground, 2.0, 2.0);
    painter->setFont(detailFont());
    painter->setPen(Qt::white);
    painter->drawText(m_detailTextRect, Qt::AlignCenter,
                      m_detailText);
}

QFont MovingBaseMapMarkerItem::titleFont()
{
    QFont font;
    font.setPixelSize(10);
    font.setBold(true);
    return font;
}

QFont MovingBaseMapMarkerItem::detailFont()
{
    QFont font;
    font.setPixelSize(9);
    return font;
}

void MovingBaseMapMarkerItem::updateGeometry()
{
    const QFontMetricsF titleMetrics(titleFont());
    const QFontMetricsF detailMetrics(detailFont());
    const QSizeF titleSize = titleMetrics.size(
        Qt::TextSingleLine, QStringLiteral("BASE"));
    const QSizeF detailSize = detailMetrics.size(
        Qt::TextSingleLine, m_detailText);

    m_markerRect = QRectF(-9.0, -9.0, 18.0, 18.0);
    m_titleRect = QRectF(-titleSize.width() / 2.0 - 2.0, -27.0,
                        titleSize.width() + 4.0,
                        titleSize.height() + 2.0);
    m_detailTextRect = QRectF(
        -detailSize.width() / 2.0 - 2.0, 13.0,
        detailSize.width() + 4.0, detailSize.height() + 2.0);
    m_detailBackground = m_detailTextRect.adjusted(-2.0, -1.0,
                                                   2.0, 1.0);
    m_bounds = m_markerRect.united(m_titleRect)
        .united(m_detailBackground).adjusted(-1.0, -1.0, 1.0, 1.0);
}
