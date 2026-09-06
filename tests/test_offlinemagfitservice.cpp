#include "services/OfflineMagFitService.h"
#include "comm/TlogReader.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
bool put(const QString &path, const QByteArray &data)
{
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
QVector<MagVector> sphere(int count, double radius, MagVector offsets = {})
{
    QVector<MagVector> points;
    for (int i = 0; i < count; ++i) {
        const double y = 1 - 2 * (i + 0.5) / count;
        const double r = std::sqrt(qMax(0.0, 1 - y * y));
        const double phi = i * std::acos(-1.0) * (3 - std::sqrt(5.0));
        points.append({radius * std::cos(phi) * r - offsets.x,
                       radius * y - offsets.y, radius * std::sin(phi) * r - offsets.z});
    }
    return points;
}
QByteArray n(double value) { return QByteArray::number(value, 'g', 17); }
QByteArray row(const MagVector &v, MagVector old = {4, 5, 6}, int instance = 0)
{
    return "MAG," + QByteArray::number(instance) + ',' + n(v.x + old.x) + ','
        + n(v.y + old.y) + ',' + n(v.z + old.z) + ','
        + n(old.x) + ',' + n(old.y) + ',' + n(old.z) + '\n';
}
QByteArray textLog(int count = 180)
{
    QByteArray data("FMT,130,28,MAG,Bffffff,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ\n");
    for (const auto &v : sphere(count, 430, {31, -18, 7})) data += row(v);
    return data;
}
QByteArray frame(quint8 id, int length)
{
    QByteArray data(length, '\0'); data[0] = char(0xa3); data[1] = char(0x95); data[2] = char(id); return data;
}
QByteArray fmt(quint8 id, quint8 length, QByteArray name, QByteArray format, QByteArray columns)
{
    QByteArray data = frame(128, 89); data[3] = char(id); data[4] = char(length);
    std::memcpy(data.data() + 5, name.constData(), size_t(name.size()));
    std::memcpy(data.data() + 9, format.constData(), size_t(format.size()));
    std::memcpy(data.data() + 25, columns.constData(), size_t(columns.size())); return data;
}
void real(QByteArray &data, int offset, double value)
{
    quint64 bits = 0; std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian(bits, reinterpret_cast<uchar *>(data.data() + offset));
}
QByteArray binaryLog()
{
    QByteArray data = fmt(130, 52, "MAG", "Bdddddd", "I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ");
    for (const auto &v : sphere(180, 430, {31, -18, 7})) {
        auto r = frame(130, 52);
        real(r, 4, v.x + 4); real(r, 12, v.y + 5); real(r, 20, v.z + 6);
        real(r, 28, 4); real(r, 36, 5); real(r, 44, 6); data += r;
    }
    return data;
}
void packet(QByteArray &log, const mavlink_message_t &message)
{
    QByteArray stamp(8, '\0'); qToBigEndian<quint64>(1000000, reinterpret_cast<uchar *>(stamp.data()));
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]; const int len = mavlink_msg_to_send_buffer(buffer, &message);
    log += stamp; log.append(reinterpret_cast<const char *>(buffer), len);
}
void hud(QByteArray &log, int throttle, int sys = 1, int comp = 1)
{
    mavlink_message_t m{}; mavlink_msg_vfr_hud_pack(sys, comp, &m, 0, 0, 0, throttle, 0, 0); packet(log, m);
}
void offsets(QByteArray &log, MagVector v, int sys = 1, int comp = 1)
{
    mavlink_sensor_offsets_t of{}; of.mag_ofs_x = qint16(v.x); of.mag_ofs_y = qint16(v.y); of.mag_ofs_z = qint16(v.z);
    mavlink_message_t m{}; mavlink_msg_sensor_offsets_encode(sys, comp, &m, &of); packet(log, m);
}
void imu(QByteArray &log, MagVector v, int compass = 1, int sys = 1, int comp = 1)
{
    mavlink_message_t m{};
    if (compass == 1) {
        mavlink_raw_imu_t p{}; p.xmag = qint16(std::round(v.x)); p.ymag = qint16(std::round(v.y)); p.zmag = qint16(std::round(v.z));
        mavlink_msg_raw_imu_encode(sys, comp, &m, &p);
    } else if (compass == 2) {
        mavlink_scaled_imu2_t p{}; p.xmag = qint16(std::round(v.x)); p.ymag = qint16(std::round(v.y)); p.zmag = qint16(std::round(v.z));
        mavlink_msg_scaled_imu2_encode(sys, comp, &m, &p);
    } else {
        mavlink_scaled_imu3_t p{}; p.xmag = qint16(std::round(v.x)); p.ymag = qint16(std::round(v.y)); p.zmag = qint16(std::round(v.z));
        mavlink_msg_scaled_imu3_encode(sys, comp, &m, &p);
    }
    packet(log, m);
}
OfflineMagFitService::Options sphereOptions()
{
    OfflineMagFitService::Options o; o.useEllipsoid = false; return o;
}
QByteArray provenanceParameters(int compasses = 1)
{
    QByteArray data("FMT,129,23,PARM,Nf,Name,Value\nPARM,AHRS_ORIENTATION,0\n");
    for (int compass=1;compass<=compasses;++compass) {
        const QByteArray suffix=compass==1?QByteArray():QByteArray::number(compass);
        for(char axis:QByteArray("XYZ")) {
            data+="PARM,COMPASS_DIA"+suffix+'_'+axis+",1\n";
            data+="PARM,COMPASS_ODI"+suffix+'_'+axis+",0\n";
        }
        data+="PARM,COMPASS_SCALE"+suffix+",1\n";
        data+="PARM,COMPASS_DEV_ID"+suffix+','+QByteArray::number(compass*101)+"\n";
        data+="PARM,COMPASS_PRIO"+QByteArray::number(compass)+"_ID,"+QByteArray::number(compass*101)+"\n";
        data+="PARM,COMPASS_ORIENT"+suffix+",0\n";
        data+="PARM,"+(compass==1?QByteArray("COMPASS_EXTERNAL"):"COMPASS_EXTERN"+suffix)+",1\n";
    }
    return data;
}
QByteArray eligibleLog(int compasses = 1)
{
    QByteArray data=provenanceParameters(compasses)
        +"FMT,130,41,MAG,BfffffffffB,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ,MOX,MOY,MOZ,Health\n";
    for(int compass=1;compass<=compasses;++compass)
        for(auto v:sphere(180,430,{31,-18,7})) {
            auto r=row(v,{4,5,6},compass-1); r.chop(1); data+=r+",0,0,0,1\n";
        }
    return data;
}
}

class OfflineMagFitServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void sphereGoldenAdditiveSign();
    void fullEllipsoidGolden();
    void telemetryBucketsAndTruncation();
    void modernAndLegacyCompasses_data();
    void modernAndLegacyCompasses();
    void binaryParityAndTail();
    void scalarEncodings_data();
    void scalarEncodings();
    void telemetryThrottleOffsetsAndThreeCompasses();
    void telemetryNeverMixesSources_data();
    void telemetryNeverMixesSources();
    void telemetryCorruptionWarnings();
    void invalidInputsAndSamples();
    void cancellationAndMutation_data();
    void cancellationAndMutation();
    void limitsAndNonfinite();
    void readerEnforcesSampleLimit();
    void callbackPinsInputsAndOutput();
    void explicitStableProvenanceAllowsApply();
    void incompleteUnsafeProvenanceIsAnalysisOnly_data();
    void incompleteUnsafeProvenanceIsAnalysisOnly();
};

void OfflineMagFitServiceTest::sphereGoldenAdditiveSign()
{
    const auto samples = sphere(320, 480, {42, -27, 13});
    OfflineMagFitResult r; QString error;
    QVERIFY2(OfflineMagFitService::fitSamples(1, samples, samples.size(), {10,20,30}, false, &r, &error), qPrintable(error));
    QVERIFY(std::abs(r.offsets.x - 42) < .05); QVERIFY(std::abs(r.offsets.y + 27) < .05); QVERIFY(std::abs(r.offsets.z - 13) < .05);
    QVERIFY(std::abs(r.sphereRadius - 480) < .05); QVERIFY(r.rmsError < .01);
    QCOMPARE(r.coverageOctants, 8); QVERIFY(!r.hasEllipsoid); QCOMPARE(r.loggedOffsets.y, 20.0);
    QCOMPARE(r.diagonals.x, 1.0); QCOMPARE(r.offDiagonals.x, 0.0);
}

