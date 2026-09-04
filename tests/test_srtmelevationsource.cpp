#include <QtTest>

#include "ui/configuration/SrtmElevationSource.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <quazip.h>
#include <quazipfile.h>
#include <quazipnewinfo.h>

#include <cmath>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace {
bool writeSample(QFile *file, int size, int row, int column, qint16 value)
{
    if (!file || row < 0 || column < 0 || row >= size || column >= size) {
        return false;
    }
    const qint64 offset = (static_cast<qint64>(row) * size + column) * 2;
    const quint16 raw = static_cast<quint16>(value);
    const char bytes[2] = {
        static_cast<char>((raw >> 8) & 0xff),
        static_cast<char>(raw & 0xff)};
    return file->seek(offset) && file->write(bytes, 2) == 2;
}

bool writeSyntheticTile(const QString &path, int size, int row, int column)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || !file.resize(static_cast<qint64>(size) * size * 2)) {
        return false;
    }
    return writeSample(&file, size, row, column, 100)
        && writeSample(&file, size, row, column + 1, 200)
        && writeSample(&file, size, row + 1, column, 300)
        && writeSample(&file, size, row + 1, column + 1, 400);
}

QByteArray zipTile(const QString &tileName, const QByteArray &terrain)
{
    QByteArray archive;
    QBuffer buffer(&archive);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdCreate)) {
        return {};
    }
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(tileName))
        || file.write(terrain) != terrain.size()) {
        return {};
    }
    file.close();
    zip.close();
    return archive;
}
}

class SrtmElevationSourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void tileNamesUseSouthWestFlooring();
    void samplesSrtm3UsingBigEndianBilinearInterpolation();
    void acceptsSrtm1AndRejectsCorruptOrVoidTiles();
    void localCacheSamplingDoesNotRequireNetwork();
    void cacheOnlyMissNeverSchedulesNetwork();
    void requestAdmissionIsDeduplicatedBoundedAndThreadSafe();
    void downloadsAndAtomicallyInstallsArchive();
};

void SrtmElevationSourceTest::initTestCase()
{
    QCoreApplication::setOrganizationName(
        QStringLiteral("APMPlannerSrtmTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("SrtmElevationSource"));
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.clear();
    settings.setValue(QString::fromLatin1(
        SrtmElevationSource::AutoDownloadSettingsKey), false);
}

void SrtmElevationSourceTest::tileNamesUseSouthWestFlooring()
{
    QCOMPARE(SrtmElevationSource::TileName(35.1, 33.2),
             QStringLiteral("N35E033.hgt"));
    QCOMPARE(SrtmElevationSource::TileName(-0.1, -0.1),
             QStringLiteral("S01W001.hgt"));
    QCOMPARE(SrtmElevationSource::TileName(-35.2, 179.9),
             QStringLiteral("S36E179.hgt"));
    QVERIFY(SrtmElevationSource::TileName(90.0, 0.0).isEmpty());
    QVERIFY(SrtmElevationSource::TileName(0.0, 180.0).isEmpty());
    QVERIFY(SrtmElevationSource::TileName(
        std::numeric_limits<double>::quiet_NaN(), 0.0).isEmpty());
}

void SrtmElevationSourceTest::samplesSrtm3UsingBigEndianBilinearInterpolation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("N35E033.hgt"));
    QVERIFY(writeSyntheticTile(path, 1201, 600, 600));

    double altitude = 0.0;
    QVERIFY(SrtmElevationSource::SampleHgtFile(
        path, 35.4995, 33.5005, &altitude));
    QVERIFY(qAbs(altitude - 280.0) < 0.001);
}

void SrtmElevationSourceTest::acceptsSrtm1AndRejectsCorruptOrVoidTiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fine = directory.filePath(QStringLiteral("fine.hgt"));
    QVERIFY(writeSyntheticTile(fine, 3601, 600, 600));
    double altitude = 0.0;
    QVERIFY(SrtmElevationSource::SampleHgtFile(
        fine, 35.833166666667, 33.166833333333, &altitude));
    QVERIFY(qAbs(altitude - 280.0) < 0.01);

    const QString corrupt = directory.filePath(QStringLiteral("corrupt.hgt"));
    QFile corruptFile(corrupt);
    QVERIFY(corruptFile.open(QIODevice::WriteOnly));
    QCOMPARE(corruptFile.write("bad", 3), qint64(3));
    corruptFile.close();
    QVERIFY(!SrtmElevationSource::SampleHgtFile(
        corrupt, 35.5, 33.5, &altitude));

    const QString voidTile = directory.filePath(QStringLiteral("void.hgt"));
    QVERIFY(writeSyntheticTile(voidTile, 1201, 600, 600));
    QFile file(voidTile);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(writeSample(&file, 1201, 600, 600, -32768));
    file.close();
    QVERIFY(!SrtmElevationSource::SampleHgtFile(
        voidTile, 35.499175, 33.500825, &altitude));
}

