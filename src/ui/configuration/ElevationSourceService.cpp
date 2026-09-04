#include "ElevationSourceService.h"
#include "SrtmElevationSource.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QLocale>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace {
constexpr unsigned int kGdalOpenReadOnlyRaster = 0x00u | 0x02u | 0x40u;
constexpr int kGdalRead = 0;
constexpr int kGdalByte = 1;
constexpr int kGdalFloat64 = 7;
constexpr int kGdalBilinear = 1;
constexpr int kTraditionalGisOrder = 0;
constexpr double kEarthRadius = 6378137.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kWebMercatorHalfWorld = kPi * kEarthRadius;
constexpr int kGrayColorInterpretation = 1;
constexpr int kPaletteColorInterpretation = 2;
constexpr int kRedColorInterpretation = 3;
constexpr int kGreenColorInterpretation = 4;
constexpr int kBlueColorInterpretation = 5;
constexpr int kAlphaColorInterpretation = 6;
constexpr int kAllValidMask = 0x01;
constexpr int kAlphaMask = 0x04;

#if defined(_MSC_VER) && !defined(CPL_DISABLE_STDCALL)
#define APM_GDAL_STDCALL __stdcall
#else
#define APM_GDAL_STDCALL
#endif

QString pathError(const QString &message, const QString &path)
{
    return path.isEmpty() ? message : message + QStringLiteral(": ") + path;
}

Qt::CaseSensitivity elevationPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

Qt::CaseSensitivity rasterPathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool lessPath(const QString &left, const QString &right,
              Qt::CaseSensitivity sensitivity)
{
    const int primary = QString::compare(left, right, sensitivity);
    if (primary != 0) {
        return primary < 0;
    }
    return QString::compare(left, right, Qt::CaseSensitive) < 0;
}

bool isGeoTiff(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(QStringLiteral("tif"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("tiff"), Qt::CaseInsensitive) == 0;
}

int elevationFormatOrder(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff")) {
        return 0;
    }
    if (suffix == QStringLiteral("dt2")) {
        return 1;
    }
    if (suffix == QStringLiteral("dt1")) {
        return 2;
    }
    if (suffix == QStringLiteral("dt0")) {
        return 3;
    }
    return 4;
}

bool isSupportedElevation(const QString &path)
{
    return elevationFormatOrder(path) < 4;
}

