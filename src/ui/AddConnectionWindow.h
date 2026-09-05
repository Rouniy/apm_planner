#ifndef ADDCONNECTIONWINDOW_H
#define ADDCONNECTIONWINDOW_H

#include <QDialog>
#include <QStringList>

class AddConnectionViewModel;
class QComboBox;
class QPushButton;

class AddConnectionWindow final : public QDialog
{
    Q_OBJECT
public:
    explicit AddConnectionWindow(QWidget *parent = nullptr);
    AddConnectionWindow(const QStringList &serialPorts,
                        QWidget *parent = nullptr);

    static AddConnectionWindow *OpenWindow(QWidget *owner = nullptr);
    AddConnectionViewModel *viewModel() const;

signals:
    void connectRequested(const QString &connection, int baud);

private slots:
    void BUT_connect_Click();

private:
    explicit AddConnectionWindow(AddConnectionViewModel *viewModel,
                                 QWidget *parent);
    void buildUi();
    void bindViewModel();

    AddConnectionViewModel *m_viewModel = nullptr;
    QComboBox *m_serialPortCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
};

#endif
