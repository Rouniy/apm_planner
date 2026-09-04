#include "WaypointLeaderProfileControl.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
const QColor kBackground(QStringLiteral("#151817"));
const QColor kGrid(QStringLiteral("#46504B"));
const QColor kPath(QStringLiteral("#FF5B72"));
const QColor kGroundMaster(QStringLiteral("#32CD32"));
const QColor kAirMaster(QStringLiteral("#FFD700"));
const QColor kFollower(QStringLiteral("#00BFFF"));
const QColor kPrimaryText(QStringLiteral("#FFFFFF"));
const QColor kSecondaryText(QStringLiteral("#D3D3D3"));

constexpr int kMinimumPaintWidth = 180;
constexpr int kMinimumPaintHeight = 130;

struct AltitudeScale
{
    double scale = 1.0;
    double minimumScaled = 0.0;
    double maximumScaled = 1.0;
};

bool validRole(WaypointLeaderVehicleRole role)
{
    switch (role) {
    case WaypointLeaderVehicleRole::GroundMaster:
    case WaypointLeaderVehicleRole::AirMaster:
    case WaypointLeaderVehicleRole::Follower:
        return true;
    }
    return false;
}

bool sameProfile(const QVector<SwarmWaypointLeaderProfilePoint> &left,
                 const QVector<SwarmWaypointLeaderProfilePoint> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (int index = 0; index < left.size(); ++index) {
        if (left.at(index).distanceM != right.at(index).distanceM
            || left.at(index).relativeAltitudeM
                != right.at(index).relativeAltitudeM) {
            return false;
        }
    }
    return true;
}

QVector<SwarmWaypointLeaderProfilePoint> normalizedProfile(
    const QVector<SwarmWaypointLeaderProfilePoint> &source)
{
    qint64 finiteCount = 0;
    for (const SwarmWaypointLeaderProfilePoint &point : source) {
        if (std::isfinite(point.distanceM)
            && std::isfinite(point.relativeAltitudeM)) {
            ++finiteCount;
        }
    }

    QVector<SwarmWaypointLeaderProfilePoint> finite;
    const int retainedCount = finiteCount
            < WaypointLeaderProfileControl::MaximumProfilePoints
        ? static_cast<int>(finiteCount)
        : WaypointLeaderProfileControl::MaximumProfilePoints;
    finite.reserve(retainedCount);

    qint64 finiteIndex = 0;
    qint64 retainedIndex = 0;
    const qint64 retainedLast = retainedCount - 1;
    const qint64 finiteLast = finiteCount - 1;
    for (const SwarmWaypointLeaderProfilePoint &point : source) {
        if (!std::isfinite(point.distanceM)
            || !std::isfinite(point.relativeAltitudeM)) {
            continue;
        }
        const qint64 selectedFiniteIndex = finiteCount > retainedCount
            ? (retainedIndex * finiteLast) / retainedLast
            : retainedIndex;
        if (retainedIndex < retainedCount
            && finiteIndex == selectedFiniteIndex) {
            finite.append(point);
            ++retainedIndex;
        }
        ++finiteIndex;
    }
    std::stable_sort(finite.begin(), finite.end(),
                     [](const SwarmWaypointLeaderProfilePoint &left,
                        const SwarmWaypointLeaderProfilePoint &right) {
        return left.distanceM < right.distanceM;
    });
    return finite;
}

QVector<WaypointLeaderVehicleMarker> normalizedMarkers(
    const QVector<WaypointLeaderVehicleMarker> &source)
{
    QVector<WaypointLeaderVehicleMarker> result;
    result.reserve(source.size()
                           < WaypointLeaderProfileControl::MaximumVehicleMarkers
                       ? static_cast<int>(source.size())
                       : WaypointLeaderProfileControl::MaximumVehicleMarkers);
    for (const WaypointLeaderVehicleMarker &marker : source) {
        if (result.size()
                >= WaypointLeaderProfileControl::MaximumVehicleMarkers) {
            break;
        }
        if (!validRole(marker.role)
            || !std::isfinite(marker.pathDistanceM)
            || !std::isfinite(marker.altitudeM)) {
            continue;
        }
        result.append(marker);
    }
    return result;
}

