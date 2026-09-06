#include "ui/Loghandling/DataFlashLogAnalyzer.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
using A = DataFlashLogAnalyzer;
A::Sample sample(qint64 line, A::Values values, A::TextValues text = {})
{ return {line, line / 10.0, std::move(values), std::move(text)}; }
A::Data data(QVector<A::RecordGroup> records = {}, A::VehicleType type = A::VehicleType::Unknown, A::Values parameters = {})
{ return {1000, type, std::move(records), std::move(parameters)}; }
A::TestResult find(const A::Result &result, const QString &name)
{
    for (const auto &test : result.tests) if (test.name == name) return test;
    return {};
}
bool write(const QString &path, const QByteArray &bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString &path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QByteArray record(int id, QByteArray payload)
{ return QByteArray::fromHex("a395") + char(id) + payload; }
QByteArray fmt(int id, int length, const QByteArray &name, const QByteArray &format, const QByteArray &columns)
{
    QByteArray payload; payload += char(id); payload += char(length);
    payload += name.leftJustified(4, '\0'); payload += format.leftJustified(16, '\0'); payload += columns.leftJustified(64, '\0');
    return record(128, payload);
}
QByteArray number(float value)
{
    quint32 bits; std::memcpy(&bits, &value, sizeof(bits)); QByteArray bytes(4, '\0');
    qToLittleEndian(bits, reinterpret_cast<uchar *>(bytes.data())); return bytes;
}
}

class DataFlashLogAnalyzerTest final : public QObject
{
    Q_OBJECT
private slots:
    void completeCheckInventoryAndExactFormat()
    {
        const auto result = A::Analyze(data()); QVERIFY2(result.success, qPrintable(result.error));
        QStringList names; for (const auto &test : result.tests) names.append(test.name);
        QCOMPARE(names, QStringList({"Empty", "Vibration", "GPS", "VCC", "Compass", "Motor balance", "NaN",
            "Event/Failsafe", "Brownout", "Duplicate data", "Parameters", "PM", "Pitch/Roll", "Thrust",
            "IMU mismatch", "Autotune", "Optical flow"}));
        QCOMPARE(A::Format({{"Empty", "FAIL", "log contains no records"}, {"NaN", "GOOD", "no invalid numeric values"}}),
            QStringLiteral("[FAIL] Empty: log contains no records\n[GOOD] NaN: no invalid numeric values"));
        QCOMPARE(A::Classify(30, 30, 60, true), QStringLiteral("WARN"));
        QCOMPARE(A::Classify(60, 30, 60, true), QStringLiteral("FAIL"));
        QCOMPARE(A::Classify(7, 7, 5, false), QStringLiteral("WARN"));
        QCOMPARE(A::Classify(5, 7, 5, false), QStringLiteral("FAIL"));
        QCOMPARE(A::Classify(std::numeric_limits<double>::quiet_NaN(), 30, 60, true), QStringLiteral("NA"));
        QCOMPARE(A::Classify(10, 30, 60, true), QStringLiteral("GOOD"));
        QCOMPARE(A::Format({}), QString());
    }

