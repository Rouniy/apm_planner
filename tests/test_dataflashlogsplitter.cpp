#include "ui/Loghandling/DataFlashLogSplitter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cstring>

namespace {
QByteArray record(quint8 id, int size)
{
    QByteArray bytes(size, '\0');
    bytes[0] = char(0xa3); bytes[1] = char(0x95); bytes[2] = char(id);
    return bytes;
}
void stringField(QByteArray &bytes, int offset, const QByteArray &value)
{
    std::memcpy(bytes.data() + offset, value.constData(), size_t(value.size()));
}
QByteArray fmt(quint8 id, quint8 size, const QByteArray &name,
               const QByteArray &format, const QByteArray &columns)
{
    QByteArray bytes = record(128, 89);
    bytes[3] = char(id); bytes[4] = char(size);
    stringField(bytes, 5, name); stringField(bytes, 9, format); stringField(bytes, 25, columns);
    return bytes;
}
QByteArray sample(int number)
{
    QByteArray bytes = record(1, 7);
    qToLittleEndian<quint32>(quint32(number), reinterpret_cast<uchar *>(bytes.data() + 3));
    return bytes;
}
bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray simpleBinary(int count)
{
    QByteArray bytes = fmt(1, 7, "TEST", "I", "Value");
    for (int index = 0; index < count; ++index) bytes += sample(index);
    return bytes;
}
QStringList siblings(const QString &path)
{
    return QDir(path).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
}
}

class DataFlashLogSplitterTest final : public QObject
{
    Q_OBJECT
private slots:
    void binaryWholeRecordsBalancedAndRemainder_data();
    void binaryWholeRecordsBalancedAndRemainder();
    void textLinesCommasAndFinalDelimiter();
    void optionalMetadataAndRepeatedTimestamps();
    void malformedAndConflictingInputs_data();
    void malformedAndConflictingInputs();
    void pathsLimitsAndExistingOutputs();
    void symlinkOutputAndParentAliases();
    void cancellationBeforePublication_data();
    void cancellationBeforePublication();
    void sourceMutationBetweenAndAfterPasses_data();
    void sourceMutationBetweenAndAfterPasses();
    void partialPublicationPreservesEarlierPartsAndCollision();
    void cancellationAfterPublicationBeginsDoesNotLeaveMissingParts();
    void halfFloatsKnownTailAndBlankLines();
    void metadataBoundRejectsBeforePublication();
    void lastCancellationGatePreventsPublication();
};

void DataFlashLogSplitterTest::binaryWholeRecordsBalancedAndRemainder_data()
{
    QTest::addColumn<int>("pieces");
    QTest::newRow("two") << 2;
    QTest::newRow("three") << 3;
    QTest::newRow("seven") << 7;
    QTest::newRow("maximum 1000") << 1000;
}

void DataFlashLogSplitterTest::binaryWholeRecordsBalancedAndRemainder()
{
    QFETCH(int, pieces);
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    const int count = pieces * 3 + 2;
    const QByteArray source = simpleBinary(count);
    QVERIFY(put(input, source));
    const auto result = DataFlashLogSplitter::Split(input, pieces);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.outputs, DataFlashLogSplitter::OutputPaths(input, pieces));
    QCOMPARE(result.recordsRead, qint64(count + 1));
    QCOMPARE(result.dataRecords, qint64(count));
    QCOMPARE(result.bytesWritten, qint64(pieces * 89 + count * 7));
    QByteArray data;
    qint64 minimum = 1000000, maximum = 0;
    for (const QString &path : result.outputs) {
        const QByteArray part = get(path);
        QVERIFY(part.startsWith(source.left(89)));
        const QByteArray body = part.mid(89);
        QVERIFY(!body.isEmpty());
        QCOMPARE(body.size() % 7, 0);
        data += body;
        minimum = qMin(minimum, qint64(body.size())); maximum = qMax(maximum, qint64(body.size()));
    }
    QCOMPARE(data, source.mid(89));
    QVERIFY(maximum - minimum <= 7);
    QCOMPARE(get(input), source);
    QVERIFY(!result.warnings.isEmpty());
}