void OfflineMagFitServiceTest::fullEllipsoidGolden()
{
    auto samples = sphere(420, 500);
    for (auto &v : samples) { v.x = v.x / 1.22 - 35; v.y = v.y / .82 + 22; v.z = v.z / .98 - 17; }
    OfflineMagFitResult r; QString error;
    QVERIFY2(OfflineMagFitService::fitSamples(1, samples, samples.size(), {}, true, &r, &error), qPrintable(error));
    QVERIFY(r.hasEllipsoid); QVERIFY(r.rmsError < r.sphereRmsError * .6);
    QVERIFY(std::abs(r.offsets.x - 35) < 1); QVERIFY(std::abs(r.offsets.y + 22) < 1); QVERIFY(std::abs(r.offsets.z - 17) < 1);
    QVERIFY(r.diagonals.x > 1.15 && r.diagonals.x < 1.3);
    QVERIFY(r.diagonals.y > .75 && r.diagonals.y < .9);
    QVERIFY(r.diagonals.z > .9 && r.diagonals.z < 1.05);
    QVERIFY(std::abs(r.diagonals.x*r.diagonals.x + r.diagonals.y*r.diagonals.y + r.diagonals.z*r.diagonals.z - 3) < 1e-10);
}

void OfflineMagFitServiceTest::telemetryBucketsAndTruncation()
{
    QVector<MagVector> samples;
    for (int i = 0; i < 10; ++i) samples.append({101 + i * .1,102,103});
    for (int i = 0; i < 29; ++i) samples.append({200 + i * 25.0,i * 23.0,-i * 21.0});
    samples.append({50000,50000,50000}); QString error;
    const auto filtered = OfflineMagFitService::prepareTelemetrySamples(samples, &error);
    QVERIFY(error.isEmpty()); QCOMPARE(filtered.size(),31);
    int dense = 0; for (auto v : filtered) { dense += v.x >= 100 && v.x < 120; QVERIFY(v.x != 50000); }
    QCOMPARE(dense,3);
    samples = {{-1,1,1},{1,1,1},{-19,1,1},{19,1,1}};
    QCOMPARE(OfflineMagFitService::prepareTelemetrySamples(samples).size(),3); // floor would create two cells.
}

void OfflineMagFitServiceTest::modernAndLegacyCompasses_data()
{
    QTest::addColumn<QByteArray>("alias");
    QTest::newRow("I") << QByteArray("I"); QTest::newRow("Instance") << QByteArray("Instance"); QTest::newRow("C") << QByteArray("C");
    QTest::newRow("legacy") << QByteArray();
}
void OfflineMagFitServiceTest::modernAndLegacyCompasses()
{
    QFETCH(QByteArray, alias); QTemporaryDir dir; QByteArray data;
    if (!alias.isEmpty()) data = "FMT,130,28,MAG,Bffffff," + alias + ",X,Y,Z,OX,OY,OZ\n";
    else data = "FMT,130,15,MAG,fff,X,Y,Z\nFMT,131,15,MAG2,fff,X,Y,Z\nFMT,132,15,MAG3,fff,X,Y,Z\n";
    for (int compass = 1; compass <= 3; ++compass) {
        for (const auto &v : sphere(180, 400 + 20 * compass, {double(compass*10),-18,7})) {
            if (!alias.isEmpty()) data += row(v, {4,5,6}, compass - 1);
            else data += (compass == 1 ? QByteArray("MAG") : "MAG" + QByteArray::number(compass))
                + ',' + n(v.x) + ',' + n(v.y) + ',' + n(v.z) + '\n';
        }
    }
    const auto path = dir.filePath("in.log"); QVERIFY(put(path,data));
    const auto r = OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.success,qPrintable(r.error)); QCOMPARE(r.results.size(),3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(r.results[i].compass,i+1); QCOMPARE(r.results[i].sourceSamples,180);
        QVERIFY(std::abs(r.results[i].offsets.x-(i+1)*10)<.1);
        QCOMPARE(r.results[i].loggedOffsets.x,alias.isEmpty()?0.0:4.0);
    }
}

