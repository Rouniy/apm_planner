#include <QtTest>

#include "ui/configuration/ConfigElevationSourcesView.h"
#include "ui/configuration/ConfigElevationSourcesViewModel.h"
#include "ui/configuration/ElevationSourceService.h"
#include "ui/configuration/SrtmElevationSource.h"
#include "ui/BackstageView.h"

#include <QAbstractItemView>
#include <QBuffer>
#include <QColor>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include <memory>
#include <cmath>

namespace {
bool writeSizedFile(const QString &path, int size)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(QByteArray(size, 'x')) == size;
}

bool writeSyntheticGeoTiff(const QString &path)
{
    constexpr quint16 entryCount = 14;
    constexpr quint32 scaleOffset = 8 + 2 + entryCount * 12 + 4;
    constexpr quint32 tiePointOffset = scaleOffset + 3 * sizeof(double);
    constexpr quint32 geoKeyOffset = tiePointOffset + 6 * sizeof(double);
    constexpr quint32 pixelsOffset = geoKeyOffset + 16 * sizeof(quint16);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("II", 2);
    stream << quint16(42) << quint32(8);
    stream << entryCount;
    const auto shortEntry = [&stream](quint16 tag, quint16 value) {
        stream << tag << quint16(3) << quint32(1) << value << quint16(0);
    };
    const auto longEntry = [&stream](quint16 tag, quint32 value) {
        stream << tag << quint16(4) << quint32(1) << value;
    };
    const auto offsetEntry = [&stream](quint16 tag, quint16 type,
                                       quint32 count, quint32 offset) {
        stream << tag << type << count << offset;
    };
    longEntry(256, 2);                    // ImageWidth
    longEntry(257, 2);                    // ImageLength
    shortEntry(258, 32);                  // BitsPerSample
    shortEntry(259, 1);                   // Compression = none
    shortEntry(262, 1);                   // BlackIsZero
    longEntry(273, pixelsOffset);         // StripOffsets
    shortEntry(277, 1);                   // SamplesPerPixel
    longEntry(278, 2);                    // RowsPerStrip
    longEntry(279, 16);                   // StripByteCounts
    shortEntry(284, 1);                   // PlanarConfiguration
    shortEntry(339, 3);                   // SampleFormat = IEEE float
    offsetEntry(33550, 12, 3, scaleOffset);
    offsetEntry(33922, 12, 6, tiePointOffset);
    offsetEntry(34735, 3, 16, geoKeyOffset);
    stream << quint32(0);                 // no next IFD

    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << 0.01 << 0.01 << 0.0;        // ModelPixelScale
    stream << 0.0 << 0.0 << 0.0
           << 30.0 << 35.0 << 0.0;       // ModelTiepoint
    const quint16 geoKeys[] = {
        1, 1, 0, 3,
        1024, 0, 1, 2,                   // Geographic model
        1025, 0, 1, 1,                   // Pixel is area
        2048, 0, 1, 4326                 // WGS84
    };
    for (quint16 value : geoKeys) {
        stream << value;
    }
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << float(100.0) << float(110.0)
           << float(120.0) << float(130.0);
    buffer.close();
    if (bytes.size() < 2048) {
        bytes.append(QByteArray(2048 - bytes.size(), '\0'));
    }

    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size();
}

