#include "GeoRefRuntimeFixture.h"
#include "ui/Loghandling/GeoRefExif.h"
#include "ui/Loghandling/GeoRefService.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <limits>

namespace {

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool overwrite(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

GeoRefService::Options fixtureOptions(const QString &log,
                                      const QString &photos)
{
    GeoRefService::Options options;
    options.logPath = log;
    options.photoDirectory = photos;
    return options;
}

bool validXml(const QByteArray &bytes)
{
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd())
        xml.readNext();
    return !xml.hasError();
}

} // namespace

class TestGeoRefService final : public QObject
{
    Q_OBJECT

private slots:
    void camPlanAndExecuteReportsAndExif();
    void timeOffsetGps2AndCameraCorrections();
    void trigRequiresExactCount();
    void estimateUsesReferenceMedian();
    void preparePinsInputsAndDestinations();
    void executeRefusesMutationAndNoOverwriteRace();
    void cancelAfterFirstPublicationIsTruthful();
    void invalidAndUnsupportedInputsFailClosed();
};

void TestGeoRefService::camPlanAndExecuteReportsAndExif()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    const QByteArray sourceLog = readFile(log);
    const QByteArray firstPhoto = readFile(QDir(photos).filePath("one.jpg"));

    QVector<QPair<qint64, qint64>> progress;
    const auto prepared = GeoRefService::Prepare(
        fixtureOptions(log, photos), {},
        [&](qint64 done, qint64 total) {
            progress.append(qMakePair(done, total));
        });
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    QVERIFY(prepared.plan);
    QVERIFY(prepared.plan->isValid());
    QCOMPARE(prepared.plan->matches().size(), 2);
    QCOMPARE(prepared.plan->outputPaths().size(), 4);
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().first, qint64(1));
    QCOMPARE(progress.last().second, qint64(1));
    QCOMPARE(prepared.plan->options().logPath, QFileInfo(log).canonicalFilePath());
    const auto matches = prepared.plan->matches();
    QCOMPARE(matches.at(0).latitude, 47.5);
    QCOMPARE(matches.at(0).longitude, 8.5);
    QCOMPARE(matches.at(0).altitude, 123.5);
    QCOMPARE(matches.at(0).roll, 1.0);
    QCOMPARE(matches.at(0).pitch, 2.0);
    QCOMPARE(matches.at(0).yaw, 3.0);
    QVERIFY(matches.at(0).timeUtc < matches.at(1).timeUtc);
    for (const QString &output : prepared.plan->outputPaths())
        QVERIFY2(!QFileInfo::exists(output), qPrintable(output));

    const auto executed = GeoRefService::Execute(*prepared.plan);
    QVERIFY2(executed.success, qPrintable(executed.error));
    QVERIFY(!executed.cancelled);
    QCOMPARE(executed.taggedPhotos, 2);
    QCOMPARE(executed.failedPhotos, 0);
    QCOMPARE(executed.publishedPaths, prepared.plan->outputPaths());
    QCOMPARE(executed.matches.size(), prepared.plan->matches().size());
    QCOMPARE(readFile(log), sourceLog);
    QCOMPARE(readFile(QDir(photos).filePath("one.jpg")), firstPhoto);

    const QString output = QDir(photos).filePath("geotagged");
    const QByteArray location = readFile(QDir(output).filePath("location.txt"));
    QVERIFY(location.startsWith(
        "#name latitude/Y longitude/X height/Z yaw pitch roll SAlt\n"));
    QVERIFY(location.contains("one.jpg 47.5 8.5 123.5 3 2 1 0\n"));
    const QByteArray kml = readFile(QDir(output).filePath("location.kml"));
    QVERIFY(validXml(kml));
    QVERIFY(kml.contains("http://www.opengis.net/kml/2.2"));
    QVERIFY(kml.contains("8.5,47.5,123.5"));

    const auto metadata = GeoRefExif::Inspect(
        QDir(output).filePath("one_geotag.jpg"));
    QVERIFY2(metadata.success, qPrintable(metadata.error));
    QVERIFY(metadata.hasCoordinates);
    QVERIFY(std::abs(metadata.coordinates.latitude - 47.5) < 1e-8);
    QVERIFY(std::abs(metadata.coordinates.longitude - 8.5) < 1e-8);
    QVERIFY(std::abs(metadata.coordinates.altitude - 123.5) < 1e-5);
}