void OfflineMagFitServiceTest::binaryParityAndTail()
{
    QTemporaryDir dir; const auto text = dir.filePath("in.log"), bin = dir.filePath("in.bin");
    QVERIFY(put(text,textLog())); QVERIFY(put(bin,binaryLog()));
    const auto a = OfflineMagFitService::analyze(text,sphereOptions());
    const auto b = OfflineMagFitService::analyze(bin,sphereOptions());
    QVERIFY2(a.success,qPrintable(a.error)); QVERIFY2(b.success,qPrintable(b.error));
    QVERIFY(std::abs(a.results[0].offsets.x-b.results[0].offsets.x)<1e-8);
    QVERIFY(put(bin,binaryLog()+frame(130,9)));
    const auto tail = OfflineMagFitService::analyze(bin,sphereOptions());
    QVERIFY2(tail.success,qPrintable(tail.error)); QCOMPARE(tail.results[0].usedSamples,180);
    QVERIFY(tail.warnings.join(' ').contains("9 bytes"));
    QVERIFY(put(bin,binaryLog()+frame(128,9)));
    QVERIFY(!OfflineMagFitService::analyze(bin,sphereOptions()).success);
}

void OfflineMagFitServiceTest::scalarEncodings_data()
{
    QTest::addColumn<char>("type"); QTest::addColumn<int>("width"); QTest::addColumn<double>("factor");
    QTest::newRow("int16") << 'h' << 2 << 1.0; QTest::newRow("centi16") << 'c' << 2 << 100.0;
    QTest::newRow("centi32") << 'e' << 4 << 100.0; QTest::newRow("lat32") << 'L' << 4 << 1e7;
    QTest::newRow("float") << 'f' << 4 << 1.0;
}
void OfflineMagFitServiceTest::scalarEncodings()
{
    QFETCH(char,type); QFETCH(int,width); QFETCH(double,factor);
    const int len=3+3*width; QByteArray data=fmt(130,len,"MAG",QByteArray(3,type),"X,Y,Z");
    const double radius=type=='L'?100:200;
    for (auto v : sphere(180,radius,{12,-8,4})) {
        auto r=frame(130,len); const double values[]={v.x,v.y,v.z};
        for(int j=0;j<3;++j) {
            auto *at=reinterpret_cast<uchar*>(r.data()+3+j*width);
            if(type=='f') { float number=float(values[j]); quint32 bits; std::memcpy(&bits,&number,4); qToLittleEndian(bits,at); }
            else if(width==2) qToLittleEndian<qint16>(qint16(std::round(values[j]*factor)),at);
            else qToLittleEndian<qint32>(qint32(std::round(values[j]*factor)),at);
        }
        data+=r;
    }
    QTemporaryDir dir; const auto path=dir.filePath("in.bin"); QVERIFY(put(path,data));
    const auto r=OfflineMagFitService::analyze(path,sphereOptions()); QVERIFY2(r.success,qPrintable(r.error));
    QVERIFY(std::abs(r.results[0].offsets.x-12)<.2); QVERIFY(std::abs(r.results[0].offsets.y+8)<.2);
}

