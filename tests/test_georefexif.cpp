#include "ui/Loghandling/GeoRefExif.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
#include <QtEndian>
#include <cmath>
#include <limits>
#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void word(QByteArray &b, quint16 x, bool le)
{ char p[2]; if (le) qToLittleEndian(x, p); else qToBigEndian(x, p); b.append(p, 2); }
void dword(QByteArray &b, quint32 x, bool le)
{ char p[4]; if (le) qToLittleEndian(x, p); else qToBigEndian(x, p); b.append(p, 4); }
quint16 readWord(const QByteArray &b, int p, bool le)
{ return le ? qFromLittleEndian<quint16>(b.constData() + p) : qFromBigEndian<quint16>(b.constData() + p); }
quint32 readDword(const QByteArray &b, int p, bool le)
{ return le ? qFromLittleEndian<quint32>(b.constData() + p) : qFromBigEndian<quint32>(b.constData() + p); }
QByteArray encodedLong(quint32 n, bool le) { QByteArray b; dword(b, n, le); return b; }
QByteArray field(int tag, int type, quint32 count, QByteArray value, bool le)
{
    QByteArray b; word(b, quint16(tag), le); word(b, quint16(type), le); dword(b, count, le);
    b += value; while (b.size() < 12) b += char(0); return b;
}
QByteArray shortField(int tag, quint16 n, bool le)
{ QByteArray value; word(value, n, le); return field(tag, 3, 1, value, le); }
QByteArray longField(int tag, quint32 n, bool le)
{ return field(tag, 4, 1, encodedLong(n, le), le); }
quint32 addDirectory(QByteArray &b, const QVector<QByteArray> &entries, bool le, quint32 next = 0)
{
    if (b.size() & 1) b += char(0);
    const quint32 offset = quint32(b.size());
    word(b, quint16(entries.size()), le);
    for (const auto &e : entries) b += e;
    dword(b, next, le); return offset;
}

