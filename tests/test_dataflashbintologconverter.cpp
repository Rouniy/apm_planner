#include "ui/Loghandling/DataFlashBinToLogConverter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>

#include <cstring>
#include <limits>

namespace {

QByteArray header(quint8 type)
{
    QByteArray bytes;
    bytes.append(char(0xa3));
    bytes.append(char(0x95));
    bytes.append(char(type));
    return bytes;
}

template<typename Integer>
void appendLittle(QByteArray *bytes, Integer value)
{
    uchar encoded[sizeof(Integer)]{};
    qToLittleEndian<Integer>(value, encoded);
    bytes->append(reinterpret_cast<const char *>(encoded), sizeof(encoded));
}

void appendFloat(QByteArray *bytes, float value)
{
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLittle(bytes, bits);
}

void appendDouble(QByteArray *bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLittle(bytes, bits);
}

QByteArray fmt(quint8 type, quint8 length, const QByteArray &name,
               const QByteArray &format, const QByteArray &labels)
{
    QByteArray payload(86, '\0');
    payload[0] = char(type);
    payload[1] = char(length);
    payload.replace(2, qMin(4, name.size()), name.left(4));
    payload.replace(6, qMin(16, format.size()), format.left(16));
    payload.replace(22, qMin(64, labels.size()), labels.left(64));
    return header(128) + payload;
}

QByteArray record(quint8 type, const QByteArray &payload)
{
    return header(type) + payload;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QStringList stagedFiles(const QString &directory)
{
    return QDir(directory).entryList(
        QStringList() << QStringLiteral(".apm-bin-to-log-*.partial"),
        QDir::Files | QDir::Hidden);
}

} // namespace

class DataFlashBinToLogConverterTest final : public QObject
{
    Q_OBJECT

private slots:
    void numericFormatsMatchMissionPlannerText();
    void stringsArraysModesAndUnknownEncodingMatchReference();
    void emptyBlobRecordIsSkippedLikeReference();
    void corruptUnknownAndTruncatedRecordsAreSalvagedWithWarnings();
    void emptyInputProducesAnEmptyLogWithWarning();
    void cancellationRemovesPrivateStaging();
    void existingOrRacingDestinationIsNeverOverwritten();
    void sourceMutationPreventsPublication();
    void linkedInputAndInvalidExtensionsAreRejected();
};

void DataFlashBinToLogConverterTest::numericFormatsMatchMissionPlannerText()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("numeric.bin"));
    const QString output = directory.filePath(QStringLiteral("numeric.log"));

    QByteArray integers;
    integers.append(char(0xff));
    integers.append(char(250));
    appendLittle(&integers, qint16(-1234));
    appendLittle(&integers, quint16(65000));
    appendLittle(&integers, qint32(-123456));
    appendLittle(&integers, quint32(4000000000u));
    appendLittle(&integers, qint64(-9000000000000LL));
    appendLittle(&integers, quint64(18000000000000000000ULL));

    QByteArray scaled;
    appendLittle(&scaled, quint16(0x3e00)); // IEEE-754 half 1.5
    appendFloat(&scaled, 0.1f);
    appendDouble(&scaled, -0.25);
    appendLittle(&scaled, qint16(-123));
    appendLittle(&scaled, quint16(123));
    appendLittle(&scaled, qint32(-12345));
    appendLittle(&scaled, quint32(12345));
    appendLittle(&scaled, qint32(-353629380));
    scaled.append(char(3));

    QByteArray general;
    appendDouble(&general, 1.0e15);
    appendDouble(&general, 1.0e16);
    appendDouble(&general, 1.0e17);
    appendDouble(&general, std::numeric_limits<double>::denorm_min());

    QByteArray ties;
    appendFloat(&ties, -0.020507812f);
    appendFloat(&ties, 273.39062f);
    appendFloat(&ties, 6.4414062f);

    const QByteArray source =
        fmt(1, 33, "TEST", "bBhHiIqQ",
            "I8,U8,I16,U16,I32,U32,I64,U64")
        + record(1, integers)
        + fmt(2, 34, "VALS", "gfdcCeELM",
              "Half,Float,Double,CentiI16,CentiU16,CentiI32,CentiU32,Lat,Mode")
        + record(2, scaled)
        + fmt(3, 35, "GEN", "dddd", "A,B,C,D") + record(3, general)
        + fmt(4, 15, "TIES", "fff", "Negative,Large,Small")
        + record(4, ties);
    QVERIFY(writeFile(input, source));

    QVector<QPair<qint64, qint64>> progress;
    const auto result = DataFlashBinToLogConverter::Convert(
        input, output, {}, [&progress](qint64 done, qint64 total) {
            progress.append(qMakePair(done, total));
        });
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.outputPath, QFileInfo(output).absoluteFilePath());
    QCOMPARE(result.recordsWritten, qint64(8));
    QCOMPARE(result.recordsSkipped, qint64(0));
    QCOMPARE(result.bytesRead, qint64(source.size()));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().first, qint64(source.size()));
    QCOMPARE(progress.last().second, qint64(source.size()));

    const QByteArray expected =
        "FMT, 1, 33, TEST, bBhHiIqQ, I8,U8,I16,U16,I32,U32,I64,U64\r\n"
        "TEST, -1, 250, -1234, 65000, -123456, 4000000000, "
        "-9000000000000, 18000000000000000000\r\n"
        "FMT, 2, 34, VALS, gfdcCeELM, Half,Float,Double,CentiI16,"
        "CentiU16,CentiI32,CentiU32,Lat,Mode\r\n"
        "VALS, 1.5, 0.1, -0.25, -1.23, 1.23, -123.45, 123.45, "
        "-35.362938, 3\r\n"
        "FMT, 3, 35, GEN, dddd, A,B,C,D\r\n"
        "GEN, 1000000000000000, 10000000000000000, 1E+17, 5E-324\r\n"
        "FMT, 4, 15, TIES, fff, Negative,Large,Small\r\n"
        "TIES, -0.020507812, 273.39062, 6.4414062\r\n";
    QCOMPARE(readFile(output), expected);
    QCOMPARE(result.bytesWritten, qint64(expected.size()));
    QCOMPARE(readFile(input), source);
}