void OfflineMagFitServiceTest::telemetryThrottleOffsetsAndThreeCompasses()
{
    QByteArray data; offsets(data,{7,-4,3}); hud(data,10);
    for(auto v:sphere(80,350,{18,-12,8})) imu(data,{v.x+7,v.y-4,v.z+3});
    hud(data,60);
    // Foreign HUD and offsets must not contaminate the magnetometer source.
    hud(data,0,2); offsets(data,{100,200,300},2);
    for(auto v:sphere(240,420,{18,-12,8})) imu(data,{v.x+7,v.y-4,v.z+3});
    for(auto v:sphere(240,390,{-15,24,11})) imu(data,v,2);
    for(auto v:sphere(240,440,{21,9,-19})) imu(data,v,3);
    QTemporaryDir dir; const auto path=dir.filePath("in.tlog"); QVERIFY(put(path,data));
    const auto r=OfflineMagFitService::analyze(path,sphereOptions()); QVERIFY2(r.success,qPrintable(r.error));
    QCOMPARE(r.results.size(),3); QVERIFY(r.isTelemetryLog); QCOMPARE(r.throttleThreshold,30);
    QVERIFY(!r.applyEligible); QVERIFY(r.applyUnavailableReason.contains("Telemetry"));
    QCOMPARE(r.results[0].sourceSamples,240); QVERIFY(r.results[0].usedSamples<240);
    QCOMPARE(r.results[0].loggedOffsets.x,7.0); QCOMPARE(r.results[1].loggedOffsets.x,0.0);
    QVERIFY(std::abs(r.results[0].offsets.x-18)<3); QVERIFY(std::abs(r.results[1].offsets.x+15)<3);
    QVERIFY(std::abs(r.results[2].offsets.z+19)<3);
}

void OfflineMagFitServiceTest::telemetryNeverMixesSources_data()
{
    QTest::addColumn<int>("system"); QTest::addColumn<int>("component");
    QTest::newRow("other system")<<2<<1; QTest::newRow("same system other component")<<1<<2;
}
void OfflineMagFitServiceTest::telemetryNeverMixesSources()
{
    QFETCH(int,system); QFETCH(int,component); QByteArray data;
    hud(data,0); imu(data,{1,2,3}); imu(data,{},1,system,component);
    QTemporaryDir dir; const auto path=dir.filePath("in.tlog"); QVERIFY(put(path,data));
    const auto r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY(!r.success); QVERIFY(r.error.contains("Multiple")); QVERIFY(r.results.isEmpty());
}

void OfflineMagFitServiceTest::telemetryCorruptionWarnings()
{
    QByteArray data("garbage"); hud(data,60);
    for(auto v:sphere(180,430,{31,-18,7})) imu(data,v);
    data += QByteArray(3,'x');
    QTemporaryDir dir; const auto path=dir.filePath("in.tlog"); QVERIFY(put(path,data));
    const auto r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.success,qPrintable(r.error)); QVERIFY(r.warnings.join(' ').contains("skipped"));
    QVERIFY(r.warnings.join(' ').contains("tail"));
}

void OfflineMagFitServiceTest::invalidInputsAndSamples()
{
    QTemporaryDir dir;
    QVERIFY(!OfflineMagFitService::analyze(dir.path(),sphereOptions()).success);
    QVERIFY(!OfflineMagFitService::analyze(dir.filePath("missing.log"),sphereOptions()).success);
    const auto path=dir.filePath("in.log"); QVERIFY(put(path,{}));
    QVERIFY(!OfflineMagFitService::analyze(path,sphereOptions()).success);
    QVERIFY(put(path,textLog(9))); QVERIFY(!OfflineMagFitService::analyze(path,sphereOptions()).success);
    QVERIFY(put(path,textLog()+"MAG,0,1,2\n")); QVERIFY(!OfflineMagFitService::analyze(path,sphereOptions()).success);
    OfflineMagFitResult r; QString error;
    QVERIFY(!OfflineMagFitService::fitSamples(0,sphere(20,300),20,{},false,&r,&error));
    QVERIFY(!OfflineMagFitService::fitSamples(1,sphere(20,300),20,{},false,nullptr,&error));
    QVERIFY(!OfflineMagFitService::fitSamples(1,QVector<MagVector>(10),10,{},false,&r,&error));
    QVERIFY(!error.isEmpty());
}

