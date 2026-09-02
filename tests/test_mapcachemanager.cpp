#include <QtTest>

#include "MapCacheManager.h"
#include "point.h"
#include "pureimagecache.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLockFile>
#include <QTemporaryDir>

namespace {
QByteArray imageBytes(const char *format, QRgb color)
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)
        || !image.save(&buffer, format, 95)) {
        return QByteArray();
    }
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size();
}

bool setModified(const QString &path, const QDateTime &time)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    return file.setFileTime(time, QFileDevice::FileModificationTime);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class MapCacheManagerTest final : public QObject
{
    Q_OBJECT

private slots:
    void scansProvidersAndFormatsSizes();
    void deletesOldOrAllTilesWithinCanonicalRoot();
    void refusesOutsideTargetsAndHonorsMaintenanceLock();
    void preservesForeignFilesAndProviderDirectories();
    void parsesOfficialInjectionPaths();
    void importsImagesAndReplacesExistingTilesDeterministically();
    void firstFullLexicographicDuplicateWins();
    void cancellationPreservesAlreadyImportedTiles();
    void skipsOversizedTilesAndSourceSymlinks();
    void refusesDestinationSymlinkAndHonorsImportLock();
    void refusesUnsupportedProvidersAndOverlappingRoots();
};

void MapCacheManagerTest::scansProvidersAndFormatsSizes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString a = root.filePath(QStringLiteral("provider-a/1/2/a.tile"));
    const QString b = root.filePath(QStringLiteral("provider-a/1/2/b.tile"));
    const QString c = root.filePath(QStringLiteral("provider-b/c.tile"));
    QVERIFY(writeFile(a, QByteArray(3, 'a')));
    QVERIFY(writeFile(b, QByteArray(4, 'b')));
    QVERIFY(writeFile(c, QByteArray(2, 'c')));

    const QList<MapCacheSnapshot> snapshots = MapCacheManager::scan(root.path());
    QCOMPARE(snapshots.size(), 3);
    QCOMPARE(snapshots.at(0).name, QStringLiteral("provider-a"));
    QCOMPARE(snapshots.at(0).fileCount, qint64(2));
    QCOMPARE(snapshots.at(0).sizeBytes, qint64(7));
    QCOMPARE(snapshots.at(1).name, QStringLiteral("provider-b"));
    QVERIFY(snapshots.at(2).isTotal);
    QCOMPARE(snapshots.at(2).name, QStringLiteral("Total"));
    QCOMPARE(snapshots.at(2).fileCount, qint64(3));
    QCOMPARE(snapshots.at(2).sizeBytes, qint64(9));

    QCOMPARE(MapCacheManager::formatBytes(-1), QStringLiteral("0 B"));
    QCOMPARE(MapCacheManager::formatBytes(1023), QStringLiteral("1023 B"));
    QCOMPARE(MapCacheManager::formatBytes(1024), QStringLiteral("1 KiB"));
    QCOMPARE(MapCacheManager::formatBytes(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(MapCacheManager::formatBytes(5 * 1024 * 1024),
             QStringLiteral("5 MiB"));
}

void MapCacheManagerTest::deletesOldOrAllTilesWithinCanonicalRoot()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());

    const QString oldTile = root.filePath(
        QStringLiteral("provider-a/1/2/old.tile"));
    const QString newTile = root.filePath(
        QStringLiteral("provider-a/1/2/new.tile"));
    const QString otherTile = root.filePath(
        QStringLiteral("provider-b/other.tile"));
    QVERIFY(writeFile(oldTile, QByteArray(3, 'o')));
    QVERIFY(writeFile(newTile, QByteArray(4, 'n')));
    QVERIFY(writeFile(otherTile, QByteArray(2, 'x')));
    QVERIFY(setModified(oldTile,
                        QDateTime::currentDateTimeUtc().addDays(-40)));

    const QString outsideFile = outside.filePath(QStringLiteral("keep.tile"));
    QVERIFY(writeFile(outsideFile, QByteArrayLiteral("outside")));
    const QString link = root.filePath(
        QStringLiteral("provider-a/1/2/outside-link.tile"));
    const bool symlinkCreated = QFile::link(outsideFile, link);

    QList<MapCacheSnapshot> snapshots = MapCacheManager::scan(root.path());
    const MapCacheSnapshot provider = snapshots.at(0);
    QCOMPARE(provider.fileCount, qint64(2));
    const MapCacheDeleteResult oldResult = MapCacheManager::deleteOlderThan(
        provider, QDateTime::currentDateTimeUtc().addDays(-30), root.path());
    QCOMPARE(oldResult.removedFiles, qint64(1));
    QCOMPARE(oldResult.freedBytes, qint64(3));
    QCOMPARE(oldResult.failedFiles, qint64(0));
    QVERIFY(oldResult.error.isEmpty());
    QVERIFY(!QFile::exists(oldTile));
    QVERIFY(QFile::exists(newTile));

    snapshots = MapCacheManager::scan(root.path());
    const MapCacheDeleteResult allResult = MapCacheManager::deleteAll(
        snapshots.constLast(), root.path());
    QCOMPARE(allResult.removedFiles, qint64(2));
    QCOMPARE(allResult.freedBytes, qint64(6));
    QVERIFY(QFile::exists(outsideFile));
    if (symlinkCreated) {
        QVERIFY(QFileInfo(link).isSymLink());
    }
}

