#include "ConfigPlannerAdvViewModel.h"

#include <QMetaType>
#include <QSettings>

#include <algorithm>

ConfigPlannerAdvViewModel::ConfigPlannerAdvViewModel(QObject *parent)
    : QObject(parent)
{
    Activate();
}

QList<PlannerAdvancedSettingRow> ConfigPlannerAdvViewModel::Params() const
{
    return m_params;
}

QString ConfigPlannerAdvViewModel::DisplayValue(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return QString();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = value.typeId();
#else
    const int typeId = value.userType();
#endif
    if (typeId == QMetaType::QByteArray) {
        return tr("<binary data: %1 bytes>")
            .arg(value.toByteArray().size());
    }
    if (typeId == QMetaType::QStringList) {
        return value.toStringList().join(QStringLiteral(", "));
    }
    return value.toString();
}

void ConfigPlannerAdvViewModel::Activate()
{
    QSettings settings;
    // Mission Planner's Settings.Instance is one concrete per-user profile;
    // organization/system fallback keys are not part of config.xml.
    settings.setFallbacksEnabled(false);
    settings.sync();
    const QStringList keys = settings.allKeys();
    QList<PlannerAdvancedSettingRow> params;
    params.reserve(keys.size());
    for (const QString &key : keys) {
        params.append({key, DisplayValue(settings.value(key))});
    }
    std::stable_sort(
        params.begin(), params.end(),
        [](const PlannerAdvancedSettingRow &left,
           const PlannerAdvancedSettingRow &right) {
        const int insensitive = QString::compare(
            left.Name, right.Name, Qt::CaseInsensitive);
        return insensitive == 0
            ? QString::compare(left.Name, right.Name, Qt::CaseSensitive) < 0
            : insensitive < 0;
    });
    m_params = params;
    emit paramsChanged();
}
