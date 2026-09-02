#include "ProximityRadarControl.h"

#include "Proximity.h"

#include <QHideEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {
const QColor kBackground(QStringLiteral("#151817"));
const QColor kGrid(QStringLiteral("#46504B"));
const QColor kVehicle(QStringLiteral("#2F81F7"));
const QColor kLimeGreen(QStringLiteral("#32CD32"));
constexpr double kMaximumRadiusCm = 100000.0;
constexpr double kMaximumVehicleSizeCm = 100000.0;
}

ProximityRadarControl::ProximityRadarControl(QWidget *parent)
    : ProximityRadarControl(nullptr, parent)
{
}

ProximityRadarControl::ProximityRadarControl(
    Proximity *proximity, QWidget *parent)
    : QWidget(parent),
      m_timer(new QTimer(this))
{
    setObjectName(QStringLiteral("ProximityRadarControl"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this,
            qOverload<>(&ProximityRadarControl::update));
    setProximity(proximity);
}

Proximity *ProximityRadarControl::ProximityState() const
{
    return m_proximity;
}

void ProximityRadarControl::setProximity(Proximity *proximity)
{
    if (m_proximity == proximity) {
        return;
    }
    m_proximity = proximity;
    update();
}

QSize ProximityRadarControl::sizeHint() const
{
    return QSize(620, 620);
}

void ProximityRadarControl::setRadiusCm(double radiusCm)
{
    if (!std::isfinite(radiusCm)) {
        return;
    }
    const double bounded = std::max(
        50.0, std::min(radiusCm, kMaximumRadiusCm));
    if (qFuzzyCompare(m_radiusCm, bounded)) {
        return;
    }
    m_radiusCm = bounded;
    emit scaleChanged();
    update();
}

void ProximityRadarControl::setVehicleSizeCm(double vehicleSizeCm)
{
    if (!std::isfinite(vehicleSizeCm)) {
        return;
    }
    const double bounded = std::max(
        10.0, std::min(vehicleSizeCm, kMaximumVehicleSizeCm));
    if (qFuzzyCompare(m_vehicleSizeCm, bounded)) {
        return;
    }
    m_vehicleSizeCm = bounded;
    emit scaleChanged();
    update();
}

void ProximityRadarControl::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_timer->start();
    setFocus(Qt::OtherFocusReason);
}

void ProximityRadarControl::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

void ProximityRadarControl::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        setRadiusCm(m_radiusCm - 50.0);
        event->accept();
        return;
    case Qt::Key_Minus:
        setRadiusCm(m_radiusCm + 50.0);
        event->accept();
        return;
    case Qt::Key_BracketLeft:
        setVehicleSizeCm(m_vehicleSizeCm - 10.0);
        event->accept();
        return;
    case Qt::Key_BracketRight:
        setVehicleSizeCm(m_vehicleSizeCm + 10.0);
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
}

QPointF ProximityRadarControl::Polar(const QPointF &center, double radius,
                                     double angleDegrees)
{
    const double radians = qDegreesToRadians(angleDegrees);
    return QPointF(center.x() + std::sin(radians) * radius,
                   center.y() - std::cos(radians) * radius);
}

void ProximityRadarControl::DrawArc(
    QPainter &painter, const QPointF &center, double radius,
    double startAngle, double endAngle, const QPen &pen)
{
    if (radius <= 0.0) {
        return;
    }
    const QRectF bounds(center.x() - radius, center.y() - radius,
                        radius * 2.0, radius * 2.0);
    // Qt starts at three o'clock and sweeps counter-clockwise; Mission
    // Planner starts at north and increases clockwise.
    const double qtStart = 90.0 - startAngle;
    const double qtSweep = -(endAngle - startAngle);
    QPainterPath path;
    path.arcMoveTo(bounds, qtStart);
    path.arcTo(bounds, qtStart, qtSweep);
    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.restore();
}

