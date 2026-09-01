#ifndef CONNECTIONOPTIONSWINDOW_H
#define CONNECTIONOPTIONSWINDOW_H

#include <QDialog>

class ConnectionOptionsViewModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSettings;
class QShowEvent;
class QSpinBox;

class ConnectionOptionsWindow final : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionOptionsWindow(QWidget *parent = nullptr);
    ConnectionOptionsWindow(QSettings *settings, QWidget *parent = nullptr);

    static ConnectionOptionsWindow *OpenWindow(QWidget *owner = nullptr,
                                               QSettings *settings = nullptr);
    ConnectionOptionsViewModel *viewModel() const;

signals:
    void settingsApplied(int baud, bool sendHeartbeat, int gcsSystemId);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void OnApply();
    void OnClose();

private:
    void buildUi();
    void bindViewModel();

    ConnectionOptionsViewModel *m_viewModel = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QSpinBox *m_gcsSystemId = nullptr;
    QCheckBox *m_sendHeartbeat = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    bool m_centered = false;
};

#endif
