#include "ui/Loghandling/DataFlashDashWareCsvExporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#include <limits>

namespace {
bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray record(quint8 id, int length)
{
    QByteArray bytes(length, '\0'); bytes[0] = char(0xa3); bytes[1] = char(0x95); bytes[2] = char(id); return bytes;
}
void bytesAt(QByteArray &bytes, int offset, const QByteArray &value)
{
    std::memcpy(bytes.data() + offset, value.constData(), size_t(value.size()));
}
QByteArray fmt(quint8 id, quint8 length, const QByteArray &name, const QByteArray &format, const QByteArray &columns)
{
    QByteArray bytes = record(128, 89); bytes[3] = char(id); bytes[4] = char(length);
    bytesAt(bytes, 5, name); bytesAt(bytes, 9, format); bytesAt(bytes, 25, columns); return bytes;
}
template<class T> void little(QByteArray &bytes, int offset, T value)
{
    qToLittleEndian<T>(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}
void single(QByteArray &bytes, int offset, float value)
{
    quint32 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); little(bytes, offset, bits);
}
void real(QByteArray &bytes, int offset, double value)
{
    quint64 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); little(bytes, offset, bits);
}
const QByteArray TextFixture = "FMT,1,15,TEST,Qf,TimeUS,Value\nTEST,2000,2.5\n"
    "FMT,2,12,AUX,QB,TimeUS,State\nAUX,1500,7\nTEST,1000,-3\n";
}

class DataFlashDashWareCsvExporterTest final : public QObject
{
    Q_OBJECT
private slots:
    void fullSchemaSparseColumnsRawOrderAndFilters();
    void emptyNoMatchAndNoDataSchemas_data();
    void emptyNoMatchAndNoDataSchemas();
    void binaryIntegerWidthsAndWireScales();
    void binaryStringsArraysAndQuotedCsv();
    void timestampPrecedenceAndExactFractions();
    void nonfiniteValuesAreExplicit();
    void modesUseUnfilteredPrescan_data();
    void modesUseUnfilteredPrescan();
    void finalTailBlanksAndNoFmtuScaling();
    void failuresPreserveOldOutput_data();
    void failuresPreserveOldOutput();
    void cancellationAndMutation_data();
    void cancellationAndMutation();
    void pathAliasesSymlinksAndMissingParents();
    void schemaGrowthBetweenPassesFailsBeforeIndexing();
};

void DataFlashDashWareCsvExporterTest::fullSchemaSparseColumnsRawOrderAndFilters()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, TextFixture));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {" aux ", "test", "TEST"});
    QVERIFY2(result.success, qPrintable(result.error));
    const QByteArray expected = "GLOBAL_TimeMS,TEST_TimeUS,TEST_Value,AUX_TimeUS,AUX_State,\n"
        "2,2000,2.5,,,\n1.5,,,1500,7,\n1,1000,-3,,,\n";
    QCOMPARE(get(output), expected); QCOMPARE(result.columns, qint64(5));
    QCOMPARE(result.rowsWritten, qint64(3)); QCOMPARE(result.bytesWritten, qint64(expected.size()));
    QCOMPARE(get(input), TextFixture);
}

void DataFlashDashWareCsvExporterTest::emptyNoMatchAndNoDataSchemas_data()
{
    QTest::addColumn<QByteArray>("source"); QTest::addColumn<QStringList>("types"); QTest::addColumn<QByteArray>("expected");
    QTest::newRow("empty") << QByteArray() << QStringList{} << QByteArray("GLOBAL_TimeMS,\n");
    QTest::newRow("no match") << TextFixture << QStringList{"OTHER"} << QByteArray("GLOBAL_TimeMS,\n");
    QTest::newRow("declared but no data") << QByteArray("FMT,1,15,TEST,Qf,TimeUS,Value\n")
        << QStringList{} << QByteArray("GLOBAL_TimeMS,TEST_TimeUS,TEST_Value,FMT_Type,FMT_Length,FMT_Name,FMT_Format,FMT_Columns,\n");
    QTest::newRow("explicit synthetic FMT") << QByteArray("FMT,1,15,TEST,Qf,TimeUS,Value\nTEST,1000,2\n")
        << QStringList{"FMT"} << QByteArray("GLOBAL_TimeMS,FMT_Type,FMT_Length,FMT_Name,FMT_Format,FMT_Columns,\n");
    QTest::newRow("late actual FMT preserves synthetic position")
        << QByteArray("FMT,1,15,TEST,Qf,TimeUS,Value\nFMT,2,7,AUX,I,Value\nFMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n")
        << QStringList{} << QByteArray("GLOBAL_TimeMS,TEST_TimeUS,TEST_Value,FMT_Type,FMT_Length,FMT_Name,FMT_Format,FMT_Columns,AUX_Value,\n");
    QTest::newRow("observed FMT columns but no FMT rows")
        << QByteArray("FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n") << QStringList{"FMT"}
        << QByteArray("GLOBAL_TimeMS,FMT_Type,FMT_Length,FMT_Name,FMT_Format,FMT_Columns,\n");
}

