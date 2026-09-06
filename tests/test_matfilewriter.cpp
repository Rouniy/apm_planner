#include "comm/MatFileWriter.h"

#include <QFile>
#include <QTemporaryFile>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <climits>
#include <cstring>
#include <limits>

namespace {

quint32 uint32At(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

qint32 int32At(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

double doubleAt(const QByteArray &bytes, qint64 offset)
{
    const quint64 bits = qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QByteArray contents(QFileDevice *file)
{
    if (!file->seek(0)) {
        return {};
    }
    return file->readAll();
}

struct ParsedCell {
    bool valid = false;
    quint32 type = 0;
    qint32 rows = 0, columns = 0;
    QByteArray name;
    QString text;
    double number = 0;
    QVector<ParsedCell> children;
    int end = 0;
};

// Independent Level-5 tag walker: tests do not reuse writer layout helpers.
ParsedCell parseCell(const QByteArray &bytes, int offset, int depth = 0)
{
    ParsedCell value;
    if (depth > 20 || offset < 0 || offset > bytes.size() - 48
        || uint32At(bytes, offset) != 14) return value;
    const quint32 length = uint32At(bytes, offset + 4);
    if (length > quint32(bytes.size() - offset - 8)) return value;
    value.end = offset + 8 + int(length);
    if (uint32At(bytes, offset + 8) != 6 || uint32At(bytes, offset + 12) != 8
        || uint32At(bytes, offset + 24) != 5 || uint32At(bytes, offset + 28) != 8
        || uint32At(bytes, offset + 40) != 1) return value;
    value.type = uint32At(bytes, offset + 16);
    value.rows = int32At(bytes, offset + 32); value.columns = int32At(bytes, offset + 36);
    const quint32 nameBytes = uint32At(bytes, offset + 44);
    if (nameBytes > quint32(value.end - offset - 48)) return value;
    value.name = bytes.mid(offset + 48, int(nameBytes));
    int at = offset + 48 + int((nameBytes + 7) & ~quint32(7));
    if (value.type == 1) {
        const qint64 count = qint64(value.rows) * value.columns;
        if (value.rows < 0 || value.columns < 0 || count > 10000) return value;
        for (qint64 i = 0; i < count; ++i) {
            const auto child = parseCell(bytes, at, depth + 1);
            if (!child.valid || child.end > value.end) return value;
            value.children.append(child); at = child.end;
        }
    } else if (value.type == 4 || value.type == 6) {
        if (at > value.end - 8) return value;
        const quint32 type = uint32At(bytes, at), size = uint32At(bytes, at + 4);
        at += 8;
        if (size > quint32(value.end - at)) return value;
        if (value.type == 4) {
            if (type != 4 || size % 2 || qint64(value.rows) * value.columns != size / 2) return value;
            for (quint32 i = 0; i < size; i += 2)
                value.text.append(QChar(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + at + i))));
        } else {
            if (type != 9 || size != 8 || value.rows != 1 || value.columns != 1) return value;
            value.number = doubleAt(bytes, at);
        }
        at += int((size + 7) & ~quint32(7));
    } else return value;
    value.valid = at == value.end;
    return value;
}

MatFileWriter::CellValue cellText(const QString &text)
{
    MatFileWriter::CellValue value;
    value.type = MatFileWriter::CellValue::Type::Text; value.text = text;
    return value;
}

} // namespace

class MatFileWriterTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesOfficialLevelFiveLayoutAndInterleavedColumns();
    void supportsMultipleAndEmptyMatrices();
    void rejectsInvalidNamesDimensionsAndElementBounds();
    void rejectsHolesOutOfOrderWritesAndForgedReceipts();
    void chunksLargeColumnsAndPreservesDoubleBits();
    void refusesUnsuitableOrExternallyChangedDevices();
    void writesColumnMajorCellsNestedRowsAndUtf16();
    void appendsCellsBeforeAndAfterReservedNumericMatrices();
    void emptyCellsAndTextRetainDimensions();
    void streamsMatrixLargerThanSingleValueBound();
    void cellPreflightRefusalsDoNotModifyOutput();
    void cellProviderFailurePermanentlyInvalidatesWriter();
    void cellBounds_data();
    void cellBounds();
    void cellProviderReentryDeletionAndOutputMutation_data();
    void cellProviderReentryDeletionAndOutputMutation();
};

