#include "ui/Loghandling/DataFlashLogAnonymizer.h"

#include <QBuffer>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr int FmtLength = 89;

void writeAscii(QByteArray *destination, int offset, int length,
                const QByteArray &text)
{
    const int count = qMin(length, text.size());
    if (count > 0) {
        std::memcpy(destination->data() + offset, text.constData(),
                    static_cast<size_t>(count));
    }
}

QByteArray formatRecord(quint8 type, quint8 length,
                        const QByteArray &name,
                        const QByteArray &format,
                        const QByteArray &columns)
{
    QByteArray result(FmtLength, '\0');
    result[0] = static_cast<char>(0xa3);
    result[1] = static_cast<char>(0x95);
    result[2] = static_cast<char>(128);
    result[3] = static_cast<char>(type);
    result[4] = static_cast<char>(length);
    writeAscii(&result, 5, 4, name);
    writeAscii(&result, 9, 16, format);
    writeAscii(&result, 25, 64, columns);
    return result;
}

QByteArray messageRecord(quint8 type, int length)
{
    QByteArray result(length, '\0');
    result[0] = static_cast<char>(0xa3);
    result[1] = static_cast<char>(0x95);
    result[2] = static_cast<char>(type);
    return result;
}

QByteArray fmtuRecord(quint8 fmtuType, quint8 describedType,
                      const QByteArray &units,
                      const QByteArray &multipliers = {})
{
    QByteArray result = messageRecord(fmtuType, 44);
    result[11] = static_cast<char>(describedType);
    writeAscii(&result, 12, 16, units);
    writeAscii(&result, 28, 16, multipliers);
    return result;
}