void DataFlashBinToLogConverterTest::stringsArraysModesAndUnknownEncodingMatchReference()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("strings.bin"));
    const QString output = directory.filePath(QStringLiteral("strings.log"));

    QByteArray message(16, '\0');
    message.replace(0, 10, QByteArrayLiteral("ArduCopter"));
    QByteArray strings(84, '\0');
    strings.replace(0, 2, QByteArrayLiteral("AB"));
    strings.replace(4, 5, QByteArrayLiteral("Hello"));
    const QByteArray blob("slash\\line\n\t", 12);
    strings.replace(20, blob.size(), blob);
    strings[20 + blob.size()] = char(0x80);
    QByteArray array;
    for (int value = -16; value < 16; ++value) {
        appendLittle(&array, qint16(value));
    }
    QByteArray newer(128, char(0x5a));

    const QByteArray source =
        fmt(10, 19, "MSG", "N", "Message") + record(10, message)
        + fmt(11, 4, "MODE", "M", "Mode") + record(11, QByteArray(1, char(3)))
        + fmt(12, 87, "TXT", "nNZ", "Short,Name,Message") + record(12, strings)
        + fmt(13, 67, "ARR", "a", "Values") + record(13, array)
        + fmt(14, 131, "NEW", "A", "Array") + record(14, newer);
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashBinToLogConverter::Convert(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsWritten, qint64(10));
    QCOMPARE(result.recordsSkipped, qint64(0));

    QByteArray arrayText("[");
    for (int value = -16; value < 16; ++value) {
        if (value != -16) {
            arrayText += ' ';
        }
        arrayText += QByteArray::number(value);
    }
    arrayText += ']';
    const QByteArray expected =
        QByteArrayLiteral("FMT, 10, 19, MSG, N, Message\r\n")
        + QByteArrayLiteral("MSG, ArduCopter\r\n")
        + QByteArrayLiteral("FMT, 11, 4, MODE, M, Mode\r\n")
        + QByteArrayLiteral("MODE, Auto\r\n")
        + QByteArrayLiteral("FMT, 12, 87, TXT, nNZ, Short,Name,Message\r\n")
        + QByteArrayLiteral("TXT, AB, Hello, slash\\\\line\\n\\t?\r\n")
        + QByteArrayLiteral("FMT, 13, 67, ARR, a, Values\r\nARR, ")
        + arrayText + QByteArrayLiteral("\r\n")
        + QByteArrayLiteral("FMT, 14, 131, NEW, A, Array\r\nNEW, \r\n");
    QCOMPARE(readFile(output), expected);
}

void DataFlashBinToLogConverterTest::emptyBlobRecordIsSkippedLikeReference()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("blob.bin"));
    const QString output = directory.filePath(QStringLiteral("blob.log"));
    QByteArray populated(64, '\0');
    populated[0] = 'x';
    const QByteArray source = fmt(20, 67, "TEXT", "Z", "Message")
        + record(20, QByteArray(64, '\0')) + record(20, populated);
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashBinToLogConverter::Convert(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsWritten, qint64(2));
    QCOMPARE(result.recordsSkipped, qint64(1));
    QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(
        QStringLiteral("empty Z field")));
    QCOMPARE(readFile(output),
             QByteArray("FMT, 20, 67, TEXT, Z, Message\r\nTEXT, x\r\n"));
}