void DataFlashDashWareCsvExporterTest::emptyNoMatchAndNoDataSchemas()
{
    QFETCH(QByteArray, source); QFETCH(QStringList, types); QFETCH(QByteArray, expected);
    QTemporaryDir dir; const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, source));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, types);
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.rowsWritten, qint64(0)); QCOMPARE(get(output), expected);
}

void DataFlashDashWareCsvExporterTest::binaryIntegerWidthsAndWireScales()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.bin"), output = dir.filePath("out.csv");
    QByteArray integer = record(1, 33);
    integer[3] = char(-1); integer[4] = char(255);
    little<qint16>(integer, 5, -1234); little<quint16>(integer, 7, 65535);
    little<qint32>(integer, 9, -123456); little<quint32>(integer, 13, 4294967295U);
    little<qint64>(integer, 17, -9223372036854775807LL); little<quint64>(integer, 25, 18446744073709551615ULL);
    QByteArray scales = record(2, 33);
    little<quint16>(scales, 3, 0x3e00); single(scales, 5, 0.1F); real(scales, 9, 0.2);
    little<qint16>(scales, 17, -123); little<quint16>(scales, 19, 321);
    little<qint32>(scales, 21, -12345); little<quint32>(scales, 25, 123456);
    little<qint32>(scales, 29, -1172500000);
    QVERIFY(put(input, fmt(1, 33, "INTS", "bBhHiIqQ", "b,B,h,H,i,I,q,Q") + integer
        + fmt(2, 33, "REAL", "gfdcCeEL", "g,f,d,c,C,e,E,L") + scales));
    auto result = DataFlashDashWareCsvExporter::Export(input, output, {"INTS"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,INTS_b,INTS_B,INTS_h,INTS_H,INTS_i,INTS_I,INTS_q,INTS_Q,\n"
        "0,-1,255,-1234,65535,-123456,4294967295,-9223372036854775807,18446744073709551615,\n"));
    result = DataFlashDashWareCsvExporter::Export(input, output, {"REAL"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,REAL_g,REAL_f,REAL_d,REAL_c,REAL_C,REAL_e,REAL_E,REAL_L,\n"
        "0,1.5,0.1,0.2,-1.23,3.21,-123.45,1234.56,-117.25,\n"));
}

void DataFlashDashWareCsvExporterTest::binaryStringsArraysAndQuotedCsv()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.bin"), output = dir.filePath("out.csv");
    QByteArray data = record(1, 152);
    bytesAt(data, 3, "a\"b"); bytesAt(data, 7, QByteArray("hi,") + char(0xe9));
    const QByteArray nulString("line1\nline2\0tail", 16);
    bytesAt(data, 23, nulString); data[87] = 7;
    QByteArray array = "[";
    for (int index = 0; index < 32; ++index) {
        little<qint16>(data, 88 + index * 2, qint16(index - 16));
        if (index) array += ' '; array += QByteArray::number(index - 16);
    }
    array += ']';
    QVERIFY(put(input, fmt(1, 152, "STR", "nNZMa", "N4,N16,Z64,Mode,Array") + data));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {"STR"});
    QVERIFY2(result.success, qPrintable(result.error));
    const QByteArray expected = QByteArray("GLOBAL_TimeMS,STR_N4,STR_N16,STR_Z64,STR_Mode,STR_Array,\n0,\"a\"\"b\",\"hi,?\",\"")
        + nulString + "\",7," + array + ",\n";
    QCOMPARE(get(output), expected);
    QVERIFY(result.warnings.join('\n').contains("Embedded NUL"));
}

