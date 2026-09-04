#include "ui/tools/MjpegAviWriter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

namespace
{
QByteArray jpeg(quint8 color, bool oddLength = false)
{
    QByteArray frame = QByteArray::fromHex("ffd8ffe000104a46494600ffd9");
    frame.insert(frame.size() - 2, char(color));
    if (oddLength) frame.insert(frame.size() - 2, '\0');
    return frame;
}

quint32 littleUInt32(const QByteArray &bytes, int offset)
{
    return quint32(quint8(bytes.at(offset)))
        | (quint32(quint8(bytes.at(offset + 1))) << 8)
        | (quint32(quint8(bytes.at(offset + 2))) << 16)
        | (quint32(quint8(bytes.at(offset + 3))) << 24);
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
}

class MjpegAviWriterTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesIndexedMjpegAviAndUpdatesHeaders();
    void checkpointLeavesReadablePartialHeaders();
    void destructorFinalizesPartialRecording();
    void rejectsOverwriteAndInvalidJpeg();
    void riffCapacityBoundaryIsCheckedWithoutAllocatingFourGiB();
};

void MjpegAviWriterTest::writesIndexedMjpegAviAndUpdatesHeaders()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("overlay.avi"));
    MjpegAviWriter writer(path, 64, 48, 5);
    QVERIFY2(writer.isOpen(), qPrintable(writer.errorString()));
    QVERIFY(writer.writeJpeg(jpeg(0x11)));
    QVERIFY(writer.writeJpeg(jpeg(0x22, true)));
    QCOMPARE(writer.frameCount(), 2);
    QVERIFY2(writer.finalize(), qPrintable(writer.errorString()));
    QVERIFY(!writer.isOpen());
    QVERIFY(writer.finalize());

    const QByteArray avi = readAll(path);
    QCOMPARE(avi.left(4), QByteArray("RIFF"));
    QCOMPARE(avi.mid(8, 4), QByteArray("AVI "));
    QVERIFY(avi.contains("MJPG"));
    const int avih = avi.indexOf("avih");
    const int strh = avi.indexOf("strh");
    const int idx1 = avi.indexOf("idx1");
    QVERIFY(avih >= 0);
    QVERIFY(strh >= 0);
    QVERIFY(idx1 > strh);
    QCOMPARE(littleUInt32(avi, 4), quint32(avi.size() - 8));
    QCOMPARE(littleUInt32(avi, avih + 8 + 16), quint32(2));
    QCOMPARE(littleUInt32(avi, strh + 8 + 32), quint32(2));
    QCOMPARE(littleUInt32(avi, idx1 + 4), quint32(32));
    QCOMPARE(avi.mid(idx1 + 8, 4), QByteArray("00dc"));
    QCOMPARE(avi.mid(idx1 + 24, 4), QByteArray("00dc"));
}

void MjpegAviWriterTest::checkpointLeavesReadablePartialHeaders()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("partial.avi"));
    MjpegAviWriter writer(path, 80, 60, 25);
    QVERIFY(writer.isOpen());
    QVERIFY(writer.writeJpeg(jpeg(0x44)));
    QVERIFY(writer.checkpoint());

    const QByteArray partial = readAll(path);
    QVERIFY(partial.size() > 224);
    QCOMPARE(partial.left(4), QByteArray("RIFF"));
    QCOMPARE(partial.mid(8, 4), QByteArray("AVI "));
    QCOMPARE(littleUInt32(partial, 4), quint32(partial.size() - 8));
    QCOMPARE(partial.indexOf("idx1"), -1);
    const int avih = partial.indexOf("avih");
    QVERIFY(avih >= 0);
    QCOMPARE(littleUInt32(partial, avih + 8 + 16), quint32(1));
    QVERIFY(writer.finalize());
}

void MjpegAviWriterTest::destructorFinalizesPartialRecording()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("scoped.avi"));
    {
        MjpegAviWriter writer(path, 32, 24, 10);
        QVERIFY(writer.isOpen());
        QVERIFY(writer.writeJpeg(jpeg(0x66)));
    }

    const QByteArray avi = readAll(path);
    QVERIFY(avi.indexOf("idx1") > 0);
    QCOMPARE(littleUInt32(avi, 4), quint32(avi.size() - 8));
}

void MjpegAviWriterTest::rejectsOverwriteAndInvalidJpeg()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString existing = temporary.filePath(QStringLiteral("existing.avi"));
    QFile file(existing);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("keep"), qint64(4));
    file.close();

    MjpegAviWriter overwrite(existing, 64, 48, 5);
    QVERIFY(!overwrite.isOpen());
    QVERIFY(overwrite.errorString().contains(QStringLiteral("create"),
                                             Qt::CaseInsensitive));
    QCOMPARE(readAll(existing), QByteArray("keep"));

    const QString fresh = temporary.filePath(QStringLiteral("fresh.avi"));
    MjpegAviWriter writer(fresh, 64, 48, 5);
    QVERIFY(writer.isOpen());
    QVERIFY(!writer.writeJpeg(QByteArray("not jpeg")));
    QCOMPARE(writer.frameCount(), 0);
    QVERIFY(!writer.errorString().isEmpty());
}

void MjpegAviWriterTest::riffCapacityBoundaryIsCheckedWithoutAllocatingFourGiB()
{
    const quint64 limit = std::numeric_limits<quint32>::max();
    QVERIFY(MjpegAviWriter::FitsRiffCapacity(limit + 7, 1));
    QVERIFY(MjpegAviWriter::FitsRiffCapacity(limit + 8, 0));
    QVERIFY(!MjpegAviWriter::FitsRiffCapacity(limit + 8, 1));
    QVERIFY(!MjpegAviWriter::FitsRiffCapacity(
        std::numeric_limits<quint64>::max(), 1));
    QVERIFY(MjpegAviWriter::FitsRiffCapacity(100, 28, 120));
    QVERIFY(!MjpegAviWriter::FitsRiffCapacity(100, 29, 120));
}

QTEST_GUILESS_MAIN(MjpegAviWriterTest)
#include "test_mjpegaviwriter.moc"