QByteArray tiffFixture(bool le = true, QByteArray original = "2024:02:29 12:34:56",
                      QByteArray digitized = "2023:01:02 03:04:05")
{
    QByteArray b = le ? QByteArray::fromHex("49492a0000000000") : QByteArray::fromHex("4d4d002a00000000");
    const quint32 pixelOffset = quint32(b.size()); b += QByteArray::fromHex("197fc1");
    const quint32 noteOffset = quint32(b.size()); b += QByteArray("MakerNote with relative offsets\0\1", 33);
    const quint32 thumbnailOffset = quint32(b.size()); b += QByteArray::fromHex("ffd811223344ffd9");
    QVector<QByteArray> exif;
    if (!original.isEmpty()) {
        const quint32 p = quint32(b.size()); b += original; b += char(0);
        exif += field(0x9003, 2, quint32(original.size() + 1), encodedLong(p, le), le);
    }
    if (!digitized.isEmpty()) {
        const quint32 p = quint32(b.size()); b += digitized; b += char(0);
        exif += field(0x9004, 2, quint32(digitized.size() + 1), encodedLong(p, le), le);
    }
    exif += field(0x927c, 7, 33, encodedLong(noteOffset, le), le);
    exif += field(0xa000, 7, 4, QByteArrayLiteral("0100"), le);
    const quint32 exifOffset = addDirectory(b, exif, le);
    const quint32 thumbnailIfd = addDirectory(b, {longField(513, thumbnailOffset, le), longField(514, 8, le)}, le);
    const QVector<QByteArray> root = {
        longField(256, 3, le), longField(257, 1, le), shortField(258, 8, le),
        shortField(259, 1, le), shortField(262, 1, le), longField(273, pixelOffset, le),
        shortField(274, 6, le), shortField(277, 1, le), longField(278, 1, le),
        longField(279, 3, le), longField(0x8769, exifOffset, le)
    };
    const quint32 rootOffset = addDirectory(b, root, le, thumbnailIfd);
    b.replace(4, 4, encodedLong(rootOffset, le));
    return b;
}
QByteArray segment(quint8 marker, const QByteArray &payload)
{
    QByteArray b; b += char(0xff); b += char(marker); word(b, quint16(payload.size() + 2), false); b += payload; return b;
}
QByteArray jpegFixture(const QByteArray &tiff = {})
{
    QByteArray b = QByteArray::fromHex("ffd8");
    b += segment(0xe0, QByteArray("JFIF\0unrelated", 14));
    b += segment(0xe2, QByteArrayLiteral("ICC_PROFILE_UNCHANGED"));
    if (!tiff.isEmpty()) b += segment(0xe1, QByteArray("Exif\0\0", 6) + tiff);
    b += segment(0xfe, QByteArrayLiteral("original comment"));
    b += segment(0xda, QByteArray::fromHex("010100003f00"));
    b += QByteArray::fromHex("19ff007fffd0c1ffd9");
    return b;
}
bool saveFile(const QString &path, const QByteArray &b)
{ QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(b) == b.size(); }
QByteArray readFile(const QString &path)
{ QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

// Independent small fixture reader: no GeoRefExif inspection/decoding is used
// to establish the GPS values or preserved source blocks.
QByteArray exifBlob(const QByteArray &jpeg)
{
    int p = 2;
    while (p + 4 <= jpeg.size() && quint8(jpeg[p]) == 0xff) {
        const int marker = quint8(jpeg[p + 1]);
        if (marker == 0xda || marker == 0xd9) break;
        const int len = readWord(jpeg, p + 2, false);
        if (len < 2 || p + 2 + len > jpeg.size()) return {};
        if (marker == 0xe1 && jpeg.mid(p + 4, 6) == QByteArray("Exif\0\0", 6))
            return jpeg.mid(p + 10, len - 8);
        p += 2 + len;
    }
    return {};
}
QByteArray withoutExif(QByteArray jpeg)
{
    int p = 2;
    while (p + 4 <= jpeg.size() && quint8(jpeg[p]) == 0xff) {
        const int marker = quint8(jpeg[p + 1]);
        if (marker == 0xda || marker == 0xd9) break;
        const int len = readWord(jpeg, p + 2, false);
        if (marker == 0xe1 && jpeg.mid(p + 4, 6) == QByteArray("Exif\0\0", 6)) {
            jpeg.remove(p, len + 2); return jpeg;
        }
        p += len + 2;
    }
    return jpeg;
}
QByteArray tagEntry(const QByteArray &b, quint32 ifd, int tag)
{
    const bool le = b.startsWith("II");
    if (ifd + 2 > quint32(b.size())) return {};
    const int n = readWord(b, int(ifd), le);
    for (int i = 0; i < n; ++i) {
        const int pos = int(ifd) + 2 + i * 12;
        if (pos + 12 > b.size()) return {};
        if (readWord(b, pos, le) == tag) return b.mid(pos, 12);
    }
    return {};
}
quint32 childIfd(const QByteArray &b, int tag)
{
    const bool le = b.startsWith("II");
    const QByteArray e = tagEntry(b, readDword(b, 4, le), tag);
    return e.size() == 12 ? readDword(e, 8, le) : 0;
}
double rational(const QByteArray &b, int pos)
{
    const bool le = b.startsWith("II");
    return double(readDword(b, pos, le)) / readDword(b, pos + 4, le);
}
double coordinate(const QByteArray &b, int tag)
{
    const bool le = b.startsWith("II");
    const QByteArray e = tagEntry(b, childIfd(b, 0x8825), tag);
    if (e.size() != 12) return std::numeric_limits<double>::quiet_NaN();
    const int pos = int(readDword(e, 8, le));
    return rational(b, pos) + rational(b, pos + 8) / 60 + rational(b, pos + 16) / 3600;
}
class ShortDevice : public QIODevice
{
public:
    ShortDevice() { open(QIODevice::WriteOnly); }
    bool isSequential() const override { return true; }
    QByteArray bytes;
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *p, qint64 n) override {
        if (bytes.size() >= 30) { setErrorString("injected disk full"); return -1; }
        const qint64 used = qMin<qint64>(n, 7); bytes.append(p, int(used)); return used;
    }
};
}