bool isKnownSidecar(const QString &path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    const QString suffix = info.suffix();
    return name.endsWith(QStringLiteral(".aux.xml"), Qt::CaseInsensitive)
        || suffix.compare(QStringLiteral("prj"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("tfw"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("jgw"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("pgw"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("wld"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("ovr"), Qt::CaseInsensitive) == 0;
}

QString compactNumber(double value, int decimals)
{
    QString text = QLocale().toString(value, 'f', decimals);
    const QChar decimal = QLocale().decimalPoint();
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(decimal)) {
        text.chop(1);
    }
    return text;
}

double mercatorLongitude(double x)
{
    return x / kEarthRadius * 180.0 / kPi;
}

double mercatorLatitude(double y)
{
    return (2.0 * std::atan(std::exp(y / kEarthRadius)) - kPi / 2.0)
        * 180.0 / kPi;
}

struct GdalColorEntry
{
    short red = 0;
    short green = 0;
    short blue = 0;
    short alpha = 0;
};

static_assert(sizeof(GdalColorEntry) == 4 * sizeof(short),
              "GDALColorEntry ABI must contain four short values");

struct MercatorExtent
{
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;

    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
};

bool intersectExtents(const MercatorExtent &left,
                      const MercatorExtent &right,
                      MercatorExtent *intersection)
{
    if (!intersection) {
        return false;
    }
    const MercatorExtent result{
        std::max(left.minX, right.minX),
        std::max(left.minY, right.minY),
        std::min(left.maxX, right.maxX),
        std::min(left.maxY, right.maxY)};
    if (result.minX >= result.maxX || result.minY >= result.maxY) {
        return false;
    }
    *intersection = result;
    return true;
}

unsigned char clampColor(short value)
{
    return static_cast<unsigned char>(std::clamp<int>(value, 0, 255));
}

void blendPixel(unsigned char *destination, unsigned char red,
                unsigned char green, unsigned char blue,
                unsigned char alpha)
{
    const int destinationAlpha = destination[3];
    const int outputAlpha = alpha
        + destinationAlpha * (255 - alpha) / 255;
    if (outputAlpha == 0) {
        return;
    }
    destination[0] = static_cast<unsigned char>((red * alpha
        + destination[0] * destinationAlpha * (255 - alpha) / 255)
        / outputAlpha);
    destination[1] = static_cast<unsigned char>((green * alpha
        + destination[1] * destinationAlpha * (255 - alpha) / 255)
        / outputAlpha);
    destination[2] = static_cast<unsigned char>((blue * alpha
        + destination[2] * destinationAlpha * (255 - alpha) / 255)
        / outputAlpha);
    destination[3] = static_cast<unsigned char>(outputAlpha);
}

class NativeGdalApi final
{
public:
    using GdalAllRegister = void (APM_GDAL_STDCALL *)();
    using GdalVersionInfo = const char *(APM_GDAL_STDCALL *)(const char *);
    using GdalOpenEx = void *(APM_GDAL_STDCALL *)(
        const char *, unsigned int, const char *const *,
        const char *const *, const char *const *);
    using GdalClose = int (APM_GDAL_STDCALL *)(void *);
    using GdalAutoCreateWarpedVrt = void *(APM_GDAL_STDCALL *)(
        void *, const char *, const char *, int, double, const void *);
    using GdalGetInteger = int (APM_GDAL_STDCALL *)(void *);
    using GdalGetHandle = void *(APM_GDAL_STDCALL *)(void *);
    using GdalGetString = const char *(APM_GDAL_STDCALL *)(void *);
    using GdalGetGeoTransform = int (APM_GDAL_STDCALL *)(void *, double *);
    using GdalGetRasterBand = void *(APM_GDAL_STDCALL *)(void *, int);
    using GdalGetColorEntryAsRgb = int (APM_GDAL_STDCALL *)(
        void *, int, GdalColorEntry *);
    using GdalRasterIo = int (APM_GDAL_STDCALL *)(
        void *, int, int, int, int, int, void *, int, int, int, int, int);
    using GdalGetRasterNoDataValue = double (APM_GDAL_STDCALL *)(
        void *, int *);
    using OsrNewSpatialReference = void *(APM_GDAL_STDCALL *)(const char *);
    using OsrImportFromEpsg = int (APM_GDAL_STDCALL *)(void *, int);
    // This newer OSR API is intentionally cdecl in the official header.
    using OsrSetAxisMappingStrategy = void (*)(void *, int);
    using OsrExportToWkt = int (APM_GDAL_STDCALL *)(void *, char **);
    using OsrDestroySpatialReference = void (APM_GDAL_STDCALL *)(void *);
    using VsiFree = void (*)(void *);
    using CplErrorReset = void (APM_GDAL_STDCALL *)();
    using CplGetLastErrorMessage = const char *(APM_GDAL_STDCALL *)();

    static std::shared_ptr<NativeGdalApi> tryLoad(QString *status)
    {
        QStringList errors;
        for (const QString &candidate
             : ElevationSourceService::nativeGdalLibraryCandidates()) {
            std::unique_ptr<QLibrary> library(new QLibrary(candidate));
            library->setLoadHints(QLibrary::ResolveAllSymbolsHint
                                  | QLibrary::PreventUnloadHint);
            if (!library->load()) {
                continue;
            }
            std::shared_ptr<NativeGdalApi> api(
                new NativeGdalApi(std::move(library), candidate));
            QString error;
            if (api->resolveAll(&error)) {
                api->m_allRegister();
                api->m_version = QString::fromUtf8(
                    api->m_versionInfo("RELEASE_NAME"));
                api->m_wgs84Wkt = api->createWkt(4326, &error);
                if (!api->m_wgs84Wkt.isEmpty()) {
                    api->m_webMercatorWkt = api->createWkt(3857, &error);
                }
                if (!api->m_wgs84Wkt.isEmpty()
                    && !api->m_webMercatorWkt.isEmpty()) {
                    if (status) {
                        *status = QStringLiteral("GDAL %1 via %2")
                            .arg(api->m_version, candidate);
                    }
                    // PreventUnloadHint keeps the native module resident. Drop
                    // the QObject wrapper on the thread that created it so the
                    // resolved C function pointers remain thread-neutral.
                    api->m_library.reset();
                    return api;
                }
            }
            errors.append(candidate + QStringLiteral(": ") + error);
        }
        if (status) {
            *status = QStringLiteral(
                "Native GDAL is not installed. Local GeoTIFF/DTED elevation "
                "requires a raster backend in this Qt port; install a current "
                "GDAL runtime or set MISSIONPLANNER_GDAL_LIBRARY to its exact path.");
            if (!errors.isEmpty()) {
                *status += QStringLiteral(" ")
                    + errors.mid(0, 3).join(QStringLiteral("; "));
            }
        }
        return {};
    }

    void *open(const QString &path) const
    {
        m_errorReset();
        // GDAL's C API consumes UTF-8 paths on every supported desktop OS.
        const QByteArray encoded = path.toUtf8();
        return m_openEx(encoded.constData(), kGdalOpenReadOnlyRaster,
                        nullptr, nullptr, nullptr);
    }

    void close(void *dataset) const
    {
        if (dataset) {
            (void)m_close(dataset);
        }
    }

    void *warp(void *source, bool webMercator) const
    {
        m_errorReset();
        const QByteArray wkt = (webMercator ? m_webMercatorWkt : m_wgs84Wkt)
                                   .toUtf8();
        return m_autoCreateWarpedVrt(source, nullptr, wkt.constData(),
                                     kGdalBilinear, 0.0, nullptr);
    }

    int width(void *dataset) const { return m_getRasterXSize(dataset); }
    int height(void *dataset) const { return m_getRasterYSize(dataset); }
    int bands(void *dataset) const { return m_getRasterCount(dataset); }
    int geoTransform(void *dataset, double *transform) const
    {
        return m_getGeoTransform(dataset, transform);
    }

    void *rasterBand(void *dataset, int band) const
    {
        return dataset && band > 0 ? m_getRasterBand(dataset, band) : nullptr;
    }

    int colorInterpretation(void *band) const
    {
        return band ? m_getRasterColorInterpretation(band) : 0;
    }

    void *colorTable(void *band) const
    {
        return band ? m_getRasterColorTable(band) : nullptr;
    }

    void *maskBand(void *band) const
    {
        return band ? m_getMaskBand(band) : nullptr;
    }

    int maskFlags(void *band) const
    {
        return band ? m_getMaskFlags(band) : kAllValidMask;
    }

    bool color(void *table, int index, GdalColorEntry *entry) const
    {
        return table && entry
            && m_getColorEntryAsRgb(table, index, entry) != 0;
    }

    bool readByteBand(void *band, int xOffset, int yOffset,
                      int xSize, int ySize, unsigned char *destination,
                      int destinationWidth, int destinationHeight) const
    {
        if (!band || !destination || xSize <= 0 || ySize <= 0
            || destinationWidth <= 0 || destinationHeight <= 0) {
            return false;
        }
        m_errorReset();
        return m_rasterIo(band, kGdalRead, xOffset, yOffset,
                          xSize, ySize, destination,
                          destinationWidth, destinationHeight,
                          kGdalByte, 0, 0) == 0;
    }

    QString driver(void *dataset) const
    {
        void *driverHandle = m_getDatasetDriver(dataset);
        if (!driverHandle) {
            return QStringLiteral("Unknown");
        }
        const char *name = m_getDriverShortName(driverHandle);
        return name ? QString::fromUtf8(name) : QStringLiteral("Unknown");
    }

    bool sample(void *dataset, int x, int y, double *value) const
    {
        if (!dataset || !value || x < 0 || y < 0
            || x >= width(dataset) || y >= height(dataset)) {
            return false;
        }
        void *band = m_getRasterBand(dataset, 1);
        if (!band) {
            return false;
        }
        double sampleValue = std::numeric_limits<double>::quiet_NaN();
        m_errorReset();
        if (m_rasterIo(band, kGdalRead, x, y, 1, 1, &sampleValue,
                       1, 1, kGdalFloat64, 0, 0) != 0
            || !std::isfinite(sampleValue)) {
            return false;
        }
        int hasNoData = 0;
        const double noData = m_getRasterNoDataValue(band, &hasNoData);
        if (hasNoData && (sampleValue == noData
                          || (std::isnan(noData) && std::isnan(sampleValue)))) {
            return false;
        }
        *value = sampleValue;
        return true;
    }

    QString lastError() const
    {
        const char *message = m_getLastErrorMessage();
        const QString text = message ? QString::fromUtf8(message).trimmed()
                                     : QString();
        return text.isEmpty() ? QStringLiteral("unknown GDAL error") : text;
    }

    QString version() const { return m_version; }
    QString libraryPath() const { return m_libraryPath; }

private:
    NativeGdalApi(std::unique_ptr<QLibrary> library, QString path)
        : m_library(std::move(library)), m_libraryPath(std::move(path))
    {
    }

    template<typename Function>
    bool resolve(Function *target, const char *name, QString *error,
                 bool required = true)
    {
        *target = reinterpret_cast<Function>(m_library->resolve(name));
        if (*target || !required) {
            return true;
        }
        if (error) {
            *error = QStringLiteral("missing GDAL C symbol %1")
                         .arg(QString::fromLatin1(name));
        }
        return false;
    }

    bool resolveAll(QString *error)
    {
        return resolve(&m_allRegister, "GDALAllRegister", error)
            && resolve(&m_versionInfo, "GDALVersionInfo", error)
            && resolve(&m_openEx, "GDALOpenEx", error)
            && resolve(&m_close, "GDALClose", error)
            && resolve(&m_autoCreateWarpedVrt,
                       "GDALAutoCreateWarpedVRT", error)
            && resolve(&m_getRasterXSize, "GDALGetRasterXSize", error)
            && resolve(&m_getRasterYSize, "GDALGetRasterYSize", error)
            && resolve(&m_getRasterCount, "GDALGetRasterCount", error)
            && resolve(&m_getGeoTransform, "GDALGetGeoTransform", error)
            && resolve(&m_getDatasetDriver, "GDALGetDatasetDriver", error)
            && resolve(&m_getDriverShortName, "GDALGetDriverShortName", error)
            && resolve(&m_getRasterBand, "GDALGetRasterBand", error)
            && resolve(&m_getRasterColorInterpretation,
                       "GDALGetRasterColorInterpretation", error)
            && resolve(&m_getRasterColorTable,
                       "GDALGetRasterColorTable", error)
            && resolve(&m_getColorEntryAsRgb,
                       "GDALGetColorEntryAsRGB", error)
            && resolve(&m_getMaskBand, "GDALGetMaskBand", error)
            && resolve(&m_getMaskFlags, "GDALGetMaskFlags", error)
            && resolve(&m_rasterIo, "GDALRasterIO", error)
            && resolve(&m_getRasterNoDataValue,
                       "GDALGetRasterNoDataValue", error)
            && resolve(&m_newSpatialReference,
                       "OSRNewSpatialReference", error)
            && resolve(&m_importFromEpsg, "OSRImportFromEPSG", error)
            && resolve(&m_setAxisMappingStrategy,
                       "OSRSetAxisMappingStrategy", error, false)
            && resolve(&m_exportToWkt, "OSRExportToWkt", error)
            && resolve(&m_destroySpatialReference,
                       "OSRDestroySpatialReference", error)
            && resolve(&m_vsiFree, "VSIFree", error)
            && resolve(&m_errorReset, "CPLErrorReset", error)
            && resolve(&m_getLastErrorMessage, "CPLGetLastErrorMsg", error);
    }

    QString createWkt(int epsg, QString *error) const
    {
        void *spatialReference = m_newSpatialReference(nullptr);
        if (!spatialReference) {
            if (error) {
                *error = QStringLiteral("OSRNewSpatialReference returned null");
            }
            return QString();
        }
        QString result;
        if (m_importFromEpsg(spatialReference, epsg) != 0) {
            if (error) {
                *error = QStringLiteral("GDAL cannot initialize EPSG:%1: %2")
                             .arg(epsg).arg(lastError());
            }
        } else {
            if (m_setAxisMappingStrategy) {
                m_setAxisMappingStrategy(spatialReference,
                                         kTraditionalGisOrder);
            }
            char *text = nullptr;
            if (m_exportToWkt(spatialReference, &text) == 0 && text) {
                result = QString::fromUtf8(text);
                m_vsiFree(text);
            } else if (error) {
                *error = QStringLiteral("GDAL cannot export EPSG:%1: %2")
                             .arg(epsg).arg(lastError());
            }
        }
        m_destroySpatialReference(spatialReference);
        return result;
    }

    std::unique_ptr<QLibrary> m_library;
    QString m_libraryPath;
    QString m_version;
    QString m_wgs84Wkt;
    QString m_webMercatorWkt;
    GdalAllRegister m_allRegister = nullptr;
    GdalVersionInfo m_versionInfo = nullptr;
    GdalOpenEx m_openEx = nullptr;
    GdalClose m_close = nullptr;
    GdalAutoCreateWarpedVrt m_autoCreateWarpedVrt = nullptr;
    GdalGetInteger m_getRasterXSize = nullptr;
    GdalGetInteger m_getRasterYSize = nullptr;
    GdalGetInteger m_getRasterCount = nullptr;
    GdalGetGeoTransform m_getGeoTransform = nullptr;
    GdalGetHandle m_getDatasetDriver = nullptr;
    GdalGetString m_getDriverShortName = nullptr;
    GdalGetRasterBand m_getRasterBand = nullptr;
    GdalGetInteger m_getRasterColorInterpretation = nullptr;
    GdalGetHandle m_getRasterColorTable = nullptr;
    GdalGetColorEntryAsRgb m_getColorEntryAsRgb = nullptr;
    GdalGetHandle m_getMaskBand = nullptr;
    GdalGetInteger m_getMaskFlags = nullptr;
    GdalRasterIo m_rasterIo = nullptr;
    GdalGetRasterNoDataValue m_getRasterNoDataValue = nullptr;
    OsrNewSpatialReference m_newSpatialReference = nullptr;
    OsrImportFromEpsg m_importFromEpsg = nullptr;
    OsrSetAxisMappingStrategy m_setAxisMappingStrategy = nullptr;
    OsrExportToWkt m_exportToWkt = nullptr;
    OsrDestroySpatialReference m_destroySpatialReference = nullptr;
    VsiFree m_vsiFree = nullptr;
    CplErrorReset m_errorReset = nullptr;
    CplGetLastErrorMessage m_getLastErrorMessage = nullptr;
};

struct GdalBackend
{
    std::shared_ptr<NativeGdalApi> api;
    QString status;
};

GdalBackend &gdalBackend()
{
    static GdalBackend backend = []() {
        GdalBackend result;
        result.api = NativeGdalApi::tryLoad(&result.status);
        return result;
    }();
    return backend;
}

class GdalDataset final
{
public:
    GdalDataset(std::shared_ptr<NativeGdalApi> api, void *source,
                void *warped, QString path, QString driver,
                const double *transform, int width, int height, int bands)
        : m_api(std::move(api)), m_source(source), m_warped(warped),
          m_path(std::move(path)), m_driver(std::move(driver)),
          m_width(width), m_height(height), m_bands(bands)
    {
        std::copy(transform, transform + 6, m_transform);
        const double x2 = m_transform[0] + m_width * m_transform[1];
        const double y2 = m_transform[3] + m_height * m_transform[5];
        m_extent = {std::min(m_transform[0], x2),
                    std::min(m_transform[3], y2),
                    std::max(m_transform[0], x2),
                    std::max(m_transform[3], y2)};
        m_resolution = std::max(std::abs(m_transform[1]),
                                std::abs(m_transform[5]));
        m_interpretations.reserve(m_bands);
        for (int index = 1; index <= m_bands; ++index) {
            m_interpretations.push_back(m_api->colorInterpretation(
                m_api->rasterBand(m_warped, index)));
        }
    }

    ~GdalDataset()
    {
        m_api->close(m_warped);
        m_api->close(m_source);
    }

    GdalDataset(const GdalDataset &) = delete;
    GdalDataset &operator=(const GdalDataset &) = delete;

    QString path() const { return m_path; }
    QString driver() const { return m_driver; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int bands() const { return m_bands; }
    const double *transform() const { return m_transform; }
    double resolution() const { return m_resolution; }

    bool sampleLonLat(double latitude, double longitude,
                      double *altitude) const
    {
        QMutexLocker locker(&m_gate);
        if (std::abs(m_transform[2]) > 1e-9
            || std::abs(m_transform[4]) > 1e-9
            || m_transform[1] <= 0.0 || m_transform[5] >= 0.0) {
            return false;
        }
        const int x = static_cast<int>(std::floor(
            (longitude - m_transform[0]) / m_transform[1]));
        const int y = static_cast<int>(std::floor(
            (latitude - m_transform[3]) / m_transform[5]));
        return m_api->sample(m_warped, x, y, altitude);
    }

    bool render(const MercatorExtent &request, int tileSize,
                unsigned char *destination) const
    {
        QMutexLocker locker(&m_gate);
        MercatorExtent intersection;
        if (!destination || tileSize <= 0
            || !intersectExtents(request, m_extent, &intersection)) {
            return false;
        }

        const int destinationLeft = std::clamp(
            static_cast<int>(std::floor(
                (intersection.minX - request.minX)
                / request.width() * tileSize)),
            0, tileSize - 1);
        const int destinationRight = std::clamp(
            static_cast<int>(std::ceil(
                (intersection.maxX - request.minX)
                / request.width() * tileSize)),
            1, tileSize);
        const int destinationTop = std::clamp(
            static_cast<int>(std::floor(
                (request.maxY - intersection.maxY)
                / request.height() * tileSize)),
            0, tileSize - 1);
        const int destinationBottom = std::clamp(
            static_cast<int>(std::ceil(
                (request.maxY - intersection.minY)
                / request.height() * tileSize)),
            1, tileSize);
        const int outputWidth = destinationRight - destinationLeft;
        const int outputHeight = destinationBottom - destinationTop;
        if (outputWidth <= 0 || outputHeight <= 0) {
            return false;
        }

        const int xOffset = std::clamp(
            static_cast<int>(std::floor(
                (intersection.minX - m_transform[0]) / m_transform[1])),
            0, m_width - 1);
        const int xEnd = std::clamp(
            static_cast<int>(std::ceil(
                (intersection.maxX - m_transform[0]) / m_transform[1])),
            xOffset + 1, m_width);
        const int yOffset = std::clamp(
            static_cast<int>(std::floor(
                (m_transform[3] - intersection.maxY) / -m_transform[5])),
            0, m_height - 1);
        const int yEnd = std::clamp(
            static_cast<int>(std::ceil(
                (m_transform[3] - intersection.minY) / -m_transform[5])),
            yOffset + 1, m_height);
        const int count = outputWidth * outputHeight;

        const int redBand = findBand(kRedColorInterpretation);
        const int greenBand = findBand(kGreenColorInterpretation);
        const int blueBand = findBand(kBlueColorInterpretation);
        const int grayBand = findBand(kGrayColorInterpretation);
        const int paletteBand = findBand(kPaletteColorInterpretation);
        const int alphaBand = findBand(kAlphaColorInterpretation);
        const int fallbackBand = redBand > 0 ? redBand
            : grayBand > 0 ? grayBand
            : paletteBand > 0 ? paletteBand : 1;
        const bool channelsUndefined = redBand == 0 && greenBand == 0
            && blueBand == 0 && grayBand == 0 && paletteBand == 0;

        const auto read = [this, xOffset, yOffset, xEnd, yEnd,
                           outputWidth, outputHeight, count](int bandIndex,
                                                            std::vector<unsigned char> *bytes) {
            if (!bytes || bandIndex <= 0) {
                return false;
            }
            void *band = m_api->rasterBand(m_warped, bandIndex);
            if (!band) {
                return false;
            }
            bytes->assign(static_cast<size_t>(count), 0);
            return m_api->readByteBand(
                band, xOffset, yOffset, xEnd - xOffset, yEnd - yOffset,
                bytes->data(), outputWidth, outputHeight);
        };
        const auto readHandle = [this, xOffset, yOffset, xEnd, yEnd,
                                 outputWidth, outputHeight, count](
                                    void *band,
                                    std::vector<unsigned char> *bytes) {
            if (!band || !bytes) {
                return false;
            }
            bytes->assign(static_cast<size_t>(count), 0);
            return m_api->readByteBand(
                band, xOffset, yOffset, xEnd - xOffset, yEnd - yOffset,
                bytes->data(), outputWidth, outputHeight);
        };

        std::vector<unsigned char> red;
        std::vector<unsigned char> green;
        std::vector<unsigned char> blue;
        std::vector<unsigned char> alpha;
        if (!read(redBand > 0 ? redBand
                             : grayBand > 0 ? grayBand : fallbackBand,
                  &red)
            || !read(greenBand > 0 ? greenBand
                                   : channelsUndefined && m_bands >= 3 ? 2
                                   : grayBand > 0 ? grayBand : fallbackBand,
                     &green)
            || !read(blueBand > 0 ? blueBand
                                  : channelsUndefined && m_bands >= 3 ? 3
                                  : grayBand > 0 ? grayBand : fallbackBand,
                     &blue)
            || (alphaBand > 0 && !read(alphaBand, &alpha))) {
            return false;
        }

        void *firstBand = m_api->rasterBand(m_warped, fallbackBand);
        std::vector<unsigned char> mask;
        const int maskFlags = m_api->maskFlags(firstBand);
        // RFC 15 exposes an explicit alpha band as the per-dataset mask of
        // every color band. Applying both would square the opacity (for
        // example, alpha 128 would incorrectly become 64).
        const bool alphaAlreadyRead = alphaBand > 0
            && (maskFlags & kAlphaMask) != 0;
        if (firstBand && (maskFlags & kAllValidMask) == 0
            && !alphaAlreadyRead) {
            if (!readHandle(m_api->maskBand(firstBand), &mask)) {
                mask.clear();
            }
        }
        void *colorTable = paletteBand > 0
            ? m_api->colorTable(m_api->rasterBand(m_warped, paletteBand))
            : nullptr;

        bool any = false;
        for (int row = 0; row < outputHeight; ++row) {
            for (int column = 0; column < outputWidth; ++column) {
                const int input = row * outputWidth + column;
                unsigned char sourceRed = red[static_cast<size_t>(input)];
                unsigned char sourceGreen = green[static_cast<size_t>(input)];
                unsigned char sourceBlue = blue[static_cast<size_t>(input)];
                unsigned char sourceAlpha = alpha.empty()
                    ? 255 : alpha[static_cast<size_t>(input)];
                GdalColorEntry color;
                if (paletteBand > 0 && m_api->color(
                        colorTable, sourceRed, &color)) {
                    sourceRed = clampColor(color.red);
                    sourceGreen = clampColor(color.green);
                    sourceBlue = clampColor(color.blue);
                    sourceAlpha = static_cast<unsigned char>(
                        sourceAlpha * clampColor(color.alpha) / 255);
                }
                if (!mask.empty()) {
                    sourceAlpha = static_cast<unsigned char>(
                        sourceAlpha * mask[static_cast<size_t>(input)] / 255);
                }
                if (sourceAlpha == 0) {
                    continue;
                }
                const int output = ((destinationTop + row) * tileSize
                    + destinationLeft + column) * 4;
                blendPixel(destination + output, sourceRed, sourceGreen,
                           sourceBlue, sourceAlpha);
                any = true;
            }
        }
        return any;
    }

private:
    int findBand(int interpretation) const
    {
        const auto found = std::find(m_interpretations.cbegin(),
                                     m_interpretations.cend(),
                                     interpretation);
        return found == m_interpretations.cend()
            ? 0
            : static_cast<int>(std::distance(m_interpretations.cbegin(),
                                             found)) + 1;
    }

    std::shared_ptr<NativeGdalApi> m_api;
    void *m_source = nullptr;
    void *m_warped = nullptr;
    QString m_path;
    QString m_driver;
    double m_transform[6] = {};
    int m_width = 0;
    int m_height = 0;
    int m_bands = 0;
    MercatorExtent m_extent;
    double m_resolution = 0.0;
    std::vector<int> m_interpretations;
    mutable QMutex m_gate;
};

struct OpenedRaster
{
    std::shared_ptr<GdalDataset> dataset;
    QString driver;
    QString error;
    bool recognized = false;
};

OpenedRaster openRaster(const std::shared_ptr<NativeGdalApi> &api,
                        const QString &path, bool webMercator)
{
    OpenedRaster result;
    if (!api) {
        result.error = gdalBackend().status;
        return result;
    }
    void *source = api->open(path);
    if (!source) {
        return result;
    }
    result.driver = api->driver(source);
    result.recognized = api->bands(source) > 0;
    if (!result.recognized) {
        api->close(source);
        return result;
    }
    void *warped = api->warp(source, webMercator);
    if (!warped) {
        result.error = QStringLiteral("Cannot warp to EPSG:%1: %2")
            .arg(webMercator ? 3857 : 4326).arg(api->lastError());
        api->close(source);
        return result;
    }
    const int width = api->width(warped);
    const int height = api->height(warped);
    const int bands = api->bands(warped);
    double transform[6] = {};
    if (width <= 0 || height <= 0 || bands <= 0
        || api->geoTransform(warped, transform) != 0
        || !std::all_of(transform, transform + 6,
                        [](double value) { return std::isfinite(value); })
        || std::abs(transform[2]) > 1e-9
        || std::abs(transform[4]) > 1e-9
        || transform[1] <= 0.0 || transform[5] >= 0.0) {
        result.error = QStringLiteral(
            "Warped raster has no valid north-up geotransform.");
        api->close(warped);
        api->close(source);
        return result;
    }
    const double x2 = transform[0] + width * transform[1];
    const double y2 = transform[3] + height * transform[5];
    if (!std::isfinite(x2) || !std::isfinite(y2)
        || qFuzzyCompare(transform[0], x2)
        || qFuzzyCompare(transform[3], y2)) {
        result.error = QStringLiteral("Warped raster coverage is empty.");
        api->close(warped);
        api->close(source);
        return result;
    }
    result.dataset = std::make_shared<GdalDataset>(
        api, source, warped, path, result.driver,
        transform, width, height, bands);
    return result;
}

QString elevationCoverage(const GdalDataset &dataset)
{
    const double *transform = dataset.transform();
    const double east = transform[0] + dataset.width() * transform[1];
    const double south = transform[3] + dataset.height() * transform[5];
    return QStringLiteral("%1×%2; N %3, S %4, W %5, E %6")
        .arg(dataset.width()).arg(dataset.height())
        .arg(compactNumber(transform[3], 6))
        .arg(compactNumber(south, 6))
        .arg(compactNumber(transform[0], 6))
        .arg(compactNumber(east, 6));
}

QString rasterCoverage(const GdalDataset &dataset)
{
    const double *transform = dataset.transform();
    const double eastX = transform[0] + dataset.width() * transform[1];
    const double southY = transform[3] + dataset.height() * transform[5];
    const double west = mercatorLongitude(transform[0]);
    const double north = mercatorLatitude(transform[3]);
    const double east = mercatorLongitude(eastX);
    const double south = mercatorLatitude(southY);
    return QStringLiteral("%1,%2 → %3,%4")
        .arg(compactNumber(south, 5), compactNumber(west, 5),
             compactNumber(north, 5), compactNumber(east, 5));
}

struct ScanArtifacts
{
    ElevationSourcesScanResult result;
    std::vector<std::shared_ptr<GdalDataset>> elevationDatasets;
    std::vector<std::shared_ptr<GdalDataset>> rasterDatasets;
};

using ProgressCallback = std::function<void(
    const ElevationSourcesScanProgress &)>;

ScanArtifacts scanDirectory(const QString &requestedDirectory,
                            const std::shared_ptr<std::atomic_bool> &cancel,
                            const ProgressCallback &progress)
{
    ScanArtifacts artifacts;
    QString error;
    const QString root = ElevationSourceService::normalizeExistingDirectory(
        requestedDirectory, &error);
    if (root.isEmpty()) {
        artifacts.result.Error = error;
        return artifacts;
    }
    artifacts.result.Directory = root;
    const GdalBackend &backend = gdalBackend();
    artifacts.result.Backend = backend.status;
    artifacts.result.NativeGdalAvailable = bool(backend.api);

    const QStringList elevationFiles =
        ElevationSourceService::findSupportedFiles(root, &error);
    if (!error.isEmpty()) {
        artifacts.result.Error = error;
        return artifacts;
    }
    for (int index = 0; index < elevationFiles.size(); ++index) {
        if (cancel->load()) {
            artifacts.result.Cancelled = true;
            return artifacts;
        }
        const QString path = elevationFiles.at(index);
        progress({ElevationSourcesScanProgress::Stage::Elevation,
                  index, elevationFiles.size(), path});
        ElevationSourceFile file;
        file.FullPath = path;
        file.Format = isGeoTiff(path) ? QStringLiteral("GeoTIFF")
                                     : QStringLiteral("DTED");
        OpenedRaster opened = openRaster(backend.api, path, false);
        if (opened.dataset) {
            file.Indexed = true;
            file.Coverage = elevationCoverage(*opened.dataset);
            artifacts.elevationDatasets.push_back(std::move(opened.dataset));
        } else {
            file.Coverage = QStringLiteral("—");
            file.Error = opened.error.isEmpty()
                ? QStringLiteral("GDAL could not open this elevation file.")
                : opened.error;
        }
        artifacts.result.Files.append(file);
    }
    progress({ElevationSourcesScanProgress::Stage::Elevation,
              elevationFiles.size(), elevationFiles.size(), QString()});

    if (cancel->load()) {
        artifacts.result.Cancelled = true;
        return artifacts;
    }
    if (!backend.api) {
        return artifacts;
    }

    const QStringList rasterFiles =
        ElevationSourceService::findNativeRasterCandidates(root, &error);
    if (!error.isEmpty()) {
        artifacts.result.Error = error;
        return artifacts;
    }
    artifacts.result.ExaminedRasterFiles = rasterFiles.size();
    for (int index = 0; index < rasterFiles.size(); ++index) {
        if (cancel->load()) {
            artifacts.result.Cancelled = true;
            return artifacts;
        }
        const QString path = rasterFiles.at(index);
        progress({ElevationSourcesScanProgress::Stage::NativeGdal,
                  index, rasterFiles.size(), path});
        OpenedRaster opened = openRaster(backend.api, path, true);
        if (!opened.recognized) {
            ++artifacts.result.UnrecognizedRasterFiles;
            continue;
        }
        NativeGdalRasterFile file;
        file.FullPath = path;
        file.Driver = opened.driver.isEmpty()
            ? QStringLiteral("Unknown") : opened.driver;
        if (opened.dataset) {
            file.Indexed = true;
            file.Size = QStringLiteral("%1 × %2, %3 band(s)")
                .arg(QLocale().toString(opened.dataset->width()))
                .arg(QLocale().toString(opened.dataset->height()))
                .arg(opened.dataset->bands());
            file.Coverage = rasterCoverage(*opened.dataset);
            artifacts.rasterDatasets.push_back(std::move(opened.dataset));
        } else {
            file.Size = QStringLiteral("—");
            file.Coverage = QStringLiteral("—");
            file.Error = opened.error;
        }
        artifacts.result.RasterFiles.append(file);
    }
    progress({ElevationSourcesScanProgress::Stage::NativeGdal,
              rasterFiles.size(), rasterFiles.size(), QString()});
    if (cancel->load()) {
        artifacts.result.Cancelled = true;
        return artifacts;
    }
    std::stable_sort(
        artifacts.result.RasterFiles.begin(),
        artifacts.result.RasterFiles.end(),
        [](const NativeGdalRasterFile &left,
           const NativeGdalRasterFile &right) {
            return lessPath(left.FullPath, right.FullPath,
                            rasterPathCaseSensitivity());
        });
    std::stable_sort(
        artifacts.rasterDatasets.begin(),
        artifacts.rasterDatasets.end(),
        [](const std::shared_ptr<GdalDataset> &left,
           const std::shared_ptr<GdalDataset> &right) {
            // Coarse imagery is painted first so higher-resolution datasets
            // replace it in overlap areas, matching Mission Planner.
            return left && right
                ? left->resolution() > right->resolution()
                : bool(left);
        });
    return artifacts;
}
}

QString ElevationSourceFile::Name() const
{
    return QFileInfo(FullPath).fileName();
}

QString ElevationSourceFile::State() const
{
    return !Error.isEmpty() ? QStringLiteral("Error")
                            : Indexed ? QStringLiteral("Indexed")
                                      : QStringLiteral("Skipped");
}

QString NativeGdalRasterFile::Name() const
{
    return QFileInfo(FullPath).fileName();
}

QString NativeGdalRasterFile::State() const
{
    return !Error.isEmpty() ? QStringLiteral("Error")
                            : Indexed ? QStringLiteral("Indexed")
                                      : QStringLiteral("Skipped");
}

int ElevationSourcesScanResult::IndexedCount() const
{
    return std::count_if(Files.cbegin(), Files.cend(),
                         [](const ElevationSourceFile &file) {
                             return file.Indexed;
                         });
}

int ElevationSourcesScanResult::ErrorCount() const
{
    return std::count_if(Files.cbegin(), Files.cend(),
                         [](const ElevationSourceFile &file) {
                             return !file.Error.isEmpty();
                         });
}

int ElevationSourcesScanResult::GeoTiffCount() const
{
    return std::count_if(Files.cbegin(), Files.cend(),
                         [](const ElevationSourceFile &file) {
                             return file.Format == QStringLiteral("GeoTIFF");
                         });
}

int ElevationSourcesScanResult::DtedCount() const
{
    return std::count_if(Files.cbegin(), Files.cend(),
                         [](const ElevationSourceFile &file) {
                             return file.Format == QStringLiteral("DTED");
                         });
}

int ElevationSourcesScanResult::RasterIndexedCount() const
{
    return std::count_if(RasterFiles.cbegin(), RasterFiles.cend(),
                         [](const NativeGdalRasterFile &file) {
                             return file.Indexed;
                         });
}

int ElevationSourcesScanResult::RasterErrorCount() const
{
    return std::count_if(RasterFiles.cbegin(), RasterFiles.cend(),
                         [](const NativeGdalRasterFile &file) {
                             return !file.Error.isEmpty();
                         });
}

class ElevationSourceService::Private
{
public:
    QPointer<QThread> thread;
    std::shared_ptr<std::atomic_bool> cancellation;
    ElevationSourcesScanResult lastResult;
    std::vector<std::shared_ptr<GdalDataset>> elevationDatasets;
    std::vector<std::shared_ptr<GdalDataset>> rasterDatasets;
    std::unique_ptr<SrtmElevationSource> srtm;
    mutable QMutex stateMutex;
    QString startupError;
    quint64 generation = 0;
    bool busy = false;
    bool hasLastResult = false;
    bool initialized = false;
    bool shuttingDown = false;
};

ElevationSourceService::ElevationSourceService(QObject *parent)
    : QObject(parent), d(new Private)
{
    qRegisterMetaType<ElevationSourcesScanProgress>();
    qRegisterMetaType<ElevationSourcesScanResult>();
    d->srtm.reset(new SrtmElevationSource(this));
    connect(d->srtm.get(), &SrtmElevationSource::TileAvailable,
            this, &ElevationSourceService::srtmTileAvailable);
    connect(d->srtm.get(), &SrtmElevationSource::DownloadFailed,
            this, &ElevationSourceService::srtmDownloadFailed);
}

ElevationSourceService::~ElevationSourceService()
{
    shutdown();
}

ElevationSourceService *ElevationSourceService::instance()
{
    static ElevationSourceService *service =
        new ElevationSourceService(QCoreApplication::instance());
    return service;
}

QString ElevationSourceService::savedDirectory(QSettings *settings)
{
    if (settings) {
        const bool fallbacks = settings->fallbacksEnabled();
        settings->setFallbacksEnabled(false);
        const QString value = settings->value(
            QString::fromLatin1(SettingsKey), QString()).toString().trimmed();
        settings->setFallbacksEnabled(fallbacks);
        return value;
    }
    QSettings local;
    local.setFallbacksEnabled(false);
    return local.value(QString::fromLatin1(SettingsKey), QString())
        .toString().trimmed();
}

QString ElevationSourceService::normalizeExistingDirectory(
    const QString &directory, QString *error)
{
    const QString trimmed = directory.trimmed();
    if (trimmed.isEmpty()) {
        if (error) {
            *error = tr("Choose a DEM directory first.");
        }
        return QString();
    }
    const QFileInfo info(trimmed);
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QFileInfo normalizedInfo(absolute);
    if (!normalizedInfo.exists() || !normalizedInfo.isDir()) {
        if (error) {
            *error = pathError(tr("DEM directory does not exist"), absolute);
        }
        return QString();
    }
    if (!normalizedInfo.isReadable()) {
        if (error) {
            *error = pathError(tr("DEM directory is not readable"), absolute);
        }
        return QString();
    }
    if (error) {
        error->clear();
    }
    return absolute;
}

bool ElevationSourceService::saveDirectory(QSettings *settings,
                                           const QString &directory,
                                           QString *normalized,
                                           QString *error)
{
    QString validationError;
    const QString path = normalizeExistingDirectory(directory,
                                                    &validationError);
    if (path.isEmpty()) {
        if (error) {
            *error = validationError;
        }
        return false;
    }
    std::unique_ptr<QSettings> owned;
    if (!settings) {
        owned.reset(new QSettings);
        settings = owned.get();
    }
    settings->setFallbacksEnabled(false);
    settings->setValue(QString::fromLatin1(SettingsKey), path);
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        if (error) {
            *error = tr("Unable to save the elevation directory.");
        }
        return false;
    }
    if (normalized) {
        *normalized = path;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool ElevationSourceService::clearSavedDirectory(QSettings *settings,
                                                 QString *error)
{
    std::unique_ptr<QSettings> owned;
    if (!settings) {
        owned.reset(new QSettings);
        settings = owned.get();
    }
    settings->setFallbacksEnabled(false);
    settings->setValue(QString::fromLatin1(SettingsKey), QString());
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        if (error) {
            *error = tr("Unable to clear the saved elevation directory.");
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QStringList ElevationSourceService::findSupportedFiles(
    const QString &directory, QString *error)
{
    QString validationError;
    const QString root = normalizeExistingDirectory(directory,
                                                    &validationError);
    if (root.isEmpty()) {
        if (error) {
            *error = validationError;
        }
        return {};
    }
    QStringList result;
    QDirIterator iterator(root,
                          QDir::Files | QDir::NoSymLinks
                              | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = QDir::cleanPath(
            QFileInfo(iterator.next()).absoluteFilePath());
        if (isSupportedElevation(path)) {
            result.append(path);
        }
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const QString &left, const QString &right) {
        const int leftOrder = elevationFormatOrder(left);
        const int rightOrder = elevationFormatOrder(right);
        return leftOrder == rightOrder
            ? lessPath(left, right, elevationPathCaseSensitivity())
            : leftOrder < rightOrder;
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const QString &left, const QString &right) {
        return QString::compare(left, right,
                                elevationPathCaseSensitivity()) == 0;
    }), result.end());
    if (error) {
        error->clear();
    }
    return result;
}

QStringList ElevationSourceService::findNativeRasterCandidates(
    const QString &directory, QString *error)
{
    QString validationError;
    const QString root = normalizeExistingDirectory(directory,
                                                    &validationError);
    if (root.isEmpty()) {
        if (error) {
            *error = validationError;
        }
        return {};
    }
    QStringList result;
    QDirIterator iterator(root,
                          QDir::Files | QDir::NoSymLinks
                              | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo info(iterator.next());
        if (info.size() >= 1024 && !isKnownSidecar(info.absoluteFilePath())) {
            result.append(QDir::cleanPath(info.absoluteFilePath()));
        }
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const QString &left, const QString &right) {
        return lessPath(left, right, rasterPathCaseSensitivity());
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const QString &left, const QString &right) {
        return QString::compare(left, right,
                                rasterPathCaseSensitivity()) == 0;
    }), result.end());
    if (error) {
        error->clear();
    }
    return result;
}

QStringList ElevationSourceService::nativeGdalLibraryCandidates()
{
    QStringList candidates;
    const QString configured = QString::fromLocal8Bit(
        qgetenv("MISSIONPLANNER_GDAL_LIBRARY")).trimmed();
    if (!configured.isEmpty()) {
        candidates.append(configured);
    }
#ifdef Q_OS_WIN
    candidates.append({QStringLiteral("gdal.dll"),
                       QStringLiteral("gdal313.dll"),
                       QStringLiteral("gdal312.dll"),
                       QStringLiteral("gdal311.dll"),
                       QStringLiteral("gdal310.dll"),
                       QStringLiteral("gdal309.dll"),
                       QStringLiteral("gdal308.dll"),
                       QStringLiteral("gdal307.dll"),
                       QStringLiteral("gdal306.dll"),
                       QStringLiteral("gdal305.dll")});
#elif defined(Q_OS_MACOS)
    candidates.append({QStringLiteral("libgdal.dylib"),
                       QStringLiteral("/opt/homebrew/lib/libgdal.dylib"),
                       QStringLiteral("/usr/local/lib/libgdal.dylib")});
#else
    candidates.append(QStringLiteral("libgdal.so"));
    for (int abi = 40; abi >= 30; --abi) {
        candidates.append(QStringLiteral("libgdal.so.%1").arg(abi));
    }
#endif
    candidates.removeDuplicates();
    return candidates;
}

bool ElevationSourceService::isBusy() const
{
    return d->busy;
}

bool ElevationSourceService::hasLastResult() const
{
    QMutexLocker locker(&d->stateMutex);
    return d->hasLastResult;
}

ElevationSourcesScanResult ElevationSourceService::lastResult() const
{
    QMutexLocker locker(&d->stateMutex);
    return d->lastResult;
}

QString ElevationSourceService::startupError() const
{
    QMutexLocker locker(&d->stateMutex);
    return d->startupError;
}

bool ElevationSourceService::requiresRestartToSwitch(
    const QString &directory) const
{
    ElevationSourcesScanResult active;
    {
        QMutexLocker locker(&d->stateMutex);
        if (!d->hasLastResult || d->lastResult.IndexedCount() == 0) {
            return false;
        }
        active = d->lastResult;
    }
    QString error;
    const QString normalized = normalizeExistingDirectory(directory, &error);
    if (normalized.isEmpty()) {
        return false;
    }
    return QString::compare(
        QDir::cleanPath(active.Directory), normalized,
        elevationPathCaseSensitivity()) != 0;
}

QString ElevationSourceService::backendStatus() const
{
    return gdalBackend().status;
}

bool ElevationSourceService::isNativeGdalAvailable() const
{
    return bool(gdalBackend().api);
}

bool ElevationSourceService::sampleAltitude(double latitude,
                                            double longitude,
                                            double *altitude) const
{
    if (sampleAltitudeCached(latitude, longitude, altitude)) {
        return true;
    }
    requestSrtmTile(latitude, longitude);
    return false;
}

bool ElevationSourceService::sampleAltitudeCached(
    double latitude, double longitude, double *altitude) const
{
    if (!altitude || !std::isfinite(latitude) || !std::isfinite(longitude)
        || latitude < -90.0 || latitude > 90.0
        || longitude < -180.0 || longitude > 180.0) {
        return false;
    }
    std::vector<std::shared_ptr<GdalDataset>> datasets;
    {
        QMutexLocker locker(&d->stateMutex);
        datasets = d->elevationDatasets;
    }
    for (const std::shared_ptr<GdalDataset> &dataset : datasets) {
        if (dataset && dataset->sampleLonLat(latitude, longitude, altitude)) {
            return true;
        }
    }
    return d->srtm && d->srtm->SampleAltitudeCached(
        latitude, longitude, altitude);
}

bool ElevationSourceService::requestSrtmTile(
    double latitude, double longitude) const
{
    return d->srtm
        && d->srtm->RequestTileForCoordinate(latitude, longitude);
}

QString ElevationSourceService::srtmCacheDirectory() const
{
    return d->srtm ? d->srtm->CacheDirectory() : QString();
}

bool ElevationSourceService::srtmAutoDownloadEnabled() const
{
    return d->srtm && d->srtm->AutoDownloadEnabled();
}

void ElevationSourceService::setSrtmAutoDownloadEnabled(bool enabled)
{
    if (d->srtm) {
        d->srtm->SetAutoDownloadEnabled(enabled);
    }
}

QByteArray ElevationSourceService::renderRasterTile(
    int tileX, int tileY, int zoom, int tileSize) const
{
    if (zoom < 0 || zoom > 30 || tileSize < 1 || tileSize > 2048) {
        return {};
    }
    const qint64 tileCount = qint64(1) << zoom;
    if (tileX < 0 || tileY < 0
        || tileX >= tileCount || tileY >= tileCount) {
        return {};
    }

    std::vector<std::shared_ptr<GdalDataset>> datasets;
    {
        QMutexLocker locker(&d->stateMutex);
        datasets = d->rasterDatasets;
    }
    if (datasets.empty()) {
        return {};
    }

    const double tileSpan = 2.0 * kWebMercatorHalfWorld
        / static_cast<double>(tileCount);
    const double minX = -kWebMercatorHalfWorld + tileX * tileSpan;
    const double maxY = kWebMercatorHalfWorld - tileY * tileSpan;
    const MercatorExtent request{
        minX, maxY - tileSpan, minX + tileSpan, maxY};

    QImage image(tileSize, tileSize, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);
    bool rendered = false;
    for (const std::shared_ptr<GdalDataset> &dataset : datasets) {
        if (dataset) {
            rendered |= dataset->render(request, tileSize, image.bits());
        }
    }
    if (!rendered) {
        return {};
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly)
        || !image.save(&buffer, "PNG")) {
        return {};
    }
    return png;
}

bool ElevationSourceService::startScan(const QString &directory, bool startup)
{
    if (d->busy || d->shuttingDown) {
        return false;
    }
    d->busy = true;
    const quint64 generation = ++d->generation;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    d->cancellation = cancellation;
    auto *thread = new QThread;
    auto *worker = new QObject;
    worker->moveToThread(thread);
    d->thread = thread;
    emit busyChanged(true);
    emit scanStarted(startup);

    const QPointer<ElevationSourceService> self(this);
    connect(thread, &QThread::started, worker,
            [self, thread, directory, cancellation, startup, generation]() {
        const ProgressCallback report =
            [self, startup, generation](
                const ElevationSourcesScanProgress &progress) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self,
                    [self, startup, generation, progress]() {
                        if (self && self->d->busy
                            && self->d->generation == generation) {
                            emit self->scanProgress(progress, startup);
                        }
                    },
                    Qt::QueuedConnection);
            };
        ScanArtifacts artifacts = scanDirectory(directory, cancellation, report);
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, startup, generation,
                 artifacts = std::move(artifacts)]() mutable {
                    if (!self || self->d->generation != generation) {
                        return;
                    }
                    self->d->busy = false;
                    self->d->cancellation.reset();
                    if (artifacts.result.Cancelled) {
                        emit self->scanCancelled(startup);
                    } else if (!artifacts.result.Error.isEmpty()) {
                        if (startup) {
                            QMutexLocker locker(&self->d->stateMutex);
                            self->d->startupError = artifacts.result.Error;
                        }
                        emit self->scanFailed(artifacts.result.Error, startup);
                    } else {
                        ElevationSourcesScanResult published;
                        {
                            QMutexLocker locker(&self->d->stateMutex);
                            self->d->lastResult = artifacts.result;
                            self->d->hasLastResult = true;
                            self->d->startupError.clear();
                            self->d->elevationDatasets =
                                std::move(artifacts.elevationDatasets);
                            self->d->rasterDatasets =
                                std::move(artifacts.rasterDatasets);
                            published = self->d->lastResult;
                        }
                        emit self->scanFinished(published, startup);
                        emit self->nativeRastersChanged();
                    }
                    emit self->busyChanged(false);
                },
                Qt::QueuedConnection);
        }
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return true;
}