void DataFlashDashWareCsvExporterTest::timestampPrecedenceAndExactFractions()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, "FMT,1,19,BOTH,QIi,TimeUS,TimeMS,T\nBOTH,999999,0,123\nBOTH,999999,100,123\nBOTH,999999,50,123\n"
        "FMT,2,11,MICR,q,TimeUS\nMICR,-1001\nMICR,9223372036854775807\n"
        "FMT,3,7,ONLY,i,T\nONLY,-5\nFMT,4,7,NONE,I,Value\nNONE,9\n"));
    auto result = DataFlashDashWareCsvExporter::Export(input, output, {"BOTH"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,BOTH_TimeUS,BOTH_TimeMS,BOTH_T,\n0,999999,0,123,\n100,999999,100,123,\n50,999999,50,123,\n"));
    result = DataFlashDashWareCsvExporter::Export(input, output, {"MICR"}); QVERIFY(result.success);
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,MICR_TimeUS,\n-1.001,-1001,\n9223372036854775.807,9223372036854775807,\n"));
    result = DataFlashDashWareCsvExporter::Export(input, output, {"ONLY", "NONE"}); QVERIFY(result.success);
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,ONLY_T,NONE_Value,\n-5,-5,,\n0,,9,\n"));
}

void DataFlashDashWareCsvExporterTest::nonfiniteValuesAreExplicit()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.bin"), output = dir.filePath("out.csv");
    QByteArray data = record(1, 15); single(data, 3, std::numeric_limits<float>::quiet_NaN());
    single(data, 7, std::numeric_limits<float>::infinity()); single(data, 11, -std::numeric_limits<float>::infinity());
    QVERIFY(put(input, fmt(1, 15, "BAD", "fff", "A,B,C") + data + data));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {"BAD"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,BAD_A,BAD_B,BAD_C,\n0,NaN,Infinity,-Infinity,\n0,NaN,Infinity,-Infinity,\n"));
    QCOMPARE(result.warnings.size(), 3);
}

void DataFlashDashWareCsvExporterTest::modesUseUnfilteredPrescan_data()
{
    QTest::addColumn<QByteArray>("firmware"); QTest::addColumn<int>("mode"); QTest::addColumn<QByteArray>("expected");
    QTest::newRow("Copter") << QByteArray("ArduCopter 4.5") << 4 << QByteArray("Guided");
    QTest::newRow("Plane") << QByteArray("ArduPlane 4.5") << 16 << QByteArray("INITIALISING");
    QTest::newRow("Rover") << QByteArray("ArduRover 4.5") << 0 << QByteArray("Manual");
    QTest::newRow("Tracker") << QByteArray("AntennaTracker") << 10 << QByteArray("AUTO");
    QTest::newRow("Unknown firmware") << QByteArray("Unknown firmware") << 4 << QByteArray("4");
    QTest::newRow("Unknown mode") << QByteArray("ArduCopter 4.5") << 255 << QByteArray("255");
}

void DataFlashDashWareCsvExporterTest::modesUseUnfilteredPrescan()
{
    QFETCH(QByteArray, firmware); QFETCH(int, mode); QFETCH(QByteArray, expected);
    QTemporaryDir dir; const QString input = dir.filePath("in.bin"), output = dir.filePath("out.csv");
    QByteArray modeRecord = record(1, 4); modeRecord[3] = char(mode);
    QByteArray msg = record(2, 67); bytesAt(msg, 3, firmware);
    QVERIFY(put(input, fmt(1, 4, "MODE", "M", "Mode") + modeRecord
        + fmt(2, 67, "MSG", "Z", "Message") + msg));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {"MODE"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,MODE_Mode,\n0,") + expected + ",\n");
}

void DataFlashDashWareCsvExporterTest::finalTailBlanksAndNoFmtuScaling()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, "\nFMT,1,7,TEST,L,Lat\nTEST,34.5\n"
        "FMT,2,44,FMTU,QBNN,TimeUS,FmtType,UnitIds,MultIds\nFMTU,0,1,D,a\n"
        "FMT,3,20,MULT,Qbd,TimeUS,Id,Mult\nMULT,0,97,100\n"
        "FMT,4,76,UNIT,QbZ,TimeUS,Id,Label\nUNIT,1000,68,deg\n"));
    auto result = DataFlashDashWareCsvExporter::Export(input, output, {"TEST"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,TEST_Lat,\n0,34.5,\n"));
    QVERIFY(result.warnings.join('\n').contains("Ignored 1 blank"));
    result = DataFlashDashWareCsvExporter::Export(input, output, {"FMTU", "MULT", "UNIT"});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.rowsWritten, qint64(3)); QCOMPARE(result.columns, qint64(11));
    QCOMPARE(get(output), QByteArray("GLOBAL_TimeMS,FMTU_TimeUS,FMTU_FmtType,FMTU_UnitIds,FMTU_MultIds,"
        "MULT_TimeUS,MULT_Id,MULT_Mult,UNIT_TimeUS,UNIT_Id,UNIT_Label,\n"
        "0,0,1,D,a," ",,," ",,," "\n"
        "0,,,,," "0,97,100," ",,," "\n"
        "1,,,,," ",,," "1000,68,deg,\n"));
    const QString binary = dir.filePath("in.bin");
    QByteArray data = record(1, 7); little<quint32>(data, 3, 7);
    QVERIFY(put(binary, fmt(1, 7, "TEST", "I", "Value") + data + data.left(4)));
    result = DataFlashDashWareCsvExporter::Export(binary, output);
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.rowsWritten, qint64(1));
    QVERIFY(result.warnings.join('\n').contains("4 trailing bytes"));
}

