#include "ui/Loghandling/LogIndexTileCache.h"
#include "pureimagecache.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

class LogIndexTileCacheTest final : public QObject
{
    Q_OBJECT
private slots:
    void readsOnlyCanonicalLayout();
    void missesDoNotCreateCacheDirectories();
    void rejectsOversizedAndLinkedTiles();
};

void LogIndexTileCacheTest::readsOnlyCanonicalLayout()
{
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    const auto type = core::MapType::GoogleHybrid;
    const QString path = core::PureImageCache::sharedTilePath(
        cache.path(), type, core::Point(4, 5), 6);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes = QByteArray::fromHex("00ff7e424300");
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
    const auto modified = QFileInfo(path).lastModified();
    const auto reader = LogIndexTileCache::reader(cache.path(), type);
    QVERIFY(reader);
    QCOMPARE(reader(4, 5, 6), bytes);
    QVERIFY(reader(5, 4, 6).isEmpty());
    QVERIFY(reader(-1, 5, 6).isEmpty());
    QVERIFY(reader(64, 5, 6).isEmpty());
    QVERIFY(reader(4, 5, 22).isEmpty());
    QVERIFY(reader(4, 5, -1).isEmpty());
    QCOMPARE(QFileInfo(path).lastModified(), modified);
    QStringList files;
    QDirIterator iterator(cache.path(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) files.append(iterator.next());
    QCOMPARE(files, QStringList{path});
}

void LogIndexTileCacheTest::missesDoNotCreateCacheDirectories()
{
    QTemporaryDir directory;
    const QString missing = directory.filePath(QStringLiteral("uncreated shared cache"));
    const auto reader = LogIndexTileCache::reader(missing, core::MapType::OpenStreetMap);
    QVERIFY(reader);
    QVERIFY(reader(0, 0, 0).isEmpty());
    QVERIFY(!QFileInfo::exists(missing));
}

void LogIndexTileCacheTest::rejectsOversizedAndLinkedTiles()
{
    QTemporaryDir directory;
    const auto type = core::MapType::BingSatellite;
    const QString path = core::PureImageCache::sharedTilePath(
        directory.path(), type, core::Point(0, 0), 1);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(32LL * 1024 * 1024 + 1));
    file.close();
    const auto reader = LogIndexTileCache::reader(directory.path(), type);
    QVERIFY(reader(0, 0, 1).isEmpty());
#ifdef Q_OS_UNIX
    const QString linked = core::PureImageCache::sharedTilePath(
        directory.path(), type, core::Point(1, 0), 1);
    const QString small = directory.filePath(QStringLiteral("source"));
    QFile source(small);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("tile"), qint64(4));
    source.close();
    QVERIFY(QDir().mkpath(QFileInfo(linked).absolutePath()));
    QVERIFY(QFile::link(small, linked));
    QVERIFY(reader(1, 0, 1).isEmpty());
    QVERIFY(QFileInfo::exists(small));
#endif
}

QTEST_MAIN(LogIndexTileCacheTest)
#include "test_logindextilecache.moc"
