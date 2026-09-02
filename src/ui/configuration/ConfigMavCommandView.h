#ifndef CONFIGMAVCOMMANDVIEW_H
#define CONFIGMAVCOMMANDVIEW_H

#include <QWidget>

class BackstagePage;
class ConfigMavCommandViewModel;
class QLabel;
class QTableView;

class ConfigMavCommandView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigMavCommandView(QWidget *parent = nullptr);
    ConfigMavCommandView(ConfigMavCommandViewModel *viewModel,
                         QWidget *parent = nullptr);

    ConfigMavCommandViewModel *viewModel() const;
    QTableView *commandTable() const;

private slots:
    void addCommand();
    void removeCommand();

private:
    ConfigMavCommandViewModel *m_viewModel = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_statusLabel = nullptr;
};

BackstagePage configMavCommandBackstagePage();

#endif // CONFIGMAVCOMMANDVIEW_H