void DataFlashBinToLogConverterTest::corruptUnknownAndTruncatedRecordsAreSalvagedWithWarnings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("salvage.bin"));
    const QString output = directory.filePath(QStringLiteral("salvage.log"));
    QByteArray source("garbage", 7);
    source += header(77) + QByteArrayLiteral("xx");
    source += fmt(1, 4, "ONE", "B", "Value");
    source += record(1, QByteArray(1, char(42)));
    source += header(1);
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashBinToLogConverter::Convert(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsWritten, qint64(2));
    QCOMPARE(result.recordsSkipped, qint64(2));
    const QString warnings = result.warnings.join(QLatin1Char('\n'));
    QVERIFY(warnings.contains(QStringLiteral("resynchronizing")));
    QVERIFY(warnings.contains(QStringLiteral("no preceding usable FMT")));
    QVERIFY(warnings.contains(QStringLiteral("incomplete final record")));
    QCOMPARE(readFile(output),
             QByteArray("FMT, 1, 4, ONE, B, Value\r\nONE, 42\r\n"));
}

void DataFlashBinToLogConverterTest::emptyInputProducesAnEmptyLogWithWarning()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("empty.bin"));
    const QString output = directory.filePath(QStringLiteral("empty.log"));
    QVERIFY(writeFile(input, QByteArray()));

    const auto result = DataFlashBinToLogConverter::Convert(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsWritten, qint64(0));
    QCOMPARE(result.bytesRead, qint64(0));
    QCOMPARE(result.bytesWritten, qint64(0));
    QVERIFY(QFileInfo::exists(output));
    QCOMPARE(QFileInfo(output).size(), qint64(0));
    QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(
        QStringLiteral("no complete DataFlash records")));
}

void DataFlashBinToLogConverterTest::cancellationRemovesPrivateStaging()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("large.bin"));
    const QString output = directory.filePath(QStringLiteral("large.log"));
    QByteArray source = fmt(1, 4, "ONE", "B", "Value");
    const QByteArray one = record(1, QByteArray(1, char(7)));
    while (source.size() < 256 * 1024) {
        source += one;
    }
    QVERIFY(writeFile(input, source));

    bool stop = false;
    const auto result = DataFlashBinToLogConverter::Convert(
        input, output, [&stop]() { return stop; },
        [&stop](qint64, qint64) { stop = true; });
    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
    QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(result.bytesWritten, qint64(0));
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashBinToLogConverterTest::existingOrRacingDestinationIsNeverOverwritten()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.bin"));
    const QString output = directory.filePath(QStringLiteral("result.log"));
    const QByteArray source = fmt(1, 4, "ONE", "B", "Value")
        + record(1, QByteArray(1, char(8)));
    QVERIFY(writeFile(input, source));
    QVERIFY(writeFile(output, QByteArrayLiteral("OLD")));

    auto result = DataFlashBinToLogConverter::Convert(input, output);
    QVERIFY(!result.success);
    QCOMPARE(readFile(output), QByteArray("OLD"));
    QVERIFY(stagedFiles(directory.path()).isEmpty());

    QVERIFY(QFile::remove(output));
    bool installed = false;
    result = DataFlashBinToLogConverter::Convert(
        input, output, {}, [&installed, output](qint64, qint64) {
            if (!installed) {
                installed = writeFile(output, QByteArrayLiteral("FOREIGN"));
            }
        });
    QVERIFY(installed);
    QVERIFY(!result.success);
    QCOMPARE(readFile(output), QByteArray("FOREIGN"));
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashBinToLogConverterTest::sourceMutationPreventsPublication()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("changing.bin"));
    const QString output = directory.filePath(QStringLiteral("changing.log"));
    const QByteArray source = fmt(1, 4, "ONE", "B", "Value")
        + record(1, QByteArray(1, char(8)));
    QVERIFY(writeFile(input, source));

    bool changed = false;
    const auto result = DataFlashBinToLogConverter::Convert(
        input, output, {}, [&changed, input](qint64, qint64) {
            if (changed) {
                return;
            }
            QFile file(input);
            changed = file.open(QIODevice::Append)
                && file.write("x", 1) == 1 && file.flush();
        });
    QVERIFY(changed);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("changed")));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashBinToLogConverterTest::linkedInputAndInvalidExtensionsAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("real.bin"));
    const QString link = directory.filePath(QStringLiteral("linked.bin"));
    const QString output = directory.filePath(QStringLiteral("out.log"));
    QVERIFY(writeFile(input, fmt(1, 4, "ONE", "B", "Value")));

    auto result = DataFlashBinToLogConverter::Convert(
        directory.filePath(QStringLiteral("wrong.txt")), output);
    QVERIFY(!result.success);
    QVERIFY(!QFileInfo::exists(output));

    if (!QFile::link(input, link)) {
        QSKIP("This filesystem cannot create a file symlink/link fixture.");
    }
    result = DataFlashBinToLogConverter::Convert(link, output);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("non-symlink")));
    QVERIFY(!QFileInfo::exists(output));
}

QTEST_GUILESS_MAIN(DataFlashBinToLogConverterTest)

#include "test_dataflashbintologconverter.moc"