void OfflineMagFitServiceTest::cancellationAndMutation_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("pre cancel")<<0; QTest::newRow("read cancel")<<1;
    QTest::newRow("solver cancel")<<2; QTest::newRow("final cancel")<<3;
    QTest::newRow("read mutation")<<4; QTest::newRow("final mutation")<<5;
}
void OfflineMagFitServiceTest::cancellationAndMutation()
{
    QFETCH(int,mode); QTemporaryDir dir; const auto path=dir.filePath("in.log"); QVERIFY(put(path,textLog(1500)));
    auto o=sphereOptions(); bool cancel=mode==0, mutated=false; int solverChecks=0; double progress=0;
    o.progress=[&](double p) {
        progress=p;
        if((mode==1 && p>0 && p<.75)||(mode==3 && p==1)) cancel=true;
        if(!mutated && ((mode==4 && p>0 && p<.75)||(mode==5 && p==1))) {
            mutated=true; QFile f(path); if(f.open(QIODevice::Append)) f.write("\n");
        }
    };
    o.isCancelled=[&] { if(mode==2 && progress>=.75 && ++solverChecks>20) return true; return cancel; };
    const auto r=OfflineMagFitService::analyze(path,o);
    QVERIFY(!r.success); QVERIFY(r.results.isEmpty());
    if(mode<4) QVERIFY(r.cancelled); else { QVERIFY(mutated); QVERIFY(r.error.contains("changed")); }
}

void OfflineMagFitServiceTest::limitsAndNonfinite()
{
    auto points=sphere(20,300); points[0].x=std::numeric_limits<double>::quiet_NaN();
    OfflineMagFitResult r; QString error;
    QVERIFY(!OfflineMagFitService::fitSamples(1,points,20,{},false,&r,&error));
    QVERIFY(OfflineMagFitService::prepareTelemetrySamples(points,&error).isEmpty()); QVERIFY(!error.isEmpty());
    points=QVector<MagVector>(OfflineMagFitService::MaximumSamplesPerCompass+1,MagVector{1,2,3});
    QVERIFY(!OfflineMagFitService::fitSamples(1,points,points.size(),{},false,&r,&error));
    QVERIFY(OfflineMagFitService::prepareTelemetrySamples(points,&error).isEmpty());
    QTemporaryDir dir; const auto path=dir.filePath("in.log");
    QVERIFY(put(path,textLog()+"MAG,0,nan,2,3,4,5,6\nMAG,0,4,5,6,4,5,6\n\n"));
    const auto report=OfflineMagFitService::analyze(path,sphereOptions()); QVERIFY2(report.success,qPrintable(report.error));
    QCOMPARE(report.results[0].sourceSamples,180); QVERIFY(report.warnings.join(' ').contains("Skipped 2"));
    QVERIFY(report.warnings.join(' ').contains("blank"));
}

void OfflineMagFitServiceTest::callbackPinsInputsAndOutput()
{
    auto points=sphere(180,430,{31,-18,7}); OfflineMagFitResult r; r.loggedOffsets={4,5,6}; QString error;
    bool once=false;
    QVERIFY2(OfflineMagFitService::fitSamples(1,points,180,r.loggedOffsets,false,&r,&error,[&] {
        if(!once) { once=true; points.clear(); } return false;
    }),qPrintable(error));
    QCOMPARE(r.sourceSamples,180); QCOMPARE(r.loggedOffsets.y,5.0);
    QVERIFY(std::abs(r.offsets.x-31)<.1);
}

