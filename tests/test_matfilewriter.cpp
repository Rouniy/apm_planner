#include "comm/MatFileWriter.h"

#include <QFile>
#include <QTemporaryFile>
#include <QtEndian>
#include <QtTest>

#include <cmath>
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

QTEST_GUILESS_MAIN(MatFileWriterTest)
#include "test_matfilewriter.moc"
