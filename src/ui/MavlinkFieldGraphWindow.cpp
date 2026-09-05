#include "MavlinkFieldGraphWindow.h"

#include "qcustomplot.h"

#include <QColor>
#include <QFont>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPen>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QColor seriesColor(int index)
{
    static const QColor palette[] = {
        QColor(QStringLiteral("red")),
        QColor(QStringLiteral("green")),
        QColor(QStringLiteral("blue")),
        QColor(QStringLiteral("violet")),
        QColor(QStringLiteral("orange")),
        QColor(QStringLiteral("cyan")),
    };
    constexpr int paletteSize = sizeof(palette) / sizeof(palette[0]);
    return palette[index % paletteSize];
}

} // namespace

MavlinkFieldGraphWindow::MavlinkFieldGraphWindow(
    const MavlinkGraphSelection &selection,
    int history,
    QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_model(selection, history)
{
    setObjectName(QStringLiteral("mavlinkFieldGraphWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setWindowTitle(
        QStringLiteral("MAVLink Graph \u2014 %1.%2")
            .arg(selection.messageName, selection.fieldName));
    resize(800, 500);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_sourceStatus = new QLabel(
        tr("Waiting for MAVLink traffic"), this);
    m_sourceStatus->setObjectName(
        QStringLiteral("mavlinkFieldGraphSourceStatus"));
    m_sourceStatus->setWordWrap(true);
    layout->addWidget(m_sourceStatus);

    m_plot = new QCustomPlot(this);
    m_plot->setObjectName(QStringLiteral("mavlinkFieldGraphPlot"));
    m_plot->plotLayout()->insertRow(0);
    auto *title = new QCPTextElement(
        m_plot,
        tr("Vehicle %1, component %2")
            .arg(selection.systemId)
            .arg(selection.componentId),
        QFont(font().family(), 11, QFont::Bold));
    title->setObjectName(QStringLiteral("mavlinkFieldGraphVehicleTitle"));
    m_plot->plotLayout()->addElement(0, 0, title);
    m_plot->xAxis->setLabel(tr("Time since graph opened (s)"));
    // The generated C MAVLink metadata has no field-unit attribute. Keep the
    // Mission Planner fallback until the dialect metadata exposes one.
    m_plot->yAxis->setLabel(tr("Value"));
    m_plot->xAxis->setRange(0.0, 10.0);
    m_plot->yAxis->setRange(-1.0, 1.0);
    m_plot->legend->setVisible(true);
    m_plot->legend->setBrush(QBrush(QColor(255, 255, 255, 220)));
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    m_plot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    layout->addWidget(m_plot, 1);

    connect(m_plot, &QCustomPlot::mousePress,
            this, [this](QMouseEvent *) {
                m_automaticRanges = false;
            });
    connect(m_plot, &QCustomPlot::mouseWheel,
            this, [this](QWheelEvent *) {
                m_automaticRanges = false;
            });
    connect(m_plot, &QCustomPlot::mouseDoubleClick,
            this, [this](QMouseEvent *) {
                m_automaticRanges = true;
                m_samplesDirty = true;
            });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setObjectName(
        QStringLiteral("mavlinkFieldGraphRefreshTimer"));
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout,
            this, &MavlinkFieldGraphWindow::refreshPlot);
    m_clock.start();
    m_refreshTimer->start();

    centerOnOwner(owner);
}

void MavlinkFieldGraphWindow::receiveMessage(mavlink_message_t message)
{
    if (QThread::currentThread() != thread()) {
        const QPointer<MavlinkFieldGraphWindow> guardedWindow(this);
        QMetaObject::invokeMethod(
            this,
            [guardedWindow, message]() {
                if (guardedWindow) {
                    guardedWindow->receiveMessage(message);
                }
            },
            Qt::QueuedConnection);
        return;
    }

    const double seconds = static_cast<double>(m_clock.nsecsElapsed())
        / 1000000000.0;
    if (m_model.observe(message, seconds)) {
        m_samplesDirty = true;
    }
}

void MavlinkFieldGraphWindow::clearSamples()
{
    if (QThread::currentThread() != thread()) {
        const QPointer<MavlinkFieldGraphWindow> guardedWindow(this);
        QMetaObject::invokeMethod(
            this,
            [guardedWindow]() {
                if (guardedWindow) {
                    guardedWindow->clearSamples();
                }
            },
            Qt::QueuedConnection);
        return;
    }

    m_model.clear();
    for (QCPGraph *graph : m_graphs) {
        if (graph) {
            graph->data()->clear();
        }
    }
    m_automaticRanges = true;
    m_samplesDirty = false;
    m_plot->xAxis->setRange(0.0, 10.0);
    m_plot->yAxis->setRange(-1.0, 1.0);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void MavlinkFieldGraphWindow::setSourceStatus(const QString &status)
{
    if (QThread::currentThread() != thread()) {
        const QPointer<MavlinkFieldGraphWindow> guardedWindow(this);
        QMetaObject::invokeMethod(
            this,
            [guardedWindow, status]() {
                if (guardedWindow) {
                    guardedWindow->setSourceStatus(status);
                }
            },
            Qt::QueuedConnection);
        return;
    }

    m_sourceStatus->setText(status);
}

void MavlinkFieldGraphWindow::refreshPlot()
{
    if (!m_samplesDirty) {
        return;
    }

    const QVector<MavlinkGraphSeriesSnapshot> snapshots =
        m_model.snapshot();
    bool hasPoints = false;
    for (const MavlinkGraphSeriesSnapshot &snapshot : snapshots) {
        ensureGraph(snapshot.index, snapshot.label);
        if (snapshot.index < 0 || snapshot.index >= m_graphs.size()) {
            continue;
        }

        QVector<double> keys;
        QVector<double> values;
        keys.reserve(snapshot.points.size());
        values.reserve(snapshot.points.size());
        for (const MavlinkGraphPoint &point : snapshot.points) {
            keys.append(point.seconds);
            values.append(point.value);
        }
        m_graphs.at(snapshot.index)->setData(keys, values, true);
        hasPoints = hasPoints || !keys.isEmpty();
    }

    if (hasPoints && m_automaticRanges) {
        updateAutomaticRanges();
    }
    m_samplesDirty = false;
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void MavlinkFieldGraphWindow::centerOnOwner(QWidget *owner)
{
    if (!owner) {
        return;
    }

    const QPoint ownerCenter = owner->isWindow()
        ? owner->frameGeometry().center()
        : owner->mapToGlobal(owner->rect().center());
    move(ownerCenter - rect().center());
}

void MavlinkFieldGraphWindow::ensureGraph(
    int seriesIndex,
    const QString &label)
{
    if (seriesIndex < 0) {
        return;
    }

    while (m_graphs.size() <= seriesIndex) {
        const int index = m_graphs.size();
        QCPGraph *const graph = m_plot->addGraph();
        graph->setPen(QPen(seriesColor(index), 2.0));
        graph->setLineStyle(QCPGraph::lsLine);
        graph->setAdaptiveSampling(true);
        m_graphs.append(graph);
    }
    m_graphs.at(seriesIndex)->setName(label);
}

void MavlinkFieldGraphWindow::updateAutomaticRanges()
{
    m_plot->rescaleAxes();

    QCPRange xRange = m_plot->xAxis->range();
    if (xRange.size() <= 0.000001) {
        m_plot->xAxis->setRange(
            qMax(0.0, xRange.center() - 1.0),
            xRange.center() + 1.0);
    } else {
        m_plot->xAxis->scaleRange(1.05, xRange.center());
        xRange = m_plot->xAxis->range();
        if (xRange.lower < 0.0) {
            m_plot->xAxis->setRange(
                0.0, qMax(1.0, xRange.upper));
        }
    }

    const QCPRange yRange = m_plot->yAxis->range();
    if (yRange.size() <= 0.000001) {
        const double padding = qMax(1.0, qAbs(yRange.center()) * 0.05);
        m_plot->yAxis->setRange(
            yRange.center() - padding,
            yRange.center() + padding);
    } else {
        m_plot->yAxis->scaleRange(1.08, yRange.center());
    }
}