void TestGeoRefService::timeOffsetGps2AndCameraCorrections()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    QByteArray text = readFile(log);
    const QByteArray gps2Definition =
        "FMT,151,34,GPS2,QBIHLLee,TimeUS,Status,GMS,GWk,Lat,Lng,Alt,RAlt\n";
    const int insertion = text.indexOf("FMT,153");
    QVERIFY(insertion > 0);
    text.insert(insertion, gps2Definition);
    text += "GPS2,1000000,3,200000,2400,-35.5,149.5,550,30\n"
            "GPS2,3000000,3,202000,2400,-35.6,149.6,551,31\n";
    QVERIFY(overwrite(log, text));

    auto options = fixtureOptions(log, photos);
    options.mode = GeoRefService::Mode::TimeOffset;
    options.timeOffsetSeconds = 13.0;
    options.useGps2 = true;
    options.outputDirectory = directory.filePath("offset-output");
    auto prepared = GeoRefService::Prepare(options);
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    QCOMPARE(prepared.plan->matches().size(), 2);
    QCOMPARE(prepared.plan->matches().at(0).latitude, -35.5);

    options = fixtureOptions(log, photos);
    options.shutterLagMilliseconds = 250;
    options.useGpsAltitude = true;
    options.baseAltitudeAdjustmentMeters = -12.5;
    options.outputDirectory = directory.filePath("corrected-output");
    prepared = GeoRefService::Prepare(options);
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    const auto corrected = prepared.plan->matches();
    QCOMPARE(corrected.size(), 2);
    // The nearest GPS sample supplies position/attitude and GPS altitude;
    // the explicit base adjustment is applied last.
    // CAM's reference 17-second offset plus lag places the first target
    // closer to the second GPS fix (GPS uses the 18-second offset).
    QCOMPARE(corrected.at(0).latitude, 47.6);
    QCOMPARE(corrected.at(0).longitude, 8.6);
    QCOMPARE(corrected.at(0).altitude, 438.5);
    QCOMPARE(corrected.at(1).altitude, 438.5);
}

void TestGeoRefService::trigRequiresExactCount()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    QByteArray text = readFile(log);
    text.replace("CAM", "TRIG");
    QVERIFY(overwrite(log, text));
    auto options = fixtureOptions(log, photos);
    options.mode = GeoRefService::Mode::Trig;
    auto prepared = GeoRefService::Prepare(options);
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    QCOMPARE(prepared.plan->matches().size(), 2);

    QVERIFY(QFile::remove(QDir(photos).filePath("two.jpg")));
    prepared = GeoRefService::Prepare(options);
    QVERIFY(!prepared.success);
    QVERIFY(prepared.error.contains("counts differ"));
    QVERIFY(!QFileInfo::exists(QDir(photos).filePath("geotagged")));
}

void TestGeoRefService::estimateUsesReferenceMedian()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));

    auto estimate = GeoRefService::Estimate(fixtureOptions(log, photos));
    QVERIFY2(estimate.success, qPrintable(estimate.error));
    QVERIFY(estimate.hasEstimate);
    QCOMPARE(estimate.offsetSeconds, 12.0);

    auto options = fixtureOptions(log, photos);
    options.mode = GeoRefService::Mode::TimeOffset;
    estimate = GeoRefService::Estimate(options);
    QVERIFY2(estimate.success, qPrintable(estimate.error));
    QCOMPARE(estimate.offsetSeconds, 13.0);
}