    void thresholdBoundaries_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("check"); QTest::addColumn<double>("number"); QTest::addColumn<QString>("status");
        QTest::newRow("vibration-below") << QString("VIBE") << QString("VibeX") << QString("Vibration") << 29.99 << QString("GOOD");
        QTest::newRow("vibration-warn") << QString("VIBE") << QString("VibeX") << QString("Vibration") << -30.0 << QString("WARN");
        QTest::newRow("vibration-fail") << QString("VIBE") << QString("VibeX") << QString("Vibration") << 60.0 << QString("FAIL");
        QTest::newRow("gps-sat5") << QString("GPS") << QString("NSat") << QString("GPS") << 5.0 << QString("WARN");
        QTest::newRow("gps-sat6") << QString("GPS") << QString("NSat") << QString("GPS") << 6.0 << QString("GOOD");
        QTest::newRow("gps-hdop10") << QString("GPS2") << QString("EPH") << QString("GPS") << 10.0 << QString("WARN");
        QTest::newRow("gps-hdop-over10") << QString("GPS2") << QString("EPH") << QString("GPS") << 10.01 << QString("FAIL");
        QTest::newRow("vcc-millivolt") << QString("POWR") << QString("Vcc") << QString("VCC") << 5000.0 << QString("GOOD");
        QTest::newRow("vcc-warn") << QString("POWR") << QString("Vcc") << QString("VCC") << 4.3 << QString("WARN");
        QTest::newRow("vcc-fail") << QString("POWR") << QString("Vcc") << QString("VCC") << 4.299 << QString("FAIL");
        QTest::newRow("normalized-throttle-fail") << QString("CTUN") << QString("ThO") << QString("Empty") << 0.19 << QString("FAIL");
        QTest::newRow("normalized-throttle-good") << QString("CTUN") << QString("ThO") << QString("Empty") << 0.2 << QString("GOOD");
        QTest::newRow("copter-throttle-fail") << QString("CTUN") << QString("ThrOut") << QString("Empty") << 199.0 << QString("FAIL");
        QTest::newRow("copter-throttle-good") << QString("CTUN") << QString("ThrOut") << QString("Empty") << 200.0 << QString("GOOD");
    }
    void thresholdBoundaries()
    {
        QFETCH(QString, type); QFETCH(QString, field); QFETCH(QString, check); QFETCH(double, number); QFETCH(QString, status);
        const auto result = A::Analyze(data({{type, {sample(1, {{field, number}})}}}, A::VehicleType::Copter));
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(find(result, check).status, status);
    }

    void actualDotNetNumericFormattingAndSaturatingConversion()
    {
        QCOMPARE(find(A::Analyze(data({{"VIBE", {sample(1, {{"VibeX", 1.25}})}}})), "Vibration").message,
            QStringLiteral("maximum 1.3 m/s/s"));
        for (const auto &tie : QVector<QPair<double, QString>>{{0.125, "0.13"}, {2.675, "2.68"}, {1.005, "1.01"}}) {
            const auto result = A::Analyze(data({{"POWR", {sample(1, {{"Vcc", tie.first}})}}}));
            QCOMPARE(find(result, "VCC").message, "minimum " + tie.second + " V, spread 0.00 V");
        }
        const auto grouped = A::Analyze(data({{"MAG", {
            sample(1, {{"MagX", 1}, {"MagY", 0}, {"MagZ", 0}}),
            sample(2, {{"MagX", 581.5}, {"MagY", 0}, {"MagZ", 0}})}}}));
        QCOMPARE(find(grouped, "Compass").message,
            QStringLiteral("magnetic field change 58,050.0 %, range 1-582"));
        const auto large = A::Analyze(data({}, A::VehicleType::Copter, {{"THR_MIN", 1e10}}));
        QCOMPARE(find(large, "Parameters").message, QStringLiteral("THR_MIN=10000000000 must be below 200"));
        const auto nanFrame = A::Analyze(data({{"RCOU", {sample(1, {{"C1", 1000}, {"C2", 1000},
            {"C3", 1000}, {"C4", 1000}, {"C5", 2500}})}}}, A::VehicleType::Copter,
            {{"FRAME_CLASS", std::numeric_limits<double>::quiet_NaN()}}));
        QCOMPARE(find(nanFrame, "Motor balance").message, QStringLiteral("4 channel averages, output spread 0 us"));
        // Huge positive event codes saturate rather than wrapping to a valid event.
        const auto hugeEvent = A::Analyze(data({{"EV", {sample(1, {{"Id", 1e10}})}},
            {"CTUN", {sample(2, {{"Alt", 10}})}}}));
        QCOMPARE(find(hugeEvent, "Brownout").status, QStringLiteral("GOOD"));
    }

    void aliasesFallbacksAndNonfiniteOrder()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        auto source = data({{"GPS", {sample(1, {{"Status", 3}})}},
            {"GPS2", {sample(2, {{"NSats", 10}, {"HDop", 1}})}},
            {"CURR", {sample(3, {{"Vcc", 5000}}), sample(4, {{"Vcc", 4500}})}},
            {"VIBE", {sample(5, {{"VibeZ", nan}, {"VibeX", nan}})}}});
        const auto result = A::Analyze(source); QVERIFY(result.success);
        QCOMPARE(find(result, "GPS").status, QStringLiteral("UNKNOWN")); // GPS2 only if GPS has no rows.
        QCOMPARE(find(result, "VCC").message, QStringLiteral("minimum 4.50 V, spread 0.50 V"));
        QCOMPARE(find(result, "NaN").message, QStringLiteral("invalid number in VIBE.VibeZ at line 5"));
        source = data({}, A::VehicleType::Copter, {{"second", nan}, {"first", nan}});
        QCOMPARE(find(A::Analyze(source), "NaN").message, QStringLiteral("invalid parameter value: second"));
        QCOMPARE(find(A::Analyze(source), "Parameters").message, QStringLiteral("second is not finite; first is not finite"));
    }

    void compassMotorEventsAndBrownout()
    {
        const auto source = data({{"MAG", {sample(1, {{"OfsX", 700}, {"OfsY", 0}, {"OfsZ", 0}, {"MagX", 100}, {"MagY", 0}, {"MagZ", 0}}),
                sample(2, {{"MagX", 125}, {"MagY", 0}, {"MagZ", 0}})}},
            {"RCOU", {sample(3, {{"C1", 1000}, {"Ch2", 1200}, {"Chan3", 1100}, {"C4", 1000}, {"C5", 2900}})}},
            {"ERR", {sample(4, {{"Subsys", 9}, {"ECode", 1}}), sample(5, {{"Subsys", 11}, {"ECode", 2}})}},
            {"GPS", {sample(6, {{"NSats", 10}, {"HDop", 1}})}},
            {"EV", {sample(7, {{"Id", 10}})}}, {"CTUN", {sample(8, {{"BarAlt", 7.5}, {"ThO", 0.5}})}}},
            A::VehicleType::Copter, {{"FRAME_CLASS", 0}, {"COMPASS_OFS_X", 400}, {"COMPASS_OFS_Y", 0}, {"COMPASS_OFS_Z", 0}});
        const auto result = A::Analyze(source); QVERIFY(result.success);
        QCOMPARE(find(result, "Compass").message, QStringLiteral("parameter compass offset magnitude 400; logged compass offset magnitude 700; magnetic field change 25.0 %, range 100-125"));
        QCOMPARE(find(result, "Motor balance").message, QStringLiteral("4 channel averages, output spread 200 us"));
        QCOMPARE(find(result, "Event/Failsafe").message, QStringLiteral("FENCE, GPS_GLITCH"));
        QCOMPARE(find(result, "GPS").status, QStringLiteral("FAIL"));
        QCOMPARE(find(result, "Brownout").message, QStringLiteral("log ends armed at 7.50 m; possible truncation/brownout"));
        const auto fence = A::Analyze(data({{"ERR", {sample(1, {{"Subsys", 9}, {"ECode", 1}})}}}));
        QCOMPARE(find(fence, "Event/Failsafe").status, QStringLiteral("WARN"));
    }

    void duplicateWindowsAndConstantSamples()
    {
        QVector<A::Sample> samples;
        for (int i = 0; i < 100; ++i) samples.append(sample(i, {{"Pitch", double(i >= 40 && i < 60 ? i - 40 : i)}}));
        auto result = A::Analyze(data({{"ATT", samples}})); QVERIFY(result.success);
        QCOMPARE(find(result, "Duplicate data").message, QStringLiteral("duplicate 20-sample ATT.Pitch chunks at lines 0 and 40"));
        for (auto &s : samples) s.values[0].second = 0;
        result = A::Analyze(data({{"ATT", samples}})); QCOMPARE(find(result, "Duplicate data").status, QStringLiteral("GOOD"));
    }

    void parametersPerformanceAndUnsortedLeanJoins()
    {
        auto source = data({{"ATT", {sample(10, {{"Roll", 80}, {"Pitch", 0}})}},
            {"CTUN", {sample(20, {{"BarAlt", 0}}), sample(5, {{"BarAlt", 5}})}},
            {"MODE", {sample(50, {}, {{"Mode", "ACRO"}}), sample(1, {}, {{"Mode", "LOITER"}})}},
            {"PM", {sample(30, {{"NLon", 7}, {"NLoop", 100}})}}}, A::VehicleType::Copter,
            {{"MAG_ENABLE", 0}, {"THR_MIN", 200}, {"THR_MID", 701}, {"ANGLE_MAX", 4500}});
        auto result = A::Analyze(source); QVERIFY(result.success);
        QCOMPARE(find(result, "Parameters").message, QStringLiteral("MAG_ENABLE=0 must equal 1; THR_MIN=200 must be below 200; THR_MID=701 must be between 300 and 700"));
        QCOMPARE(find(result, "PM").status, QStringLiteral("WARN"));
        QCOMPARE(find(result, "Pitch/Roll").message, QStringLiteral("roll 80.00° exceeds buffered lean limit 55.00° at line 10"));
        for (const QString mode : {QString("ACRO"), QString("SPORT"), QString("FLIP"), QString("AUTOTUNE")}) {
            source.records[2].samples[1].textValues[0].second = mode;
            QCOMPARE(find(A::Analyze(source), "Pitch/Roll").status, QStringLiteral("GOOD"));
        }
    }

    void thrustRequiresMoreThanFiftyAndNearestTieUsesSourceOrder()
    {
        QVector<A::Sample> tuning;
        for (int i = 1; i <= 50; ++i) tuning.append(sample(i, {{"ThrOut", 800}, {"CRate", 20}}));
        auto source = data({{"ATT", {sample(0, {{"Roll", 0}, {"Pitch", 0}})}}, {"CTUN", tuning}}, A::VehicleType::Copter);
        QCOMPARE(find(A::Analyze(source), "Thrust").status, QStringLiteral("GOOD"));
        source.records[1].samples.append(sample(51, {{"ThrOut", 800}, {"CRate", 20}}));
        QCOMPARE(find(A::Analyze(source), "Thrust").message, QStringLiteral("average climb 20.0 cm/s at throttle 800"));
        // Equidistant samples: source-first later line has high roll and must win.
        source.records[0].samples = {sample(2, {{"Roll", 30}, {"Pitch", 0}}), sample(0, {{"Roll", 0}, {"Pitch", 0}})};
        for (auto &s : source.records[1].samples) s.line = 1;
        QCOMPARE(find(A::Analyze(source), "Thrust").status, QStringLiteral("GOOD"));
    }

    void imuTimeAlignmentAndAutotuneFinalSession()
    {
        QVector<A::Sample> first, second;
        for (int i = 1; i <= 400; ++i) {
            first.append(sample(i, {{"AccX", 4}, {"AccY", 0}, {"AccZ", 9.8}}));
            second.append(sample(i, {{"AccX", 0}, {"AccY", 0}, {"AccZ", 9.8}}));
        }
        const auto mismatch = A::Analyze(data({{"IMU", first}, {"IMU2", second}})); QVERIFY(mismatch.success);
        QCOMPARE(find(mismatch, "IMU mismatch").status, QStringLiteral("FAIL"));
        for (int outcome : {33, 34, 35}) {
            const auto result = A::Analyze(data({{"EV", {sample(1, {{"Id", 30}}), sample(2, {{"Id", double(outcome)}})}},
                {"ATUN", {sample(2, {{"Axis", 0}})}}}, A::VehicleType::Copter));
            QCOMPARE(find(result, "Autotune").status, outcome == 33 ? QStringLiteral("GOOD") : QStringLiteral("FAIL"));
        }
        const auto lastIncomplete = A::Analyze(data({{"EV", {sample(1, {{"Id", 30}}), sample(2, {{"Id", 33}}), sample(3, {{"Id", 30}})}},
            {"ATDE", {sample(2, {})}}}, A::VehicleType::Copter));
        QCOMPARE(find(lastIncomplete, "Autotune").message, QStringLiteral("2 session(s); last session has no final result"));
    }

    void opticalFlowFitProducesReviewableScalers()
    {
        QVector<A::Sample> flow;
        for (int i = 1; i <= 140; ++i) {
            double body = (i % 20 - 10) / 10.0; if (body == 0) body = 0.1;
            flow.append(sample(i, {{"bodyX", body}, {"flowX", body}, {"bodyY", body}, {"flowY", body}, {"Qual", 200}}));
        }
        auto source = data({{"OF", flow}, {"ATT", {sample(1, {{"Roll", 20}, {"Pitch", 20}})}}});
        auto result = A::Analyze(source); QVERIFY(result.success);
        QCOMPARE(find(result, "Optical flow").message, QStringLiteral("recommended FLOW_FXSCALER=0, FLOW_FYSCALER=0; slope σ=0.0/0.0"));
        QCOMPARE(find(result, "Optical flow").status, QStringLiteral("GOOD"));
        for (auto &s : source.records[0].samples) s.values[4].second = 124;
        QCOMPARE(find(A::Analyze(source), "Optical flow").message, QStringLiteral("insufficient high-quality samples (X=0, Y=0, need 100)"));
    }

    void rawBinaryTextParityAndFloatDecimalSemantics()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QByteArray bin = fmt(1, 15, "VIBE", "fff", "VibeX,VibeY,VibeZ")
            + fmt(2, 7, "POWR", "f", "Vcc")
            + record(1, number(1.1f) + number(30) + number(-2)) + record(2, number(5.1f));
        const QByteArray text = "FMT, 1, 15, VIBE, fff, VibeX,VibeY,VibeZ\nFMT, 2, 7, POWR, f, Vcc\nVIBE, 1.1, 30, -2\nPOWR, 5.1\n";
        const auto binaryPath = directory.filePath("fixture.BIN"), textPath = directory.filePath("fixture.LOG");
        QVERIFY(write(binaryPath, bin)); QVERIFY(write(textPath, text));
        const auto binary = A::Analyze(binaryPath), ascii = A::Analyze(textPath);
        QVERIFY2(binary.success, qPrintable(binary.error)); QVERIFY2(ascii.success, qPrintable(ascii.error));
        QCOMPARE(A::Format(binary.tests), A::Format(ascii.tests));
        QCOMPARE(find(binary, "VCC").message, QStringLiteral("minimum 5.10 V, spread 0.00 V"));
        QCOMPARE(read(binaryPath), bin); QCOMPARE(read(textPath), text);
        // An incomplete known final data record is explicitly reported.
        QVERIFY(write(binaryPath, bin + record(1, number(40))));
        const auto tail = A::Analyze(binaryPath); QVERIFY(tail.success); QVERIFY(!tail.warnings.isEmpty());
        QCOMPARE(A::Format(tail.tests), A::Format(binary.tests));
    }

    void fileVehicleDetectionParametersAndModeNames()
    {
        QTemporaryDir directory; const auto path = directory.filePath("fixture.log");
        const QByteArray text = "FMT, 1, 23, PARM, Nf, Name,Value\nFMT, 2, 67, MSG, Z, Message\n"
            "FMT, 3, 11, ATT, ff, Roll,Pitch\nFMT, 4, 7, CTUN, f, BarAlt\nFMT, 5, 19, MODE, N, Mode\n"
            "PARM, FRAME_CLASS, 0\nPARM, MAG_ENABLE, 0\nPARM, mag_enable, 1\nMODE, ACRO\nCTUN, 5\nATT, 80, 0\nMSG, ArduCopter V4.6\n";
        QVERIFY(write(path, text)); auto result = A::Analyze(path); QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(find(result, "Pitch/Roll").status, QStringLiteral("GOOD"));
        QCOMPARE(find(result, "Parameters").status, QStringLiteral("GOOD"));
        QVERIFY(write(path, text + "MSG, ArduPlane V4.6\n")); result = A::Analyze(path); QVERIFY(result.success);
        QCOMPARE(find(result, "Pitch/Roll").status, QStringLiteral("NA"));
        const QByteArray nan = "FMT, 1, 15, VIBE, fff, VibeZ,VibeY,VibeX\nVIBE, NaN, NaN, NaN\n";
        QVERIFY(write(path, nan)); result = A::Analyze(path); QVERIFY(result.success);
        QCOMPARE(find(result, "NaN").message, QStringLiteral("invalid number in VIBE.VibeX at line 1"));
    }

    void cancellationMutationAndResourceFailures()
    {
        QTemporaryDir directory; const auto path = directory.filePath("fixture.log");
        const QByteArray text = "FMT, 1, 7, VIBE, f, VibeX\nVIBE, 10\n";
        QVERIFY(write(path, text));
        auto result = A::Analyze(path, [] { return true; }); QVERIFY(result.cancelled); QVERIFY(result.tests.isEmpty());
        bool stop = false; qint64 previous = 0;
        result = A::Analyze(path, [&] { return stop; }, [&](qint64 done, qint64 total) {
            QVERIFY(done >= previous); QVERIFY(done <= total); previous = done; if (done >= 800) stop = true;
        });
        QVERIFY(result.cancelled); QVERIFY(result.tests.isEmpty());
        const auto modified = QFileInfo(path).lastModified(); bool changed = false;
        result = A::Analyze(path, {}, [&](qint64 done, qint64) {
            if (done != 650 || changed) return;
            changed = true; QVERIFY(write(path, QByteArray(text).replace("10", "99")));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        });
        QVERIFY(changed); QVERIFY(!result.success); QVERIFY(!result.error.isEmpty());
        auto invalid = data(); invalid.lineCount = -1;
        QVERIFY(!A::Analyze(invalid).success);
        invalid = data({{"ATT", QVector<A::Sample>(A::MaximumSamples + 1)}});
        result = A::Analyze(invalid); QVERIFY(!result.success); QVERIFY(result.error.contains("limit"));
        invalid = data({{"ATT", {sample(1, {{"Roll", 0}, {"roll", 1}})}}});
        QVERIFY(!A::Analyze(invalid).success);
        invalid = data({{"ATT", {sample(1, {}, {{"Mode", QString(int(A::MaximumTextBytes / 2) + 1, 'x')}})}}});
        QVERIFY(!A::Analyze(invalid).success);
    }

    void missingEmptyMalformedAndLinkedFiles()
    {
        QTemporaryDir directory; const auto path = directory.filePath("fixture.bin");
        QVERIFY(!A::Analyze(path).success); QVERIFY(write(path, {}));
        const auto empty = A::Analyze(path); QVERIFY(empty.success);
        QCOMPARE(find(empty, "Empty").status, QStringLiteral("FAIL"));
        QVERIFY(write(path, "not a DataFlash log")); QVERIFY(!A::Analyze(path).success);
        QVERIFY(write(path, fmt(1, 7, "VIBE", "f", "VibeX").left(50)));
        QVERIFY(!A::Analyze(path).success);
#ifdef Q_OS_UNIX
        const auto link = directory.filePath("alias.bin"); QVERIFY(QFile::link(path, link));
        QVERIFY(!A::Analyze(link).success);
#endif
        const auto text = directory.filePath("blank.log"); QVERIFY(write(text, "\n\n"));
        const auto blank = A::Analyze(text); QVERIFY(blank.success); QVERIFY(!blank.warnings.isEmpty());
    }
};

QTEST_GUILESS_MAIN(DataFlashLogAnalyzerTest)
#include "test_dataflashloganalyzer.moc"