void MapCacheManagerTest::refusesOutsideTargetsAndHonorsMaintenanceLock()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideTile = outside.filePath(QStringLiteral("tile.bin"));
    QVERIFY(writeFile(outsideTile, QByteArrayLiteral("keep")));

    MapCacheSnapshot forged;
    forged.name = QStringLiteral("outside");
    forged.path = outside.path();
    const MapCacheDeleteResult refused = MapCacheManager::deleteAll(
        forged, root.path());
    QVERIFY(!refused.error.isEmpty());
    QVERIFY(QFile::exists(outsideTile));

    const QString linkedRoot = root.filePath(QStringLiteral("linked-root"));
    const bool rootLinkCreated = QFile::link(outside.path(), linkedRoot);
    if (rootLinkCreated) {
        const QList<MapCacheSnapshot> linkedScan =
            MapCacheManager::scan(linkedRoot);
        QCOMPARE(linkedScan.size(), 1);
        QVERIFY(linkedScan.constLast().isTotal);
        QCOMPARE(linkedScan.constLast().fileCount, qint64(0));
        const MapCacheDeleteResult linkedDelete =
            MapCacheManager::deleteAll(linkedScan.constLast(), linkedRoot);
        QVERIFY(!linkedDelete.error.isEmpty());
        QVERIFY(QFile::exists(outsideTile));
    }

    const QString providerTile = root.filePath(
        QStringLiteral("provider/tile.bin"));
    QVERIFY(writeFile(providerTile, QByteArrayLiteral("locked")));
    const MapCacheSnapshot provider = MapCacheManager::scan(root.path()).first();
    QLockFile lock(root.filePath(QStringLiteral(".maintenance.lock")));
    QVERIFY(lock.tryLock(0));
    const MapCacheDeleteResult busy = MapCacheManager::deleteAll(
        provider, root.path());
    QVERIFY(busy.maintenanceBusy);
    QCOMPARE(busy.removedFiles, qint64(0));
    QVERIFY(QFile::exists(providerTile));
}

void MapCacheManagerTest::preservesForeignFilesAndProviderDirectories()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString provider = root.filePath(QStringLiteral("provider"));
    const QString tile = QDir(provider).filePath(
        QStringLiteral("3/4/5.tile"));
    const QString foreign = QDir(provider).filePath(
        QStringLiteral("keep.txt"));
    QVERIFY(writeFile(tile, QByteArrayLiteral("tile")));
    QVERIFY(writeFile(foreign, QByteArrayLiteral("foreign")));

    QList<MapCacheSnapshot> snapshots = MapCacheManager::scan(root.path());
    QCOMPARE(snapshots.first().fileCount, qint64(1));
    MapCacheDeleteResult result = MapCacheManager::deleteAll(
        snapshots.constLast(), root.path());
    QCOMPARE(result.removedFiles, qint64(1));
    QVERIFY(QFile::exists(foreign));
    QVERIFY(QDir(provider).exists());

    MapCacheSnapshot forgedNested;
    forgedNested.name = QStringLiteral("nested");
    forgedNested.path = QDir(provider).filePath(QStringLiteral("nested"));
    QVERIFY(QDir().mkpath(forgedNested.path));
    const QString nestedTile = QDir(forgedNested.path).filePath(
        QStringLiteral("forged.tile"));
    QVERIFY(writeFile(nestedTile, QByteArrayLiteral("keep")));
    result = MapCacheManager::deleteAll(forgedNested, root.path());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(QFile::exists(nestedTile));
}

void MapCacheManagerTest::parsesOfficialInjectionPaths()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    MapTileIndex index;
    QVERIFY(MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("survey/Z12/1362/2048.jpg")),
        &index));
    QCOMPARE(index.zoom, 12);
    QCOMPARE(index.column, 2048);
    QCOMPARE(index.row, 1362);

    QVERIFY(!MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("Z12/5000/2048.jpg")),
        &index));
    QVERIFY(!MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("12/1362/2048.jpg")),
        &index));
    QVERIFY(!MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("Z22/1/1.png")), &index));
    QVERIFY(!MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("Z2/+1/1.png")), &index));
    QVERIFY(!MapTileImporter::tryParseOfficialPath(
        root.path(), root.filePath(QStringLiteral("../Z12/1362/2048.jpg")),
        &index));
}

