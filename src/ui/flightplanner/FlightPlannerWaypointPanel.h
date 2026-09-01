#ifndef FLIGHTPLANNERWAYPOINTPANEL_H
#define FLIGHTPLANNERWAYPOINTPANEL_H

#include <QPointer>
#include <QWidget>

class QAction;
class FlightPlannerViewModel;
class QModelIndex;
class QObject;
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

signals:
    void selectedWaypointChanged(int row);

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

    QPointer<FlightPlannerViewModel> m_viewModel;
    QTableView *m_waypointTable = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
    int m_selectedWaypoint = -1;
};

#endif