bool writeSyntheticRgbaGeoTiff(const QString &path, int width, int height,
                               const QColor &color)
{
    constexpr quint16 entryCount = 14;
    constexpr quint32 bitsOffset = 8 + 2 + entryCount * 12 + 4;
    constexpr quint32 scaleOffset = bitsOffset + 4 * sizeof(quint16);
    constexpr quint32 tiePointOffset = scaleOffset + 3 * sizeof(double);
    constexpr quint32 geoKeyOffset = tiePointOffset + 6 * sizeof(double);
    constexpr quint32 pixelsOffset = geoKeyOffset + 16 * sizeof(quint16);
    if (width <= 0 || height <= 0) {
        return false;
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("II", 2);
    stream << quint16(42) << quint32(8);
    stream << entryCount;
    const auto shortEntry = [&stream](quint16 tag, quint16 value) {
        stream << tag << quint16(3) << quint32(1) << value << quint16(0);
    };
    const auto longEntry = [&stream](quint16 tag, quint32 value) {
        stream << tag << quint16(4) << quint32(1) << value;
    };
    const auto offsetEntry = [&stream](quint16 tag, quint16 type,
                                       quint32 count, quint32 offset) {
        stream << tag << type << count << offset;
    };
    longEntry(256, static_cast<quint32>(width));
    longEntry(257, static_cast<quint32>(height));
    offsetEntry(258, 3, 4, bitsOffset);    // BitsPerSample
    shortEntry(259, 1);                   // Compression = none
    shortEntry(262, 2);                   // RGB
    longEntry(273, pixelsOffset);         // StripOffsets
    shortEntry(277, 4);                   // SamplesPerPixel
    longEntry(278, static_cast<quint32>(height));
    longEntry(279, static_cast<quint32>(width * height * 4));
    shortEntry(284, 1);                   // chunky RGBA
    shortEntry(338, 2);                   // unassociated alpha
    offsetEntry(33550, 12, 3, scaleOffset);
    offsetEntry(33922, 12, 6, tiePointOffset);
    offsetEntry(34735, 3, 16, geoKeyOffset);
    stream << quint32(0);

    stream << quint16(8) << quint16(8) << quint16(8) << quint16(8);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    const double pixelScale = 0.02 / static_cast<double>(width);
    stream << pixelScale << pixelScale << 0.0;
    stream << 0.0 << 0.0 << 0.0
           << 30.0 << 35.0 << 0.0;
    const quint16 geoKeys[] = {
        1, 1, 0, 3,
        1024, 0, 1, 2,
        1025, 0, 1, 1,
        2048, 0, 1, 4326
    };
    for (quint16 value : geoKeys) {
        stream << value;
    }
    QByteArray pixels;
    pixels.reserve(width * height * 4);
    for (int index = 0; index < width * height; ++index) {
        pixels.append(static_cast<char>(color.red()));
        pixels.append(static_cast<char>(color.green()));
        pixels.append(static_cast<char>(color.blue()));
        pixels.append(static_cast<char>(color.alpha()));
    }
    stream.writeRawData(pixels.constData(), pixels.size());
    buffer.close();
    if (bytes.size() < 2048) {
        bytes.append(QByteArray(2048 - bytes.size(), '\0'));
    }

    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size();
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class FakeElevationSourceService final : public ElevationSourceService
{
public:
    explicit FakeElevationSourceService(QObject *parent = nullptr)
        : ElevationSourceService(parent)
    {
    }

    bool isBusy() const override { return busy; }
    bool hasLastResult() const override { return hasSnapshot; }
    ElevationSourcesScanResult lastResult() const override { return snapshot; }
    bool requiresRestartToSwitch(const QString &directory) const override
    {
        return restartRequired
            && QDir::cleanPath(directory) == QDir::cleanPath(restartDirectory);
    }
    QString backendStatus() const override
    {
        return QStringLiteral("Fake optional GDAL backend");
    }
    bool startScan(const QString &directory, bool startup) override
    {
        if (busy || rejectStart) {
            return false;
        }
        ++startCount;
        startedDirectory = directory;
        startedAsStartup = startup;
        busy = true;
        emit busyChanged(true);
        emit scanStarted(startup);
        return true;
    }
    void cancelScan() override { ++cancelCount; }
    void unloadNativeRasters() override { ++unloadCount; }
    void initializeFromSettings() override {}
    void shutdown() override {}

    void report(const ElevationSourcesScanProgress &progress)
    {
        emit scanProgress(progress, startedAsStartup);
    }
    void finish(const ElevationSourcesScanResult &result)
    {
        snapshot = result;
        hasSnapshot = true;
        busy = false;
        emit scanFinished(result, startedAsStartup);
        emit busyChanged(false);
    }
    void fail(const QString &error)
    {
        busy = false;
        emit scanFailed(error, startedAsStartup);
        emit busyChanged(false);
    }
    void cancelFinished()
    {
        busy = false;
        emit scanCancelled(startedAsStartup);
        emit busyChanged(false);
    }

    bool busy = false;
    bool hasSnapshot = false;
    bool restartRequired = false;
    bool rejectStart = false;
    bool startedAsStartup = false;
    int startCount = 0;
    int cancelCount = 0;
    int unloadCount = 0;
    QString startedDirectory;
    QString restartDirectory;
    ElevationSourcesScanResult snapshot;
};

class DefaultLocaleGuard final
{
public:
    explicit DefaultLocaleGuard(const QLocale &locale)
        : previous(QLocale())
    {
        QLocale::setDefault(locale);
    }
    ~DefaultLocaleGuard() { QLocale::setDefault(previous); }

private:
    QLocale previous;
};
}

class ConfigElevationSourcesViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void initialStateUsesOnlyExactProfileAndHasNoSideEffects();
    void discoveryIsRecursiveDeterministicAndCacheNeutral();
    void runtimeBackendIsDynamicallyLoadedAndIsolatesCorruptFiles();
    void nativeGdalRendererProducesTransparentOverlayTiles();
    void nativeGdalRendererHonorsExplicitAlpha();
    void nativeGdalRendererPrefersFineRasterOnOverlap();
    void startupRestorationIsBackgroundAndCancellable();
    void startupFailureIsRetainedForLazyViewModel();
    void validationPersistenceRestartAndCancelMatchReference();
    void resultAndClearPreserveManagedIndexAndUnrelatedState();
    void viewMatchesMissionPlannerContract();
    void routeIsAdvancedOfflineLazySubpage();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void ConfigElevationSourcesViewTest::initTestCase()
{
    m_settingsDirectory.reset(new QTemporaryDir);
    QVERIFY(m_settingsDirectory->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       m_settingsDirectory->path());
    QCoreApplication::setOrganizationName(
        QStringLiteral("APMPlannerElevationTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ConfigElevationSourcesView"));
}

void ConfigElevationSourcesViewTest::init()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.clear();
    settings.setValue(QString::fromLatin1(
        SrtmElevationSource::AutoDownloadSettingsKey), false);
    settings.sync();
}

