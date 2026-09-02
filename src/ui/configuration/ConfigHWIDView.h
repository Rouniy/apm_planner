#ifndef CONFIGHWIDVIEW_H
#define CONFIGHWIDVIEW_H

#include "core/parameters/ParameterStore.h"

#include <QList>
#include <QWidget>

class ConfigHWIDViewModel;
class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;

/*
 * Mission Planner 10 SETUP > HW ID page
 * (GCSViews/ConfigurationView/ConfigHWIDView.axaml): title, Refresh button
 * and a read-only, sortable, resizable six-column grid.
 */
class ConfigHWIDView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigHWIDView(QWidget *parent = nullptr);
    ConfigHWIDView(ConfigHWIDViewModel *viewModel, QWidget *parent = nullptr);

    ConfigHWIDViewModel *viewModel() const { return m_viewModel; }
    QTableView *deviceTable() const { return m_table; }
    QSortFilterProxyModel *sortProxy() const { return m_proxy; }

    // Feeds the current committed ParameterStore snapshot to the model.
    void setParameterSnapshot(const QList<ParameterRecord> &records,
                              int preferredComponent = 1);

signals:
    // Refresh pressed: the owner should re-read the parameter store and call
    // setParameterSnapshot() again for this component.
    void refreshRequested(int preferredComponent);

private:
    void buildUi();

    ConfigHWIDViewModel *m_viewModel = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QTableView *m_table = nullptr;
};

#endif