void OfflineMagFitServiceTest::readerEnforcesSampleLimit()
{
    QTemporaryDir dir; const auto path=dir.filePath("in.log");
    const QByteArray data="FMT,130,28,MAG,Bffffff,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ\n"
        + QByteArray("MAG,0,1,2,3,0,0,0\n").repeated(OfflineMagFitService::MaximumSamplesPerCompass+1);
    QVERIFY(put(path,data));
    const auto r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY(!r.success); QVERIFY(r.error.contains("safe limit")); QVERIFY(r.results.isEmpty());
}

void OfflineMagFitServiceTest::explicitStableProvenanceAllowsApply()
{
    QTemporaryDir dir; const auto path=dir.filePath("in.log");
    auto data=eligibleLog(3); QVERIFY(put(path,data));
    auto r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.success,qPrintable(r.error)); QVERIFY2(r.applyEligible,qPrintable(r.applyUnavailableReason));
    QCOMPARE(r.results.size(),3); QCOMPARE(r.loggedDeviceIds.value(1),quint32(101));
    QCOMPARE(r.loggedDeviceIds.value(2),quint32(202)); QCOMPARE(r.loggedDeviceIds.value(3),quint32(303));
    QCOMPARE(r.loggedFrameParameters.size(),7);
    QCOMPARE(r.loggedFrameParameters.value("AHRS_ORIENTATION"),0.0);
    QCOMPARE(r.loggedFrameParameters.value("COMPASS_EXTERN2"),1.0);
    QVERIFY(r.applyUnavailableReason.isEmpty());
    // Explicit disabled matrix/scale values are allowed; absence is not.
    data.replace("_X,1\n","_X,0\n"); data.replace("_Y,1\n","_Y,0\n"); data.replace("_Z,1\n","_Z,0\n");
    data.replace("COMPASS_SCALE,1\n","COMPASS_SCALE,0\n");
    QVERIFY(put(path,data)); r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.applyEligible,qPrintable(r.applyUnavailableReason));
    data.replace("AHRS_ORIENTATION,0\n","AHRS_ORIENTATION,43\n");
    data.replace("COMPASS_ORIENT,0\n","COMPASS_ORIENT,43\n");
    data.replace("COMPASS_EXTERNAL,1\n","COMPASS_EXTERNAL,2\n");
    QVERIFY(put(path,data)); r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.applyEligible,qPrintable(r.applyUnavailableReason));
    QCOMPARE(r.loggedFrameParameters.value("AHRS_ORIENTATION"),43.0);
    QCOMPARE(r.loggedFrameParameters.value("COMPASS_EXTERNAL"),2.0);
}