void ConfigElevationSourcesViewTest::initialStateUsesOnlyExactProfileAndHasNoSideEffects()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("GDALImageDir"),
                      QStringLiteral("  /not/validated/on-read  "));
    settings.setValue(QStringLiteral("MapType"),
                      QStringLiteral("Future Map Backend"));
    settings.setValue(QStringLiteral("typedValue"), 42);
    settings.sync();
    QSettings organizationFallback(
        QSettings::IniFormat, QSettings::UserScope,
        QCoreApplication::organizationName());
    organizationFallback.setValue(QStringLiteral("GDALImageDir"),
                                  QStringLiteral("/must/not/be/read"));
    organizationFallback.sync();
    const QByteArray before = fileContents(settings.fileName());
    FakeElevationSourceService service;

    ConfigElevationSourcesViewModel model(&service, &settings);
    QCOMPARE(model.DirectoryPath(), QStringLiteral("/not/validated/on-read"));
    QCOMPARE(model.Status(), QStringLiteral(
        "Choose a directory containing local elevation or raster-map files."));
    QCOMPARE(model.NativeGdalStatus(), QStringLiteral(
        "Raster map: Fake optional GDAL backend"));
    QCOMPARE(model.Progress(), 0);
    QCOMPARE(model.ProgressMaximum(), 1);
    QVERIFY(!model.IsBusy());
    QVERIFY(model.Files().isEmpty());
    QVERIFY(model.RasterFiles().isEmpty());
    QCOMPARE(service.startCount, 0);
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("Future Map Backend"));
    QCOMPARE(settings.value(QStringLiteral("typedValue")).toInt(), 42);
    QCOMPARE(fileContents(settings.fileName()), before);
    organizationFallback.clear();
    organizationFallback.sync();
}

