#ifndef CONFIGPLANNERADVVIEW_H
#define CONFIGPLANNERADVVIEW_H

#include <QWidget>

class QTableWidget;

class ConfigPlannerAdvView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPlannerAdvView(QWidget *parent = nullptr);

public slots:
    void refresh();

private:
    QTableWidget *m_settingsTable = nullptr;
};

#endif
