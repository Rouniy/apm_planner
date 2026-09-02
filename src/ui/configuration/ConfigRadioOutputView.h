#ifndef CONFIGRADIOOUTPUTVIEW_H
#define CONFIGRADIOOUTPUTVIEW_H

#include "ConfigRadioOutputViewModel.h"

#include <QHash>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QScrollArea;
class QSpinBox;
class QTimer;
class QVBoxLayout;

class ConfigRadioOutputView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigRadioOutputView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigRadioOutputView() override;

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    ConfigRadioOutputViewModel *viewModel() const { return m_viewModel; }
    bool telemetryTimerActive() const;

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);
    void servoOutputChanged(int oneBasedChannel, int pwm);

signals:
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct RowWidgets;

    void rebuildRows();
    void syncRow(int oneBasedChannel);
    void clearRows();

    ConfigRadioOutputViewModel *m_viewModel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QWidget *m_rowsWidget = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QTimer *m_telemetryTimer = nullptr;
    QHash<int, RowWidgets *> m_rows;
};

#endif
