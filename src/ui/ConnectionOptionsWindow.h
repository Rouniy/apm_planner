#ifndef CONNECTIONOPTIONSWINDOW_H
#define CONNECTIONOPTIONSWINDOW_H

#include <QDialog>
#include <QStringList>

class ConnectionOptionsViewModel;
class QComboBox;
class QPushButton;

class ConnectionOptionsWindow final : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionOptionsWindow(QWidget *parent = nullptr);
    ConnectionOptionsWindow(const QStringList &serialPorts,
                            QWidget *parent = nullptr);

    static ConnectionOptionsWindow *OpenWindow(QWidget *owner = nullptr);
    ConnectionOptionsViewModel *viewModel() const;

signals:
    void connectRequested(const QString &connection, int baud);

private slots:
    void BUT_connect_Click();

private:
    explicit ConnectionOptionsWindow(ConnectionOptionsViewModel *viewModel,
                                     QWidget *parent);
    void buildUi();
    void bindViewModel();

    ConnectionOptionsViewModel *m_viewModel = nullptr;
    QComboBox *m_serialPortCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
};

#endif