class TestGeoRefExif : public QObject
{
    Q_OBJECT
private slots:
    void preserveAndTag_data()
    {
        QTest::addColumn<bool>("le"); QTest::addColumn<bool>("jpeg");
        QTest::newRow("tiff-little") << true << false;
        QTest::newRow("tiff-big") << false << false;
        QTest::newRow("jpeg-little") << true << true;
        QTest::newRow("jpeg-big") << false << true;
    }
    void preserveAndTag()
    {
        QFETCH(bool, le); QFETCH(bool, jpeg);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QByteArray oldTiff = tiffFixture(le);
        const QByteArray original = jpeg ? jpegFixture(oldTiff) : oldTiff;
        const QString path = dir.filePath("PHOTO.ANY"); QVERIFY(saveFile(path, original));
        const auto before = GeoRefExif::Inspect(path);
        QVERIFY2(before.success, qPrintable(before.error));
        QCOMPARE(before.format, jpeg ? QString("JPEG") : QString("TIFF"));
        QCOMPARE(before.photoTime.date(), QDate(2024, 2, 29));
        QCOMPARE(before.photoTime.time(), QTime(12, 34, 56));
        QCOMPARE(before.photoTime.timeSpec(), Qt::LocalTime);
        QVERIFY(!before.hasCoordinates);
        QBuffer output; QVERIFY(output.open(QIODevice::WriteOnly));
        const GeoRefExif::Coordinates wanted{-35.123456789, 149.987654321, -23.456789};
        QVector<QPair<qint64, qint64>> progress;
        const auto result = GeoRefExif::Write(path, &output, wanted, {}, [&](qint64 a, qint64 b) { progress += qMakePair(a, b); });
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.bytesWritten, qint64(output.data().size()));
        QCOMPARE(output.pos(), result.bytesWritten);
        QVERIFY(!progress.isEmpty()); QCOMPARE(progress.last().first, progress.last().second);
        for (int i = 1; i < progress.size(); ++i) QVERIFY(progress[i].first >= progress[i - 1].first);
        QCOMPARE(readFile(path), original);
        const QByteArray tagged = jpeg ? exifBlob(output.data()) : output.data();
        QVERIFY(tagged.size() > oldTiff.size());
        QCOMPARE(tagged.mid(8, oldTiff.size() - 8), oldTiff.mid(8));
        QCOMPARE(tagged.left(4), oldTiff.left(4));
        if (jpeg) QCOMPARE(withoutExif(output.data()), withoutExif(original));
        const quint32 root = readDword(tagged, 4, le);
        QCOMPARE(tagEntry(tagged, root, 274), tagEntry(oldTiff, readDword(oldTiff, 4, le), 274));
        const quint32 gps = childIfd(tagged, 0x8825); QVERIFY(gps > 0);
        QCOMPARE(tagEntry(tagged, gps, 1).mid(8, 2), QByteArray("S\0", 2));
        QCOMPARE(tagEntry(tagged, gps, 3).mid(8, 2), QByteArray("E\0", 2));
        QCOMPARE(quint8(tagEntry(tagged, gps, 5)[8]), quint8(1));
        QVERIFY(std::abs(coordinate(tagged, 2) - std::abs(wanted.latitude)) < 1e-9);
        QVERIFY(std::abs(coordinate(tagged, 4) - wanted.longitude) < 1e-9);
        const int altPos = int(readDword(tagEntry(tagged, gps, 6), 8, le));
        QVERIFY(std::abs(rational(tagged, altPos) - std::abs(wanted.altitude)) < 1e-6);
        const QString roundtrip = dir.filePath("tagged"); QVERIFY(saveFile(roundtrip, output.data()));
        const auto inspected = GeoRefExif::Inspect(roundtrip);
        QVERIFY2(inspected.success, qPrintable(inspected.error)); QVERIFY(inspected.hasCoordinates);
        QCOMPARE(inspected.photoTime, before.photoTime);
        QVERIFY(std::abs(inspected.coordinates.altitude - wanted.altitude) < 1e-6);
        QBuffer second; QVERIFY(second.open(QIODevice::WriteOnly));
        const auto replaced = GeoRefExif::Write(roundtrip, &second, {0, -180, 0});
        QVERIFY2(replaced.success, qPrintable(replaced.error));
        const QByteArray secondTiff = jpeg ? exifBlob(second.data()) : second.data();
        QCOMPARE(coordinate(secondTiff, 2), 0.0); QCOMPARE(coordinate(secondTiff, 4), 180.0);
        QCOMPARE(tagEntry(secondTiff, childIfd(secondTiff, 0x8825), 3).mid(8, 2), QByteArray("W\0", 2));
        QCOMPARE(secondTiff.mid(8, tagged.size() - 8), tagged.mid(8));
    }
    void missingDatesAndFallback()
    {
        QTemporaryDir dir;
        const QString p = dir.filePath("input");
        QVERIFY(saveFile(p, tiffFixture(true, "not a valid date text", "2020:07:08 09:10:11")));
        auto metadata = GeoRefExif::Inspect(p);
        QVERIFY2(metadata.success, qPrintable(metadata.error));
        QCOMPARE(metadata.photoTime.date(), QDate(2020, 7, 8));
        QVERIFY(saveFile(p, tiffFixture(true, {}, {})));
        metadata = GeoRefExif::Inspect(p); QVERIFY(metadata.success); QVERIFY(!metadata.photoTime.isValid());
        const QByteArray noExif = jpegFixture(); QVERIFY(saveFile(p, noExif));
        metadata = GeoRefExif::Inspect(p); QVERIFY(metadata.success); QVERIFY(!metadata.photoTime.isValid());
        QBuffer output; output.open(QIODevice::WriteOnly);
        const auto result = GeoRefExif::Write(p, &output, {90, 180, 100});
        QVERIFY2(result.success, qPrintable(result.error));
        QVERIFY(!exifBlob(output.data()).isEmpty());
        QCOMPARE(withoutExif(output.data()), noExif);
        QCOMPARE(coordinate(exifBlob(output.data()), 2), 90.0);
    }
    void malformed_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("not-image") << QByteArrayLiteral("not an image");
        QTest::newRow("bigtiff") << QByteArray::fromHex("49492b00080000000000000000000000");
        QTest::newRow("missing-root") << QByteArray::fromHex("49492a0000000000");
        QTest::newRow("outside-root") << QByteArray::fromHex("49492a00ffffffffff0000000000");
        QTest::newRow("cycle") << QByteArray::fromHex("49492a0008000000000008000000");
        QTest::newRow("truncated-directory") << QByteArray::fromHex("49492a0008000000020000000000");
        QByteArray invalid = tiffFixture();
        const int root = int(readDword(invalid, 4, true));
        invalid.replace(root + 2 + 5 * 12 + 8, 4, encodedLong(0xfffffff0U, true));
        QTest::newRow("strip-outside") << invalid;
        invalid = tiffFixture(); const int exif = int(childIfd(invalid, 0x8769));
        invalid.replace(exif + 2 + 8, 4, encodedLong(0xffffff00U, true));
        QTest::newRow("date-outside") << invalid;
        invalid = tiffFixture(); invalid.replace(exif + 2 + 4, 4, encodedLong(0xffffffffU, true));
        QTest::newRow("huge-count") << invalid;
        invalid = tiffFixture(); invalid.replace(exif + 2 + 2, 2, QByteArray::fromHex("1000"));
        QTest::newRow("unsupported-type") << invalid;
        QTest::newRow("truncated-jpeg") << jpegFixture(tiffFixture()).chopped(1);
        QTest::newRow("bad-segment") << QByteArray::fromHex("ffd8ffe10001ffd9");
        invalid = jpegFixture(tiffFixture()); invalid.insert(2, segment(0xe1, QByteArray("Exif\0\0", 6) + tiffFixture()));
        QTest::newRow("ambiguous-exif") << invalid;
    }
    void malformed()
    {
        QFETCH(QByteArray, bytes); QTemporaryDir dir;
        const QString path = dir.filePath("input"); QVERIFY(saveFile(path, bytes));
        const auto metadata = GeoRefExif::Inspect(path); QVERIFY(!metadata.success); QVERIFY(!metadata.error.isEmpty());
        QBuffer output; output.open(QIODevice::WriteOnly);
        const auto result = GeoRefExif::Write(path, &output, {1, 2, 3});
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QCOMPARE(result.bytesWritten, qint64(0));
        QVERIFY(output.data().isEmpty()); QCOMPARE(readFile(path), bytes);
    }
    void invalidCoordinatesAndDevices()
    {
        QTemporaryDir dir; const QString path = dir.filePath("input"); QVERIFY(saveFile(path, tiffFixture()));
        const QVector<GeoRefExif::Coordinates> invalid = {
            {91, 0, 0}, {0, -181, 0}, {0, 0, 4294967296.0},
            {std::numeric_limits<double>::quiet_NaN(), 0, 0},
            {0, std::numeric_limits<double>::infinity(), 0}, {0, 0, -std::numeric_limits<double>::infinity()}
        };
        for (const auto &coords : invalid) {
            QBuffer out; out.open(QIODevice::WriteOnly);
            const auto result = GeoRefExif::Write(path, &out, coords);
            QVERIFY(!result.success); QVERIFY(out.data().isEmpty());
        }
        QVERIFY(!GeoRefExif::Write(path, nullptr, {}).success);
        QBuffer closed; QVERIFY(!GeoRefExif::Write(path, &closed, {}).success);
        QBuffer shifted; shifted.open(QIODevice::WriteOnly); shifted.write("old");
        QVERIFY(!GeoRefExif::Write(path, &shifted, {}).success); QCOMPARE(shifted.data(), QByteArrayLiteral("old"));
        QVERIFY(shifted.seek(0));
        QVERIFY(!GeoRefExif::Write(path, &shifted, {}).success); QCOMPARE(shifted.data(), QByteArrayLiteral("old"));
        QFile same(path); QVERIFY(same.open(QIODevice::ReadWrite));
        QVERIFY(!GeoRefExif::Write(path, &same, {}).success); QCOMPARE(readFile(path), tiffFixture());
        ShortDevice failing; const auto failed = GeoRefExif::Write(path, &failing, {1, 2, 3});
        QVERIFY(!failed.success); QVERIFY(failed.error.contains("disk full"));
        QCOMPARE(failed.bytesWritten, qint64(failing.bytes.size())); QVERIFY(failed.bytesWritten > 0);
    }
    void cancellationAndCallbackLifetime()
    {
        QTemporaryDir dir; const QString path = dir.filePath("input"); QVERIFY(saveFile(path, jpegFixture(tiffFixture())));
        const auto metadata = GeoRefExif::Inspect(path, [] { return true; }); QVERIFY(!metadata.success);
        QBuffer zero; zero.open(QIODevice::WriteOnly);
        const auto early = GeoRefExif::Write(path, &zero, {}, [] { return true; });
        QVERIFY(early.cancelled); QVERIFY(zero.data().isEmpty());
        bool cancel = false; QBuffer partial; partial.open(QIODevice::WriteOnly);
        const auto mid = GeoRefExif::Write(path, &partial, {}, [&] { return cancel; }, [&](qint64 n, qint64) { if (n) cancel = true; });
        QVERIFY(mid.cancelled); QVERIFY(!mid.success); QCOMPARE(mid.bytesWritten, qint64(partial.data().size()));
        QPointer<QBuffer> removed = new QBuffer; removed->open(QIODevice::WriteOnly);
        const auto gone = GeoRefExif::Write(path, removed, {}, {}, [&](qint64 n, qint64) { if (n && removed) delete removed.data(); });
        QVERIFY(!gone.success); QVERIFY(removed.isNull()); QVERIFY(gone.bytesWritten > 0);
        QBuffer changed; changed.open(QIODevice::WriteOnly);
        const auto mutated = GeoRefExif::Write(path, &changed, {}, {}, [&](qint64 n, qint64) {
            if (!n) { QFile f(path); if (f.open(QIODevice::Append)) f.write("changed"); }
        });
        QVERIFY(!mutated.success); QVERIFY(mutated.error.contains("changed")); QVERIFY(changed.data().isEmpty());
    }
    void destinationMutation_data()
    {
        QTest::addColumn<int>("mutation");
        QTest::newRow("seek-after-write") << 0;
        QTest::newRow("append-before-write") << 1;
        QTest::newRow("truncate-after-write") << 2;
        QTest::newRow("cancel-observer-seek") << 3;
    }
    void destinationMutation()
    {
        QFETCH(int, mutation); QTemporaryDir dir; const QString path = dir.filePath("input");
        const QByteArray original = tiffFixture(); QVERIFY(saveFile(path, original));
        QBuffer output; output.open(QIODevice::WriteOnly);
        bool once = false;
        const auto result = GeoRefExif::Write(path, &output, {}, [&] {
            if (mutation == 3 && output.pos() && !once) { once = true; output.seek(0); }
            return false;
        }, [&](qint64 n, qint64) {
            if (once || mutation == 3) return;
            if (mutation == 1 && !n) { once = true; output.write("foreign"); }
            if (mutation == 0 && n) { once = true; output.seek(0); }
            if (mutation == 2 && n) { once = true; output.buffer().clear(); }
        });
        QVERIFY(once); QVERIFY(!result.success); QVERIFY(result.error.contains("Destination"));
        QCOMPARE(readFile(path), original);
    }
    void hardlinkedDestinationCannotOverwriteSource()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("source"), alias = dir.filePath("alias");
        const QByteArray original = tiffFixture(); QVERIFY(saveFile(source, original));
