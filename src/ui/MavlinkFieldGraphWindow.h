#ifndef MAVLINKFIELDGRAPHWINDOW_H
#define MAVLINKFIELDGRAPHWINDOW_H

#include "MavlinkFieldGraphModel.h"

#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class QLabel;
class QCPGraph;
class QCustomPlot;
class QTimer;

/**
 * Independent modeless graph for one numeric MAVLink message field.
 *
 * The owner is expected to be the application main window, not the inspector
 * which launched the graph. Packet routing remains external so the graph can
 * keep receiving its pinned traffic source after that inspector is closed.
 */
class MavlinkFieldGraphWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MavlinkFieldGraphWindow(
        const MavlinkGraphSelection &selection,
        int history,
        QWidget *owner);
    const MavlinkFieldGraphModel &model() const { return m_model; }

public slots:
    void receiveMessage(mavlink_message_t message);
    void clearSamples();
    void setSourceStatus(const QString &status);

private slots:
    void refreshPlot();

private:
    void centerOnOwner(QWidget *owner);
    void ensureGraph(int seriesIndex, const QString &label);
    void updateAutomaticRanges();

    MavlinkFieldGraphModel m_model;
    QCustomPlot *m_plot = nullptr;
    QLabel *m_sourceStatus = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QElapsedTimer m_clock;
    QVector<QCPGraph *> m_graphs;
    bool m_samplesDirty = false;
    bool m_automaticRanges = true;
};

#endif // MAVLINKFIELDGRAPHWINDOW_H