void MatFileWriterTest::writesOfficialLevelFiveLayoutAndInterleavedColumns()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    MatFileWriter writer(&file);
    QString error;
    QVERIFY2(writer.begin(&error), qPrintable(error));
    MatFileWriter::Matrix matrix;
    QVERIFY2(writer.reserveDoubleMatrix("values", 3, 2, &matrix, &error),
             qPrintable(error));
    QCOMPARE(matrix.dataOffset, qint64(192));
    QVERIFY(writer.writeDoubleColumn(matrix, 0, 0, {1.0, 2.0}, &error));
    QVERIFY(writer.writeDoubleColumn(matrix, 1, 0, {10.0}, &error));
    QVERIFY(writer.writeDoubleColumn(matrix, 0, 2, {3.0}, &error));
    QVERIFY(writer.writeDoubleColumn(matrix, 1, 1, {20.0, 30.0}, &error));
    QVERIFY2(writer.finish(&error), qPrintable(error));

    const QByteArray bytes = contents(&file);
    QCOMPARE(bytes.size(), 240);
    QVERIFY(bytes.startsWith("MATLAB 5.0 MAT-file"));
    QCOMPARE(bytes.mid(116, 8), QByteArray(8, '\0'));
    QCOMPARE(quint8(bytes.at(124)), quint8(0));
    QCOMPARE(quint8(bytes.at(125)), quint8(1));
    QCOMPARE(bytes.mid(126, 2), QByteArray("IM"));
    QCOMPARE(uint32At(bytes, 128), quint32(14));
    QCOMPARE(uint32At(bytes, 132), quint32(104));
    QCOMPARE(uint32At(bytes, 136), quint32(6));
    QCOMPARE(uint32At(bytes, 140), quint32(8));
    QCOMPARE(uint32At(bytes, 144), quint32(6));
    QCOMPARE(uint32At(bytes, 148), quint32(0));
    QCOMPARE(uint32At(bytes, 152), quint32(5));
    QCOMPARE(uint32At(bytes, 156), quint32(8));
    QCOMPARE(int32At(bytes, 160), qint32(3));
    QCOMPARE(int32At(bytes, 164), qint32(2));
    QCOMPARE(uint32At(bytes, 168), quint32(1));
    QCOMPARE(uint32At(bytes, 172), quint32(6));
    QCOMPARE(bytes.mid(176, 8), QByteArray("values\0\0", 8));
    QCOMPARE(uint32At(bytes, 184), quint32(9));
    QCOMPARE(uint32At(bytes, 188), quint32(48));
    const QVector<double> expected{1.0, 2.0, 3.0, 10.0, 20.0, 30.0};
    for (int index = 0; index < expected.size(); ++index) {
        QCOMPARE(doubleAt(bytes, matrix.dataOffset + qint64(index) * 8),
                 expected.at(index));
    }
}

void MatFileWriterTest::supportsMultipleAndEmptyMatrices()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    MatFileWriter writer(&file);
    QString error;
    QVERIFY(writer.begin(&error));
    MatFileWriter::Matrix empty;
    QVERIFY2(writer.reserveDoubleMatrix(QString::fromUtf8("α").toUtf8(),
                                        0, 1000000, &empty, &error),
             qPrintable(error));
    MatFileWriter::Matrix scalar;
    QVERIFY(writer.reserveDoubleMatrix("scalar", 1, 1, &scalar, &error));
    QVERIFY(writer.writeDoubleColumn(scalar, 0, 0, {42.5}, &error));
    QVERIFY2(writer.finish(&error), qPrintable(error));
    QVERIFY(empty.dataOffset >= 128);
    QVERIFY(scalar.dataOffset > empty.dataOffset);
    QCOMPARE(doubleAt(contents(&file), scalar.dataOffset), 42.5);
}