void ConfigElevationSourcesViewTest::discoveryIsRecursiveDeterministicAndCacheNeutral()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("nested"))));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("z.DT0")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("a.dt1")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("nested/b.Dt2")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("nested/c.TIFF")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("a.tif")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("ignored.hgt")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("small.bin")), 100));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("image.tif.aux.xml")), 1200));
    QVERIFY(writeSizedFile(source.filePath(QStringLiteral("image.prj")), 1200));

    QTemporaryDir sharedCache;
    QVERIFY(sharedCache.isValid());
    const QString cacheSentinel = sharedCache.filePath(QStringLiteral("map-tiles.keep"));
    QVERIFY(writeSizedFile(cacheSentinel, 33));
    const QByteArray cacheBefore = fileContents(cacheSentinel);

    QString error;
    const QStringList elevation =
        ElevationSourceService::findSupportedFiles(source.path(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(elevation.size(), 5);
    QCOMPARE(QFileInfo(elevation.at(0)).fileName(), QStringLiteral("a.tif"));
    QCOMPARE(QFileInfo(elevation.at(1)).fileName(), QStringLiteral("c.TIFF"));
    QCOMPARE(QFileInfo(elevation.at(2)).fileName(), QStringLiteral("b.Dt2"));
    QCOMPARE(QFileInfo(elevation.at(3)).fileName(), QStringLiteral("a.dt1"));
    QCOMPARE(QFileInfo(elevation.at(4)).fileName(), QStringLiteral("z.DT0"));

    const QStringList rasters =
        ElevationSourceService::findNativeRasterCandidates(source.path(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(rasters.contains(source.filePath(QStringLiteral("ignored.hgt"))));
    QVERIFY(!rasters.contains(source.filePath(QStringLiteral("small.bin"))));
    QVERIFY(!rasters.contains(source.filePath(QStringLiteral("image.tif.aux.xml"))));
    QVERIFY(!rasters.contains(source.filePath(QStringLiteral("image.prj"))));
    QCOMPARE(fileContents(cacheSentinel), cacheBefore);
}

void ConfigElevationSourcesViewTest::runtimeBackendIsDynamicallyLoadedAndIsolatesCorruptFiles()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString corrupt = source.filePath(QStringLiteral("corrupt.tif"));
    const QString unicodeDirectory = source.filePath(
        QStringLiteral("Данные с пробелом"));
    QVERIFY(QDir().mkpath(unicodeDirectory));
    const QString valid = QDir(unicodeDirectory).filePath(
        QStringLiteral("рельеф.TIFF"));
    QVERIFY(writeSizedFile(corrupt, 2048));
    QVERIFY(writeSyntheticGeoTiff(valid));
    const QByteArray before = fileContents(corrupt);
    DefaultLocaleGuard locale(
        QLocale(QLocale::German, QLocale::Germany));

    ElevationSourceService service;
    QSignalSpy finished(&service, &ElevationSourceService::scanFinished);
    QSignalSpy failed(&service, &ElevationSourceService::scanFailed);
    QSignalSpy cancelled(&service, &ElevationSourceService::scanCancelled);
    QVERIFY(service.startScan(source.path()));
    QTRY_VERIFY_WITH_TIMEOUT(
        finished.count() + failed.count() + cancelled.count() == 1, 15000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(finished.count(), 1);
    const ElevationSourcesScanResult result =
        qvariant_cast<ElevationSourcesScanResult>(finished.at(0).at(0));
    QCOMPARE(result.Files.size(), 2);
    QCOMPARE(result.Files.first().FullPath, corrupt);
    QCOMPARE(result.Files.first().State(), QStringLiteral("Error"));
    QVERIFY(!result.Files.first().Error.isEmpty());
    const ElevationSourceFile validRow = result.Files.at(1);
    QCOMPARE(validRow.FullPath, valid);
    double altitude = 0.0;
    if (result.NativeGdalAvailable) {
        QCOMPARE(validRow.State(), QStringLiteral("Indexed"));
        QVERIFY(validRow.Coverage.contains(QChar(0x00D7)));
        QVERIFY(validRow.Coverage.contains(QLatin1Char(',')));
        QVERIFY(service.sampleAltitude(34.995, 30.005, &altitude));
        QVERIFY(std::isfinite(altitude));
        QVERIFY(altitude >= 90.0 && altitude <= 140.0);
        QCOMPARE(result.RasterIndexedCount(), 1);
    } else {
        QCOMPARE(validRow.State(), QStringLiteral("Error"));
        QVERIFY(!service.sampleAltitude(34.995, 30.005, &altitude));
    }
    QCOMPARE(fileContents(corrupt), before);
}

void ConfigElevationSourcesViewTest::nativeGdalRendererProducesTransparentOverlayTiles()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeSyntheticGeoTiff(
        source.filePath(QStringLiteral("local-raster.tif"))));

    ElevationSourceService service;
    QSignalSpy finished(&service, &ElevationSourceService::scanFinished);
    QSignalSpy changed(&service,
                       &ElevationSourceService::nativeRastersChanged);
    QVERIFY(service.startScan(source.path()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
    const ElevationSourcesScanResult result =
        qvariant_cast<ElevationSourcesScanResult>(finished.at(0).at(0));
    if (!result.NativeGdalAvailable) {
        QSKIP("Native GDAL runtime is not installed.");
    }
    QCOMPARE(result.RasterIndexedCount(), 1);
    QCOMPARE(changed.count(), 1);

    constexpr int zoom = 14;
    constexpr double pi = 3.14159265358979323846;
    constexpr double longitude = 30.005;
    constexpr double latitude = 34.995;
    const int tileCount = 1 << zoom;
    const int tileX = static_cast<int>(std::floor(
        (longitude + 180.0) / 360.0 * tileCount));
    const double latitudeRadians = latitude * pi / 180.0;
    const int tileY = static_cast<int>(std::floor(
        (1.0 - std::asinh(std::tan(latitudeRadians)) / pi)
        * 0.5 * tileCount));

    const QByteArray overlay = service.renderRasterTile(
        tileX, tileY, zoom);
    QVERIFY(!overlay.isEmpty());
    const QImage image = QImage::fromData(overlay, "PNG")
        .convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(image.size(), QSize(256, 256));
    int opaquePixels = 0;
    int transparentPixels = 0;
    int minimumGray = 255;
    int maximumGray = 0;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const uchar *pixel = line + x * 4;
            if (pixel[3] != 0) {
                ++opaquePixels;
                QCOMPARE(pixel[0], pixel[1]);
                QCOMPARE(pixel[1], pixel[2]);
                minimumGray = qMin(minimumGray, static_cast<int>(pixel[0]));
                maximumGray = qMax(maximumGray, static_cast<int>(pixel[0]));
            } else {
                ++transparentPixels;
            }
        }
    }
    QVERIFY(opaquePixels > 0);
    QVERIFY(transparentPixels > 0);
    QVERIFY(minimumGray >= 80);
    QVERIFY(maximumGray <= 150);
    QVERIFY(maximumGray > minimumGray);
    QVERIFY(service.renderRasterTile(0, 0, zoom).isEmpty());

    service.unloadNativeRasters();
    QCOMPARE(changed.count(), 2);
    QVERIFY(service.renderRasterTile(tileX, tileY, zoom).isEmpty());
}

void ConfigElevationSourcesViewTest::nativeGdalRendererHonorsExplicitAlpha()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeSyntheticRgbaGeoTiff(
        source.filePath(QStringLiteral("rgba.tif")), 4, 4,
        QColor(200, 100, 50, 128)));

    ElevationSourceService service;
    QSignalSpy finished(&service, &ElevationSourceService::scanFinished);
    QVERIFY(service.startScan(source.path()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
    const ElevationSourcesScanResult result =
        qvariant_cast<ElevationSourcesScanResult>(finished.at(0).at(0));
    if (!result.NativeGdalAvailable) {
        QSKIP("Native GDAL runtime is not installed.");
    }
    QCOMPARE(result.RasterIndexedCount(), 1);

    constexpr int zoom = 14;
    constexpr double pi = 3.14159265358979323846;
    const int tileCount = 1 << zoom;
    const int tileX = static_cast<int>(std::floor(
        (30.01 + 180.0) / 360.0 * tileCount));
    const double latitudeRadians = 34.99 * pi / 180.0;
    const int tileY = static_cast<int>(std::floor(
        (1.0 - std::asinh(std::tan(latitudeRadians)) / pi)
        * 0.5 * tileCount));
    const QImage image = QImage::fromData(
        service.renderRasterTile(tileX, tileY, zoom), "PNG")
        .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!image.isNull());

    int maximumAlpha = 0;
    QColor strongest;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > maximumAlpha) {
                maximumAlpha = pixel.alpha();
                strongest = pixel;
            }
        }
    }
    QVERIFY(maximumAlpha >= 120 && maximumAlpha <= 136);
    QVERIFY(qAbs(strongest.red() - 200) <= 2);
    QVERIFY(qAbs(strongest.green() - 100) <= 2);
    QVERIFY(qAbs(strongest.blue() - 50) <= 2);
}

