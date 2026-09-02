#ifndef FLIGHTDATAMAPOVERLAY_H
#define FLIGHTDATAMAPOVERLAY_H

#include <QObject>
#include <QPointer>
#include <QString>

class AbstractMapWidget;
class QCheckBox;
class QGridLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

class FlightDataMapOverlay final : public QObject
{
public:
    FlightDataMapOverlay(AbstractMapWidget *mapBackend,
                         QGridLayout *mapLayout,
                         QWidget *mapHost);

    void setTelemetry(double satelliteCount,
                      double gpsHdop,
                      double groundSpeed,
                      double windDirection,
                      double windSpeed,
                      const QString &missionProgressText,
                      double missionProgress);

private:
    QPointer<AbstractMapWidget> m_mapBackend;
    QCheckBox *m_autoPanCheckBox = nullptr;
    QPushButton *m_clearTrackButton = nullptr;
    QLabel *m_gpsLabel = nullptr;
    QLabel *m_motionLabel = nullptr;
    QLabel *m_missionLabel = nullptr;
    QProgressBar *m_missionProgress = nullptr;
};

#endif // FLIGHTDATAMAPOVERLAY_H
