#ifndef CONFIGPARACHUTEVIEW_H
#define CONFIGPARACHUTEVIEW_H

#include "ConfigParachuteViewModel.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

class ConfigParachuteView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigParachuteView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigParachuteView() override;

    QSize sizeHint() const override;
    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    ConfigParachuteViewModel *viewModel() const { return m_viewModel; }

    static QString ArmedRefreshWarningTitle();
    static QString ArmedRefreshWarningText();

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
    void syncServo();
    void requestRefresh();
    void clearFields();

    ConfigParachuteViewModel *m_viewModel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QWidget *m_fieldsWidget = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QComboBox *m_servoOptions = nullptr;
    QLabel *m_servoStatus = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QHash<QString, FieldWidgets *> m_fields;
    bool m_armed = false;
};

#endif
