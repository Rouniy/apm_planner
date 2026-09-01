#ifndef CONFIGSERIALVIEW_H
#define CONFIGSERIALVIEW_H

#include "ConfigSerialViewModel.h"

#include <QHash>
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QToolButton;
class QVBoxLayout;

class ConfigSerialView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigSerialView(const ParameterMetaDataCatalog &catalog,
                              QWidget *parent = nullptr);

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    ConfigSerialViewModel *viewModel() const { return m_viewModel; }

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct RowWidgets;

    void rebuildRows();
    void syncRow(const QString &portName);
    void syncWarning();
    void clearRows();

    ConfigSerialViewModel *m_viewModel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QWidget *m_rowsWidget = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_warningLabel = nullptr;
    QHash<QString, RowWidgets *> m_rows;
};

#endif