void ConfigElevationSourcesViewTest::nativeGdalRendererPrefersFineRasterOnOverlap()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeSyntheticRgbaGeoTiff(
        source.filePath(QStringLiteral("coarse.tif")), 2, 2,
        QColor(220, 30, 20, 255)));
    QVERIFY(writeSyntheticRgbaGeoTiff(
        source.filePath(QStringLiteral("fine.tif")), 8, 8,
        QColor(20, 80, 220, 255)));

    ElevationSourceService service;
    QSignalSpy finished(&service, &ElevationSourceService::scanFinished);
    QVERIFY(service.startScan(source.path()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
    const ElevationSourcesScanResult result =
        qvariant_cast<ElevationSourcesScanResult>(finished.at(0).at(0));
    if (!result.NativeGdalAvailable) {
        QSKIP("Native GDAL runtime is not installed.");
    }
    QCOMPARE(result.RasterIndexedCount(), 2);

    constexpr int zoom = 14;
    constexpr double pi = 3.14159265358979323846;
    const int tileCount = 1 << zoom;
    const int tileX = static_cast<int>(std::floor(
        (30.01 + 180.0) / 360.0 * tileCount));
    const double latitudeRadians = 34.99 * pi / 180.0;
    const int tileY = static_cast<int>(std::floor(
        (1.0 - std::asinh(std::tan(latitudeRadians)) / pi)
        * 0.5 * tileCount));
    const QImage image = QImage::fromData(
        service.renderRasterTile(tileX, tileY, zoom), "PNG")
        .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!image.isNull());

    int finePixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 255 && pixel.blue() >= 210
                && pixel.red() <= 30) {
                ++finePixels;
            }
        }
    }
    QVERIFY(finePixels > 0);
}