void MapCacheManagerTest::importsImagesAndReplacesExistingTilesDeterministically()
{
    QTemporaryDir source;
    QTemporaryDir cacheRoot;
    QVERIFY(source.isValid());
    QVERIFY(cacheRoot.isValid());
    const QByteArray jpeg = imageBytes("JPEG", qRgb(255, 64, 32));
    const QByteArray png = imageBytes("PNG", qRgb(48, 96, 192));
    QVERIFY(!jpeg.isEmpty());
    QVERIFY(!png.isEmpty());

    QVERIFY(writeFile(source.filePath(QStringLiteral("a/Z3/2/4.jpg")), jpeg));
    QVERIFY(writeFile(source.filePath(QStringLiteral("a/Z3/2/5.png")), png));
    QVERIFY(writeFile(source.filePath(QStringLiteral("a/Z3/2/6.jpg")),
                      QByteArrayLiteral("not an image")));
    QVERIFY(writeFile(source.filePath(QStringLiteral("a/Z3/20/7.jpg")), jpeg));
    QVERIFY(writeFile(source.filePath(QStringLiteral("b/Z3/2/4.png")), png));
    QVERIFY(writeFile(source.filePath(QStringLiteral("a/Z3/2/ignored.txt")),
                      QByteArrayLiteral("not a tile")));

    const core::Point replacedPosition(4, 2);
    core::PureImageCache existing(cacheRoot.path());
    QVERIFY(existing.PutImageToCache(
        png, core::MapType::GoogleSatellite, replacedPosition, 3));

    QList<MapTileImportProgress> progress;
    const MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::GoogleSatellite, cacheRoot.path(),
        [&progress](const MapTileImportProgress &value) {
            progress.append(value);
        });

    QVERIFY(result.error.isEmpty());
    QVERIFY(!result.canceled);
    QCOMPARE(result.discovered, qint64(5));
    QCOMPARE(result.imported, qint64(2));
    QCOMPARE(result.skipped, qint64(3));
    QCOMPARE(result.failed, qint64(0));
    QCOMPARE(result.importedBytes, qint64(jpeg.size() + png.size()));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.constLast().discovered, result.discovered);

    QCOMPARE(readFile(core::PureImageCache::sharedTilePath(
                 cacheRoot.path(), core::MapType::GoogleSatellite,
                 replacedPosition, 3)),
             jpeg);
    QCOMPARE(readFile(core::PureImageCache::sharedTilePath(
                 cacheRoot.path(), core::MapType::GoogleSatellite,
                 core::Point(5, 2), 3)),
             png);
    QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
        cacheRoot.path(), core::MapType::GoogleSatellite,
        core::Point(6, 2), 3)));
}

void MapCacheManagerTest::firstFullLexicographicDuplicateWins()
{
    QTemporaryDir source;
    QTemporaryDir cacheRoot;
    QVERIFY(source.isValid());
    QVERIFY(cacheRoot.isValid());
    const QByteArray first = imageBytes("PNG", qRgb(10, 20, 30));
    const QByteArray later = imageBytes("PNG", qRgb(200, 210, 220));
    const QString lexicographicallyFirst =
        source.filePath(QStringLiteral("Z2/1/0/Z2/1/1.png"));
    const QString lexicographicallyLater =
        source.filePath(QStringLiteral("Z2/1/1.png"));
    QVERIFY(lexicographicallyFirst < lexicographicallyLater);
    QVERIFY(writeFile(lexicographicallyFirst, first));
    QVERIFY(writeFile(lexicographicallyLater, later));

    const MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::GoogleHybrid, cacheRoot.path());
    QCOMPARE(result.discovered, qint64(2));
    QCOMPARE(result.imported, qint64(1));
    QCOMPARE(result.skipped, qint64(1));
    QCOMPARE(readFile(core::PureImageCache::sharedTilePath(
                 cacheRoot.path(), core::MapType::GoogleHybrid,
                 core::Point(1, 1), 2)),
             first);
}

