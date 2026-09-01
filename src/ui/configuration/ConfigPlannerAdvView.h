#ifndef CONFIGPLANNERADVVIEW_H
#define CONFIGPLANNERADVVIEW_H

#include "ConfigPlannerAdvViewModel.h"

#include <QWidget>

class QResizeEvent;
class QTableWidget;

class ConfigPlannerAdvView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPlannerAdvView(QWidget *parent = nullptr);
    ConfigPlannerAdvViewModel *viewModel() const { return m_viewModel; }

public slots:
    void refresh();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void syncRows();
    void syncColumnWidths();

    ConfigPlannerAdvViewModel *m_viewModel = nullptr;
    QTableWidget *m_settingsTable = nullptr;
};

#endif
