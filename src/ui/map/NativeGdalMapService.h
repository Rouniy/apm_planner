#ifndef NATIVEGDALMAPSERVICE_H
#define NATIVEGDALMAPSERVICE_H

#include "opmaps.h"

#include <QObject>

class ElevationSourceService;

class NativeGdalMapService final : public QObject,
                                   public core::LocalTileProvider
{
    Q_OBJECT

public:
    explicit NativeGdalMapService(
        ElevationSourceService *elevationService = nullptr,
        QObject *parent = nullptr);
    ~NativeGdalMapService() override;

    QByteArray tileImage(core::MapType::Types type,
                         const core::Point &position,
                         int zoom) override;

private:
    static QByteArray transparentTile();

    ElevationSourceService *m_elevationService = nullptr;
};

#endif