void writeI32(QByteArray *bytes, int offset, qint32 value)
{
    qToLittleEndian<qint32>(
        value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void writeU32(QByteArray *bytes, int offset, quint32 value)
{
    qToLittleEndian<quint32>(
        value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void writeFloat(QByteArray *bytes, int offset, float value)
{
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bytes, offset, bits);
}

void writeDouble(QByteArray *bytes, int offset, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian<quint64>(
        bits, reinterpret_cast<uchar *>(bytes->data() + offset));
}

qint32 readI32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 readU32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

float readFloat(const QByteArray &bytes, int offset)
{
    const quint32 bits = readU32(bytes, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double readDouble(const QByteArray &bytes, int offset)
{
    const quint64 bits = qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct Run
{
    QByteArray output;
    LogAnonymizeResult result;
};

Run anonymize(QByteArray input,
              const LogAnonymizeOptions &options = {1.25, -0.5},
              const LogAnonymizeCancel &cancel = {},
              const LogAnonymizeProgress &progress = {})
{
    QBuffer inputDevice(&input);
    QBuffer outputDevice;
    inputDevice.open(QIODevice::ReadOnly);
    outputDevice.open(QIODevice::WriteOnly);
    Run run;
    run.result = DataFlashLogAnonymizer::anonymize(
        &inputDevice, &outputDevice, options, cancel, progress);
    run.output = outputDevice.data();
    return run;
}

} // namespace

class DataFlashLogAnonymizerTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesBinaryAndPatchesEverySupportedRepresentation();
    void fmtuIsAuthoritativeAndMissingTypeUsesNameFallback();
    void fmtuMissingCoordinateUnitsUsePerFieldNameFallback();
    void identicalMetadataDuplicatesAreHarmless();
    void conflictingMetadataFailsBeforeWriting();
    void malformedUnsupportedAndNonfiniteCoordinatesFail();
    void unsignedFormatUsesSignedCoordinateBits();
    void integerOverflowFailsInsteadOfWrapping();
    void preservesOnlyIncompleteNonCoordinateTail();
    void cancellationIsBoundedAndReportsPartialStaging();
    void detectsInputMutationBetweenPasses();
    void rejectsOutputMutationByProgressCallback();
    void rejectsInvalidDevicesAndUnknownFraming();
};

void DataFlashLogAnonymizerTest::
preservesBinaryAndPatchesEverySupportedRepresentation()
{
    // L/i/I use degrees * 1e7. Small floats and doubles use degrees;
    // large floats retain the reference's scaled-coordinate heuristic.
    const QByteArray definition = formatRecord(
        42, 32, "GPS", "LiIffdB",
        "Lat,Lng,HLat,CLng,DLat,TP_Lng,Status");
    QByteArray record = messageRecord(42, 32);
    writeI32(&record, 3, 100000000);
    writeI32(&record, 7, 200000000);
    writeU32(&record, 11, 300000000U);
    writeFloat(&record, 15, 40.0F);
    writeFloat(&record, 19, 400000000.0F);
    writeDouble(&record, 23, 50.0);
    record[31] = static_cast<char>(7);
    const QByteArray input = definition + record;

    const Run run = anonymize(input);
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(run.result.coordinateFields, qint64(6));
    QCOMPARE(run.result.patchedValues, qint64(6));
    QCOMPARE(run.result.records, qint64(2));
    QCOMPARE(run.result.inputBytes, qint64(input.size()));
    QCOMPARE(run.result.outputBytes, qint64(input.size()));
    QCOMPARE(run.output.size(), input.size());
    QCOMPARE(run.output.left(definition.size()), definition);

    const int base = definition.size();
    QCOMPARE(readI32(run.output, base + 3), qint32(112500000));
    QCOMPARE(readI32(run.output, base + 7), qint32(195000000));
    QCOMPARE(readU32(run.output, base + 11), quint32(312500000));
    QCOMPARE(readFloat(run.output, base + 15), 39.5F);
    QCOMPARE(readFloat(run.output, base + 19), 412500000.0F);
    QCOMPARE(readDouble(run.output, base + 23), 49.5);
    QCOMPARE(static_cast<quint8>(run.output.at(base + 31)), quint8(7));

    QByteArray originalWithoutCoordinates = input;
    QByteArray outputWithoutCoordinates = run.output;
    for (int index = base + 3; index < base + 31; ++index) {
        originalWithoutCoordinates[index] = 0;
        outputWithoutCoordinates[index] = 0;
    }
    QCOMPARE(outputWithoutCoordinates, originalWithoutCoordinates);
}

void DataFlashLogAnonymizerTest::
fmtuIsAuthoritativeAndMissingTypeUsesNameFallback()
{
    const QByteArray fmtuDefinition = formatRecord(
        200, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    // FMTU deliberately precedes the definition it describes. Explicit D/U
    // remains authoritative even when the field names say the opposite.
    const QByteArray units = fmtuRecord(200, 43, "DU", "GG");
    const QByteArray unitDefinition = formatRecord(
        43, 11, "POS", "ii", "Lng,Lat");
    const QByteArray fallbackDefinition = formatRecord(
        44, 23, "GPS", "LLLLL", "Lat,Lng,Alt,CustomLat,CustomLng");
    QByteArray unitRecord = messageRecord(43, 11);
    writeI32(&unitRecord, 3, 10000000);
    writeI32(&unitRecord, 7, 20000000);
    QByteArray fallbackRecord = messageRecord(44, 23);
    writeI32(&fallbackRecord, 3, 30000000);
    writeI32(&fallbackRecord, 7, 0); // Zero sentinel remains byte-exact.
    writeI32(&fallbackRecord, 11, 123456789); // Alt must never match "lt".
    writeI32(&fallbackRecord, 15, 50000000); // Unambiguous L suffix.
    writeI32(&fallbackRecord, 19, 60000000);
    const QByteArray input = fmtuDefinition + units + unitDefinition
        + fallbackDefinition + unitRecord + fallbackRecord;

    const Run run = anonymize(input, {1.0, 2.0});
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(run.result.coordinateFields, qint64(6));
    QCOMPARE(run.result.patchedValues, qint64(5));
    const int unitBase = fmtuDefinition.size() + units.size()
        + unitDefinition.size() + fallbackDefinition.size();
    QCOMPARE(readI32(run.output, unitBase + 3), qint32(20000000));
    QCOMPARE(readI32(run.output, unitBase + 7), qint32(40000000));
    const int fallbackBase = unitBase + unitRecord.size();
    QCOMPARE(readI32(run.output, fallbackBase + 3), qint32(40000000));
    QCOMPARE(readI32(run.output, fallbackBase + 7), qint32(0));
    QCOMPARE(readI32(run.output, fallbackBase + 11), qint32(123456789));
    QCOMPARE(readI32(run.output, fallbackBase + 15), qint32(60000000));
    QCOMPARE(readI32(run.output, fallbackBase + 19), qint32(80000000));
}

void DataFlashLogAnonymizerTest::
fmtuMissingCoordinateUnitsUsePerFieldNameFallback()
{
    const QByteArray fmtuDefinition = formatRecord(
        200, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    const QByteArray replayDefinition = formatRecord(
        61, 11, "RGPJ", "ii", "Lat,Lon");
    const QByteArray aisDefinition = formatRecord(
        62, 12, "AIS1", "BLL", "ICAO,lon,lat");
    QByteArray replay = messageRecord(61, 11);
    writeI32(&replay, 3, 10000000);
    writeI32(&replay, 7, 20000000);
    QByteArray ais = messageRecord(62, 12);
    ais[3] = 7;
    writeI32(&ais, 4, 30000000);
    writeI32(&ais, 8, 40000000);

    Run run = anonymize(fmtuDefinition
                        + fmtuRecord(200, 61, "--", "GG")
                        + fmtuRecord(200, 62, "s--", "0GG")
                        + replayDefinition + aisDefinition + replay + ais,
                        {1.0, 2.0});
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(run.result.coordinateFields, qint64(4));
    QCOMPARE(run.result.patchedValues, qint64(4));
    QCOMPARE(run.result.warnings.size(), 4);
    const int replayBase = fmtuDefinition.size() + 2 * 44
        + replayDefinition.size() + aisDefinition.size();
    QCOMPARE(readI32(run.output, replayBase + 3), qint32(20000000));
    QCOMPARE(readI32(run.output, replayBase + 7), qint32(40000000));
    const int aisBase = replayBase + replay.size();
    QCOMPARE(readI32(run.output, aisBase + 4), qint32(50000000));
    QCOMPARE(readI32(run.output, aisBase + 8), qint32(50000000));

    run = anonymize(fmtuDefinition
                    + fmtuRecord(200, 61, "m-", "GG")
                    + replayDefinition + replay,
                    {1.0, 2.0});
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(
        QStringLiteral("explicit non-coordinate FMTU unit")));
    QVERIFY(run.output.isEmpty());
}

void DataFlashLogAnonymizerTest::identicalMetadataDuplicatesAreHarmless()
{
    const QByteArray fmtuDefinition = formatRecord(
        200, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    const QByteArray definition = formatRecord(
        45, 7, "GPS", "L", "Lat");
    const QByteArray units = fmtuRecord(200, 45, "D", "G");
    QByteArray record = messageRecord(45, 7);
    writeI32(&record, 3, 10000000);
    const Run run = anonymize(fmtuDefinition + fmtuDefinition
                              + definition + definition
                              + units + units + record,
                              {1.0, 0.0});
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(run.result.coordinateFields, qint64(1));
    QCOMPARE(run.result.patchedValues, qint64(1));
    QCOMPARE(readI32(run.output, run.output.size() - record.size() + 3),
             qint32(20000000));
}

void DataFlashLogAnonymizerTest::conflictingMetadataFailsBeforeWriting()
{
    const QByteArray definition = formatRecord(
        46, 7, "GPS", "L", "Lat");
    const QByteArray conflicting = formatRecord(
        46, 11, "GPS", "LL", "Lat,Lng");
    Run run = anonymize(definition + conflicting);
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("Conflicting FMT")));
    QVERIFY(run.output.isEmpty());

    const QByteArray fmtuDefinition = formatRecord(
        200, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    run = anonymize(fmtuDefinition
                    + formatRecord(47, 7, "GPS", "L", "Lat")
                    + fmtuRecord(200, 47, "D", "G")
                    + fmtuRecord(200, 47, "U", "G"));
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("Conflicting FMTU")));
    QVERIFY(run.output.isEmpty());

    run = anonymize(fmtuDefinition
                    + formatRecord(47, 7, "GPS", "L", "Lat")
                    + fmtuRecord(200, 47, "D", "G")
                    + fmtuRecord(200, 47, "D", "0"));
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("Conflicting FMTU")));
    QVERIFY(run.output.isEmpty());
}

void DataFlashLogAnonymizerTest::
malformedUnsupportedAndNonfiniteCoordinatesFail()
{
    Run run = anonymize(formatRecord(48, 8, "GPS", "L", "Lat"));
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("declares length")));
    QVERIFY(run.output.isEmpty());

    const QByteArray fmtuDefinition = formatRecord(
        200, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    run = anonymize(fmtuDefinition
                    + formatRecord(49, 11, "GPS", "q", "Value")
                    + fmtuRecord(200, 49, "D", "G"));
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("unsupported format")));
    QVERIFY(run.output.isEmpty());

    const QByteArray floatDefinition = formatRecord(
        50, 7, "GPS", "f", "Lat");
    QByteArray floatRecord = messageRecord(50, 7);
    writeFloat(&floatRecord, 3, std::numeric_limits<float>::quiet_NaN());
    run = anonymize(floatDefinition + floatRecord);
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("not finite")));

    QByteArray ordinary = messageRecord(55, 7);
    writeU32(&ordinary, 3, 17);
    run = anonymize(formatRecord(55, 7, "TEST", "I", "Count")
                    + ordinary);
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("No coordinate fields")));
    QVERIFY(run.output.isEmpty());

    run = anonymize(formatRecord(
        201, 92, "FMTU", "QBaN",
        "TimeUS,FmtType,UnitIds,MultIds"));
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("16-byte N")));
    QVERIFY(run.output.isEmpty());
}