double pathRatio(double distanceM, double lengthM)
{
    if (distanceM <= 0.0) {
        return 0.0;
    }
    if (distanceM >= lengthM) {
        return 1.0;
    }
    const double ratio = distanceM / lengthM;
    return std::isfinite(ratio) ? qBound(0.0, ratio, 1.0) : 0.0;
}

AltitudeScale altitudeScale(
    const QVector<SwarmWaypointLeaderProfilePoint> &profile,
    const QVector<WaypointLeaderVehicleMarker> &markers)
{
    double minimum = 0.0;
    double maximum = 1.0;
    for (const SwarmWaypointLeaderProfilePoint &point : profile) {
        minimum = std::min(minimum, point.relativeAltitudeM);
        maximum = std::max(maximum, point.relativeAltitudeM);
    }
    for (const WaypointLeaderVehicleMarker &marker : markers) {
        minimum = std::min(minimum, marker.altitudeM);
        maximum = std::max(maximum, marker.altitudeM);
    }

    // Normalize before subtraction.  This remains finite even for the full
    // [-DBL_MAX, DBL_MAX] range on platforms where long double equals double.
    AltitudeScale result;
    result.scale = std::max({1.0, std::abs(minimum), std::abs(maximum)});
    const double minimumScaled = minimum / result.scale;
    const double maximumScaled = maximum / result.scale;
    const double spanScaled = maximumScaled - minimumScaled;
    const double paddingScaled = std::max(
        2.0 / result.scale, spanScaled * 0.1);
    result.minimumScaled = minimumScaled - paddingScaled;
    result.maximumScaled = maximumScaled + paddingScaled;
    return result;
}

double altitudeRatio(double altitudeM, const AltitudeScale &scale)
{
    const double span = scale.maximumScaled - scale.minimumScaled;
    if (!(span > 0.0) || !std::isfinite(span)) {
        return 0.5;
    }
    const double ratio = (altitudeM / scale.scale - scale.minimumScaled) / span;
    return std::isfinite(ratio) ? qBound(0.0, ratio, 1.0) : 0.5;
}

QPointF plotPoint(const QRectF &plot, double distanceM, double altitudeM,
                  double lengthM, const AltitudeScale &scale)
{
    return QPointF(plot.left() + pathRatio(distanceM, lengthM) * plot.width(),
                   plot.bottom()
                       - altitudeRatio(altitudeM, scale) * plot.height());
}

QPointF invalidPoint()
{
    const double value = std::numeric_limits<double>::quiet_NaN();
    return QPointF(value, value);
}

QColor markerColor(WaypointLeaderVehicleRole role)
{
    switch (role) {
    case WaypointLeaderVehicleRole::GroundMaster:
        return kGroundMaster;
    case WaypointLeaderVehicleRole::AirMaster:
        return kAirMaster;
    case WaypointLeaderVehicleRole::Follower:
        return kFollower;
    }
    return kFollower;
}

QFont plotFont(int pixelSize)
{
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(pixelSize);
    return font;
}

void drawText(QPainter &painter, const QString &text, const QPointF &topLeft,
              const QColor &color, int pixelSize)
{
    painter.save();
    const QFont font = plotFont(pixelSize);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(topLeft + QPointF(0.0, QFontMetricsF(font).ascent()), text);
    painter.restore();
}

void drawCentered(QPainter &painter, const QString &text,
                  const QPointF &center, const QColor &color, int pixelSize)
{
    painter.save();
    const QFont font = plotFont(pixelSize);
    const QFontMetricsF metrics(font);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QPointF(center.x() - metrics.horizontalAdvance(text) / 2.0,
                             center.y()
                                 + (metrics.ascent() - metrics.descent()) / 2.0),
                     text);
    painter.restore();
}

void drawRightCentered(QPainter &painter, const QString &text,
                       const QPointF &rightCenter, const QColor &color,
                       int pixelSize)
{
    painter.save();
    const QFont font = plotFont(pixelSize);
    const QFontMetricsF metrics(font);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QPointF(rightCenter.x() - metrics.horizontalAdvance(text),
                             rightCenter.y()
                                 + (metrics.ascent() - metrics.descent()) / 2.0),
                     text);
    painter.restore();
}

