#ifndef FLIGHTPLANNERPREFETCHCONTROLLER_H
#define FLIGHTPLANNERPREFETCHCONTROLLER_H

#include "MapPrefetchService.h"

#include <QPointer>
#include <QObject>

#include <atomic>
#include <memory>

class QAction;
class AbstractMapWidget;
class FlightPlannerViewModel;
class QThread;

class FlightPlannerPrefetchController final : public QObject
{
    Q_OBJECT

public:
    explicit FlightPlannerPrefetchController(
        AbstractMapWidget *map,
        FlightPlannerViewModel *viewModel,
        QObject *parent = nullptr);
    ~FlightPlannerPrefetchController() override;

    QAction *PrefetchVisibleAreaAction() const
    {
        return m_prefetchVisibleAreaAction;
    }
    QAction *PrefetchWaypointPathAction() const
    {
        return m_prefetchWaypointPathAction;
    }
    bool IsRunning() const { return m_running; }

public slots:
    void PrefetchVisibleArea();
    void PrefetchWaypointPath();

private:
    bool promptZoomRange(int *minimumZoom, int *maximumZoom);
    void startPrefetch(bool pathOnly);
    void runPrefetch(core::MapType::Types mapType,
                     const QVector<MapTileInfo> &tiles);
    void reportProgress(int done, int total);
    void finishPrefetch(core::MapType::Types mapType,
                        const MapPrefetchResult &result);

    QPointer<AbstractMapWidget> m_map;
    QPointer<FlightPlannerViewModel> m_viewModel;
    QAction *m_prefetchVisibleAreaAction = nullptr;
    QAction *m_prefetchWaypointPathAction = nullptr;
    QPointer<QThread> m_workerThread;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
};

#endif // FLIGHTPLANNERPREFETCHCONTROLLER_H
