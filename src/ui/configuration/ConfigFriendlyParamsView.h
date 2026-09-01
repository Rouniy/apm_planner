#ifndef CONFIGFRIENDLYPARAMSVIEW_H
#define CONFIGFRIENDLYPARAMSVIEW_H

#include "ConfigFriendlyParamsViewModel.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QLabel;
class QTimer;
class QVBoxLayout;

class ConfigFriendlyParamsView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFriendlyParamsView(bool advanced,
                                      const ParameterMetaDataCatalog &catalog,
                                      QWidget *parent = nullptr);
    ~ConfigFriendlyParamsView() override;

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setUnavailableMessage(const QString &message);
    int visibleParameterCount() const;
    ConfigFriendlyParamsViewModel *viewModel() const { return m_viewModel; }

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void refreshRequested(int componentId);
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct Row;

    void rebuildRows();
    void applyLayout();
    void updateComponentSelector();
    void hydrateRow(Row *row, const QVariant &value);
    void submitValue(Row *row, const QVariant &value);
    void setOutOfRange(Row *row, bool outOfRange);
    void updateFavoriteButton(Row *row);
    void updateBitmaskSummary(Row *row);
    void clearRows();
    QString rowKey(int componentId, const QString &name) const;

    ConfigFriendlyParamsViewModel *m_viewModel = nullptr;
    QComboBox *m_componentSelector = nullptr;
    QLineEdit *m_searchBox = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QObject *m_wheelFilter = nullptr;
    QWidget *m_fieldsContent = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QTimer *m_searchDebounce = nullptr;
    QList<Row *> m_rows;
    QHash<QString, Row *> m_rowsByKey;
};

#endif