void ElevationSourceService::cancelScan()
{
    if (d->cancellation) {
        d->cancellation->store(true);
    }
}

void ElevationSourceService::unloadNativeRasters()
{
    {
        QMutexLocker locker(&d->stateMutex);
        d->rasterDatasets.clear();
        if (d->hasLastResult) {
            d->lastResult.RasterFiles.clear();
            d->lastResult.ExaminedRasterFiles = 0;
            d->lastResult.UnrecognizedRasterFiles = 0;
        }
    }
    emit nativeRastersChanged();
}

void ElevationSourceService::initializeFromSettings()
{
    if (d->initialized || d->shuttingDown) {
        return;
    }
    d->initialized = true;
    const QString directory = savedDirectory();
    if (!directory.isEmpty()) {
        startScan(directory, true);
    }
}

void ElevationSourceService::shutdown()
{
    if (d->shuttingDown) {
        return;
    }
    d->shuttingDown = true;
    if (d->srtm) {
        d->srtm->Shutdown();
    }
    ++d->generation;
    if (d->cancellation) {
        d->cancellation->store(true);
    }
    if (d->thread && d->thread->isRunning()) {
        d->thread->quit();
        // QThread::quit cannot interrupt GDALOpenEx/warp in progress. Do not
        // destroy the service or the function-pointer backend until the current
        // native call returns; forced QThread termination would be less safe.
        d->thread->wait();
    }
    d->busy = false;
    d->cancellation.reset();
    {
        QMutexLocker locker(&d->stateMutex);
        d->rasterDatasets.clear();
        d->elevationDatasets.clear();
    }
}

#undef APM_GDAL_STDCALL