void MapCacheManagerTest::cancellationPreservesAlreadyImportedTiles()
{
    QTemporaryDir source;
    QTemporaryDir cacheRoot;
    QVERIFY(source.isValid());
    QVERIFY(cacheRoot.isValid());
    const QByteArray png = imageBytes("PNG", qRgb(12, 34, 56));
    QVERIFY(writeFile(source.filePath(QStringLiteral("Z2/1/0.png")), png));
    QVERIFY(writeFile(source.filePath(QStringLiteral("Z2/1/1.png")), png));

    bool cancel = false;
    const MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::OpenStreetMap, cacheRoot.path(),
        [&cancel](const MapTileImportProgress &progress) {
            if (progress.imported == 1) {
                cancel = true;
            }
        },
        [&cancel]() { return cancel; });

    QVERIFY(result.canceled);
    QCOMPARE(result.imported, qint64(1));
    QCOMPARE(readFile(core::PureImageCache::sharedTilePath(
                 cacheRoot.path(), core::MapType::OpenStreetMap,
                 core::Point(0, 1), 2)),
             png);
    QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
        cacheRoot.path(), core::MapType::OpenStreetMap,
        core::Point(1, 1), 2)));
}

void MapCacheManagerTest::skipsOversizedTilesAndSourceSymlinks()
{
    QTemporaryDir source;
    QTemporaryDir outside;
    QTemporaryDir cacheRoot;
    QVERIFY(source.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(cacheRoot.isValid());

    const QString oversized = source.filePath(QStringLiteral("Z1/0/0.png"));
    QVERIFY(QDir().mkpath(QFileInfo(oversized).absolutePath()));
    QFile largeFile(oversized);
    QVERIFY(largeFile.open(QIODevice::WriteOnly));
    QVERIFY(largeFile.resize(MapTileImporter::MaximumTileBytes + 1));
    largeFile.close();

    const QByteArray png = imageBytes("PNG", qRgb(9, 8, 7));
    QVERIFY(writeFile(outside.filePath(QStringLiteral("Z1/0/1.png")), png));
    const bool directoryLinkCreated = QFile::link(
        outside.path(), source.filePath(QStringLiteral("linked")));

    const MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::BingSatellite, cacheRoot.path());
    QCOMPARE(result.discovered, qint64(1));
    QCOMPARE(result.imported, qint64(0));
    QCOMPARE(result.skipped, qint64(1));
    if (directoryLinkCreated) {
        QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
            cacheRoot.path(), core::MapType::BingSatellite,
            core::Point(1, 0), 1)));
    }
}

void MapCacheManagerTest::refusesDestinationSymlinkAndHonorsImportLock()
{
    QTemporaryDir source;
    QTemporaryDir cacheRoot;
    QTemporaryDir outside;
    QVERIFY(source.isValid());
    QVERIFY(cacheRoot.isValid());
    QVERIFY(outside.isValid());
    const QByteArray png = imageBytes("PNG", qRgb(1, 2, 3));
    QVERIFY(writeFile(source.filePath(QStringLiteral("Z2/1/2.png")), png));

    QLockFile lock(cacheRoot.filePath(QStringLiteral(".maintenance.lock")));
    QVERIFY(lock.tryLock(0));
    MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::GoogleHybrid, cacheRoot.path());
    QVERIFY(result.maintenanceBusy);
    QCOMPARE(result.discovered, qint64(0));
    lock.unlock();

    const QString providerDirectory = cacheRoot.filePath(
        core::PureImageCache::providerCacheDirectory(
            core::MapType::GoogleHybrid));
    const bool providerLinkCreated = QFile::link(
        outside.path(), providerDirectory);
    if (providerLinkCreated) {
        result = MapTileImporter::importTiles(
            source.path(), core::MapType::GoogleHybrid, cacheRoot.path());
        QCOMPARE(result.discovered, qint64(1));
        QCOMPARE(result.imported, qint64(0));
        QCOMPARE(result.failed, qint64(1));
        QVERIFY(!QFile::exists(outside.filePath(
            QStringLiteral("2/2/1.tile"))));
    }
}

void MapCacheManagerTest::refusesUnsupportedProvidersAndOverlappingRoots()
{
    QTemporaryDir source;
    QTemporaryDir cacheRoot;
    QVERIFY(source.isValid());
    QVERIFY(cacheRoot.isValid());
    const QByteArray png = imageBytes("PNG", qRgb(4, 5, 6));
    QVERIFY(writeFile(source.filePath(QStringLiteral("Z1/0/0.png")), png));

    MapTileImportResult result = MapTileImporter::importTiles(
        source.path(), core::MapType::GDALCustom, cacheRoot.path());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.imported, qint64(0));

    const QString nestedSource = cacheRoot.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkpath(nestedSource));
    QVERIFY(writeFile(QDir(nestedSource).filePath(
                          QStringLiteral("Z1/0/0.png")), png));
    result = MapTileImporter::importTiles(
        nestedSource, core::MapType::OpenStreetMap, cacheRoot.path());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.imported, qint64(0));
}

QTEST_GUILESS_MAIN(MapCacheManagerTest)

#include "test_mapcachemanager.moc"
