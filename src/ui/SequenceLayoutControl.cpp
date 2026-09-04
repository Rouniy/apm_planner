#include "SequenceLayoutControl.h"

#include <QFont>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
const QColor kBackground(QStringLiteral("#191B1D"));
const QColor kGrid(QStringLiteral("#384047"));
const QColor kAxis(QStringLiteral("#2E8B57"));
const QColor kMarker(QStringLiteral("#00BFFF"));
const QColor kOutsideMarker(QStringLiteral("#FF4500"));
const QColor kSecondaryText(QStringLiteral("#D3D3D3"));

constexpr double kMinimumHalfSpanMeters = 4.0;
constexpr double kMaximumHalfSpanMeters = 50000.0;
constexpr double kWheelStepMeters = 2.0;
constexpr double kMarkerRadiusPixels = 8.0;
constexpr double kHitRadiusPixels = 18.0;
constexpr int kMaximumBackgroundDimension = 4096;
constexpr qint64 kMaximumBackgroundPixels = 16LL * 1024LL * 1024LL;
constexpr qint64 kMaximumBackgroundFileBytes = 64LL * 1024LL * 1024LL;

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

SequenceLayoutControl::SequenceLayoutControl(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SequenceLayoutControl"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SequenceLayoutControl::setOffsets(
    const QVector<SequenceLayoutOffset> &offsets)
{
    if (m_offsets == offsets) {
        return;
    }
    m_draggedIndex = -1;
    m_offsets = offsets;
    emit offsetsChanged();
    update();
}

void SequenceLayoutControl::setHalfSpanMeters(double halfSpanMeters)
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

bool SequenceLayoutControl::loadBackground(const QString &path,
                                           QString *errorMessage)
{
    const QFileInfo information(path);
    if (!information.exists() || !information.isFile()
        || information.size() < 0
        || information.size() > kMaximumBackgroundFileBytes) {
        if (errorMessage) {
            *errorMessage = tr("Background image is missing or exceeds the 64 MiB safety limit.");
        }
        return false;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize decodedSize = reader.size();
    if (!decodedSize.isValid()
        || decodedSize.width() > kMaximumBackgroundDimension
        || decodedSize.height() > kMaximumBackgroundDimension
        || qint64(decodedSize.width()) * decodedSize.height()
            > kMaximumBackgroundPixels) {
        if (errorMessage) {
            *errorMessage = tr("Background image exceeds the 4096 px / 16 megapixel safety limit.");
        }
        return false;
    }
    const QImage next = reader.read();
    if (next.isNull() || next.width() > kMaximumBackgroundDimension
        || next.height() > kMaximumBackgroundDimension
        || qint64(next.width()) * next.height() > kMaximumBackgroundPixels) {
        if (errorMessage) {
            *errorMessage = next.isNull()
                ? reader.errorString()
                : tr("Decoded background image exceeds the safety limit.");
        }
        return false;
    }

    m_background = next;
    if (errorMessage) {
        errorMessage->clear();
    }
    emit backgroundChanged(true);
    update();
    return true;
}

void SequenceLayoutControl::clearBackground()
{
    if (m_background.isNull()) {
        return;
    }
    m_background = QImage();
    emit backgroundChanged(false);
    update();
}

SequenceLayoutControl::BackgroundTransform
SequenceLayoutControl::backgroundTransform() const
{
    return BackgroundTransform{m_backgroundX, m_backgroundY,
                               m_backgroundWidth, m_backgroundHeight,
                               m_backgroundStep};
}

void SequenceLayoutControl::moveBackground(double deltaX, double deltaY)
{
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY)) {
        return;
    }
    if (qFuzzyIsNull(deltaX) && qFuzzyIsNull(deltaY)) {
        return;
    }
    m_backgroundX += deltaX * m_backgroundStep;
    m_backgroundY += deltaY * m_backgroundStep;
    emit backgroundTransformChanged();
    update();
}

void SequenceLayoutControl::resizeBackground(double deltaWidth,
                                             double deltaHeight)
{
    if (!std::isfinite(deltaWidth) || !std::isfinite(deltaHeight)) {
        return;
    }
    const double nextWidth = std::max(
        0.1, m_backgroundWidth + deltaWidth * m_backgroundStep);
    const double nextHeight = std::max(
        0.1, m_backgroundHeight + deltaHeight * m_backgroundStep);
    if (qFuzzyCompare(m_backgroundWidth, nextWidth)
        && qFuzzyCompare(m_backgroundHeight, nextHeight)) {
        return;
    }
    m_backgroundWidth = nextWidth;
    m_backgroundHeight = nextHeight;
    emit backgroundTransformChanged();
    update();
}

void SequenceLayoutControl::setBackgroundStep(double step)
{
    const double next = qFuzzyCompare(step, 0.1) || step == 0.1 ? 0.1 : 1.0;
    if (qFuzzyCompare(m_backgroundStep, next)) {
        return;
    }
    m_backgroundStep = next;
    emit backgroundTransformChanged();
}

QSize SequenceLayoutControl::minimumSizeHint() const
{
    return QSize(240, 200);
}

QSize SequenceLayoutControl::sizeHint() const
{
    return QSize(620, 420);
}

QRectF SequenceLayoutControl::plotBounds() const
{
    return QRectF(24.0, 28.0,
                  std::max(1.0, width() - 48.0),
                  std::max(1.0, height() - 52.0));
}

double SequenceLayoutControl::pixelsPerMeter(const QRectF &plot) const
{
    return std::max(0.001, std::min(plot.width(), plot.height())
                              / (m_halfSpanMeters * 2.0));
}