void DataFlashLogSplitterTest::textLinesCommasAndFinalDelimiter()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.log");
    const QByteArray first = "FMT,1,7,TEST,I,Value\r\n";
    const QByteArray second = "FMT,2,67,MSG,Z,Message\n";
    const QByteArray third = "FMT,3,7,LATE,I,Value"; // Valid terminal metadata has no newline.
    const QByteArray data = "TEST, 1\r\nMSG, hello, with, commas\nTEST, 2\nTEST, 3\n";
    QVERIFY(put(input, first + second + data + third));
    const auto result = DataFlashLogSplitter::Split(input, 3);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.dataRecords, qint64(4));
    const QByteArray prefix = first + second + third + '\n';
    QByteArray joined;
    for (const auto &path : result.outputs) {
        QVERIFY(path.endsWith(".log"));
        const QByteArray part = get(path);
        QVERIFY(part.startsWith(prefix)); joined += part.mid(prefix.size());
    }
    QCOMPARE(joined, data);
    const QString finalInput = dir.filePath("terminal.log");
    QVERIFY(put(finalInput, first + "TEST, 1\nTEST, 2"));
    const auto terminal = DataFlashLogSplitter::Split(finalInput, 2);
    QVERIFY2(terminal.success, qPrintable(terminal.error));
    QCOMPARE(get(terminal.outputs.last()).mid(first.size()), QByteArray("TEST, 2"));
}

void DataFlashLogSplitterTest::optionalMetadataAndRepeatedTimestamps()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("metadata.bin");
    const QByteArray dataFmt = fmt(1, 7, "TEST", "I", "Value");
    const QByteArray fmtuFmt = fmt(2, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds");
    const QByteArray unitFmt = fmt(3, 76, "UNIT", "QbZ", "TimeUS,Id,Label");
    const QByteArray multFmt = fmt(4, 20, "MULT", "Qbd", "TimeUS,Id,Mult");
    QByteArray mapping = record(2, 44); mapping[11] = 1; mapping[12] = 'm'; mapping[28] = 'a';
    QByteArray laterMapping = mapping; laterMapping[3] = 7;
    QByteArray unit = record(3, 76); unit[11] = 'm'; stringField(unit, 12, "metres");
    QByteArray laterUnit = unit; laterUnit[3] = 8;
    QByteArray mult = record(4, 20); mult[11] = 'a';
    const double multiplier = 1.0; quint64 bits = 0; std::memcpy(&bits, &multiplier, sizeof(bits));
    qToLittleEndian(bits, reinterpret_cast<uchar *>(mult.data() + 12));
    QByteArray laterMult = mult; laterMult[3] = 9;
    const QByteArray source = dataFmt + sample(1) + fmtuFmt + mapping + unitFmt + unit
        + multFmt + mult + sample(2) + laterMapping + laterUnit + laterMult + dataFmt + sample(3);
    QVERIFY(put(input, source));
    const auto result = DataFlashLogSplitter::Split(input, 3);
    QVERIFY2(result.success, qPrintable(result.error));
    const QByteArray prefix = dataFmt + fmtuFmt + unitFmt + multFmt + dataFmt
        + mapping + laterMapping + unit + laterUnit + mult + laterMult;
    for (int index = 0; index < 3; ++index)
        QCOMPARE(get(result.outputs[index]), prefix + sample(index + 1));
}

void DataFlashLogSplitterTest::malformedAndConflictingInputs_data()
{
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QString>("suffix");
    QTest::newRow("truncated final FMT") << (simpleBinary(3) + fmt(2, 7, "TWO", "I", "Value").chopped(1)) << QString("bin");
    QTest::newRow("truncated metadata") << (simpleBinary(3)
        + fmt(2, 44, "FMTU", "QBNN", "TimeUS,FmtType,UnitIds,MultIds") + record(2, 43)) << QString("bin");
    QTest::newRow("garbage prefix") << (QByteArray("bad") + simpleBinary(3)) << QString("bin");
    QTest::newRow("unknown binary type") << (simpleBinary(3) + record(9, 7)) << QString("bin");
    QTest::newRow("conflicting FMT") << (simpleBinary(3) + fmt(1, 7, "TEST", "f", "Value")) << QString("bin");
    QTest::newRow("wrong FMT length") << (fmt(1, 8, "TEST", "I", "Value") + sample(1)) << QString("bin");
    QTest::newRow("unknown format") << (fmt(1, 7, "TEST", "?", "Value") + sample(1)) << QString("bin");
    QTest::newRow("too few data records") << simpleBinary(1) << QString("bin");
    QTest::newRow("unknown text type") << QByteArray("FMT,1,7,TEST,I,Value\nOTHER,1\nTEST,2\n") << QString("log");
    QTest::newRow("missing text field") << QByteArray("FMT,1,11,TEST,II,A,B\nTEST,1\nTEST,2\n") << QString("log");
    QTest::newRow("conflicting text metadata") << QByteArray(
        "FMT,1,7,TEST,I,Value\nFMT,2,76,UNIT,QbZ,TimeUS,Id,Label\n"
        "UNIT,1,109,metres\nTEST,1\nUNIT,2,109,miles\nTEST,2\n") << QString("log");
    QTest::newRow("conflicting FMTU") << QByteArray(
        "FMT,1,7,TEST,I,Value\nFMT,2,44,FMTU,QBNN,TimeUS,FmtType,UnitIds,MultIds\n"
        "FMTU,1,1,m,a\nTEST,1\nFMTU,2,1,f,a\nTEST,2\n") << QString("log");
    QTest::newRow("conflicting MULT") << QByteArray(
        "FMT,1,7,TEST,I,Value\nFMT,2,20,MULT,Qbd,TimeUS,Id,Mult\n"
        "MULT,1,97,1\nTEST,1\nMULT,2,97,2\nTEST,2\n") << QString("log");
}

