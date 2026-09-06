#include <QtTest>
#include "services/ShapefileImportService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
void u32(QByteArray &out, quint32 value, bool big = false) {
    for (int i = 0; i < 4; ++i) out.append(char(value >> (8 * (big ? 3-i : i))));
}
void f64(QByteArray &out, double value) {
    quint64 bits; std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) out.append(char(bits >> (8*i)));
}
QByteArray point(double x, double y) {
    QByteArray out; u32(out, 1); f64(out, x); f64(out, y); return out;
}
QByteArray ring(double x, double y) {
    QByteArray out; u32(out, 5);
    f64(out,x); f64(out,y); f64(out,x+1); f64(out,y+1);
    u32(out,1); u32(out,5); u32(out,0);
    const double coords[][2] = {{x,y},{x,y+1},{x+1,y+1},{x+1,y},{x,y}};
    for (const auto &p : coords) { f64(out,p[0]); f64(out,p[1]); }
    return out;
}
QByteArray shp(quint32 type, const QVector<QByteArray> &records) {
    QByteArray out; u32(out,9994,true); out.append(20,'\0'); u32(out,0,true);
    u32(out,1000); u32(out,type); out.append(64,'\0');
    quint32 n = 0;
    for (const auto &record : records) { u32(out,++n,true); u32(out,record.size()/2,true); out += record; }
    const quint32 size = out.size()/2;
    for (int i = 0; i < 4; ++i) out[24+i] = char(size >> (8*(3-i)));
    return out;
}
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QStringList directoryEntries(const QString &directory) {
    return QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,QDir::Name);
}
QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }
QByteArray poly(const QByteArray &rows) { return QByteArray("#Shap to Poly - Mission Planner\r\n") + rows; }
bool rewriteSameStamp(const QString &path, const QByteArray &bytes) {
    const QFileInfo old(path);
    const QDateTime time = old.lastModified(); const qint64 size = old.size();
    if (bytes.size() != size) return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite) || f.write(bytes) != bytes.size() || !f.flush()
        || !f.setFileTime(time, QFileDevice::FileModificationTime)) return false;
    f.close(); return QFileInfo(path).size() == size && QFileInfo(path).lastModified() == time;
}
}