void OfflineMagFitServiceTest::incompleteUnsafeProvenanceIsAnalysisOnly_data()
{
    QTest::addColumn<QByteArray>("data"); QTest::addColumn<QString>("reason");
    QTest::newRow("unknown")<<textLog()<<QString("prior PARM");
    auto data=eligibleLog(); data.replace("PARM,COMPASS_DIA_X,1\n","");
    QTest::newRow("missing DIA")<<data<<QString("prior PARM");
    data=eligibleLog(); data.replace("COMPASS_DIA_X,1\n","COMPASS_DIA_X,1.2\n");
    QTest::newRow("DIA")<<data<<QString("nonidentity DIA");
    data=eligibleLog(); data.replace("COMPASS_ODI_X,0\n","COMPASS_ODI_X,0.1\n");
    QTest::newRow("ODI")<<data<<QString("nonzero ODI");
    data=eligibleLog(); data.replace("COMPASS_SCALE,1\n","COMPASS_SCALE,1.1\n");
    QTest::newRow("scale")<<data<<QString("scale correction");
    data=eligibleLog(); data.replace("COMPASS_DEV_ID,101\n","COMPASS_DEV_ID,0\n");
    QTest::newRow("zero ID")<<data<<QString("exact nonzero");
    data=eligibleLog(); data.replace("COMPASS_DEV_ID,101\n","COMPASS_DEV_ID,101.5\n");
    QTest::newRow("fractional ID")<<data<<QString("exact nonzero");
    data=eligibleLog(); data.replace("COMPASS_PRIO1_ID,101\n","COMPASS_PRIO1_ID,202\n");
    QTest::newRow("priority remapping")<<data<<QString("ordering disagrees");
    data=eligibleLog()+"PARM,COMPASS_DIA_X,1.1\n";
    QTest::newRow("late change")<<data<<QString("changed");
    data=eligibleLog()+"PARM,COMPASS_DIA_X,nan\n";
    QTest::newRow("late invalid")<<data<<QString("invalid/non-finite");
    data=eligibleLog(); const auto prefix=provenanceParameters(); data.remove(0,prefix.size());
    data="FMT,129,23,PARM,Nf,Name,Value\n"+data+prefix;
    QTest::newRow("metadata only after samples")<<data<<QString("prior PARM");
    data=eligibleLog(); data.replace(",4,5,6,0,0,0,1\n",",4,5,6,0,1,0,1\n");
    QTest::newRow("motor")<<data<<QString("nonzero motor");
    data=eligibleLog(); data.replace(",4,5,6,0,0,0,1\n",",nan,5,6,0,0,0,1\n");
    QTest::newRow("missing offset")<<data<<QString("complete finite logged offsets");
    data=eligibleLog(); data.replace("MAG,0,","MAG,4,"); data+=eligibleLog();
    QTest::newRow("unsafe instance")<<data<<QString("invalid or conflicting compass instances");
    data=eligibleLog(); data.replace("PARM,AHRS_ORIENTATION,0\n","");
    QTest::newRow("missing board orientation")<<data<<QString("prior frame parameter AHRS_ORIENTATION");
    data=eligibleLog(); data.replace("PARM,COMPASS_ORIENT,0\n","");
    QTest::newRow("missing sensor orientation")<<data<<QString("prior frame parameter COMPASS_ORIENT");
    data=eligibleLog(); data.replace("PARM,COMPASS_EXTERNAL,1\n","");
    QTest::newRow("missing external flag")<<data<<QString("prior frame parameter COMPASS_EXTERNAL");
    data=eligibleLog()+"PARM,COMPASS_ORIENT,2\n";
    QTest::newRow("changed orientation")<<data<<QString("changed");
    data=eligibleLog(); data.replace("COMPASS_ORIENT,0\n","COMPASS_ORIENT,100\n");
    QTest::newRow("custom orientation")<<data<<QString("custom, invalid or unsupported");
    data=eligibleLog(); data.replace("AHRS_ORIENTATION,0\n","AHRS_ORIENTATION,1.5\n");
    QTest::newRow("fractional orientation")<<data<<QString("custom, invalid or unsupported");
    data=eligibleLog(); data.replace("COMPASS_EXTERNAL,1\n","COMPASS_EXTERNAL,3\n");
    QTest::newRow("invalid external flag")<<data<<QString("custom, invalid or unsupported");
    data=eligibleLog(); data.replace(",0,0,0,1\n",",0,0,0,0\n");
    QTest::newRow("unhealthy samples")<<data<<QString("Health=1");
    data=eligibleLog(); data.replace(",Health\n",",Unknown\n");
    QTest::newRow("missing health")<<data<<QString("Health=1");
}
void OfflineMagFitServiceTest::incompleteUnsafeProvenanceIsAnalysisOnly()
{
    QFETCH(QByteArray,data); QFETCH(QString,reason); QTemporaryDir dir; const auto path=dir.filePath("in.log");
    QVERIFY(put(path,data)); const auto r=OfflineMagFitService::analyze(path,sphereOptions());
    QVERIFY2(r.success,qPrintable(r.error)); QVERIFY(!r.applyEligible);
    QVERIFY2(r.applyUnavailableReason.contains(reason),qPrintable(r.applyUnavailableReason));
    QVERIFY(!r.results.isEmpty());
}

QTEST_GUILESS_MAIN(OfflineMagFitServiceTest)
#include "test_offlinemagfitservice.moc"
