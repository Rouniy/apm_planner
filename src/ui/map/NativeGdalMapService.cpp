#include "NativeGdalMapService.h"

#include "ui/configuration/ElevationSourceService.h"

#include <QBuffer>
#include <QImage>
#include <QSemaphore>

namespace {
QSemaphore &globalRenderGate()
{
    // All map widgets share one GDAL backend. Keep the expensive native
    // render limit process-wide even if a test or future view creates another
    // provider adapter.
    static QSemaphore gate(2);
    return gate;
}

class SemaphoreRelease final
{
public:
    explicit SemaphoreRelease(QSemaphore *semaphore)
        : m_semaphore(semaphore)
    {
    }
    ~SemaphoreRelease() { m_semaphore->release(); }

private:
    QSemaphore *m_semaphore = nullptr;
};
}

NativeGdalMapService::NativeGdalMapService(
    ElevationSourceService *elevationService, QObject *parent)
    : QObject(parent),
      m_elevationService(elevationService
          ? elevationService : ElevationSourceService::instance())
{
    core::OPMaps::Instance()->setLocalTileProvider(this);
}

NativeGdalMapService::~NativeGdalMapService()
{
    core::OPMaps::Instance()->setLocalTileProvider(nullptr);
    core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
}

QByteArray NativeGdalMapService::tileImage(
    core::MapType::Types type, const core::Point &position, int zoom)
{
    if (type != core::MapType::GDALCustom || !m_elevationService) {
        return {};
    }
    QSemaphore &gate = globalRenderGate();
    gate.acquire();
    const SemaphoreRelease release(&gate);
    const QByteArray rendered = m_elevationService->renderRasterTile(
        position.X(), position.Y(), zoom);
    return rendered.isEmpty() ? transparentTile() : rendered;
}

QByteArray NativeGdalMapService::transparentTile()
{
    static const QByteArray png = []() {
        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (buffer.open(QIODevice::WriteOnly)) {
            image.save(&buffer, "PNG");
        }
        return bytes;
    }();
    return png;
}