void DataFlashLogSplitterTest::malformedAndConflictingInputs()
{
    QFETCH(QByteArray, source); QFETCH(QString, suffix);
    QTemporaryDir dir;
    const QString input = dir.filePath("flight." + suffix);
    QVERIFY(put(input, source));
    const auto result = DataFlashLogSplitter::Split(input, 2);
    QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.outputs.isEmpty());
    QCOMPARE(siblings(dir.path()), QStringList{QFileInfo(input).fileName()});
    QCOMPARE(get(input), source);
}

void DataFlashLogSplitterTest::pathsLimitsAndExistingOutputs()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("space name.BIN");
    QVERIFY(put(input, simpleBinary(4)));
    QVERIFY(DataFlashLogSplitter::OutputPaths(input, 1).isEmpty());
    QVERIFY(DataFlashLogSplitter::OutputPaths(input, 1001).isEmpty());
    QVERIFY(DataFlashLogSplitter::OutputPaths(dir.filePath("input.tlog"), 2).isEmpty());
    const auto paths = DataFlashLogSplitter::OutputPaths(input, 2);
    QCOMPARE(paths.first(), input + "_split0.bin");
    QVERIFY(put(paths.last(), "existing output"));
    auto result = DataFlashLogSplitter::Split(input, 2);
    QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
    QCOMPARE(get(paths.last()), QByteArray("existing output")); QVERIFY(!QFile::exists(paths.first()));
    result = DataFlashLogSplitter::Split(dir.filePath("missing.bin"), 2);
    QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
}

void DataFlashLogSplitterTest::symlinkOutputAndParentAliases()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    QVERIFY(put(input, simpleBinary(4)));
    const auto paths = DataFlashLogSplitter::OutputPaths(input, 2);
    if (!QFile::link(dir.filePath("does-not-exist"), paths[1]))
        QSKIP("This platform does not support the required symbolic link fixture.");
    QVERIFY(QFileInfo(paths[1]).isSymLink());
    auto result = DataFlashLogSplitter::Split(input, 2);
    QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
    QVERIFY(QFileInfo(paths[1]).isSymLink()); QVERIFY(!QFile::exists(paths[0]));
    QTemporaryDir aliases;
    const QString alias = aliases.filePath("parent");
    if (!QFile::link(dir.path(), alias)) QSKIP("Directory symlinks unavailable.");
    QCOMPARE(DataFlashLogSplitter::OutputPaths(alias + "/flight.bin", 2), paths);
}

void DataFlashLogSplitterTest::cancellationBeforePublication_data()
{
    QTest::addColumn<int>("phase");
    QTest::newRow("before scan") << 0;
    QTest::newRow("after metadata") << 250;
    QTest::newRow("after staging") << 500;
    QTest::newRow("before final validation") << 750;
}

void DataFlashLogSplitterTest::cancellationBeforePublication()
{
    QFETCH(int, phase);
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    QVERIFY(put(input, simpleBinary(21)));
    bool cancel = false;
    const auto result = DataFlashLogSplitter::Split(input, 3,
        [&] { return cancel; }, [&](qint64 value, qint64) { if (value >= phase) cancel = true; });
    QVERIFY(result.cancelled); QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
    QCOMPARE(siblings(dir.path()), QStringList{"flight.bin"});
}

void DataFlashLogSplitterTest::sourceMutationBetweenAndAfterPasses_data()
{
    QTest::addColumn<int>("phase");
    QTest::newRow("between passes") << 250;
    QTest::newRow("before publication") << 750;
}

void DataFlashLogSplitterTest::sourceMutationBetweenAndAfterPasses()
{
    QFETCH(int, phase);
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    QByteArray bytes = simpleBinary(9);
    QVERIFY(put(input, bytes));
    bool mutated = false;
    const auto result = DataFlashLogSplitter::Split(input, 3, {},
        [&](qint64 value, qint64) {
            if (!mutated && value >= phase) {
                mutated = true; bytes[bytes.size() - 1] = char(bytes.at(bytes.size() - 1) ^ 1); put(input, bytes);
            }
        });
    QVERIFY(mutated); QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
    QVERIFY(result.error.contains("changed"));
    QCOMPARE(siblings(dir.path()), QStringList{"flight.bin"});
}