void ConfigElevationSourcesViewTest::startupRestorationIsBackgroundAndCancellable()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    for (int index = 0; index < 100; ++index) {
        QVERIFY(writeSizedFile(
            source.filePath(QStringLiteral("source-%1.tif").arg(index)), 8));
    }
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("GDALImageDir"), source.path());
    settings.sync();

    ElevationSourceService service;
    QSignalSpy started(&service, &ElevationSourceService::scanStarted);
    QSignalSpy cancelled(&service, &ElevationSourceService::scanCancelled);
    QSignalSpy finished(&service, &ElevationSourceService::scanFinished);
    service.initializeFromSettings();
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.at(0).at(0).toBool(), true);
    QVERIFY(service.isBusy());
    service.cancelScan();
    QTRY_VERIFY_WITH_TIMEOUT(cancelled.count() + finished.count() == 1, 15000);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(settings.value(QStringLiteral("GDALImageDir")).toString(),
             source.path());
}

void ConfigElevationSourcesViewTest::startupFailureIsRetainedForLazyViewModel()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    const QString missing = QDir(m_settingsDirectory->path()).filePath(
        QStringLiteral("missing-startup-directory"));
    settings.setValue(QStringLiteral("GDALImageDir"), missing);
    settings.sync();

    ElevationSourceService service;
    QSignalSpy failed(&service, &ElevationSourceService::scanFailed);
    service.initializeFromSettings();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 15000);
    QCOMPARE(failed.at(0).at(1).toBool(), true);
    QVERIFY(!service.startupError().isEmpty());

    ConfigElevationSourcesViewModel model(&service, &settings);
    QCOMPARE(model.DirectoryPath(), missing);
    QVERIFY(model.Status().startsWith(QStringLiteral(
        "Saved elevation directory failed to load:")));
    QVERIFY(model.Status().contains(missing));
}

void ConfigElevationSourcesViewTest::validationPersistenceRestartAndCancelMatchReference()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("unrelated"), QStringLiteral("keep"));
    settings.sync();
    const QByteArray beforeInvalid = fileContents(settings.fileName());
    FakeElevationSourceService service;
    ConfigElevationSourcesViewModel model(&service, &settings);

    model.SetDirectoryPath(QStringLiteral("   "));
    model.Rescan();
    QVERIFY(model.Status().startsWith(QStringLiteral("Local-source scan failed:")));
    QCOMPARE(service.startCount, 0);
    QCOMPARE(fileContents(settings.fileName()), beforeInvalid);

    QTemporaryDir source;
    QVERIFY(source.isValid());
    model.SetDirectoryPath(source.path() + QStringLiteral("/./"));
    model.Rescan();
    QCOMPARE(service.startCount, 1);
    QCOMPARE(service.startedDirectory, QDir::cleanPath(source.path()));
    QCOMPARE(settings.value(QStringLiteral("GDALImageDir")).toString(),
             QDir::cleanPath(source.path()));
    QCOMPARE(settings.value(QStringLiteral("unrelated")).toString(),
             QStringLiteral("keep"));
    QVERIFY(model.IsBusy());
    QCOMPARE(model.Status(), QStringLiteral(
        "Discovering local elevation and GDAL raster-map files…"));

    service.report({ElevationSourcesScanProgress::Stage::Elevation,
                    0, 2, source.filePath(QStringLiteral("one.tif"))});
    QCOMPARE(model.Status(), QStringLiteral("Indexing 1/2: one.tif"));
    service.report({ElevationSourcesScanProgress::Stage::NativeGdal,
                    0, 3, source.filePath(QStringLiteral("overlay.tif"))});
    QCOMPARE(model.Progress(), 0);
    QCOMPARE(model.ProgressMaximum(), 5);
    model.Cancel();
    QCOMPARE(service.cancelCount, 1);
    QCOMPARE(model.Status(), QStringLiteral(
        "Cancellation requested; finishing the current file…"));
    service.cancelFinished();
    QVERIFY(!model.IsBusy());
    QCOMPARE(model.Status(), QStringLiteral(
        "Local-source indexing cancelled. The previously complete indexes remain active."));

    QTemporaryDir otherSource;
    QVERIFY(otherSource.isValid());
    service.restartRequired = true;
    service.restartDirectory = otherSource.path();
    model.Rescan();
    QCOMPARE(service.startCount, 2);
    service.cancelFinished();
    model.SetDirectoryPath(otherSource.path());
    model.Rescan();
    QCOMPARE(service.startCount, 2);
    QCOMPARE(model.Status(), QStringLiteral(
        "The new elevation directory is saved. Restart Mission Planner to unload the currently active DEM index and switch sources safely."));
}

