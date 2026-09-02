#ifndef QGCMAPTOOLBAR_H
#define QGCMAPTOOLBAR_H

#include <QWidget>
#include <QMenu>
#include <QActionGroup>
#include <QPointer>

#include "maptype.h"

class AbstractMapWidget;

namespace Ui {
    class QGCMapToolBar;
}

class QGCMapToolBar : public QWidget
{
    Q_OBJECT

public:
    explicit QGCMapToolBar(QWidget *parent = 0);
    ~QGCMapToolBar();

    void setMap(AbstractMapWidget *map);

public slots:
    void tileLoadStart();
    void tileLoadEnd();
    void tileLoadProgress(int progress);
    void setUAVTrailTime();
    void setUAVTrailDistance();
    void setUpdateInterval();
    void setMapType();
    void setMapWidgetBackend();
    void updateMapType(core::MapType::Types type);
    void showMapStatus(const QString &status);
    void goHome();

private:
    void loadSettings();
    void storeSettings();
    void rebuildMapWidgetsMenu();
    void syncMapWidgetBackend(const QString &backendId);

private:
    Ui::QGCMapToolBar *ui;

protected:
    QPointer<AbstractMapWidget> map;
    QMenu optionsMenu;
    QMenu trailPlotMenu;
    QMenu updateTimesMenu;
    QMenu mapTypesMenu;
    QMenu mapWidgetsMenu;

    QActionGroup* trailSettingsGroup;
    QActionGroup* updateTimesGroup;
    QActionGroup* mapTypesGroup;
    QActionGroup* mapWidgetsGroup;
};

#endif // QGCMAPTOOLBAR_H
