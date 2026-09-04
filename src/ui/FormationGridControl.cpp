#include "FormationGridControl.h"

#include <QFont>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
const QColor kBackground(QStringLiteral("#151817"));
const QColor kMinorGrid(QStringLiteral("#303833"));
const QColor kMajorGrid(QStringLiteral("#59645D"));
const QColor kLeader(QStringLiteral("#FFD700"));
const QColor kIncluded(QStringLiteral("#00BFFF"));
const QColor kExcluded(QStringLiteral("#696969"));
const QColor kOutside(QStringLiteral("#FF4500"));
const QColor kSecondaryText(QStringLiteral("#D3D3D3"));

constexpr double kMinimumHalfSpanMeters = 5.0;
constexpr double kMaximumHalfSpanMeters = 1000.0;
constexpr double kMarkerInsetPixels = 8.0;
constexpr double kFollowerRadiusPixels = 7.0;
constexpr double kLeaderRadiusPixels = 9.0;
constexpr double kHitRadiusPixels = 20.0;

QPointF mousePosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

double distanceSquared(const QPointF &left, const QPointF &right)
{
    const double dx = left.x() - right.x();
    const double dy = left.y() - right.y();
    return dx * dx + dy * dy;
}

bool hasFiniteOffset(const FormationGridItem &item)
{
    return std::isfinite(item.x) && std::isfinite(item.y)
        && std::isfinite(item.z);
}

double gridStep(double halfSpanMeters)
{
    const double raw = halfSpanMeters * 2.0 / 10.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalized = raw / magnitude;
    const double multiplier = normalized <= 1.0 ? 1.0
                            : normalized <= 2.0 ? 2.0
                            : normalized <= 5.0 ? 5.0 : 10.0;
    return multiplier * magnitude;
}

QString compactNumber(double value)
{
    if (qFuzzyIsNull(value)) {
        value = 0.0;
    }
    QString result = QString::number(value, 'f', 1);
    if (result.endsWith(QStringLiteral(".0"))) {
        result.chop(2);
    }
    return result;
}

void drawText(QPainter &painter, const QString &text, const QPointF &point,
              const QColor &color, int pixelSize)
{
    painter.save();
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(pixelSize);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(point + QPointF(0.0, QFontMetricsF(font).ascent()), text);
    painter.restore();
}
} // namespace