void ConfigElevationSourcesViewTest::resultAndClearPreserveManagedIndexAndUnrelatedState()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString sourceFile = source.filePath(QStringLiteral("terrain.tif"));
    const QString rasterFile = source.filePath(QStringLiteral("overlay.tif"));
    QVERIFY(writeSizedFile(sourceFile, 2048));
    QVERIFY(writeSizedFile(rasterFile, 2048));
    const QByteArray sourceBefore = fileContents(sourceFile);
    const QByteArray rasterBefore = fileContents(rasterFile);
    QTemporaryDir sharedCache;
    QVERIFY(sharedCache.isValid());
    const QString tileCacheSentinel = sharedCache.filePath(
        QStringLiteral("map-tiles/shared.cache"));
    QVERIFY(QDir().mkpath(QFileInfo(tileCacheSentinel).absolutePath()));
    QVERIFY(writeSizedFile(tileCacheSentinel, 55));
    const QByteArray tileCacheBefore = fileContents(tileCacheSentinel);

    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("MapType"), QStringLiteral("GDAL Custom"));
    settings.setValue(QStringLiteral("futureSetting"), 99);
    FakeElevationSourceService service;
    ConfigElevationSourcesViewModel model(&service, &settings);
    QSignalSpy refresh(&model,
                       &ConfigElevationSourcesViewModel::gdalMapRefreshRequested);
    model.SelectAndScan(source.path());

    ElevationSourcesScanResult result;
    result.Directory = QDir::cleanPath(source.path());
    result.Backend = QStringLiteral("GDAL 3.test via fake");
    result.NativeGdalAvailable = true;
    result.ExaminedRasterFiles = 2;
    result.UnrecognizedRasterFiles = 1;
    result.Files.append({sourceFile, QStringLiteral("GeoTIFF"), true,
                         QStringLiteral("10×20; coverage"), QString()});
    result.RasterFiles.append({rasterFile, QStringLiteral("GTiff"), true,
                               QStringLiteral("10 × 20, 3 band(s)"),
                               QStringLiteral("1,2 → 3,4"), QString()});
    service.finish(result);

    QCOMPARE(model.Files().size(), 1);
    QCOMPARE(model.RasterFiles().size(), 1);
    QCOMPARE(model.Progress(), 1);
    QCOMPARE(model.ProgressMaximum(), 1);
    QVERIFY(model.Status().contains(QStringLiteral("Indexed 1/1 file(s)")));
    QVERIFY(model.NativeGdalStatus().contains(
        QStringLiteral("Select GDAL Custom in Flight Planner")));
    QCOMPARE(refresh.count(), 1);

    model.SelectAndScan(source.path());
    service.fail(QStringLiteral("synthetic scan failure"));
    QCOMPARE(model.Files().size(), 1);
    QCOMPARE(model.RasterFiles().size(), 1);
    QVERIFY(model.Status().contains(QStringLiteral("synthetic scan failure")));
    model.SelectAndScan(source.path());
    model.Cancel();
    service.cancelFinished();
    QCOMPARE(model.Files().size(), 1);
    QCOMPARE(model.RasterFiles().size(), 1);

    model.ClearSaved(false);
    QCOMPARE(service.unloadCount, 0);
    QCOMPARE(model.RasterFiles().size(), 1);
    model.ClearSaved(true);
    QCOMPARE(service.unloadCount, 1);
    QCOMPARE(model.DirectoryPath(), QString());
    QCOMPARE(settings.value(QStringLiteral("GDALImageDir")).toString(),
             QString());
    QCOMPARE(model.Files().size(), 1);
    QVERIFY(model.RasterFiles().isEmpty());
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("GDAL Custom"));
    QCOMPARE(settings.value(QStringLiteral("futureSetting")).toInt(), 99);
    QCOMPARE(fileContents(sourceFile), sourceBefore);
    QCOMPARE(fileContents(rasterFile), rasterBefore);
    QCOMPARE(fileContents(tileCacheSentinel), tileCacheBefore);
}