double scaledAxisValue(double scaled, double scale)
{
    const double maximum = std::numeric_limits<double>::max();
    if (scaled > 0.0 && scaled > maximum / scale) {
        return maximum;
    }
    if (scaled < 0.0 && -scaled > maximum / scale) {
        return -maximum;
    }
    return scaled * scale;
}

QString axisNumber(double value, int decimalPlaces)
{
    if (qFuzzyIsNull(value)) {
        value = 0.0;
    }
    const double magnitude = std::abs(value);
    if (magnitude >= 10000000.0
        || (magnitude > 0.0 && magnitude < 0.01)) {
        return QString::number(value, 'g', 4);
    }
    return QString::number(value, 'f', decimalPlaces);
}
} // namespace

WaypointLeaderProfileControl::WaypointLeaderProfileControl(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("WaypointLeaderProfileControl"));
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void WaypointLeaderProfileControl::setProfile(
    const QVector<SwarmWaypointLeaderProfilePoint> &profile)
{
    QVector<SwarmWaypointLeaderProfilePoint> normalized =
        normalizedProfile(profile);
    if (sameProfile(m_profile, normalized)) {
        return;
    }
    m_profile = std::move(normalized);
    emit profileChanged();
    update();
}

void WaypointLeaderProfileControl::setVehicleMarkers(
    const QVector<WaypointLeaderVehicleMarker> &markers)
{
    QVector<WaypointLeaderVehicleMarker> normalized =
        normalizedMarkers(markers);
    if (m_vehicleMarkers == normalized) {
        return;
    }
    m_vehicleMarkers = std::move(normalized);
    emit vehicleMarkersChanged();
    update();
}

bool WaypointLeaderProfileControl::hasDrawableProfile() const noexcept
{
    return m_profile.size() >= 2 && m_profile.constLast().distanceM > 0.0;
}

QSize WaypointLeaderProfileControl::minimumSizeHint() const
{
    return QSize(260, 180);
}

QSize WaypointLeaderProfileControl::sizeHint() const
{
    return QSize(620, 300);
}

QRectF WaypointLeaderProfileControl::plotBounds() const
{
    return QRectF(56.0, 26.0,
                  std::max(1.0, width() - 72.0),
                  std::max(1.0, height() - 68.0));
}

QPointF WaypointLeaderProfileControl::profilePointPosition(
    int profileIndex) const
{
    if (!hasDrawableProfile() || profileIndex < 0
        || profileIndex >= m_profile.size()) {
        return invalidPoint();
    }
    const SwarmWaypointLeaderProfilePoint &point = m_profile.at(profileIndex);
    return plotPoint(plotBounds(), point.distanceM, point.relativeAltitudeM,
                     m_profile.constLast().distanceM,
                     altitudeScale(m_profile, m_vehicleMarkers));
}

QPointF WaypointLeaderProfileControl::vehicleMarkerPosition(
    int markerIndex) const
{
    if (!hasDrawableProfile() || markerIndex < 0
        || markerIndex >= m_vehicleMarkers.size()) {
        return invalidPoint();
    }
    const WaypointLeaderVehicleMarker &marker =
        m_vehicleMarkers.at(markerIndex);
    return plotPoint(plotBounds(), marker.pathDistanceM, marker.altitudeM,
                     m_profile.constLast().distanceM,
                     altitudeScale(m_profile, m_vehicleMarkers));
}

void WaypointLeaderProfileControl::drawGrid(QPainter &painter,
                                            const QRectF &plot) const
{
    const double lengthM = m_profile.constLast().distanceM;
    const AltitudeScale altitudes =
        altitudeScale(m_profile, m_vehicleMarkers);
    const QPen gridPen(kGrid, 1.0);
    for (int step = 0; step <= 4; ++step) {
        const double ratio = step / 4.0;
        const double x = plot.left() + ratio * plot.width();
        const double y = plot.bottom() - ratio * plot.height();
        painter.setPen(gridPen);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        drawCentered(painter, axisNumber(lengthM * ratio, 0),
                     QPointF(x, plot.bottom() + 12.0), kSecondaryText, 10);

        const double scaledAltitude = altitudes.minimumScaled
            + (altitudes.maximumScaled - altitudes.minimumScaled) * ratio;
        drawRightCentered(
            painter,
            axisNumber(scaledAxisValue(scaledAltitude, altitudes.scale), 1),
            QPointF(plot.left() - 5.0, y), kSecondaryText, 10);
    }
    painter.setPen(QPen(kSecondaryText, 1.5));
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.bottomLeft(), plot.topLeft());
}

