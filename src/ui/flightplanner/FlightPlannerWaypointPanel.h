#ifndef FLIGHTPLANNERWAYPOINTPANEL_H
#define FLIGHTPLANNERWAYPOINTPANEL_H

#include <QPointer>
#include <QWidget>

class QAction;
class FlightPlannerViewModel;
class QModelIndex;
class QObject;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QSlider;
class QTableView;
class QEvent;

class FlightPlannerWaypointPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit FlightPlannerWaypointPanel(FlightPlannerViewModel *viewModel,
                                        QWidget *parent = nullptr);
    ~FlightPlannerWaypointPanel() override;

    FlightPlannerViewModel *viewModel() const;
    QTableView *waypointTable() const;
    int selectedWaypoint() const;
    int zoomLevel() const;

public slots:
    void setZoomRange(int minimum, int maximum);
    void setZoomLevel(int level);

signals:
    void selectedWaypointChanged(int row);
    void zoomLevelRequested(int level);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void addWaypoint();
    void deleteSelectedWaypoint();
    void moveSelectedWaypointUp();
    void moveSelectedWaypointDown();
    void handleCurrentRowChanged(const QModelIndex &current,
                                 const QModelIndex &previous);
    void updateActions();

private:
    void selectWaypoint(int row);
    void updateRouteSummary();
    void syncAltitudePresentation();
    void syncDistancePresentation();

    QPointer<FlightPlannerViewModel> m_viewModel;
    QTableView *m_waypointTable = nullptr;
    QDoubleSpinBox *m_wpRadiusEditor = nullptr;
    QDoubleSpinBox *m_loiterRadiusEditor = nullptr;
    QDoubleSpinBox *m_defaultAltitudeEditor = nullptr;
    QComboBox *m_defaultFrameEditor = nullptr;
    QDoubleSpinBox *m_altWarnEditor = nullptr;
    QCheckBox *m_splineDefaultEditor = nullptr;
    QCheckBox *m_verifyHeightEditor = nullptr;
    QSlider *m_zoomSlider = nullptr;
    QLabel *m_totalDistanceLabel = nullptr;
    QLabel *m_homeDistanceLabel = nullptr;
    QLabel *m_previousDistanceLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
    int m_selectedWaypoint = -1;
};

#endif