class ShapefileImportServiceTest : public QObject {
    Q_OBJECT
private slots:
    void invalidPlanAndAbsentInput() {
        ShapefileImportService::Plan empty;
        QVERIFY(!empty.isValid()); QVERIFY(empty.source().isEmpty()); QVERIFY(empty.directory().isEmpty());
        QVERIFY(empty.outputs().isEmpty()); QCOMPARE(empty.pointCount(), qint64(0));
        const auto result = ShapefileImportService::exportPolyFiles(empty);
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.files.isEmpty());
        QVERIFY(!ShapefileImportService::prepare({}).plan.isValid());
        QTemporaryDir root; QVERIFY(!ShapefileImportService::prepare(root.path()+"/missing.shp").plan.isValid());
        QVERIFY(directoryEntries(root.path()).isEmpty());
    }

    void twoClosedFeaturesExactBytesAndNoWritesDuringPrepare() {
        QTemporaryDir root; const QString source = root.path()+"/shapes.SHP";
        const QByteArray original = shp(5,{ring(30,40),ring(31,41)});
        QVERIFY(write(source,original));
        const auto preparation = ShapefileImportService::prepare(source);
        if (preparation.error.startsWith("Native GDAL/GEOS polygon validation is unavailable"))
            QSKIP(qPrintable(preparation.error));
        QVERIFY2(preparation.plan.isValid(),qPrintable(preparation.error));
        const auto plan = preparation.plan;
        QCOMPARE(plan.source(),canonical(source)); QCOMPARE(plan.directory(),canonical(root.path()));
        QCOMPARE(plan.outputs().size(),2); QCOMPARE(plan.pointCount(),qint64(10));
        QCOMPARE(plan.outputs().first().pointCount,qint64(5)); QVERIFY(!plan.outputs().first().replaces);
        QVERIFY(plan.projectionName().isEmpty()); QCOMPARE(plan.discardedPoints(),qint64(0));
        QCOMPARE(directoryEntries(root.path()),QStringList{"shapes.SHP"});
        const auto result = ShapefileImportService::exportPolyFiles(plan);
        QVERIFY2(result.success,qPrintable(result.error)); QVERIFY(!result.cancelled);
        QCOMPARE(result.files,QStringList({plan.outputs().at(0).path,plan.outputs().at(1).path}));
        QCOMPARE(result.pointCount,qint64(10));
        QCOMPARE(read(result.files.at(0)),poly("40\t30\r\n41\t30\r\n41\t31\r\n40\t31\r\n40\t30\r\n"));
        QCOMPARE(read(result.files.at(1)),poly("41\t31\r\n42\t31\r\n42\t32\r\n41\t32\r\n41\t31\r\n"));
        QCOMPARE(read(source),original);
        QCOMPARE(directoryEntries(root.path()),QStringList({"poly-1.poly","poly-2.poly","shapes.SHP"}));
    }

    void emptyInvalidAndNullRecordsDoNotConsumeNumbers() {
        QTemporaryDir root; const QString source = root.path()+"/points.shp";
        QByteArray nullShape; u32(nullShape,0);
        QVERIFY(write(source,shp(1,{nullShape,point(181,0),point(30,40),point(20,-91),point(31,41)})));
        const auto prep = ShapefileImportService::prepare(source);
        QVERIFY2(prep.plan.isValid(),qPrintable(prep.error)); QCOMPARE(prep.plan.outputs().size(),2);
        QCOMPARE(prep.plan.discardedPoints(),qint64(2)); QCOMPARE(prep.plan.pointCount(),qint64(2));
        const auto result = ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY(result.success); QCOMPARE(result.pointCount,qint64(2));
        QCOMPARE(read(root.path()+"/poly-1.poly"),poly("40\t30\r\n"));
        QCOMPARE(read(root.path()+"/poly-2.poly"),poly("41\t31\r\n"));
        QVERIFY(write(source,shp(1,{nullShape,point(181,0)})));
        const auto empty = ShapefileImportService::prepare(source);
        QVERIFY2(empty.plan.isValid(),qPrintable(empty.error)); QVERIFY(empty.plan.outputs().isEmpty());
        const auto noOutput = ShapefileImportService::exportPolyFiles(empty.plan);
        QVERIFY(noOutput.success); QVERIFY(noOutput.files.isEmpty()); QCOMPARE(noOutput.pointCount,qint64(0));
        QCOMPARE(read(root.path()+"/poly-1.poly"),poly("40\t30\r\n")); // No stale-output deletion.
    }

    void malformedLaterRecordPreventsAnyPreparedOutput() {
        QTemporaryDir root; const QString source = root.path()+"/input.shp";
        QByteArray truncated = shp(1,{point(1,2),point(3,4)}); truncated.chop(1);
        QVERIFY(write(source,truncated)); QVERIFY(write(root.path()+"/poly-1.poly","untouched"));
        const auto prep = ShapefileImportService::prepare(source);
        QVERIFY(!prep.plan.isValid()); QVERIFY(!prep.error.isEmpty());
        QCOMPARE(read(root.path()+"/poly-1.poly"),QByteArray("untouched"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly"}));
    }

    void overwriteReviewedFilesButRetainHigherStaleNumber() {
        QTemporaryDir root; const QString source = root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(30,40)})));
        QVERIFY(write(root.path()+"/poly-1.poly",QByteArray(5000,'s')));
        QVERIFY(write(root.path()+"/poly-2.poly","stale higher output"));
        const auto prep = ShapefileImportService::prepare(source);
        QVERIFY(prep.plan.isValid()); QVERIFY(prep.plan.outputs().first().replaces);
        const auto result = ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY2(result.success,qPrintable(result.error));
        QCOMPARE(read(root.path()+"/poly-1.poly"),poly("40\t30\r\n"));
        QCOMPARE(read(root.path()+"/poly-2.poly"),QByteArray("stale higher output"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly","poly-2.poly"}));
    }

    void sourceChangesDuringConsent_data() {
        QTest::addColumn<bool>("preserveStamp");
        QTest::newRow("ordinary source change") << false;
        QTest::newRow("SHA detects restored size and mtime") << true;
    }
    void sourceChangesDuringConsent() {
        QFETCH(bool,preserveStamp); QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)}))); QVERIFY(write(root.path()+"/poly-1.poly","operator output"));
        const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        const QByteArray replacement=shp(1,{point(3,4)});
        if (preserveStamp) QVERIFY(rewriteSameStamp(source,replacement));
        else QVERIFY(write(source,replacement+"changed length"));
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY(!result.success); QVERIFY(!result.files.size()); QVERIFY(!result.error.isEmpty());
        QCOMPARE(read(source),preserveStamp?replacement:replacement+"changed length");
        QCOMPARE(read(root.path()+"/poly-1.poly"),QByteArray("operator output"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly"}));
    }

    void destinationChangesDuringConsent_data() {
        QTest::addColumn<bool>("existed");
        QTest::newRow("new destination") << false; QTest::newRow("changed destination") << true;
    }
    void destinationChangesDuringConsent() {
        QFETCH(bool,existed); QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)})));
        if(existed) QVERIFY(write(root.path()+"/poly-1.poly","before"));
        const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        QVERIFY(write(root.path()+"/poly-1.poly","new operator content"));
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY(!result.success); QVERIFY(result.files.isEmpty());
        QCOMPARE(read(root.path()+"/poly-1.poly"),QByteArray("new operator content"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly"}));
    }

    void sidecarAppearsDuringConsent() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)})));
        const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        QVERIFY(write(root.path()+"/INPUT.PRJ","new CRS requires another reviewed plan"));
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY(!result.success); QVERIFY(result.files.isEmpty());
        QCOMPARE(directoryEntries(root.path()),QStringList({"INPUT.PRJ","input.shp"}));
    }

    void sidecarContentDigestIsPinned() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp", prj=root.path()+"/input.PRJ";
        QVERIFY(write(source,shp(1,{point(1,2)}))); QVERIFY(write(prj,"    "));
        const auto prep=ShapefileImportService::prepare(source); QVERIFY2(prep.plan.isValid(),qPrintable(prep.error));
        QVERIFY(rewriteSameStamp(prj,"junk"));
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan);
        QVERIFY(!result.success); QVERIFY(result.files.isEmpty()); QCOMPARE(read(prj),QByteArray("junk"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.PRJ","input.shp"}));
    }

    void cancellationAndFailurePreservePartialReceipts_data() {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("cancel before second file") << true;
        QTest::newRow("foreign second destination") << false;
    }
    void cancellationAndFailurePreservePartialReceipts() {
        QFETCH(bool,cancel); QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(5,{ring(30,40),ring(31,41)})));
        const auto prep=ShapefileImportService::prepare(source);
        if (prep.error.startsWith("Native GDAL/GEOS polygon validation is unavailable"))
            QSKIP(qPrintable(prep.error));
        QVERIFY2(prep.plan.isValid(),qPrintable(prep.error));
        bool stop=false, callback=false, inserted=false;
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan,[&]{return stop;},
            [&](qint64 done,qint64,const QString &phase){
                if(done==1 && phase.startsWith("Writing ")){
                    callback=true;
                    if(cancel) stop=true;
                    else inserted=write(root.path()+"/poly-2.poly","foreign second destination");
                }
            });
        QVERIFY(callback); QVERIFY(!result.success); QCOMPARE(result.cancelled,cancel);
        QCOMPARE(result.files,QStringList{prep.plan.outputs().first().path}); QCOMPARE(result.pointCount,qint64(5));
        QCOMPARE(read(root.path()+"/poly-1.poly"),poly("40\t30\r\n41\t30\r\n41\t31\r\n40\t31\r\n40\t30\r\n"));
        if(cancel) QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly"}));
        else { QVERIFY(inserted); QCOMPARE(read(root.path()+"/poly-2.poly"),QByteArray("foreign second destination"));
            QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly","poly-2.poly"})); }
    }

    void ordinarySourceChangeInWritingCallbackAborts() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)}))); const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        bool changed=false;
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan,{},[&](qint64,qint64,const QString &phase){
            if(phase.startsWith("Writing ")) changed=write(source,"changed source");
        });
        QVERIFY(changed); QVERIFY(!result.success); QVERIFY(result.files.isEmpty());
        QCOMPARE(directoryEntries(root.path()),QStringList{"input.shp"}); QCOMPARE(read(source),QByteArray("changed source"));
    }

    void cancellationBeforeOutputAndObserverExceptions() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)})));
        const auto cancelled=ShapefileImportService::prepare(source,[]{return true;});
        QVERIFY(cancelled.cancelled); QVERIFY(!cancelled.plan.isValid());
        const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        const auto noOutput=ShapefileImportService::exportPolyFiles(prep.plan,[]{return true;});
        QVERIFY(noOutput.cancelled); QVERIFY(noOutput.files.isEmpty());
        const auto failed=ShapefileImportService::exportPolyFiles(prep.plan,{},[](qint64,qint64,const QString &){throw std::runtime_error("observer");});
        QVERIFY(!failed.success); QVERIFY(!failed.cancelled); QVERIFY(failed.files.isEmpty());
        QVERIFY(failed.error.contains("observer")); QCOMPARE(directoryEntries(root.path()),QStringList{"input.shp"});
    }

    void planIsPinnedAcrossCallerResetAndFinalCancelIsDefinitive() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2),point(3,4)})));
        auto plan=ShapefileImportService::prepare(source).plan; QVERIFY(plan.isValid());
        bool stop=false, reset=false;
        const auto result=ShapefileImportService::exportPolyFiles(plan,[&]{return stop;},[&](qint64,qint64,const QString &phase){
            if(!reset){plan={};reset=true;}
            if(phase=="POLY conversion complete")stop=true;
        });
        QVERIFY(reset); QVERIFY(!plan.isValid()); QVERIFY(result.success); QVERIFY(!result.cancelled);
        QCOMPARE(result.files.size(),2); QCOMPARE(result.pointCount,qint64(2));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly","poly-2.poly"}));
    }

    void cancelAfterStagedWritePreservesOldFileAndRemovesOwnedTemporary() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)}))); QVERIFY(write(root.path()+"/poly-1.poly","old reviewed file"));
        const auto prep=ShapefileImportService::prepare(source); QVERIFY(prep.plan.isValid());
        bool writing=false; int checks=0;
        const auto result=ShapefileImportService::exportPolyFiles(prep.plan,[&]{
            // After Writing progress: post-observer gate, pre-write gate,
            // post-write/pre-commit gate. Exercise an actual staged QSaveFile.
            return writing && ++checks==3;
        },[&](qint64,qint64,const QString &phase){if(phase.startsWith("Writing "))writing=true;});
        QCOMPARE(checks,3); QVERIFY(result.cancelled); QVERIFY(!result.success); QVERIFY(result.files.isEmpty());
        QCOMPARE(read(root.path()+"/poly-1.poly"),QByteArray("old reviewed file"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"input.shp","poly-1.poly"}));
    }

    void symlinkSourceSidecarDestinationAreRefused() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link creates shortcuts on Windows.");
