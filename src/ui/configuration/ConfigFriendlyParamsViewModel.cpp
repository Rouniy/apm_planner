#include "ConfigFriendlyParamsViewModel.h"

#include <QSettings>

#include <algorithm>

namespace {
bool nameLessThan(const QString &left, const QString &right)
{
    return QString::compare(left, right, Qt::CaseInsensitive) < 0;
}
}

ConfigFriendlyParamsViewModel::ConfigFriendlyParamsViewModel(
    bool advanced, QObject *parent)
    : QObject(parent),
      m_favoriteSettingsKey(advanced ? QStringLiteral("fav_params_adv")
                                     : QStringLiteral("fav_params_std")),
      m_advanced(advanced)
{
    loadFavorites();
}

void ConfigFriendlyParamsViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildFields();
}

void ConfigFriendlyParamsViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_parameters = parameters;
    QList<int> components;
    for (const ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId > 0
            && !components.contains(parameter.componentId)) {
            components.append(parameter.componentId);
        }
    }
    std::sort(components.begin(), components.end());
    const bool componentsWereChanged = components != m_components;
    m_components = components;

    if (!m_components.contains(m_selectedComponent)) {
        if (m_components.contains(preferredComponent)) {
            m_selectedComponent = preferredComponent;
        } else if (m_components.contains(1)) {
            m_selectedComponent = 1;
        } else {
            m_selectedComponent = m_components.value(0, 0);
        }
    }
    if (componentsWereChanged) {
        emit componentsChanged();
    }
    rebuildFields();
}

void ConfigFriendlyParamsViewModel::updateParameter(
    int componentId, const QString &name, const QVariant &value)
{
    const QString normalized = name.trimmed().toUpper();
    for (ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId == componentId
            && parameter.name.compare(normalized, Qt::CaseInsensitive) == 0) {
            parameter.value = value;
            break;
        }
    }
    for (ParamField &field : m_fields) {
        if (field.componentId == componentId
            && field.name.compare(normalized, Qt::CaseInsensitive) == 0) {
            field.value = value;
            emit parameterValueChanged(componentId, field.name, value);
            return;
        }
    }
}

void ConfigFriendlyParamsViewModel::setSelectedComponent(int componentId)
{
    if (componentId == m_selectedComponent
        || !m_components.contains(componentId)) {
        return;
    }
    m_selectedComponent = componentId;
    rebuildFields();
}

void ConfigFriendlyParamsViewModel::setSearch(const QString &search)
{
    const QString normalized = search.trimmed();
    if (normalized == m_search) {
        return;
    }
    m_search = normalized;
    emit layoutChanged();
}

void ConfigFriendlyParamsViewModel::setFavorite(
    int componentId, const QString &name, bool favorite)
{
    const QString key = favoriteKey(componentId, name);
    if (favorite) {
        m_favorites.insert(key);
    } else {
        m_favorites.remove(key);
    }
    for (ParamField &field : m_fields) {
        if (field.componentId == componentId
            && field.name.compare(name, Qt::CaseInsensitive) == 0) {
            field.favorite = favorite;
        }
    }
    persistFavorites();
    emit layoutChanged();
}

QList<ParamField> ConfigFriendlyParamsViewModel::visibleFields() const
{
    QList<ParamField> result;
    for (const ParamField &field : m_fields) {
        if (m_search.isEmpty()
            || field.name.contains(m_search, Qt::CaseInsensitive)
            || field.label.contains(m_search, Qt::CaseInsensitive)) {
            result.append(field);
        }
    }
    std::sort(result.begin(), result.end(), [](const ParamField &left,
                                               const ParamField &right) {
        if (left.favorite != right.favorite) {
            return left.favorite;
        }
        return nameLessThan(left.name, right.name);
    });
    return result;
}

void ConfigFriendlyParamsViewModel::rebuildFields()
{
    m_fields.clear();
    if (!m_catalog.isValid() || m_selectedComponent <= 0) {
        emit structureChanged();
        return;
    }

    for (const ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId != m_selectedComponent
            || !m_catalog.contains(parameter.name)) {
            continue;
        }
        const ParameterMetaData metadata = m_catalog.value(parameter.name);
        // Mission Planner deliberately hides parameters without DisplayName.
        if (metadata.title.isEmpty()) {
            continue;
        }
        const bool isAdvanced =
            metadata.userLevel == ParameterUserLevel::Advanced;
        if (m_advanced != isAdvanced) {
            continue;
        }

        ParamField field;
        field.componentId = parameter.componentId;
        field.name = metadata.name;
        field.label = metadata.title;
        field.units = metadata.units;
        field.description = metadata.description;
        field.value = parameter.value;
        field.minimum = metadata.minimum;
        field.maximum = metadata.maximum;
        field.increment = metadata.increment;
        field.hasRange = metadata.hasRange;
        field.readOnly = metadata.readOnly;
        field.favorite = m_favorites.contains(
            favoriteKey(field.componentId, field.name));

        // This ordering matches Mission Planner 10: bitmask wins when a PDEF
        // also happens to provide ordinary enum values.
        if (metadata.isBitmask()) {
            field.editorKind = ParamField::EditorKind::Bitmask;
            for (const auto &entry : metadata.bitmaskValues) {
                field.bitOptions.append({entry.first, entry.second});
            }
        } else if (metadata.isEnum()) {
            field.editorKind = ParamField::EditorKind::Combo;
            for (const ParameterMetaDataOption &entry : metadata.values) {
                field.options.append({entry.value, entry.label});
            }
        }
        m_fields.append(field);
    }
    std::sort(m_fields.begin(), m_fields.end(), [](const ParamField &left,
                                                   const ParamField &right) {
        return nameLessThan(left.name, right.name);
    });
    emit structureChanged();
}

QString ConfigFriendlyParamsViewModel::favoriteKey(
    int componentId, const QString &name) const
{
    return QString::number(componentId) + QLatin1Char(':')
        + name.trimmed().toUpper();
}

void ConfigFriendlyParamsViewModel::loadFavorites()
{
    QSettings settings;
    const QStringList stored = settings.value(
        m_favoriteSettingsKey).toStringList();
    for (const QString &entry : stored) {
        const QString trimmed = entry.trimmed();
        if (trimmed.contains(QLatin1Char(':'))) {
            m_favorites.insert(trimmed.toUpper());
        } else if (!trimmed.isEmpty()) {
            // MP10 stored name-only favorites. Migrate them to the primary
            // component without accidentally pinning a peripheral collision.
            m_favorites.insert(favoriteKey(1, trimmed));
        }
    }
}

void ConfigFriendlyParamsViewModel::persistFavorites() const
{
    QStringList stored;
    for (const QString &favorite : m_favorites) {
        const QString primaryPrefix = QStringLiteral("1:");
        stored.append(favorite.startsWith(primaryPrefix)
                          ? favorite.mid(primaryPrefix.size())
                          : favorite);
    }
    std::sort(stored.begin(), stored.end(), nameLessThan);
    QSettings settings;
    settings.setValue(m_favoriteSettingsKey, stored);
}
