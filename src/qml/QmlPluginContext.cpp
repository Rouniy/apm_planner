#include "QmlPluginContext.h"

#include <QSettings>

QmlPluginContext::QmlPluginContext(const QString &id, const QString &name,
                                   const QString &version, const QString &path,
                                   QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_name(name)
    , m_version(version)
    , m_path(path)
{
}

QVariant QmlPluginContext::setting(const QString &key,
                                   const QVariant &defaultValue) const
{
    return QSettings().value(settingsKey(key), defaultValue);
}

void QmlPluginContext::setSetting(const QString &key, const QVariant &value)
{
    QSettings().setValue(settingsKey(key), value);
}

void QmlPluginContext::removeSetting(const QString &key)
{
    QSettings().remove(settingsKey(key));
}

QString QmlPluginContext::settingsKey(const QString &key) const
{
    QString cleanKey = key;
    while (cleanKey.startsWith(QLatin1Char('/'))) {
        cleanKey.remove(0, 1);
    }
    return QStringLiteral("QmlPlugins/%1/settings/%2").arg(m_id, cleanKey);
}