QPointF SequenceLayoutControl::screenPoint(
    const SequenceLayoutOffset &offset, const QRectF &plot, double scale) const
{
    return QPointF(plot.center().x() + offset.x * scale,
                   plot.center().y() - offset.y * scale);
}

QPointF SequenceLayoutControl::clampedMarkerPoint(
    const QPointF &point, const QRectF &plot) const
{
    const double horizontalInset = std::min(kMarkerRadiusPixels,
                                            plot.width() / 2.0);
    const double verticalInset = std::min(kMarkerRadiusPixels,
                                          plot.height() / 2.0);
    return QPointF(qBound(plot.left() + horizontalInset, point.x(),
                          plot.right() - horizontalInset),
                   qBound(plot.top() + verticalInset, point.y(),
                          plot.bottom() - verticalInset));
}

void SequenceLayoutControl::drawGrid(QPainter &painter, const QRectF &plot,
                                     double scale) const
{
    const double step = gridStep(m_halfSpanMeters);
    const double first = -std::floor(m_halfSpanMeters / step) * step;
    for (double value = first;
         value <= m_halfSpanMeters + step * 0.01; value += step) {
        const QPen pen(qAbs(value) < 0.0001 ? kAxis : kGrid,
                       qAbs(value) < 0.0001 ? 1.5 : 1.0);
        painter.setPen(pen);
        const double x = plot.center().x() + value * scale;
        const double y = plot.center().y() - value * scale;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    drawText(painter, QStringLiteral("+E"),
             QPointF(plot.right() - 24.0, plot.center().y() + 5.0),
             kSecondaryText, 11);
    drawText(painter, QStringLiteral("+N"),
             QPointF(plot.center().x() + 5.0, plot.top() + 3.0),
             kSecondaryText, 11);
}

void SequenceLayoutControl::drawMarkers(QPainter &painter,
                                        const QRectF &plot,
                                        double scale) const
{
    for (const SequenceLayoutOffset &offset : m_offsets) {
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y)
            || !std::isfinite(offset.z)) {
            continue;
        }
        const QPointF rawPoint = screenPoint(offset, plot, scale);
        const bool outside = !plot.contains(rawPoint);
        const QPointF point = clampedMarkerPoint(rawPoint, plot);
        const QColor fill = outside ? kOutsideMarker : kMarker;

        painter.setBrush(fill);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawEllipse(point, kMarkerRadiusPixels, kMarkerRadiusPixels);
        drawText(painter, QString::number(offset.systemId),
                 point + QPointF(11.0, -9.0), fill, 11);
        drawText(painter,
                 QStringLiteral("z %1").arg(compactNumber(offset.z)),
                 point + QPointF(11.0, 4.0), kSecondaryText, 10);
    }
}

void SequenceLayoutControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(rect());
    painter.fillRect(rect(), kBackground);

    const QRectF plot = plotBounds();
    if (plot.width() <= 1.0 || plot.height() <= 1.0) {
        return;
    }
    const double scale = pixelsPerMeter(plot);

    if (!m_background.isNull()) {
        const QRectF target(plot.center().x() + m_backgroundX * scale,
                            plot.center().y() + m_backgroundY * scale,
                            m_backgroundWidth * scale,
                            m_backgroundHeight * scale);
        painter.drawImage(target, m_background, m_background.rect());
    }
    drawGrid(painter, plot, scale);
    drawMarkers(painter, plot, scale);

    drawText(painter,
             QStringLiteral("±%1 m · wheel zoom · drag vehicles")
                 .arg(compactNumber(m_halfSpanMeters)),
             QPointF(8.0, 7.0), kSecondaryText, 11);
}

void SequenceLayoutControl::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y() != 0
        ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    setHalfSpanMeters(m_halfSpanMeters
                      + (delta < 0 ? kWheelStepMeters : -kWheelStepMeters));
    event->accept();
}

void SequenceLayoutControl::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QRectF plot = plotBounds();
    const double scale = pixelsPerMeter(plot);
    const QPointF pointer = mousePosition(event);
    double closestDistance = kHitRadiusPixels * kHitRadiusPixels;
    int closestIndex = -1;
    for (int index = 0; index < m_offsets.size(); ++index) {
        const SequenceLayoutOffset &offset = m_offsets.at(index);
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
            continue;
        }
        const double distance = distanceSquared(
            pointer, screenPoint(offset, plot, scale));
        if (distance <= kHitRadiusPixels * kHitRadiusPixels
            && (closestIndex < 0 || distance < closestDistance)) {
            closestDistance = distance;
            closestIndex = index;
        }
    }

    if (closestIndex < 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_draggedIndex = closestIndex;
    event->accept();
}

void SequenceLayoutControl::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggedIndex < 0 || m_draggedIndex >= m_offsets.size()
        || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QRectF plot = plotBounds();
    const double scale = pixelsPerMeter(plot);
    const QPointF pointer = mousePosition(event);
    SequenceLayoutOffset &offset = m_offsets[m_draggedIndex];
    const double nextX = std::nearbyint(
        (pointer.x() - plot.center().x()) / scale * 100.0) / 100.0;
    const double nextY = std::nearbyint(
        (plot.center().y() - pointer.y()) / scale * 100.0) / 100.0;
    if (offset.x == nextX && offset.y == nextY) {
        event->accept();
        return;
    }

    offset.x = qFuzzyIsNull(nextX) ? 0.0 : nextX;
    offset.y = qFuzzyIsNull(nextY) ? 0.0 : nextY;
    emit offsetDragged(offset.systemId, offset.x, offset.y);
    emit offsetsChanged();
    update();
    event->accept();
}

void SequenceLayoutControl::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_draggedIndex >= 0) {
        m_draggedIndex = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