void TestGeoRefService::preparePinsInputsAndDestinations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    const QFileInfo before(log);
    bool changed = false;
    auto prepared = GeoRefService::Prepare(
        fixtureOptions(log, photos), {}, [&](qint64 done, qint64 total) {
            if (done != total || changed) return;
            changed = true;
            QByteArray bytes = readFile(log);
            const QByteArray original = bytes;
            const int position = bytes.indexOf("47.5");
            QVERIFY(position >= 0);
            bytes[position] = '5';
            QVERIFY(bytes != original);
            QVERIFY(overwrite(log, bytes));
            QFile file(log);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(before.lastModified(),
                                     QFileDevice::FileModificationTime));
        });
    QVERIFY(changed);
    QVERIFY(!prepared.success);
    QVERIFY(!prepared.cancelled);
    QVERIFY(prepared.error.contains("changed"));

    // A destination created by the final observer invalidates consent too.
    // Use a new independent fixture because the first source was deliberately changed.
    QTemporaryDir second;
    QVERIFY(second.isValid());
    QVERIFY(GeoRefRuntimeFixture::create(second.path(), &log, &photos));
    const QString output = QDir(photos).filePath("geotagged");
    bool occupied = false;
    prepared = GeoRefService::Prepare(
        fixtureOptions(log, photos), {}, [&](qint64 done, qint64 total) {
            if (done != total || occupied) return;
            occupied = true;
            QVERIFY(QDir().mkpath(output));
            QVERIFY(GeoRefRuntimeFixture::save(
                QDir(output).filePath("location.txt"), "foreign"));
        });
    QVERIFY(occupied);
    QVERIFY(!prepared.success);
    QCOMPARE(readFile(QDir(output).filePath("location.txt")),
             QByteArray("foreign"));
}

void TestGeoRefService::executeRefusesMutationAndNoOverwriteRace()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    auto prepared = GeoRefService::Prepare(fixtureOptions(log, photos));
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    QByteArray bytes = readFile(log);
    bytes.append('\n');
    QVERIFY(overwrite(log, bytes));
    auto executed = GeoRefService::Execute(*prepared.plan);
    QVERIFY(!executed.success);
    QVERIFY(executed.publishedPaths.isEmpty());

    QTemporaryDir second;
    QVERIFY(second.isValid());
    QVERIFY(GeoRefRuntimeFixture::create(second.path(), &log, &photos));
    prepared = GeoRefService::Prepare(fixtureOptions(log, photos));
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    const QString foreign = prepared.plan->outputPaths().at(0);
    bool injected = false;
    executed = GeoRefService::Execute(
        *prepared.plan, {}, [&](qint64 done, qint64) {
            if (done != 1 || injected) return;
            injected = true;
            QVERIFY(GeoRefRuntimeFixture::save(foreign, "foreign-report"));
        });
    QVERIFY(injected);
    QVERIFY(!executed.success);
    QVERIFY(executed.publishedPaths.isEmpty());
    QCOMPARE(readFile(foreign), QByteArray("foreign-report"));
}

void TestGeoRefService::cancelAfterFirstPublicationIsTruthful()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    const auto prepared = GeoRefService::Prepare(fixtureOptions(log, photos));
    QVERIFY2(prepared.success, qPrintable(prepared.error));
    std::atomic_bool cancel{false};
    const auto executed = GeoRefService::Execute(
        *prepared.plan, [&]() { return cancel.load(); },
        [&](qint64 done, qint64) {
            if (done == 2) cancel.store(true);
        });
    QVERIFY(!executed.success);
    QVERIFY(executed.cancelled);
    QCOMPARE(executed.publishedPaths.size(), 1);
    QCOMPARE(executed.publishedPaths.first(),
             prepared.plan->outputPaths().first());
    QVERIFY(QFileInfo::exists(executed.publishedPaths.first()));
    QVERIFY(!QFileInfo::exists(prepared.plan->outputPaths().at(1)));
    QCOMPARE(executed.taggedPhotos, 0);
}

void TestGeoRefService::invalidAndUnsupportedInputsFailClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString log, photos;
    QVERIFY(GeoRefRuntimeFixture::create(directory.path(), &log, &photos));
    auto options = fixtureOptions(log, photos);
    options.timeOffsetSeconds = std::numeric_limits<double>::infinity();
    QVERIFY(!GeoRefService::Prepare(options).success);
    options = fixtureOptions(directory.filePath("flight.tlog"), photos);
    QVERIFY(GeoRefRuntimeFixture::save(options.logPath, "not traffic"));
    const auto prepared = GeoRefService::Prepare(options);
    QVERIFY(!prepared.success);
    QVERIFY(prepared.error.contains("non-functional"));
    QVERIFY(!GeoRefService::Execute(GeoRefService::Plan()).success);
}

QTEST_MAIN(TestGeoRefService)
#include "test_georefservice.moc"
