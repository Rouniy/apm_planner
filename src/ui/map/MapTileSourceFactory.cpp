#include "MapTileSourceFactory.h"

#include "ui/configuration/ElevationSourceService.h"
#include "opmaps.h"

#include <QDir>
#include <QCoreApplication>
#include <QSettings>

MapTileSourceFactory::MapTileSourceFactory(
    ElevationSourceService *elevationService,
    QSettings *settings,
    QObject *parent)
    : QObject(parent),
      m_elevationService(elevationService
          ? elevationService : ElevationSourceService::instance())
{
    qRegisterMetaType<core::MapType::Types>("core::MapType::Types");
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    m_settings->setFallbacksEnabled(false);
    loadSettings();
    connect(m_elevationService,
            &ElevationSourceService::nativeRastersChanged,
            this, &MapTileSourceFactory::RefreshMapType);
}

MapTileSourceFactory::~MapTileSourceFactory() = default;

MapTileSourceFactory *MapTileSourceFactory::instance()
{
    static MapTileSourceFactory *factory =
        new MapTileSourceFactory(nullptr, nullptr,
                                 QCoreApplication::instance());
    return factory;
}

core::MapType::Types MapTileSourceFactory::CurrentMapType() const
{
    return m_currentMapType;
}

QString MapTileSourceFactory::LastStatus() const
{
    return m_lastStatus;
}

bool MapTileSourceFactory::IsGdalConfigured() const
{
    if (!m_elevationService
        || !m_elevationService->isNativeGdalAvailable()) {
        return false;
    }
    const QString directory =
        ElevationSourceService::savedDirectory(m_settings);
    return !directory.isEmpty() && QDir(directory).exists();
}

bool MapTileSourceFactory::IsKnownMapType(core::MapType::Types type)
{
    return type == core::MapType::GDALCustom
        || !core::MapType::StrByType(type).isEmpty();
}

QString MapTileSourceFactory::SettingsName(core::MapType::Types type)
{
    switch (type) {
    case core::MapType::GoogleSatellite:
        return QStringLiteral("GoogleSatelliteMap");
    case core::MapType::GoogleHybrid:
        return QStringLiteral("GoogleHybridMap");
    case core::MapType::BingSatellite:
        return QStringLiteral("BingSatelliteMap");
    case core::MapType::ArcGIS_Satellite:
        return QStringLiteral("EsriWorldImagery");
    default:
        return core::MapType::StrByType(type);
    }
}

core::MapType::Types MapTileSourceFactory::MapTypeFromSettingsName(
    const QString &name)
{
    if (name == QStringLiteral("GoogleSatelliteMap")) {
        return core::MapType::GoogleSatellite;
    }
    if (name == QStringLiteral("GoogleHybridMap")) {
        return core::MapType::GoogleHybrid;
    }
    if (name == QStringLiteral("BingSatelliteMap")) {
        return core::MapType::BingSatellite;
    }
    if (name == QStringLiteral("EsriWorldImagery")) {
        return core::MapType::ArcGIS_Satellite;
    }
    return core::MapType::TypeByStr(name);
}

core::MapType::Types MapTileSourceFactory::NormalizeMapType(
    core::MapType::Types requested, bool gdalConfigured, QString *status)
{
    if (status) {
        status->clear();
    }
    if (!IsKnownMapType(requested)) {
        return core::MapType::GoogleSatellite;
    }
    if (requested == core::MapType::GDALCustom && !gdalConfigured) {
        if (status) {
            *status = QString::fromLatin1(GdalUnavailableStatus);
        }
        return core::MapType::GoogleSatellite;
    }
    return requested;
}

bool MapTileSourceFactory::SetMapType(core::MapType::Types type)
{
    QString status;
    const core::MapType::Types normalized = NormalizeMapType(
        type, type != core::MapType::GDALCustom || IsGdalConfigured(),
        &status);
    m_lastStatus = status;
    const bool changed = normalized != m_currentMapType;
    m_currentMapType = normalized;
    saveCurrentMapType();
    if (changed) {
        emit MapTypeChanged(m_currentMapType);
    }
    if (!status.isEmpty()) {
        emit StatusMessage(status);
    }
    return normalized == type;
}

void MapTileSourceFactory::RefreshMapType()
{
    core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
    if (m_currentMapType == core::MapType::GDALCustom) {
        emit MapRefreshRequested();
    }
}

void MapTileSourceFactory::loadSettings()
{
    if (!m_settings->contains(QString::fromLatin1(SettingsKey))) {
        const QVariant legacy = m_settings->value(
            QString::fromLatin1(LegacySettingsKey));
        if (legacy.isValid()) {
            const core::MapType::Types oldType =
                static_cast<core::MapType::Types>(legacy.toInt());
            if (IsKnownMapType(oldType)) {
                m_currentMapType = NormalizeMapType(
                    oldType,
                    oldType != core::MapType::GDALCustom
                        || IsGdalConfigured(),
                    &m_lastStatus);
                saveCurrentMapType();
            } else {
                saveCurrentMapType();
            }
            m_settings->remove(QString::fromLatin1(LegacySettingsKey));
            m_settings->sync();
            return;
        }
        saveCurrentMapType();
        return;
    }

    const QString stored = m_settings->value(
        QString::fromLatin1(SettingsKey)).toString().trimmed();
    const core::MapType::Types requested = MapTypeFromSettingsName(stored);
    if (!IsKnownMapType(requested)) {
        // Preserve a future provider name written by a newer build. The
        // current process safely uses satellite imagery until the user makes
        // an explicit selection.
        m_currentMapType = core::MapType::GoogleSatellite;
        return;
    }
    m_currentMapType = NormalizeMapType(
        requested,
        requested != core::MapType::GDALCustom || IsGdalConfigured(),
        &m_lastStatus);
    if (m_currentMapType != requested
        || stored != SettingsName(m_currentMapType)) {
        saveCurrentMapType();
    }
}

void MapTileSourceFactory::saveCurrentMapType()
{
    m_settings->setValue(QString::fromLatin1(SettingsKey),
                         SettingsName(m_currentMapType));
    m_settings->sync();
}