void MatFileWriterTest::rejectsInvalidNamesDimensionsAndElementBounds()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    MatFileWriter writer(&file);
    QString error;
    QVERIFY(writer.begin(&error));
    MatFileWriter::Matrix matrix;
    QVERIFY(!writer.reserveDoubleMatrix({}, 1, 1, &matrix, &error));
    QVERIFY(!writer.reserveDoubleMatrix(QByteArray("bad\0name", 8), 1, 1,
                                        &matrix, &error));
    QVERIFY(!writer.reserveDoubleMatrix(QByteArray(128, 'x'), 1, 1,
                                        &matrix, &error));
    MatFileWriter::Matrix maximumName;
    QVERIFY(writer.reserveDoubleMatrix(QByteArray(127, 'x'), 0, 0,
                                       &maximumName, &error));
    QVERIFY(!writer.reserveDoubleMatrix(QByteArray("\xc3\x28", 2), 1, 1,
                                        &matrix, &error));
    QVERIFY(!writer.reserveDoubleMatrix("negative", -1, 1, &matrix, &error));
    QVERIFY(!writer.reserveDoubleMatrix(
        "rows", qint64(std::numeric_limits<qint32>::max()) + 1, 1,
        &matrix, &error));
    QVERIFY(!writer.reserveDoubleMatrix(
        "too_big", std::numeric_limits<qint32>::max(), 1, &matrix, &error));
    QVERIFY(writer.reserveDoubleMatrix("valid", 1, 1, &matrix, &error));
    MatFileWriter::Matrix duplicate;
    QVERIFY(!writer.reserveDoubleMatrix("valid", 1, 1, &duplicate, &error));
    QVERIFY(writer.writeDoubleColumn(matrix, 0, 0, {1.0}, &error));
    QVERIFY(writer.finish(&error));
}

void MatFileWriterTest::rejectsHolesOutOfOrderWritesAndForgedReceipts()
{
    QTemporaryFile first;
    QTemporaryFile second;
    QVERIFY(first.open());
    QVERIFY(second.open());
    MatFileWriter firstWriter(&first);
    MatFileWriter secondWriter(&second);
    QString error;
    QVERIFY(firstWriter.begin(&error));
    QVERIFY(secondWriter.begin(&error));
    MatFileWriter::Matrix matrix;
    MatFileWriter::Matrix other;
    QVERIFY(firstWriter.reserveDoubleMatrix("data", 3, 2, &matrix, &error));
    QVERIFY(secondWriter.reserveDoubleMatrix("data", 3, 2, &other, &error));
    QCOMPARE(matrix.dataOffset, other.dataOffset);
    QVERIFY(!firstWriter.writeDoubleColumn(other, 0, 0, {1.0}, &error));
    QVERIFY(!firstWriter.writeDoubleColumn(matrix, 0, 1, {2.0}, &error));
    QVERIFY(firstWriter.writeDoubleColumn(matrix, 0, 0, {1.0, 2.0}, &error));
    QVERIFY(!firstWriter.finish(&error));
    QVERIFY(firstWriter.writeDoubleColumn(matrix, 0, 2, {3.0}, &error));
    MatFileWriter::Matrix forged = matrix;
    forged.rows = 4;
    QVERIFY(!firstWriter.writeDoubleColumn(forged, 1, 0, {1.0}, &error));
    QVERIFY(firstWriter.writeDoubleColumn(matrix, 1, 0, {4.0, 5.0, 6.0}, &error));
    QVERIFY(firstWriter.finish(&error));
    QVERIFY(!firstWriter.finish(&error));
    QVERIFY(!firstWriter.writeDoubleColumn(matrix, 1, 0, {1.0}, &error));
}

void MatFileWriterTest::chunksLargeColumnsAndPreservesDoubleBits()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    MatFileWriter writer(&file);
    QString error;
    QVERIFY(writer.begin(&error));
    QVector<double> values(MatFileWriter::MaximumWriteChunkValues + 5, 1.25);
    values[0] = -0.0;
    values[1] = std::numeric_limits<double>::infinity();
    values[2] = -std::numeric_limits<double>::infinity();
    values[3] = std::numeric_limits<double>::quiet_NaN();
    MatFileWriter::Matrix matrix;
    QVERIFY(writer.reserveDoubleMatrix("large", values.size(), 1, &matrix, &error));
    QVERIFY(writer.writeDoubleColumn(matrix, 0, 0, values, &error));
    QVERIFY(writer.finish(&error));
    const QByteArray bytes = contents(&file);
    QVERIFY(std::signbit(doubleAt(bytes, matrix.dataOffset)));
    QVERIFY(std::isinf(doubleAt(bytes, matrix.dataOffset + 8)));
    QVERIFY(doubleAt(bytes, matrix.dataOffset + 8) > 0);
    QVERIFY(std::isinf(doubleAt(bytes, matrix.dataOffset + 16)));
    QVERIFY(doubleAt(bytes, matrix.dataOffset + 16) < 0);
    QVERIFY(std::isnan(doubleAt(bytes, matrix.dataOffset + 24)));
    QCOMPARE(doubleAt(bytes, matrix.dataOffset + qint64(values.size() - 1) * 8),
             1.25);
}