void WaypointLeaderProfileControl::drawProfile(QPainter &painter,
                                               const QRectF &plot) const
{
    const double lengthM = m_profile.constLast().distanceM;
    const AltitudeScale altitudes =
        altitudeScale(m_profile, m_vehicleMarkers);
    QPainterPath path;
    for (int index = 0; index < m_profile.size(); ++index) {
        const SwarmWaypointLeaderProfilePoint &point = m_profile.at(index);
        const QPointF position = plotPoint(
            plot, point.distanceM, point.relativeAltitudeM,
            lengthM, altitudes);
        if (index == 0) {
            path.moveTo(position);
        } else {
            path.lineTo(position);
        }
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kPath, 2.5));
    painter.drawPath(path);
    painter.setBrush(kBackground);
    for (int index = 0; index < m_profile.size(); ++index) {
        const QPointF position = plotPoint(
            plot, m_profile.at(index).distanceM,
            m_profile.at(index).relativeAltitudeM, lengthM, altitudes);
        painter.drawEllipse(position, 3.0, 3.0);
    }
}

void WaypointLeaderProfileControl::drawVehicleMarkers(
    QPainter &painter, const QRectF &plot) const
{
    const double lengthM = m_profile.constLast().distanceM;
    const AltitudeScale altitudes =
        altitudeScale(m_profile, m_vehicleMarkers);
    const QFont labelFont = plotFont(10);
    const QFontMetricsF labelMetrics(labelFont);
    for (const WaypointLeaderVehicleMarker &marker : m_vehicleMarkers) {
        const QColor color = markerColor(marker.role);
        const QPointF point = plotPoint(plot, marker.pathDistanceM,
                                        marker.altitudeM, lengthM, altitudes);
        painter.setBrush(color);
        painter.setPen(QPen(Qt::black, 1.0));
        painter.drawEllipse(point, 5.0, 5.0);

        painter.setFont(labelFont);
        painter.setPen(color);
        const int availableWidth = static_cast<int>(
            std::max(16.0, plot.right() - point.x() - 7.0));
        const QString label = labelMetrics.elidedText(
            marker.label, Qt::ElideRight, availableWidth);
        painter.drawText(point + QPointF(7.0,
                                         -8.0 + labelMetrics.ascent()), label);
    }
}

void WaypointLeaderProfileControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(rect());
    painter.fillRect(rect(), kBackground);
    if (width() < kMinimumPaintWidth || height() < kMinimumPaintHeight) {
        return;
    }

    const QRectF plot = plotBounds();
    if (!hasDrawableProfile()) {
        drawCentered(
            painter,
            QStringLiteral(
                "Select an air master with a downloaded waypoint mission"),
            plot.center(), kSecondaryText, 12);
        return;
    }

    drawGrid(painter, plot);
    drawProfile(painter, plot);
    drawVehicleMarkers(painter, plot);

    drawText(painter, QStringLiteral("Mission altitude profile"),
             QPointF(plot.left(), 4.0), kPrimaryText, 13);
    drawCentered(painter, QStringLiteral("Distance along mission (m)"),
                 QPointF(plot.center().x(), height() - 9.0),
                 kSecondaryText, 11);
    drawText(painter, QStringLiteral("Alt (m)"),
             QPointF(5.0, 7.0), kSecondaryText, 10);
    drawText(painter, QStringLiteral("Ground"),
             QPointF(plot.right() - 180.0, 5.0), kGroundMaster, 10);
    drawText(painter, QStringLiteral("Air"),
             QPointF(plot.right() - 120.0, 5.0), kAirMaster, 10);
    drawText(painter, QStringLiteral("Follower"),
             QPointF(plot.right() - 78.0, 5.0), kFollower, 10);
}
