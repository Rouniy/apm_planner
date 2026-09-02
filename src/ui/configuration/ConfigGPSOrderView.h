#ifndef CONFIGGPSORDERVIEW_H
#define CONFIGGPSORDERVIEW_H

#include "ConfigGPSOrderViewModel.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QPushButton;
class QPaintEvent;
class QTableWidget;
class QVBoxLayout;

class ConfigGPSOrderView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigGPSOrderView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigGPSOrderView() override;

    QSize sizeHint() const override;
    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    ConfigGPSOrderViewModel *viewModel() const { return m_viewModel; }

    static QString ArmedRefreshWarningTitle();
    static QString ArmedRefreshWarningText();

protected:
    void paintEvent(QPaintEvent *event) override;

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

    void rebuildRows();
    void rebuildFields();
    void syncField(const QString &name);
    void syncState();
    void requestRefresh();
    void clearFields();

    ConfigGPSOrderViewModel *m_viewModel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QWidget *m_fieldsWidget = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QHash<QString, FieldWidgets *> m_fields;
    bool m_armed = false;
};

#endif // CONFIGGPSORDERVIEW_H