void MatFileWriterTest::refusesUnsuitableOrExternallyChangedDevices()
{
    QTemporaryFile unopened;
    MatFileWriter unopenedWriter(&unopened);
    QString error;
    QVERIFY(!unopenedWriter.begin(&error));

    QTemporaryFile nonempty;
    QVERIFY(nonempty.open());
    QCOMPARE(nonempty.write("x", 1), qint64(1));
    MatFileWriter nonemptyWriter(&nonempty);
    QVERIFY(!nonemptyWriter.begin(&error));

    QTemporaryFile changed;
    QVERIFY(changed.open());
    MatFileWriter changedWriter(&changed);
    QVERIFY(changedWriter.begin(&error));
    QVERIFY(changed.seek(changed.size()));
    QCOMPARE(changed.write("x", 1), qint64(1));
    MatFileWriter::Matrix matrix;
    QVERIFY(!changedWriter.reserveDoubleMatrix("data", 1, 1, &matrix, &error));
    QVERIFY(error.contains(QStringLiteral("changed")));
}

void MatFileWriterTest::writesColumnMajorCellsNestedRowsAndUtf16()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    using Value = MatFileWriter::CellValue;
    QString unicode = QString::fromUtf8("α😀"); unicode += QChar(0); unicode += QChar(0xd800);
    QVector<Value> values{cellText("P_ONE"), cellText(unicode), {}, {}};
    values[2].number = -0.0;
    values[3].type = Value::Type::Cell; values[3].rows = 3; values[3].columns = 1;
    Value scalar; scalar.number = 42.5;
    Value nested; nested.type = Value::Type::Cell; nested.rows = 1; nested.columns = 1;
    nested.children = {cellText("tail")};
    values[3].children = {scalar, cellText(" row text "), nested};
    QVector<qint64> calls; QString error;
    QVERIFY2(writer.appendCellMatrix("PARM", 2, 2, [&](qint64 index, Value *value, QString *) {
        calls.append(index); *value = values.at(int(index)); return true;
    }, &error), qPrintable(error));
    QVERIFY(writer.finish(&error));
    QCOMPARE(calls, QVector<qint64>({0, 1, 2, 3}));
    const auto bytes = contents(&file); const auto matrix = parseCell(bytes, 128);
    QVERIFY(matrix.valid); QCOMPARE(matrix.end, bytes.size()); QCOMPARE(matrix.type, quint32(1));
    QCOMPARE(matrix.name, QByteArray("PARM")); QCOMPARE(matrix.rows, 2); QCOMPARE(matrix.columns, 2);
    QCOMPARE(matrix.children[0].text, QStringLiteral("P_ONE")); QCOMPARE(matrix.children[1].text, unicode);
    QCOMPARE(matrix.children[1].columns, unicode.size()); QVERIFY(std::signbit(matrix.children[2].number));
    const auto row = matrix.children[3]; QCOMPARE(row.rows, 3); QCOMPARE(row.columns, 1);
    QCOMPARE(row.children[0].number, 42.5); QCOMPARE(row.children[1].text, QStringLiteral(" row text "));
    QCOMPARE(row.children[2].children[0].text, QStringLiteral("tail"));
    for (const auto &child : matrix.children) QVERIFY(child.name.isEmpty());
}