#ifdef Q_OS_UNIX
        if (::link(QFile::encodeName(source).constData(), QFile::encodeName(alias).constData()) != 0)
            QSKIP("Filesystem does not support a hardlink fixture.");
#elif defined(Q_OS_WIN)
        if (!::CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(source.utf16()), nullptr))
            QSKIP("Filesystem does not support a hardlink fixture.");
#else
        QSKIP("Native hardlink fixture unavailable on this platform.");
#endif
        QFile output(alias); QVERIFY(output.open(QIODevice::ReadWrite));
        const auto result = GeoRefExif::Write(source, &output, {1, 2, 3});
        QVERIFY(!result.success); QCOMPARE(result.bytesWritten, qint64(0));
        QCOMPARE(readFile(source), original); QCOMPARE(readFile(alias), original);
    }
    void jpegCapacityAndClassicOffsetLimit()
    {
        QTemporaryDir dir; const QString path = dir.filePath("input");
        QByteArray large = tiffFixture(); large += QByteArray(65520 - large.size(), char(0));
        QVERIFY(saveFile(path, jpegFixture(large)));
        QBuffer output; output.open(QIODevice::WriteOnly);
        const auto capacity = GeoRefExif::Write(path, &output, {});
        QVERIFY(!capacity.success); QVERIFY(capacity.error.contains("65533")); QVERIFY(output.data().isEmpty());
#ifndef Q_OS_UNIX
        QSKIP("Large sparse-file fixture is restricted to POSIX filesystems.");
#endif
        QFile sparse(path); QVERIFY(sparse.open(QIODevice::ReadWrite | QIODevice::Truncate));
        const QByteArray empty = QByteArray::fromHex("49492a0008000000000000000000");
        QCOMPARE(sparse.write(empty), qint64(empty.size()));
        if (!sparse.resize(0xffffffffLL)) QSKIP("Filesystem cannot create a sparse classic-TIFF boundary fixture.");
        sparse.close();
        const auto overflow = GeoRefExif::Write(path, &output, {});
        QVERIFY(!overflow.success); QVERIFY(overflow.error.contains("offset space")); QVERIFY(output.data().isEmpty());
    }
    void symlinkRejected()
    {
        QTemporaryDir dir; const QString source = dir.filePath("source"); QVERIFY(saveFile(source, tiffFixture()));
        const QString link = dir.filePath("link");
        if (!QFile::link(source, link) || !QFileInfo(link).isSymLink()) QSKIP("Native symbolic links unavailable.");
        QVERIFY(!GeoRefExif::Inspect(link).success);
        QBuffer out; out.open(QIODevice::WriteOnly); QVERIFY(!GeoRefExif::Write(link, &out, {}).success);
        QVERIFY(out.data().isEmpty());
    }
    void unrelatedGpsTagsAndScanBoundary()
    {
        QTemporaryDir dir; const QString path = dir.filePath("input");
        QVERIFY(saveFile(path, tiffFixture()));
        QBuffer first; first.open(QIODevice::WriteOnly);
        QVERIFY(GeoRefExif::Write(path, &first, {1, 2, 3}).success);
        QByteArray tagged = first.data();
        const quint32 oldGps = childIfd(tagged, 0x8825);
        QVector<QByteArray> entries;
        for (int i = 0; i < 7; ++i) entries += tagEntry(tagged, oldGps, i);
        const QByteArray speedReference = field(12, 2, 2, QByteArray("K\0", 2), true);
        entries += speedReference;
        const quint32 gps = addDirectory(tagged, entries, true);
        const int root = int(readDword(tagged, 4, true));
        const int n = readWord(tagged, root, true);
        for (int i = 0; i < n; ++i)
            if (readWord(tagged, root + 2 + i * 12, true) == 0x8825)
                tagged.replace(root + 2 + i * 12 + 8, 4, encodedLong(gps, true));
        QVERIFY(saveFile(path, tagged));
        QBuffer second; second.open(QIODevice::WriteOnly);
        const auto result = GeoRefExif::Write(path, &second, {-2, -3, -4});
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(tagEntry(second.data(), childIfd(second.data(), 0x8825), 12), speedReference);

        QByteArray jpeg = jpegFixture();
        const int scan = jpeg.indexOf(QByteArray::fromHex("ffda")) + 10;
        jpeg.insert(scan, QByteArray(65535, char(0x44)) + QByteArray::fromHex("ff00ffffd044ff0144"));
        QVERIFY(saveFile(path, jpeg));
        QBuffer out; out.open(QIODevice::WriteOnly);
        QVERIFY2(GeoRefExif::Write(path, &out, {}).success, "Chunk-spanning entropy stuffing must be preserved.");
        QCOMPARE(withoutExif(out.data()), jpeg);
    }
    void invalidExistingGpsKeepsDate_data()
    {
        QTest::addColumn<int>("damage");
        QTest::newRow("latitude-zero-denominator") << 0;
        QTest::newRow("invalid-hemisphere") << 1;
        QTest::newRow("minutes-out-of-range") << 2;
        QTest::newRow("altitude-ref-out-of-range") << 3;
        QTest::newRow("wrong-latitude-type") << 4;
        QTest::newRow("wrong-latitude-count") << 5;
        QTest::newRow("altitude-zero-denominator") << 6;
        QTest::newRow("longitude-out-of-range") << 7;
    }
    void invalidExistingGpsKeepsDate()
    {
        QFETCH(int, damage);
        QTemporaryDir dir; const QString path = dir.filePath("input");
        QVERIFY(saveFile(path, tiffFixture()));
        QBuffer initial; initial.open(QIODevice::WriteOnly);
        QVERIFY(GeoRefExif::Write(path, &initial, {35, 149, 100}).success);
        QByteArray damaged = initial.data();
        const quint32 gps = childIfd(damaged, 0x8825);
        const auto location = [&](int tag) {
            const int n = readWord(damaged, int(gps), true);
            for (int i = 0; i < n; ++i) {
                const int pos = int(gps) + 2 + i * 12;
                if (readWord(damaged, pos, true) == tag) return pos;
            }
            return -1;
        };
        const int lat = int(readDword(damaged, location(2) + 8, true));
        const int lon = int(readDword(damaged, location(4) + 8, true));
        const int alt = int(readDword(damaged, location(6) + 8, true));
        switch (damage) {
        case 0: damaged.replace(lat + 4, 4, encodedLong(0, true)); break;
        case 1: damaged[location(1) + 8] = 'X'; break;
        case 2: damaged.replace(lat + 8, 4, encodedLong(61, true)); break;
        case 3: damaged[location(5) + 8] = char(2); break;
        case 4: damaged.replace(location(2) + 2, 2, QByteArray::fromHex("0300")); break;
        case 5: damaged.replace(location(2) + 4, 4, encodedLong(1, true)); break;
        case 6: damaged.replace(alt + 4, 4, encodedLong(0, true)); break;
        case 7: damaged.replace(lon, 4, encodedLong(181, true)); break;
        }
        QVERIFY(saveFile(path, damaged));
        const auto metadata = GeoRefExif::Inspect(path);
        QVERIFY2(metadata.success, qPrintable(metadata.error));
        QCOMPARE(metadata.photoTime.date(), QDate(2024, 2, 29));
        QCOMPARE(metadata.photoTime.time(), QTime(12, 34, 56));
        QVERIFY(!metadata.hasCoordinates); QVERIFY(!metadata.coordinateWarning.isEmpty());
        QBuffer repaired; repaired.open(QIODevice::WriteOnly);
        const auto result = GeoRefExif::Write(path, &repaired, {-25, 120, -10});
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(coordinate(repaired.data(), 2), 25.0);
        QCOMPARE(coordinate(repaired.data(), 4), 120.0);
        QCOMPARE(readFile(path), damaged);
        QVERIFY(saveFile(dir.filePath("repaired"), repaired.data()));
        const auto readBack = GeoRefExif::Inspect(dir.filePath("repaired"));
        QVERIFY(readBack.success); QVERIFY(readBack.hasCoordinates); QVERIFY(readBack.coordinateWarning.isEmpty());
        QCOMPARE(readBack.photoTime, metadata.photoTime);
    }
    void siblingIfdsAreNotNesting()
    {
        QTemporaryDir dir; const QString path = dir.filePath("input");
        QByteArray bytes = tiffFixture();
        const int root = int(readDword(bytes, 4, true));
        const int nextField = root + 2 + readWord(bytes, root, true) * 12;
        quint32 next = readDword(bytes, nextField, true);
        for (int i = 0; i < 32; ++i) next = addDirectory(bytes, {}, true, next);
        bytes.replace(nextField, 4, encodedLong(next, true));
        QVERIFY(saveFile(path, bytes));
        const auto metadata = GeoRefExif::Inspect(path);
        QVERIFY2(metadata.success, qPrintable(metadata.error));
        QCOMPARE(metadata.photoTime.date(), QDate(2024, 2, 29));
        QBuffer output; output.open(QIODevice::WriteOnly);
        const auto written = GeoRefExif::Write(path, &output, {});
        QVERIFY2(written.success, qPrintable(written.error));
        QCOMPARE(output.data().mid(8, bytes.size() - 8), bytes.mid(8));

        QByteArray nested = QByteArray::fromHex("49492a0000000000");
        next = addDirectory(nested, {}, true);
        for (int i = 0; i < 18; ++i) next = addDirectory(nested, {longField(330, next, true)}, true);
        nested.replace(4, 4, encodedLong(next, true));
        QVERIFY(saveFile(path, nested));
        const auto tooDeep = GeoRefExif::Inspect(path);
        QVERIFY(!tooDeep.success); QVERIFY(tooDeep.error.contains("nesting"));

        QByteArray many = QByteArray::fromHex("49492a0000000000");
        next = 0;
        for (int i = 0; i < 257; ++i) next = addDirectory(many, {}, true, next);
        many.replace(4, 4, encodedLong(next, true));
        QVERIFY(saveFile(path, many));
        const auto tooMany = GeoRefExif::Inspect(path);
        QVERIFY(!tooMany.success); QVERIFY(tooMany.error.contains("256"));
    }
};

QTEST_GUILESS_MAIN(TestGeoRefExif)
#include "test_georefexif.moc"