void DataFlashLogSplitterTest::partialPublicationPreservesEarlierPartsAndCollision()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    QVERIFY(put(input, simpleBinary(9)));
    const auto paths = DataFlashLogSplitter::OutputPaths(input, 3);
    bool collision = false;
    const auto result = DataFlashLogSplitter::Split(input, 3, {}, [&](qint64, qint64) {
        if (!collision && QFile::exists(paths[0])) {
            collision = true; put(paths[1], "new unrelated file");
        }
    });
    QVERIFY(collision); QVERIFY(!result.success);
    QCOMPARE(result.outputs, QStringList{paths[0]});
    QVERIFY(!get(paths[0]).isEmpty()); QCOMPARE(get(paths[1]), QByteArray("new unrelated file"));
    QVERIFY(!QFile::exists(paths[2])); QVERIFY(result.error.contains("1 earlier"));
    QCOMPARE(result.bytesWritten, QFileInfo(paths[0]).size());
}

void DataFlashLogSplitterTest::cancellationAfterPublicationBeginsDoesNotLeaveMissingParts()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.bin");
    QVERIFY(put(input, simpleBinary(9)));
    const auto paths = DataFlashLogSplitter::OutputPaths(input, 3);
    bool cancel = false;
    const auto result = DataFlashLogSplitter::Split(input, 3, [&] { return cancel; },
        [&](qint64, qint64) { if (QFile::exists(paths[0])) cancel = true; });
    QVERIFY(cancel); QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(!result.cancelled);
    QCOMPARE(result.outputs, paths);
}

void DataFlashLogSplitterTest::halfFloatsKnownTailAndBlankLines()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("half.bin");
    const QByteArray definition = fmt(1, 5, "HALF", "g", "Value");
    QByteArray first = record(1, 5); first[3] = char(0x01); first[4] = char(0x3c);
    QByteArray second = record(1, 5); second[3] = char(0x01); second[4] = char(0xc0);
    const QByteArray tail = first.left(4);
    QVERIFY(put(input, definition + first + second + tail));
    const auto result = DataFlashLogSplitter::Split(input, 2);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsRead, qint64(3)); QCOMPARE(result.dataRecords, qint64(2));
    QCOMPARE(get(result.outputs[0]), definition + first);
    QCOMPARE(get(result.outputs[1]), definition + second);
    QVERIFY(result.warnings.join('\n').contains("HALF record (type 1): 4 trailing bytes"));
    const QString text = dir.filePath("blank.log");
    const QByteArray prefix = "FMT,1,7,TEST,I,Value\n";
    QVERIFY(put(text, "\n\r\n" + prefix + "TEST,1\n \t\nTEST,2\n"));
    const auto splitText = DataFlashLogSplitter::Split(text, 2);
    QVERIFY2(splitText.success, qPrintable(splitText.error));
    QCOMPARE(splitText.recordsRead, qint64(3));
    QVERIFY(splitText.warnings.join('\n').contains("Ignored 3 blank text line(s)"));
    QCOMPARE(get(splitText.outputs[0]), prefix + "TEST,1\n");
    QCOMPARE(get(splitText.outputs[1]), prefix + "TEST,2\n");
}

void DataFlashLogSplitterTest::metadataBoundRejectsBeforePublication()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("metadata.bin");
    QFile file(input); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray definition = fmt(1, 7, "TEST", "I", "Value");
    const QByteArray chunk = definition.repeated(1024);
    for (int index = 0; index < 185; ++index) QCOMPARE(file.write(chunk), qint64(chunk.size()));
    QCOMPARE(file.write(sample(1) + sample(2)), qint64(14)); file.close();
    const auto result = DataFlashLogSplitter::Split(input, 2);
    QVERIFY(!result.success); QVERIFY(result.outputs.isEmpty());
    QVERIFY(result.error.contains("16 MiB"));
    QCOMPARE(siblings(dir.path()), QStringList{"metadata.bin"});
}

void DataFlashLogSplitterTest::lastCancellationGatePreventsPublication()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("final.bin");
    QVERIFY(put(input, simpleBinary(6)));
    bool finalVerification = false;
    int checks = 0;
    const auto result = DataFlashLogSplitter::Split(input, 2,
        [&] { return finalVerification && ++checks == 3; },
        [&](qint64 value, qint64) { if (value == 750) finalVerification = true; });
    // For this <64KiB input: pre-hash gate, before hash read, final gate.
    QCOMPARE(checks, 3); QVERIFY(result.cancelled); QVERIFY(!result.success);
    QVERIFY(result.outputs.isEmpty()); QCOMPARE(siblings(dir.path()), QStringList{"final.bin"});
}

QTEST_MAIN(DataFlashLogSplitterTest)
#include "test_dataflashlogsplitter.moc"
