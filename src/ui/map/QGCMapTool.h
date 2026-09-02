#ifndef QGCMAPTOOL_H
#define QGCMAPTOOL_H

class UASInterface;
class AbstractMapWidget;
class FlightDataMapOverlay;
class FlightDataViewModel;
enum class MapWidgetRole;
#include <QWidget>
#include <QMenu>
#include <QPointer>
#include <QTimer>

namespace Ui {
    class QGCMapTool;
}

class QGCMapTool : public QWidget
{
    Q_OBJECT

public:
    explicit QGCMapTool(QWidget *parent = 0);
    explicit QGCMapTool(MapWidgetRole role, QWidget *parent = nullptr);
    ~QGCMapTool();
    AbstractMapWidget *mapWidget() const;
    void setFlightDataViewModel(FlightDataViewModel *viewModel);

public slots:
    void setMapZoom(int zoom);
    /** @brief Update slider zoom from map change */
    void setZoom(int zoom);

signals:
    void visibilityChanged(bool visible);

private slots:
    void activeUASSet(UASInterface *uasInterface);
    void globalPositionUpdate();
    void gpsHdopChanged(double value, const QString&);
    void gpsFixChanged(int, const QString&);
    void satelliteCountChanged(int value, const QString&);

private:
    void showEvent(QShowEvent* event);
    void hideEvent(QHideEvent* event);
    void updateFlightDataOverlay();

private:
    Ui::QGCMapTool *ui;

    MapWidgetRole m_role;
    QPointer<UASInterface> m_uasInterface;
    QPointer<AbstractMapWidget> m_mapBackend;
    QPointer<FlightDataViewModel> m_flightDataViewModel;
    QPointer<FlightDataMapOverlay> m_flightDataOverlay;
};

#endif // QGCMAPTOOL_H