void DataFlashLogAnonymizerTest::unsignedFormatUsesSignedCoordinateBits()
{
    const QByteArray definition = formatRecord(
        56, 7, "RFRN", "I", "Lat");
    QByteArray record = messageRecord(56, 7);
    writeI32(&record, 3, 5000000);

    const Run run = anonymize(definition + record, {-1.0, 0.0});
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(readI32(run.output, definition.size() + 3), qint32(-5000000));
}

void DataFlashLogAnonymizerTest::integerOverflowFailsInsteadOfWrapping()
{
    const QByteArray definition = formatRecord(
        51, 7, "GPS", "L", "Lat");
    QByteArray record = messageRecord(51, 7);
    writeI32(&record, 3, std::numeric_limits<qint32>::max() - 1);
    const Run run = anonymize(definition + record, {1.0, 0.0});
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("overflowed")));
    QCOMPARE(run.result.patchedValues, qint64(0));
}

void DataFlashLogAnonymizerTest::preservesOnlyIncompleteNonCoordinateTail()
{
    const QByteArray coordinateDefinition = formatRecord(
        57, 7, "GPS", "L", "Lat");
    QByteArray coordinateRecord = messageRecord(57, 7);
    writeI32(&coordinateRecord, 3, 10000000);
    const QByteArray textDefinition = formatRecord(
        60, 67, "MSG", "Z", "Message");
    const QByteArray safeTail = messageRecord(60, 7);
    const QByteArray input = coordinateDefinition + coordinateRecord
        + textDefinition + safeTail;

    Run run = anonymize(input, {1.0, 0.0});
    QVERIFY2(run.result.success, qPrintable(run.result.error));
    QCOMPARE(run.result.records, qint64(3));
    QCOMPARE(run.result.outputBytes, qint64(input.size()));
    QCOMPARE(run.output.right(safeTail.size()), safeTail);
    QCOMPARE(readI32(run.output, coordinateDefinition.size() + 3),
             qint32(20000000));
    QCOMPARE(run.result.warnings.size(), 1);
    QVERIFY(run.result.warnings.first().contains(
        QStringLiteral("incomplete non-coordinate MSG")));

    const QByteArray partialCoordinate = messageRecord(57, 5);
    run = anonymize(coordinateDefinition + partialCoordinate,
                    {1.0, 0.0});
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(
        QStringLiteral("incomplete coordinate record")));
    QVERIFY(run.output.isEmpty());

    run = anonymize(coordinateDefinition
                    + formatRecord(58, 7, "GPS", "L", "Lat").left(8),
                    {1.0, 0.0});
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("incomplete metadata")));
    QVERIFY(run.output.isEmpty());
}

