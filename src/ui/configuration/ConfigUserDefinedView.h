#ifndef CONFIGUSERDEFINEDVIEW_H
#define CONFIGUSERDEFINEDVIEW_H

#include "ConfigUserDefinedViewModel.h"
#include "ParamField.h"

#include <QWidget>

class ConfigFriendlyParamsView;
class ParameterMetaDataCatalog;

class ConfigUserDefinedView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigUserDefinedView(
        const ParameterMetaDataCatalog &catalog,
        QWidget *parent = nullptr,
        bool enforceMetadataRanges = true);

    ConfigUserDefinedViewModel *viewModel() const { return m_viewModel; }
    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void ApplyOptions(const QString &raw);
    int visibleParameterCount() const;

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QVariant &attemptedValue,
                              const QString &reason);

signals:
    void refreshRequested(int componentId);
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    void showModifyDialog();
    void applyConfiguredOptions();

    ConfigUserDefinedViewModel *m_viewModel = nullptr;
    ConfigFriendlyParamsView *m_editor = nullptr;
};

#endif
