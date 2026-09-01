#ifndef MAPTILESOURCEFACTORY_H
#define MAPTILESOURCEFACTORY_H

#include "maptype.h"

#include <QObject>
#include <QString>

#include <memory>

class ElevationSourceService;
class QSettings;

class MapTileSourceFactory : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *SettingsKey = "MapType";
    static constexpr const char *LegacySettingsKey =
        "QGC_MAPWIDGET/MAP_TYPE";
    static constexpr const char *GdalUnavailableStatus =
        "Configure GDAL Custom before selecting it.";

    explicit MapTileSourceFactory(
        ElevationSourceService *elevationService = nullptr,
        QSettings *settings = nullptr,
        QObject *parent = nullptr);
    ~MapTileSourceFactory() override;

    static MapTileSourceFactory *instance();

    core::MapType::Types CurrentMapType() const;
    QString LastStatus() const;
    bool IsGdalConfigured() const;

    static bool IsKnownMapType(core::MapType::Types type);
    static QString SettingsName(core::MapType::Types type);
    static core::MapType::Types MapTypeFromSettingsName(
        const QString &name);
    static core::MapType::Types NormalizeMapType(
                                           core::MapType::Types requested,
                                           bool gdalConfigured,
                                           QString *status = nullptr);

public slots:
    bool SetMapType(core::MapType::Types type);
    void RefreshMapType();

signals:
    void MapTypeChanged(core::MapType::Types type);
    void MapRefreshRequested();
    void StatusMessage(const QString &message);

private:
    void loadSettings();
    void saveCurrentMapType();

    ElevationSourceService *m_elevationService = nullptr;
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    core::MapType::Types m_currentMapType = core::MapType::GoogleSatellite;
    QString m_lastStatus;
};

#endif