void DataFlashLogAnonymizerTest::
cancellationIsBoundedAndReportsPartialStaging()
{
    const QByteArray definition = formatRecord(
        52, 7, "GPS", "L", "Lat");
    QByteArray record = messageRecord(52, 7);
    writeI32(&record, 3, 10000000);
    QByteArray input = definition;
    for (int index = 0; index < 1000; ++index) input.append(record);

    int checks = 0;
    const Run run = anonymize(
        input, {1.0, 0.0}, [&checks]() { return ++checks > 1100; });
    QVERIFY(!run.result.success);
    QVERIFY(run.result.cancelled);
    QVERIFY(run.result.error.contains(QStringLiteral("cancelled")));
    QVERIFY(run.result.outputBytes > 0);
    QVERIFY(run.result.outputBytes < run.result.inputBytes);
    QCOMPARE(qint64(run.output.size()), run.result.outputBytes);
}

void DataFlashLogAnonymizerTest::detectsInputMutationBetweenPasses()
{
    QByteArray input = formatRecord(53, 8, "GPS", "LB", "Lat,Status");
    QByteArray record = messageRecord(53, 8);
    writeI32(&record, 3, 10000000);
    record[7] = 3;
    input.append(record);
    bool changed = false;
    QBuffer inputDevice(&input);
    QBuffer outputDevice;
    QVERIFY(inputDevice.open(QIODevice::ReadOnly));
    QVERIFY(outputDevice.open(QIODevice::WriteOnly));
    const LogAnonymizeResult result = DataFlashLogAnonymizer::anonymize(
        &inputDevice, &outputDevice, {1.0, 0.0}, {},
        [&input, &changed](qint64 completed, qint64) {
            if (!changed && completed == 0) {
                input[input.size() - 1] = 4;
                changed = true;
            }
        });
    QVERIFY(changed);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("changed")));
}