void DataFlashDashWareCsvExporterTest::failuresPreserveOldOutput_data()
{
    QTest::addColumn<QByteArray>("source"); QTest::addColumn<QString>("suffix");
    QTest::newRow("unknown binary framing") << QByteArray("bad") << QString("bin");
    QTest::newRow("timestamp noninteger") << QByteArray("FMT,1,11,TEST,Q,TimeUS\nTEST,NaN\n") << QString("log");
    QTest::newRow("conflicting FMT") << QByteArray("FMT,1,7,TEST,I,Value\nFMT,1,7,TEST,f,Value\n") << QString("log");
    QTest::newRow("selected unsupported A") << (fmt(1, 131, "ARR", "A", "Array") + record(1, 131)) << QString("bin");
    QTest::newRow("ambiguous constructed header")
        << QByteArray("FMT,1,7,GPS_,I,B\nFMT,2,7,GPS,I,_B\n") << QString("log");
}

void DataFlashDashWareCsvExporterTest::failuresPreserveOldOutput()
{
    QFETCH(QByteArray, source); QFETCH(QString, suffix);
    QTemporaryDir dir; const QString input = dir.filePath("in." + suffix), output = dir.filePath("out.csv");
    QVERIFY(put(input, source)); QVERIFY(put(output, "old output"));
    const auto result = DataFlashDashWareCsvExporter::Export(input, output);
    QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QCOMPARE(get(output), QByteArray("old output"));
    QCOMPARE(result.rowsWritten, qint64(0)); QCOMPARE(result.bytesWritten, qint64(0));
}

void DataFlashDashWareCsvExporterTest::cancellationAndMutation_data()
{
    QTest::addColumn<int>("phase"); QTest::addColumn<bool>("mutate");
    QTest::newRow("cancel before scan") << 0 << false;
    QTest::newRow("cancel between passes") << 400 << false;
    QTest::newRow("cancel before commit") << 800 << false;
    QTest::newRow("mutation between passes") << 400 << true;
    QTest::newRow("mutation before commit") << 800 << true;
}

void DataFlashDashWareCsvExporterTest::cancellationAndMutation()
{
    QFETCH(int, phase); QFETCH(bool, mutate);
    QTemporaryDir dir; const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, TextFixture)); QVERIFY(put(output, "old output"));
    bool reached = false;
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {},
        [&] { return reached && !mutate; }, [&](qint64 value, qint64) {
            if (!reached && value >= phase) {
                reached = true;
                if (mutate) { QByteArray changed = TextFixture; changed.replace("2.5", "9.5"); put(input, changed); }
            }
        });
    QVERIFY(reached); QVERIFY(!result.success); QCOMPARE(result.cancelled, !mutate);
    QCOMPARE(get(output), QByteArray("old output"));
    if (mutate) QVERIFY(result.error.contains("changed"));
}

void DataFlashDashWareCsvExporterTest::pathAliasesSymlinksAndMissingParents()
{
    QTemporaryDir dir; const QString input = dir.filePath("input.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, TextFixture));
    auto result = DataFlashDashWareCsvExporter::Export(input, input);
    QVERIFY(!result.success); QCOMPARE(get(input), TextFixture);
    result = DataFlashDashWareCsvExporter::Export(input, dir.filePath("missing/out.csv"));
    QVERIFY(!result.success);
    if (!QFile::link(input, output)) QSKIP("Symbolic links unavailable on this platform.");
    result = DataFlashDashWareCsvExporter::Export(input, output);
    QVERIFY(!result.success); QVERIFY(QFileInfo(output).isSymLink()); QCOMPARE(get(input), TextFixture);
    QTemporaryDir aliasRoot;
    const QString alias = aliasRoot.filePath("alias");
    if (!QFile::link(dir.path(), alias)) QSKIP("Directory symbolic links unavailable.");
    result = DataFlashDashWareCsvExporter::Export(input, alias + "/input.log");
    QVERIFY(!result.success); QCOMPARE(get(input), TextFixture);
}

void DataFlashDashWareCsvExporterTest::schemaGrowthBetweenPassesFailsBeforeIndexing()
{
    QTemporaryDir dir; const QString input = dir.filePath("in.log"), output = dir.filePath("out.csv");
    QVERIFY(put(input, "FMT,1,7,TEST,I,A\nTEST,1\n")); QVERIFY(put(output, "old output"));
    bool changed = false;
    const auto result = DataFlashDashWareCsvExporter::Export(input, output, {"TEST"}, {},
        [&](qint64 phase, qint64) {
            if (!changed && phase >= 400) {
                changed = true; put(input, "FMT,1,19,TEST,IIII,A,B,C,D\nTEST,1,2,3,4\n");
            }
        });
    QVERIFY(changed); QVERIFY(!result.success); QVERIFY(result.error.contains("schema changed"));
    QCOMPARE(get(output), QByteArray("old output"));
}

QTEST_MAIN(DataFlashDashWareCsvExporterTest)
#include "test_dataflashdashwarecsvexporter.moc"