void MatFileWriterTest::appendsCellsBeforeAndAfterReservedNumericMatrices()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    auto textProvider = [](qint64 index, MatFileWriter::CellValue *value, QString *) {
        *value = cellText(index ? "Value" : "Line"); return true;
    };
    QVERIFY(writer.appendCellMatrix("GPS_label", 2, 1, textProvider));
    const qint64 firstEnd = file.size();
    MatFileWriter::Matrix numeric;
    QVERIFY(writer.reserveDoubleMatrix("GPS", 2, 2, &numeric));
    const qint64 secondEnd = file.size();
    QVERIFY(writer.appendCellMatrix("Seen", 1, 1, textProvider));
    QVERIFY(writer.writeDoubleColumn(numeric, 1, 0, {30, 40}));
    QVERIFY(writer.writeDoubleColumn(numeric, 0, 0, {10, 20})); QVERIFY(writer.finish());
    const auto bytes = contents(&file);
    const auto labels = parseCell(bytes, 128); QVERIFY(labels.valid); QCOMPARE(qint64(labels.end), firstEnd);
    QCOMPARE(uint32At(bytes, int(firstEnd) + 16), quint32(6));
    QCOMPARE(doubleAt(bytes, numeric.dataOffset), 10.0); QCOMPARE(doubleAt(bytes, numeric.dataOffset + 24), 40.0);
    const auto seen = parseCell(bytes, int(secondEnd)); QVERIFY(seen.valid); QCOMPARE(seen.name, QByteArray("Seen"));
}

void MatFileWriterTest::emptyCellsAndTextRetainDimensions()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    QVERIFY(writer.appendCellMatrix("empty", 0, 1000000, {})); const auto end = file.size();
    QVERIFY(writer.appendCellMatrix("text", 1, 1, [](qint64, MatFileWriter::CellValue *value, QString *) {
        *value = cellText({}); return true;
    })); QVERIFY(writer.finish());
    const auto bytes = contents(&file); const auto empty = parseCell(bytes, 128);
    QVERIFY(empty.valid); QCOMPARE(empty.rows, 0); QCOMPARE(empty.columns, 1000000); QVERIFY(empty.children.isEmpty());
    const auto text = parseCell(bytes, int(end)); QVERIFY(text.valid);
    QCOMPARE(text.children[0].rows, 0); QCOMPARE(text.children[0].columns, 0); QCOMPARE(text.children[0].type, quint32(4));
}

void MatFileWriterTest::cellPreflightRefusalsDoNotModifyOutput()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    const auto before = contents(&file); QString error; bool called = false;
    const auto provider = [&](qint64, MatFileWriter::CellValue *, QString *) { called = true; return true; };
    QVERIFY(!writer.appendCellMatrix("", 1, 1, provider, &error));
    QVERIFY(!writer.appendCellMatrix(QByteArray(128, 'n'), 1, 1, provider, &error));
    QVERIFY(!writer.appendCellMatrix("negative", -1, 1, provider, &error));
    QVERIFY(!writer.appendCellMatrix("overflow", INT_MAX, INT_MAX, provider, &error));
    QVERIFY(!writer.appendCellMatrix("missing", 1, 1, {}, &error));
    QCOMPARE(contents(&file), before); QVERIFY(!called);
    QVERIFY(writer.appendCellMatrix("empty", 0, 0, {}, &error));
    QVERIFY(!writer.appendCellMatrix("empty", 0, 0, {}, &error));
    MatFileWriter::Matrix matrix; QVERIFY(!writer.reserveDoubleMatrix("empty", 0, 0, &matrix, &error));
    QVERIFY(writer.finish());
}

void MatFileWriterTest::streamsMatrixLargerThanSingleValueBound()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    const QString text(9000, 'x'); qint64 calls = 0;
    QVERIFY(writer.appendCellMatrix("stream", 600, 1, [&](qint64 index, MatFileWriter::CellValue *value, QString *) {
        if (index != calls) return false;
        ++calls; *value = cellText(text); return true;
    }));
    QCOMPARE(calls, qint64(600)); QVERIFY(file.size() > MatFileWriter::MaximumCellValueBytes);
    QVERIFY(writer.finish()); QVERIFY(file.seek(128));
    const QByteArray tag = file.read(8); QCOMPARE(uint32At(tag, 0), quint32(14));
    QCOMPARE(qint64(uint32At(tag, 4)), file.size() - 136);
}