void DataFlashLogAnonymizerTest::rejectsOutputMutationByProgressCallback()
{
    QByteArray inputBytes = formatRecord(59, 7, "GPS", "L", "Lat");
    QByteArray record = messageRecord(59, 7);
    writeI32(&record, 3, 10000000);
    inputBytes.append(record);
    QBuffer input(&inputBytes);
    QBuffer output;
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVERIFY(output.open(QIODevice::WriteOnly));

    bool mutated = false;
    const LogAnonymizeResult result = DataFlashLogAnonymizer::anonymize(
        &input, &output, {1.0, 0.0}, {},
        [&output, &mutated](qint64 completed, qint64) {
            if (!mutated && completed == 0) {
                output.write("x", 1);
                mutated = true;
            }
        });
    QVERIFY(mutated);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("invalidated")));
}

void DataFlashLogAnonymizerTest::rejectsInvalidDevicesAndUnknownFraming()
{
    QByteArray inputBytes = formatRecord(54, 7, "GPS", "L", "Lat");
    QByteArray outputBytes;
    QBuffer input(&inputBytes);
    QBuffer output(&outputBytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write("x", 1);
    LogAnonymizeResult result = DataFlashLogAnonymizer::anonymize(
        &input, &output, {1.0, 1.0});
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("empty")));

    QByteArray corrupt("not a DataFlash record");
    const Run run = anonymize(corrupt);
    QVERIFY(!run.result.success);
    QVERIFY(run.result.error.contains(QStringLiteral("framing")));
    QVERIFY(run.output.isEmpty());
}

QTEST_GUILESS_MAIN(DataFlashLogAnonymizerTest)

#include "test_dataflashloganonymizer.moc"
