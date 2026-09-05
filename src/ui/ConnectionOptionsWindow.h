#ifndef CONNECTIONOPTIONSWINDOW_H
#define CONNECTIONOPTIONSWINDOW_H

#include <QDialog>

class ConnectionOptionsViewModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

class ConnectionOptionsWindow final : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionOptionsWindow(QWidget *parent = nullptr);

    static ConnectionOptionsWindow *OpenWindow(QWidget *owner = nullptr);
    ConnectionOptionsViewModel *viewModel() const;

signals:
    void settingsApplied(int baud, bool sendGcsHeartbeat, int gcsSysid);

private:
    explicit ConnectionOptionsWindow(ConnectionOptionsViewModel *viewModel,
                                     QWidget *parent);
    void buildUi();
    void bindViewModel();

    ConnectionOptionsViewModel *m_viewModel = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QSpinBox *m_gcsSysidSpin = nullptr;
    QCheckBox *m_heartbeatCheck = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};

#endif
