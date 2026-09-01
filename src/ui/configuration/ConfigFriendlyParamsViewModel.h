#ifndef CONFIGFRIENDLYPARAMSVIEWMODEL_H
#define CONFIGFRIENDLYPARAMSVIEWMODEL_H

#include "ParamField.h"

#include <QList>
#include <QObject>
#include <QSet>

class ConfigFriendlyParamsViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigFriendlyParamsViewModel(bool advanced,
                                           QObject *parent = nullptr);

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void updateParameter(int componentId, const QString &name,
                         const QVariant &value);
    void setSelectedComponent(int componentId);
    void setSearch(const QString &search);
    void setFavorite(int componentId, const QString &name, bool favorite);

    bool advanced() const { return m_advanced; }
    int selectedComponent() const { return m_selectedComponent; }
    QList<int> availableComponents() const { return m_components; }
    QList<ParamField> fields() const { return m_fields; }
    QList<ParamField> visibleFields() const;

signals:
    void componentsChanged();
    void structureChanged();
    void layoutChanged();
    void parameterValueChanged(int componentId, const QString &name,
                               const QVariant &value);

private:
    void rebuildFields();
    QString favoriteKey(int componentId, const QString &name) const;
    void loadFavorites();
    void persistFavorites() const;

    ParameterMetaDataCatalog m_catalog;
    QList<ConfigFriendlyParameterValue> m_parameters;
    QList<ParamField> m_fields;
    QList<int> m_components;
    QSet<QString> m_favorites;
    QString m_search;
    QString m_favoriteSettingsKey;
    int m_selectedComponent = 0;
    bool m_advanced = false;
};

#endif