void ConfigElevationSourcesViewTest::viewMatchesMissionPlannerContract()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    FakeElevationSourceService service;
    ConfigElevationSourcesViewModel model(&service, &settings);
    ConfigElevationSourcesView view(&model);
    view.resize(1200, 760);

    auto *root = qobject_cast<QGridLayout *>(view.layout());
    QVERIFY(root);
    QCOMPARE(root->contentsMargins(), QMargins(16, 16, 16, 16));
    QCOMPARE(root->verticalSpacing(), 10);

    auto *title = view.findChild<QLabel *>(
        QStringLiteral("elevationSourcesTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("Elevation & Local Raster Sources"));
    auto *description = view.findChild<QLabel *>(
        QStringLiteral("elevationSourcesDescription"));
    QVERIFY(description);
    QVERIFY(description->text().startsWith(QStringLiteral(
        "Load local GeoTIFF/DTED elevation models")));
    auto *edit = view.findChild<QLineEdit *>(
        QStringLiteral("directoryPathTextBox"));
    QVERIFY(edit);
    QCOMPARE(edit->placeholderText(), QStringLiteral(
        "Directory containing elevation or georeferenced raster files"));
    auto *tabs = view.findChild<QTabWidget *>(
        QStringLiteral("elevationSourcesTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Elevation (GeoTIFF / DTED)"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("GDAL Custom map rasters"));

    auto *elevation = view.findChild<QTableWidget *>(
        QStringLiteral("elevationSourcesTable"));
    auto *raster = view.findChild<QTableWidget *>(
        QStringLiteral("nativeGdalRasterTable"));
    QVERIFY(elevation);
    QVERIFY(raster);
    QCOMPARE(elevation->columnCount(), 6);
    QCOMPARE(raster->columnCount(), 7);
    QCOMPARE(elevation->horizontalHeaderItem(3)->text(),
             QStringLiteral("Coverage / size"));
    QCOMPARE(raster->horizontalHeaderItem(1)->text(),
             QStringLiteral("Driver"));
    QCOMPARE(elevation->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(elevation->isSortingEnabled());
    QCOMPARE(elevation->columnWidth(0), 200);
    QCOMPARE(raster->columnWidth(4), 320);

    auto *progress = view.findChild<QProgressBar *>(
        QStringLiteral("elevationSourcesProgress"));
    auto *cancel = view.findChild<QPushButton *>(QStringLiteral("cancelButton"));
    auto *browse = view.findChild<QPushButton *>(QStringLiteral("BrowseButton"));
    QVERIFY(progress);
    QVERIFY(cancel);
    QVERIFY(browse);
    QCOMPARE(progress->height(), 15);
    QVERIFY(!progress->isTextVisible());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(browse->isEnabled());

    QTemporaryDir source;
    QVERIFY(source.isValid());
    model.SelectAndScan(source.path());
    QVERIFY(cancel->isEnabled());
    QVERIFY(!browse->isEnabled());
    service.cancelFinished();
}

void ConfigElevationSourcesViewTest::routeIsAdvancedOfflineLazySubpage()
{
    const BackstagePage page = configElevationSourcesBackstagePage();
    QCOMPARE(page.id, QStringLiteral("ConfigElevationSourcesView"));
    QCOMPARE(page.header, QStringLiteral("Elevation Sources"));
    QVERIFY(page.isSub);
    QVERIFY(page.isAdvanced);
    QVERIFY(!page.requiresConnection);
    QVERIFY(!page.allowsPartialParameters);
    QVERIFY(bool(page.factory));

    QWidget parent;
    QWidget *created = page.factory(&parent);
    QVERIFY(created);
    QCOMPARE(created->objectName(), QStringLiteral("ConfigElevationSourcesView"));
    QCOMPARE(created->parentWidget(), &parent);
    delete created;
}

QTEST_MAIN(ConfigElevationSourcesViewTest)
#include "test_configelevationsourcesview.moc"