#else
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)}))); QVERIFY(write(root.path()+"/foreign","keep"));
        QVERIFY(QFile::link(source,root.path()+"/linked.shp"));
        QVERIFY(!ShapefileImportService::prepare(root.path()+"/linked.shp").plan.isValid());
        QVERIFY(QFile::link(root.path()+"/foreign",root.path()+"/input.prj"));
        QVERIFY(!ShapefileImportService::prepare(source).plan.isValid());
        QVERIFY(QFile::remove(root.path()+"/input.prj"));
        QVERIFY(QFile::link(root.path()+"/foreign",root.path()+"/poly-1.poly"));
        QVERIFY(!ShapefileImportService::prepare(source).plan.isValid());
        QCOMPARE(read(root.path()+"/foreign"),QByteArray("keep"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"foreign","input.shp","linked.shp","poly-1.poly"}));
#endif
    }

    void ambiguousCaseInsensitiveSidecarsRefused() {
        QTemporaryDir root; const QString source=root.path()+"/input.shp";
        QVERIFY(write(source,shp(1,{point(1,2)}))); QVERIFY(write(root.path()+"/input.prj"," "));
        if(QFileInfo::exists(root.path()+"/INPUT.PRJ"))QSKIP("Filesystem is case-insensitive.");
        QVERIFY(write(root.path()+"/INPUT.PRJ"," "));
        const auto prep=ShapefileImportService::prepare(source);
        QVERIFY(!prep.plan.isValid()); QVERIFY(prep.error.contains("Ambiguous"));
        QCOMPARE(directoryEntries(root.path()),QStringList({"INPUT.PRJ","input.prj","input.shp"}));
    }

    void typedPolySourceAndDirectoryOutputRefused() {
        QTemporaryDir root;
        const QByteArray bytes = shp(1,{point(30,40)});
        const QString typed = root.path()+"/poly-1.poly";
        QVERIFY(write(typed, bytes));
        QVERIFY(!ShapefileImportService::prepare(typed).plan.isValid());
        QCOMPARE(read(typed), bytes);
        const QString source = root.path()+"/input.shp";
        QVERIFY(write(source, bytes));
        QVERIFY(QFile::remove(typed)); // Only this test's exact fixture.
        QVERIFY(QDir(root.path()).mkdir("poly-1.poly"));
        QVERIFY(!ShapefileImportService::prepare(source).plan.isValid());
        QVERIFY(QFileInfo(typed).isDir());
        QCOMPARE(read(source), bytes);
    }

    void observerFailureInPreparationIsNotUserCancellation() {
        QTemporaryDir root;
        const QString source = root.path()+"/input.shp";
        QVERIFY(write(source, shp(1,{point(30,40)})));
        const auto prep = ShapefileImportService::prepare(source, {},
            [](qint64,qint64,const QString &) { throw std::runtime_error("observer"); });
        QVERIFY(!prep.plan.isValid()); QVERIFY(!prep.cancelled);
        QVERIFY(prep.error.contains("observer"));
        QCOMPARE(directoryEntries(root.path()), QStringList{"input.shp"});
    }

    void invariantFormattingAndUtf16EmptyProjection() {
        QTemporaryDir root;
        const QString source = root.path()+"/input.shp";
        QVERIFY(write(source, shp(1,{point(-0.0, 1.25)})));
        QVERIFY(write(root.path()+"/INPUT.PRJ", QByteArray::fromHex("fffe20002000")));
        const QLocale previous = QLocale();
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        const auto prep = ShapefileImportService::prepare(source);
        const auto result = ShapefileImportService::exportPolyFiles(prep.plan);
        QLocale::setDefault(previous);
        QVERIFY2(prep.plan.isValid(), qPrintable(prep.error));
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(read(root.path()+"/poly-1.poly"), poly("1.25\t-0\r\n"));
        QCOMPARE(read(root.path()+"/INPUT.PRJ"), QByteArray::fromHex("fffe20002000"));
    }

    void actualUppercasePrjReprojectsUtf8AndUtf16() {
        QTemporaryDir root;
        const QString source = root.path()+"/utm.shp";
        const QByteArray shape = shp(1,{point(500000,0)});
        QVERIFY(write(source, shape));
        const QString wkt = QStringLiteral("PROJCS[\"WGS_1984_UTM_Zone_33N\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.017453292519943295]],PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"False_Easting\",500000],PARAMETER[\"False_Northing\",0],PARAMETER[\"Central_Meridian\",15],PARAMETER[\"Scale_Factor\",0.9996],PARAMETER[\"Latitude_Of_Origin\",0],UNIT[\"Meter\",1]]");
        QByteArray utf16 = QByteArray::fromHex("fffe");
        for (QChar c : wkt) {
            utf16.append(char(c.unicode() & 0xff));
            utf16.append(char(c.unicode() >> 8));
        }
        for (const QByteArray &encoded : {wkt.toUtf8(), utf16}) {
            QVERIFY(write(root.path()+"/UTM.PRJ", encoded));
            const auto prep = ShapefileImportService::prepare(source);
            if (prep.error.startsWith("Native GDAL runtime")
                || prep.error.startsWith("Native PROJ 9.2")) QSKIP(qPrintable(prep.error));
            QVERIFY2(prep.plan.isValid(), qPrintable(prep.error));
            QVERIFY(!prep.plan.projectionName().isEmpty());
            const auto result = ShapefileImportService::exportPolyFiles(prep.plan);
            QVERIFY2(result.success, qPrintable(result.error));
            const auto rows = read(root.path()+"/poly-1.poly").split('\n');
            QCOMPARE(rows.size(), 3);
            const auto coordinates = rows.at(1).trimmed().split('\t');
            QCOMPARE(coordinates.size(), 2);
            QVERIFY(qAbs(coordinates.at(0).toDouble()) < 1e-10);
            QVERIFY(qAbs(coordinates.at(1).toDouble() - 15) < 1e-10);
            QCOMPARE(read(root.path()+"/UTM.PRJ"), encoded);
            QCOMPARE(read(source), shape);
        }
    }
};
QTEST_GUILESS_MAIN(ShapefileImportServiceTest)
#include "test_shapefileimportservice.moc"
