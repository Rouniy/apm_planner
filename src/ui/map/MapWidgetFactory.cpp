#include "MapWidgetFactory.h"

#include "AbstractMapWidget.h"
#include "cache.h"
#include "pureimagecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QWidget>

MapWidgetFactory::MapWidgetFactory(QSettings *settings, QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    m_settings->setFallbacksEnabled(false);
    const QString storedBackend = m_settings->value(
        QString::fromLatin1(SettingsKey),
        QString::fromLatin1(OPMapBackendId)).toString().trimmed();
    m_requestedBackend = canonicalBackend(storedBackend);
    if (m_requestedBackend != storedBackend) {
        saveRequestedBackend();
    }
}

MapWidgetFactory::~MapWidgetFactory() = default;

MapWidgetFactory *MapWidgetFactory::instance()
{
    static MapWidgetFactory *factory = new MapWidgetFactory(
        nullptr, QCoreApplication::instance());
    return factory;
}

bool MapWidgetFactory::RegisterBackend(
    const QString &id, const QString &displayName,
    const QString &cacheContract, const Creator &creator)
{
    if (id.trimmed().isEmpty()) {
        return false;
    }
    const QString normalizedId = canonicalBackend(id);
    if (normalizedId.isEmpty() || displayName.trimmed().isEmpty()
        || cacheContract != QString::fromLatin1(SharedCacheContract)
        || !creator) {
        return false;
    }

    const bool added = !m_backends.contains(normalizedId);
    Registration registration;
    registration.displayName = displayName.trimmed();
    registration.creator = creator;
    m_backends.insert(normalizedId, registration);
    if (added) {
        m_registrationOrder.append(normalizedId);
        emit AvailableBackendsChanged();
    }
    return true;
}

QList<MapWidgetBackendInfo> MapWidgetFactory::AvailableBackends() const
{
    QList<MapWidgetBackendInfo> result;
    for (const QString &id : m_registrationOrder) {
        const auto registration = m_backends.constFind(id);
        if (registration != m_backends.constEnd()) {
            result.append({id, registration->displayName});
        }
    }
    return result;
}

QString MapWidgetFactory::RequestedBackend() const
{
    return m_requestedBackend;
}

QString MapWidgetFactory::CurrentBackend() const
{
    return m_effectiveBackend.isEmpty()
        ? normalizedBackend(m_requestedBackend)
        : m_effectiveBackend;
}

QString MapWidgetFactory::LastStatus() const
{
    return m_lastStatus;
}

QString MapWidgetFactory::SharedCacheRoot() const
{
    return core::Cache::Instance()->ImageCache.sharedCacheRootPath();
}

AbstractMapWidget *MapWidgetFactory::CreateMapWidget(
    MapWidgetRole role, QWidget *widgetParent, QObject *owner)
{
    if (m_effectiveBackend.isEmpty()) {
        m_effectiveBackend = normalizedBackend(m_requestedBackend);
    }
    const QString id = m_effectiveBackend;
    const auto registration = m_backends.constFind(id);
    if (registration == m_backends.constEnd() || !registration->creator) {
        return nullptr;
    }

    if (id != m_requestedBackend) {
        m_lastStatus = tr(
            "Map widget backend '%1' is unavailable; using %2.")
            .arg(m_requestedBackend, registration->displayName);
        emit StatusMessage(m_lastStatus);
    }
    QObject *backendOwner = owner ? owner : widgetParent;
    const QString cacheRoot = SharedCacheRoot();
    const auto isValidBackend = [&cacheRoot](
        AbstractMapWidget *backend, const QString &expectedId) {
        return backend && backend->Widget()
            && backend->BackendId() == expectedId
            && QDir::cleanPath(backend->SharedCacheRoot())
                == QDir::cleanPath(cacheRoot);
    };
    AbstractMapWidget *backend = registration->creator(
        role, cacheRoot, widgetParent, backendOwner);
    if (isValidBackend(backend, id)) {
        return backend;
    }
    delete backend;

    const QString opMapId = QString::fromLatin1(OPMapBackendId);
    const auto opMap = m_backends.constFind(opMapId);
    if (id != opMapId && opMap != m_backends.constEnd()
        && opMap->creator) {
        m_lastStatus = tr(
            "Map widget backend '%1' could not be created; using %2.")
            .arg(registration->displayName, opMap->displayName);
        emit StatusMessage(m_lastStatus);
        AbstractMapWidget *fallback = opMap->creator(
            role, cacheRoot, widgetParent, backendOwner);
        if (isValidBackend(fallback, opMapId)) {
            m_effectiveBackend = opMapId;
        } else {
            delete fallback;
            fallback = nullptr;
        }
        return fallback;
    }
    m_lastStatus = tr("Map widget backend '%1' could not be created.")
        .arg(registration->displayName);
    emit StatusMessage(m_lastStatus);
    return nullptr;
}

bool MapWidgetFactory::SetBackend(const QString &id)
{
    if (id.trimmed().isEmpty()) {
        m_lastStatus = tr("Map widget backend is not available in this build.");
        emit StatusMessage(m_lastStatus);
        return false;
    }
    const QString requested = canonicalBackend(id);
    const auto registration = m_backends.constFind(requested);
    if (registration == m_backends.constEnd()) {
        m_lastStatus = tr("Map widget backend is not available in this build.");
        emit StatusMessage(m_lastStatus);
        return false;
    }

    const QString previous = m_requestedBackend;
    m_requestedBackend = requested;
    m_lastStatus.clear();
    saveRequestedBackend();
    if (previous != m_requestedBackend) {
        emit BackendChanged(m_requestedBackend);
    }
    return true;
}

QString MapWidgetFactory::normalizedBackend(const QString &requested) const
{
    const QString canonical = canonicalBackend(requested);
    if (m_backends.contains(canonical)) {
        return canonical;
    }
    const QString opMap = QString::fromLatin1(OPMapBackendId);
    if (m_backends.contains(opMap)) {
        return opMap;
    }
    return m_registrationOrder.isEmpty()
        ? QString() : m_registrationOrder.first();
}

QString MapWidgetFactory::canonicalBackend(const QString &requested)
{
    const QString trimmed = requested.trimmed();
    if (trimmed.isEmpty()
        || trimmed.compare(QStringLiteral("OPMap"),
                           Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("QGCMapWidget"),
                           Qt::CaseInsensitive) == 0) {
        return QString::fromLatin1(OPMapBackendId);
    }
    if (trimmed.compare(QStringLiteral("QGC"),
                        Qt::CaseInsensitive) == 0) {
        return QString::fromLatin1(QGroundControlBackendId);
    }
    return trimmed;
}

void MapWidgetFactory::saveRequestedBackend()
{
    m_settings->setValue(QString::fromLatin1(SettingsKey),
                         m_requestedBackend);
    m_settings->sync();
}