FormationGridControl::FormationGridControl(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("FormationGrid"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void FormationGridControl::setItems(const QVector<FormationGridItem> &items)
{
    if (m_items == items) {
        return;
    }
    m_draggedIndex = -1;
    m_items = items;
    emit itemsChanged();
    update();
}

void FormationGridControl::setHalfSpanMeters(double halfSpanMeters)
{
    if (!std::isfinite(halfSpanMeters)) {
        return;
    }
    const double bounded = qBound(kMinimumHalfSpanMeters, halfSpanMeters,
                                  kMaximumHalfSpanMeters);
    if (qFuzzyCompare(m_halfSpanMeters, bounded)) {
        return;
    }
    m_halfSpanMeters = bounded;
    emit halfSpanMetersChanged(m_halfSpanMeters);
    update();
}

QSize FormationGridControl::minimumSizeHint() const
{
    return QSize(260, 220);
}

QSize FormationGridControl::sizeHint() const
{
    return QSize(470, 430);
}

QRectF FormationGridControl::plotBounds() const
{
    return QRectF(24.0, 32.0,
                  std::max(1.0, width() - 48.0),
                  std::max(1.0, height() - 56.0));
}

QPointF FormationGridControl::markerPosition(int itemIndex) const
{
    if (itemIndex < 0 || itemIndex >= m_items.size()) {
        const double invalid = std::numeric_limits<double>::quiet_NaN();
        return QPointF(invalid, invalid);
    }
    const QRectF plot = plotBounds();
    return clampedMarkerPoint(
        screenPoint(m_items.at(itemIndex), plot, pixelsPerMeter(plot)), plot);
}

double FormationGridControl::pixelsPerMeter(const QRectF &plot) const
{
    return std::max(0.001, std::min(plot.width(), plot.height())
                              / (m_halfSpanMeters * 2.0));
}

QPointF FormationGridControl::screenPoint(const FormationGridItem &item,
                                          const QRectF &plot,
                                          double scale) const
{
    return QPointF(plot.center().x() + item.x * scale,
                   plot.center().y() - item.y * scale);
}

QPointF FormationGridControl::clampedMarkerPoint(const QPointF &point,
                                                 const QRectF &plot) const
{
    const double horizontalInset = std::min(kMarkerInsetPixels,
                                            plot.width() / 2.0);
    const double verticalInset = std::min(kMarkerInsetPixels,
                                          plot.height() / 2.0);
    return QPointF(qBound(plot.left() + horizontalInset, point.x(),
                          plot.right() - horizontalInset),
                   qBound(plot.top() + verticalInset, point.y(),
                          plot.bottom() - verticalInset));
}

int FormationGridControl::draggableItemAt(const QPointF &position) const
{
    const QRectF plot = plotBounds();
    const double scale = pixelsPerMeter(plot);
    const double maximumDistance = kHitRadiusPixels * kHitRadiusPixels;
    double closestDistance = maximumDistance;
    int closestIndex = -1;
    for (int index = 0; index < m_items.size(); ++index) {
        const FormationGridItem &item = m_items.at(index);
        if (!item.included || !item.eligible || item.leader
            || !hasFiniteOffset(item)) {
            continue;
        }
        const double distance = distanceSquared(
            position, clampedMarkerPoint(screenPoint(item, plot, scale), plot));
        // Keep the first item for equal distances, matching LINQ OrderBy's
        // stable ordering in Mission Planner 10.
        if (distance <= maximumDistance
            && (closestIndex < 0 || distance < closestDistance)) {
            closestIndex = index;
            closestDistance = distance;
        }
    }
    return closestIndex;
}

void FormationGridControl::drawGrid(QPainter &painter, const QRectF &plot,
                                    double scale) const
{
    const double step = gridStep(m_halfSpanMeters);
    const QPen minorPen(kMinorGrid, 1.0);
    const QPen majorPen(kMajorGrid, 1.0);
    for (double value = -m_halfSpanMeters;
         value <= m_halfSpanMeters + step / 2.0; value += step) {
        const double majorRemainder = std::remainder(value, step * 5.0);
        painter.setPen(qAbs(majorRemainder) < 0.001 ? majorPen : minorPen);
        const double x = plot.center().x() + value * scale;
        const double y = plot.center().y() - value * scale;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    painter.setPen(QPen(Qt::lightGray, 1.5));
    painter.drawLine(QPointF(plot.left(), plot.center().y()),
                     QPointF(plot.right(), plot.center().y()));
    painter.drawLine(QPointF(plot.center().x(), plot.top()),
                     QPointF(plot.center().x(), plot.bottom()));
    drawText(painter, QStringLiteral("+Y"),
             QPointF(plot.center().x() + 5.0, plot.top() + 3.0),
             kSecondaryText, 11);
    drawText(painter, QStringLiteral("+X"),
             QPointF(plot.right() - 24.0, plot.center().y() + 5.0),
             kSecondaryText, 11);
}

void FormationGridControl::drawMarkers(QPainter &painter,
                                       const QRectF &plot,
                                       double scale) const
{
    for (const FormationGridItem &item : m_items) {
        if (!item.eligible || !hasFiniteOffset(item)) {
            continue;
        }
        const QPointF rawPoint = screenPoint(item, plot, scale);
        const bool outside = !plot.contains(rawPoint);
        const QPointF point = clampedMarkerPoint(rawPoint, plot);
        const QColor fill = item.leader ? kLeader
                          : item.included ? kIncluded : kExcluded;
        const double radius = item.leader ? kLeaderRadiusPixels
                                          : kFollowerRadiusPixels;

        painter.setBrush(fill);
        painter.setPen(QPen(outside ? kOutside : QColor(Qt::white), 1.5));
        painter.drawEllipse(point, radius, radius);
        if (item.leader) {
            painter.setPen(QPen(kLeader, 2.0));
            painter.drawLine(point, point + QPointF(0.0, -18.0));
        }
        drawText(painter,
                 QStringLiteral("%1:%2").arg(item.systemId).arg(item.componentId),
                 point + QPointF(10.0, -9.0), fill, 11);
    }
}

void FormationGridControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(rect());
    painter.fillRect(rect(), kBackground);
    if (width() < 180 || height() < 180) {
        return;
    }

    const QRectF plot = plotBounds();
    const double scale = pixelsPerMeter(plot);
    drawGrid(painter, plot, scale);
    drawMarkers(painter, plot, scale);
    drawText(painter,
             QStringLiteral("±%1 m · wheel zoom · drag enabled followers")
                 .arg(compactNumber(m_halfSpanMeters)),
             QPointF(8.0, 7.0), kSecondaryText, 11);
}

void FormationGridControl::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_draggedIndex = draggableItemAt(mousePosition(event));
    if (m_draggedIndex < 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    event->accept();
}

void FormationGridControl::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggedIndex < 0 || m_draggedIndex >= m_items.size()
        || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QRectF plot = plotBounds();
    const double scale = pixelsPerMeter(plot);
    const QPointF point = mousePosition(event);
    FormationGridItem &item = m_items[m_draggedIndex];
    double nextX = qBound(-m_halfSpanMeters,
        (point.x() - plot.center().x()) / scale, m_halfSpanMeters);
    double nextY = qBound(-m_halfSpanMeters,
        (plot.center().y() - point.y()) / scale, m_halfSpanMeters);
    nextX = std::nearbyint(nextX * 10.0) / 10.0;
    nextY = std::nearbyint(nextY * 10.0) / 10.0;
    if (qFuzzyIsNull(nextX)) {
        nextX = 0.0;
    }
    if (qFuzzyIsNull(nextY)) {
        nextY = 0.0;
    }
    if (item.x == nextX && item.y == nextY) {
        event->accept();
        return;
    }

    item.x = nextX;
    item.y = nextY;
    emit itemDragged(item.instanceKey, item.x, item.y);
    emit itemsChanged();
    update();
    event->accept();
}

void FormationGridControl::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_draggedIndex >= 0) {
        m_draggedIndex = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FormationGridControl::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y() != 0
        ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    setHalfSpanMeters(delta > 0 ? m_halfSpanMeters / 1.25
                                : m_halfSpanMeters * 1.25);
    event->accept();
}
