#include "HeliCollectivePlot.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace {
const QColor kBackground(QStringLiteral("#151817"));
const QColor kGrid(QStringLiteral("#46504B"));
const QColor kAxis(QStringLiteral("#D3D3D3"));
const QColor kText(QStringLiteral("#D3D3D3"));
const QColor kStabilize(QStringLiteral("#1E90FF"));
const QColor kAcro(QStringLiteral("#FFD700"));
const QColor kCursor(QStringLiteral("#FF0000"));

QPointF plotPoint(const QRectF &plot,
                  const HeliVisualization::CurvePoint &point)
{
    const double input = std::clamp(point.inputPercent, 0.0, 100.0);
    const double output = std::clamp(point.output, 0.0, 1000.0);
    return QPointF(plot.left() + input * plot.width() / 100.0,
                   plot.bottom() - output * plot.height() / 1000.0);
}

void drawCenteredText(QPainter *painter, const QPointF &center,
                      const QString &text)
{
    const QRectF bounds = QFontMetricsF(painter->font()).boundingRect(text);
    painter->drawText(center.x() - bounds.width() / 2.0,
                      center.y() + bounds.height() / 4.0, text);
}

void drawRightCenteredText(QPainter *painter, const QPointF &centerRight,
                           const QString &text)
{
    const QRectF bounds = QFontMetricsF(painter->font()).boundingRect(text);
    painter->drawText(centerRight.x() - bounds.width(),
                      centerRight.y() + bounds.height() / 4.0, text);
}

void drawCurve(QPainter *painter, const QRectF &plot,
               const QVector<HeliVisualization::CurvePoint> &curve,
               const QPen &pen, bool drawMarkers)
{
    QPainterPath path;
    bool hasPrevious = false;
    QVector<QPointF> markers;
    if (drawMarkers) {
        markers.reserve(curve.size());
    }

    for (const HeliVisualization::CurvePoint &point : curve) {
        if (!std::isfinite(point.inputPercent)
            || !std::isfinite(point.output)) {
            // Invalid samples create a gap instead of bridging unrelated data.
            hasPrevious = false;
            continue;
        }
        const QPointF current = plotPoint(plot, point);
        if (hasPrevious) {
            path.lineTo(current);
        } else {
            path.moveTo(current);
        }
        hasPrevious = true;
        if (drawMarkers) {
            markers.append(current);
        }
    }

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    if (drawMarkers) {
        painter->setBrush(kBackground);
        for (const QPointF &marker : markers) {
            painter->drawEllipse(marker, 3.5, 3.5);
        }
    }
}
} // namespace

HeliCollectivePlot::HeliCollectivePlot(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HeliCollectivePlot"));
    setMinimumSize(minimumSizeHint());
    setAutoFillBackground(false);
    setToolTip(tr("Blue: Stabilize collective points. Gold: Acro expo curve. "
                  "Red: live collective output position."));
}

void HeliCollectivePlot::setStabilizeCurve(
    const QVector<HeliVisualization::CurvePoint> &curve)
{
    m_stabilizeCurve = curve;
    update();
}

void HeliCollectivePlot::setAcroCurve(
    const QVector<HeliVisualization::CurvePoint> &curve)
{
    m_acroCurve = curve;
    update();
}

void HeliCollectivePlot::setCursorPercent(double percent)
{
    m_cursorPercent = std::isfinite(percent)
        ? std::clamp(percent, 0.0, 100.0) : 0.0;
    update();
}

QSize HeliCollectivePlot::minimumSizeHint() const
{
    return QSize(430, 280);
}

void HeliCollectivePlot::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing
                           | QPainter::TextAntialiasing);
    painter.fillRect(rect(), kBackground);

    if (width() < 140 || height() < 120) {
        return;
    }

    const QRectF plot(52.0, 28.0,
                      std::max(1.0, width() - 66.0),
                      std::max(1.0, height() - 70.0));

    painter.setFont(QFont(font().family(), 9));
    painter.setPen(QPen(kGrid, 1.0));
    for (int input = 0; input <= 100; input += 20) {
        const double x = plot.left() + input * plot.width() / 100.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(kText);
        drawCenteredText(&painter, QPointF(x, plot.bottom() + 13.0),
                         QString::number(input));
        painter.setPen(QPen(kGrid, 1.0));
    }
    for (int output = 0; output <= 1000; output += 200) {
        const double y = plot.bottom() - output * plot.height() / 1000.0;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(kText);
        drawRightCenteredText(&painter, QPointF(plot.left() - 5.0, y),
                              QString::number(output));
        painter.setPen(QPen(kGrid, 1.0));
    }

    painter.setPen(QPen(kAxis, 1.5));
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.bottomLeft(), plot.topLeft());

    drawCurve(&painter, plot, m_stabilizeCurve,
              QPen(kStabilize, 2.5), true);
    drawCurve(&painter, plot, m_acroCurve, QPen(kAcro, 2.0), false);

    const double cursorX = plot.left()
        + m_cursorPercent * plot.width() / 100.0;
    painter.setPen(QPen(kCursor, 2.0));
    painter.drawLine(QPointF(cursorX, plot.top()),
                     QPointF(cursorX, plot.bottom()));

    painter.setFont(QFont(font().family(), 10, QFont::DemiBold));
    painter.setPen(Qt::white);
    painter.drawText(QPointF(plot.left(), 17.0), tr("Collective Control"));

    painter.setFont(QFont(font().family(), 8));
    painter.setPen(kText);
    drawCenteredText(&painter,
                     QPointF(plot.center().x(), height() - 9.0),
                     tr("Collective Input (%)"));
    painter.drawText(QPointF(5.0, 17.0), tr("Output"));
    painter.setPen(kStabilize);
    painter.drawText(QPointF(plot.right() - 122.0, 17.0),
                     tr("Stabilize"));
    painter.setPen(kAcro);
    painter.drawText(QPointF(plot.right() - 60.0, 17.0), tr("Acro"));
}
