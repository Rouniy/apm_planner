#ifndef CONFIGESCCALIBRATIONVIEW_H
#define CONFIGESCCALIBRATIONVIEW_H

#include "ConfigESCCalibrationViewModel.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

class ConfigESCCalibrationView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigESCCalibrationView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigESCCalibrationView() override;

    static QString CalibrationConfirmationTitle();
    static QString CalibrationConfirmationText();
    static QString ArmedRefreshConfirmationText();

    QSize sizeHint() const override;
    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    ConfigESCCalibrationViewModel *viewModel() const { return m_viewModel; }

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);
    void refreshRequested(int componentId);

private:
    struct FieldWidgets;

    void rebuildFields();
    void syncField(const QString &name);
    void syncState();
    void clearFields();
    void calibrateClicked();
    void refreshClicked();

    ConfigESCCalibrationViewModel *m_viewModel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QWidget *m_fieldsWidget = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QPushButton *m_calibrateButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QHash<QString, FieldWidgets *> m_fields;
};

#endif