void SrtmElevationSourceTest::localCacheSamplingDoesNotRequireNetwork()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeSyntheticTile(
        directory.filePath(QStringLiteral("N35E033.hgt")),
        1201, 600, 600));
    SrtmElevationSource source(nullptr, directory.path());
    source.SetAutoDownloadEnabled(false);
    QCOMPARE(source.CacheDirectory(), QDir::cleanPath(directory.path()));
    QVERIFY(!source.AutoDownloadEnabled());
    double altitude = 0.0;
    QVERIFY(source.SampleAltitude(35.4995, 33.5005, &altitude));
    QVERIFY(qAbs(altitude - 280.0) < 0.001);
    source.Shutdown();
}

void SrtmElevationSourceTest::cacheOnlyMissNeverSchedulesNetwork()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString root = QStringLiteral("http://127.0.0.1:%1/")
        .arg(server.serverPort());
    SrtmElevationSource source(
        nullptr, directory.path(), QStringList{root});
    source.SetAutoDownloadEnabled(true);

    double altitude = 0.0;
    QVERIFY(!source.SampleAltitudeCached(35.5, 33.5, &altitude));
    QTest::qWait(50);
    QVERIFY(!server.hasPendingConnections());
    source.Shutdown();
}

void SrtmElevationSourceTest::
requestAdmissionIsDeduplicatedBoundedAndThreadSafe()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SrtmElevationSource source(
        nullptr, directory.path(),
        QStringList{QStringLiteral("http://127.0.0.1:9/")});
    source.SetAutoDownloadEnabled(true);

    int admitted = 0;
    for (int index = 0;
         index < SrtmElevationSource::MaximumPendingTiles * 2; ++index) {
        admitted += source.RequestTileForCoordinate(
            -70.5 + index, 10.5) ? 1 : 0;
    }
    QCOMPARE(admitted, SrtmElevationSource::MaximumPendingTiles);
    QVERIFY(!source.RequestTileForCoordinate(-70.5, 10.5));
    source.Shutdown();

    SrtmElevationSource threaded(
        nullptr, directory.path(),
        QStringList{QStringLiteral("http://127.0.0.1:9/")});
    threaded.SetAutoDownloadEnabled(true);
    std::atomic_int parallelAdmissions{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 16; ++index) {
        workers.emplace_back([&]() {
            if (threaded.RequestTileForCoordinate(35.5, 33.5)) {
                ++parallelAdmissions;
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    QCOMPARE(parallelAdmissions.load(), 1);
    threaded.Shutdown();
}

void SrtmElevationSourceTest::downloadsAndAtomicallyInstallsArchive()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir cacheDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(cacheDirectory.isValid());
    const QString tileName = QStringLiteral("N35E033.hgt");
    const QString sourcePath = sourceDirectory.filePath(tileName);
    QVERIFY(writeSyntheticTile(sourcePath, 1201, 600, 600));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray archive = zipTile(tileName, sourceFile.readAll());
    QVERIFY(!archive.isEmpty());
    QFile corruptCache(cacheDirectory.filePath(tileName));
    QVERIFY(corruptCache.open(QIODevice::WriteOnly));
    QCOMPARE(corruptCache.write("bad", 3), qint64(3));
    corruptCache.close();

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, archive]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket, archive]() {
                socket->readAll();
                const QByteArray header = QByteArrayLiteral(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                    "Connection: close\r\nContent-Length: ")
                    + QByteArray::number(archive.size())
                    + QByteArrayLiteral("\r\n\r\n");
                socket->write(header);
                socket->write(archive);
                socket->disconnectFromHost();
            });
        }
    });
    const QString root = QStringLiteral("http://127.0.0.1:%1/")
        .arg(server.serverPort());
    SrtmElevationSource source(
        nullptr, cacheDirectory.path(), QStringList{root});
    source.SetAutoDownloadEnabled(true);
    QSignalSpy available(&source, &SrtmElevationSource::TileAvailable);
    QSignalSpy failed(&source, &SrtmElevationSource::DownloadFailed);
    double altitude = 0.0;
    QVERIFY(!source.SampleAltitude(35.4995, 33.5005, &altitude));
    QTRY_COMPARE_WITH_TIMEOUT(available.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(available.first().first().toString(), tileName);
    QVERIFY(QFileInfo::exists(cacheDirectory.filePath(tileName)));
    QVERIFY(!QFileInfo::exists(
        cacheDirectory.filePath(tileName + QStringLiteral(".zip"))));
    QVERIFY(source.SampleAltitude(35.4995, 33.5005, &altitude));
    QVERIFY(qAbs(altitude - 280.0) < 0.001);
    source.SetAutoDownloadEnabled(false);
    source.Shutdown();
}

QTEST_MAIN(SrtmElevationSourceTest)
#include "test_srtmelevationsource.moc"