void ProximityRadarControl::DrawText(
    QPainter &painter, const QString &text, const QPointF &point,
    const QColor &color, double pointSize)
{
    painter.save();
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(qRound(pointSize));
    painter.setFont(font);
    painter.setPen(color);
    const QFontMetricsF metrics(font);
    painter.drawText(QRectF(point,
                            QSizeF(metrics.horizontalAdvance(text),
                                   metrics.height())),
                     Qt::AlignLeft | Qt::AlignTop, text);
    painter.restore();
}

void ProximityRadarControl::DrawCenteredText(
    QPainter &painter, const QString &text, const QPointF &center,
    const QColor &color, double pointSize)
{
    painter.save();
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(qRound(pointSize));
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    painter.setPen(color);
    painter.drawText(QPointF(center.x() - metrics.horizontalAdvance(text) / 2.0,
                             center.y() + (metrics.ascent() - metrics.descent()) / 2.0),
                     text);
    painter.restore();
}

void ProximityRadarControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBackground);

    const QPointF center(width() / 2.0, height() / 2.0);
    const double maxPixels = std::max(
        1.0, std::min(width(), height()) / 2.0 - 35.0);
    const double scale = m_radiusCm / maxPixels;
    const QPen gridPen(kGrid, 1.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(gridPen);
    for (double cm = 50.0; cm <= m_radiusCm; cm += 50.0) {
        const double ringRadius = cm / scale;
        painter.drawEllipse(center, ringRadius, ringRadius);
        DrawText(painter,
                 QStringLiteral("%1m").arg(cm / 100.0, 0, 'f', 1),
                 QPointF(center.x() + 4.0,
                         center.y() - ringRadius - 14.0),
                 kLimeGreen, 11.0);
    }

    for (double angle = 0.0; angle < 360.0; angle += 45.0) {
        painter.setPen(gridPen);
        painter.drawLine(center, Polar(center, maxPixels, angle));
        DrawCenteredText(painter, QString::number(angle, 'f', 0),
                         Polar(center, maxPixels + 12.0, angle),
                         Qt::gray, 11.0);
    }

    const double vehicleRadius = std::max(
        4.0, m_vehicleSizeCm / scale / 2.0);
    painter.setPen(QPen(Qt::white, 1.0));
    painter.setBrush(kVehicle);
    painter.drawEllipse(center, vehicleRadius, vehicleRadius);
    painter.setPen(QPen(Qt::white, 2.0));
    painter.drawLine(center,
                     QPointF(center.x(), center.y() - vehicleRadius - 8.0));

    const QVector<Proximity::Sample> samples = m_proximity
        ? m_proximity->directionState().GetRaw()
        : QVector<Proximity::Sample>();
    if (samples.isEmpty()) {
        DrawCenteredText(
            painter,
            tr("Waiting for DISTANCE_SENSOR / OBSTACLE_DISTANCE"),
            QPointF(center.x(), height() - 18.0), Qt::gray, 12.0);
        return;
    }

    for (const Proximity::Sample &sample : samples) {
        const double angle = sample.IsCustom()
            ? sample.Angle
            : Proximity::OrientationAngle(sample.Orientation);
        const double distance = std::min(sample.Distance, m_radiusCm);
        const double radius = distance / scale;
        const double widthDegrees = sample.IsCustom() ? sample.Size : 45.0;
        DrawArc(painter, center, radius,
                angle - widthDegrees / 2.0,
                angle + widthDegrees / 2.0,
                QPen(sample.IsCustom()
                         ? QColor(QStringLiteral("#FFD700"))
                         : QColor(Qt::red),
                     3.0));
        DrawCenteredText(
            painter,
            QStringLiteral("%1m").arg(sample.Distance / 100.0, 0, 'f', 1),
            Polar(center, std::max(20.0, radius - 12.0), angle),
            kLimeGreen, 12.0);
    }

    DrawText(
        painter,
        tr("Radius: %1 m (+/−), vehicle: %2 m ([/])")
            .arg(m_radiusCm / 100.0, 0, 'f', 1)
            .arg(m_vehicleSizeCm / 100.0, 0, 'f', 1),
        QPointF(8.0, 8.0), Qt::white, 12.0);
}
