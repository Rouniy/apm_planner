#ifndef CONFIGINITIALPARAMSVIEW_H
#define CONFIGINITIALPARAMSVIEW_H

#include "ConfigInitialParamsViewModel.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class ConfigInitialParamsView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigInitialParamsView(QWidget *parent = nullptr);

    ConfigInitialParamsViewModel *viewModel() const { return m_viewModel; }
    void setVehicleContext(bool plane, int firmwareMajor);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    bool WriteToFc(const QStringList &availableParameters, bool connected);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void writeToFcRequested();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    void syncInputs();
    void rebuildResults();
    void syncStatus();
    void syncActions();

    ConfigInitialParamsViewModel *m_viewModel = nullptr;
    QLineEdit *m_propSize = nullptr;
    QLineEdit *m_cellCount = nullptr;
    QLineEdit *m_cellMax = nullptr;
    QLineEdit *m_cellMin = nullptr;
    QComboBox *m_batteryType = nullptr;
    QCheckBox *m_tMotor = nullptr;
    QCheckBox *m_suggested = nullptr;
    QPushButton *m_calculateButton = nullptr;
    QPushButton *m_writeButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableWidget *m_resultsTable = nullptr;
    bool m_syncing = false;
};

#endif