void MatFileWriterTest::cellProviderFailurePermanentlyInvalidatesWriter()
{
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin());
    int calls = 0; QString error;
    QVERIFY(!writer.appendCellMatrix("data", 3, 1, [&](qint64 index, MatFileWriter::CellValue *value, QString *why) {
        ++calls; value->number = 7;
        if (index == 1) { *why = "cancelled by caller"; return false; } return true;
    }, &error));
    QCOMPARE(calls, 2); QCOMPARE(error, QStringLiteral("cancelled by caller"));
    QVERIFY(file.size() > 128); const auto partial = contents(&file);
    QVERIFY(!writer.finish(&error)); QCOMPARE(error, QStringLiteral("cancelled by caller"));
    QVERIFY(!writer.appendCellMatrix("other", 0, 0, {}, &error));
    MatFileWriter::Matrix receipt; QVERIFY(!writer.reserveDoubleMatrix("later", 1, 1, &receipt, &error));
    QCOMPARE(contents(&file), partial);
}

void MatFileWriterTest::cellBounds_data()
{
    QTest::addColumn<QString>("kind");
    for (const QString &kind : {QString("depth"), QString("nodes"), QString("bytes"), QString("dimensions"), QString("type")})
        QTest::newRow(qPrintable(kind)) << kind;
}

void MatFileWriterTest::cellBounds()
{
    QFETCH(QString, kind);
    QTemporaryFile file; QVERIFY(file.open()); MatFileWriter writer(&file); QVERIFY(writer.begin()); QString error;
    QVERIFY(!writer.appendCellMatrix("bounded", 1, 1, [&](qint64, MatFileWriter::CellValue *value, QString *) {
        using V = MatFileWriter::CellValue;
        if (kind == "bytes") *value = cellText(QString(int(MatFileWriter::MaximumCellValueBytes / 2), 'x'));
        else if (kind == "type") value->type = static_cast<V::Type>(99);
        else {
            value->type = V::Type::Cell; value->rows = 1; value->columns = 1;
            if (kind == "nodes") {
                value->rows = MatFileWriter::MaximumCellNodes; value->children.resize(MatFileWriter::MaximumCellNodes);
            } else if (kind == "depth") {
                V nested;
                for (int i = 0; i < MatFileWriter::MaximumCellDepth; ++i) {
                    V outer; outer.type = V::Type::Cell; outer.rows = 1; outer.columns = 1;
                    outer.children = {nested}; nested = outer;
                }
                *value = nested;
            }
        }
        return true;
    }, &error));
    QVERIFY(!error.isEmpty()); QVERIFY(!writer.finish());
}

void MatFileWriterTest::cellProviderReentryDeletionAndOutputMutation_data()
{
    QTest::addColumn<QString>("kind");
    for (const QString &kind : {QString("finish"), QString("reserve"), QString("append"), QString("delete-writer"),
                               QString("delete-file"), QString("close-file"), QString("resize-file"), QString("throw")})
        QTest::newRow(qPrintable(kind)) << kind;
}

void MatFileWriterTest::cellProviderReentryDeletionAndOutputMutation()
{
    QFETCH(QString, kind);
    auto *file = new QTemporaryFile; QVERIFY(file->open());
    auto *writer = new MatFileWriter(file); QVERIFY(writer->begin());
    QString error; int calls = 0; bool actionOk = true;
    const bool ok = writer->appendCellMatrix("data", 2, 1, [&](qint64, MatFileWriter::CellValue *, QString *) {
        ++calls;
        if (kind == "finish") { QString local; actionOk = !writer->finish(&local); }
        else if (kind == "reserve") { MatFileWriter::Matrix receipt; actionOk = !writer->reserveDoubleMatrix("nested", 1, 1, &receipt); }
        else if (kind == "append") actionOk = !writer->appendCellMatrix("nested", 0, 0, {});
        else if (kind == "delete-writer") { delete writer; writer = nullptr; }
        else if (kind == "delete-file") { delete file; file = nullptr; }
        else if (kind == "close-file") file->close();
        else if (kind == "resize-file") actionOk = file->resize(file->size() + 1);
        else throw 42;
        return true;
    }, &error);
    QVERIFY(!ok); QVERIFY(actionOk); QVERIFY(!error.isEmpty()); QCOMPARE(calls, 1);
    if (writer) { QVERIFY(!writer->finish()); delete writer; }
    delete file;
}

QTEST_GUILESS_MAIN(MatFileWriterTest)
#include "test_matfilewriter.moc"
