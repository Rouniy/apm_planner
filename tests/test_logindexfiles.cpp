#include "ui/Loghandling/LogIndexFiles.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

namespace {
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool date(const QString &path, const QDateTime &time) {
    QFile file(path); return file.open(QIODevice::ReadWrite) && file.setFileTime(time, QFileDevice::FileModificationTime);
}
LogIndex::Entry entry(const QString &path) {
    const auto found = LogIndexFiles::discover(QFileInfo(path).absolutePath());
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (const auto &item : found.files) if (item.fullPath == canonical) return item;
    return {};
}
QVector<LogIndex::Point> route() { return {{35, 33}, {35.005, 33.005}, {35.01, 33.01}}; }
QByteArray solidImage(int width, int height, const char *format = "PNG") {
    QImage image(width, height, QImage::Format_RGB32); image.fill(QColor(20, 40, 180));
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, format, 86); return bytes;
}
}

class LogIndexFilesTest final : public QObject {
    Q_OBJECT
private slots:
    void recursiveDiscoveryAndExactCompanions();
    void linkedRootsAndDescendants();
    void discoveryCancellationAndDepthLimit();
    void unchangedChecksSourceOnly();
    void fallbackAndRouteJpeg();
    void cacheIsInjectedBoundedAndReadOnly();
    void freshSidecarReuseAndNormalization();
    void invalidFreshSidecarNeverOverwritten_data();
    void invalidFreshSidecarNeverOverwritten();
    void staleSidecarReplacedWithSourceTimestamp();
    void thumbnailCallbackMutationAndCancellation_data();
    void thumbnailCallbackMutationAndCancellation();
    void linkedSidecarPreserved();
    void exactDeleteAndImmutablePlan();
    void preparationRefusesChangedOrLateCompanions_data();
    void preparationRefusesChangedOrLateCompanions();
    void executionContinuesAfterChangedSource();
    void cancellationKeepsPartialReceipts();
    void companionMutationAfterSourceDeletion();
    void linkedAncestorAndRootSwapAfterConsent();
    void ordinaryRootBelowAliasIsCanonicalAndPinned();
};

void LogIndexFilesTest::recursiveDiscoveryAndExactCompanions()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir(dir.path()).mkdir("nested"));
    for (const QString &name : {"one.TLOG", "one.rlog", "one.TLOG.jpg", "one.jpg", "other.rlog", "nested/two.Bin", "three.log", "notes.txt"})
        QVERIFY(write(dir.filePath(name), name.toUtf8()));
    const auto found = LogIndexFiles::discover(dir.path());
    QVERIFY2(found.success, qPrintable(found.error)); QCOMPARE(found.files.size(), 3);
    QCOMPARE(found.rootPath, QFileInfo(dir.path()).canonicalFilePath());
    const auto tlog = entry(dir.filePath("one.TLOG"));
    QVERIFY(tlog.source.exists); QVERIFY(tlog.thumbnail.exists); QVERIFY(tlog.pairedRlog.exists);
    QCOMPARE(tlog.pairedRlog.sizeBytes, qint64(8));
    QCOMPARE(tlog.thumbnail.sizeBytes, qint64(12));
    QVERIFY(LogIndexFiles::unchanged(tlog));
}

void LogIndexFilesTest::linkedRootsAndDescendants()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates shortcuts rather than the Unix symlinks needed by this fixture.");
#else
    QTemporaryDir dir, outside;
    QVERIFY(write(outside.filePath("external.bin"), "outside"));
    QVERIFY(QFile::link(outside.path(), dir.filePath("linked")));
    QVERIFY(QFile::link(outside.filePath("external.bin"), dir.filePath("alias.log")));
    QVERIFY(write(dir.filePath("safe.log"), "safe"));
    auto found = LogIndexFiles::discover(dir.path()); QVERIFY(found.success); QCOMPARE(found.files.size(), 1);
    found = LogIndexFiles::discover(dir.filePath("linked")); QVERIFY(!found.success); QVERIFY(!found.error.isEmpty());
    auto selected = entry(dir.filePath("safe.log"));
    selected.fullPath = outside.filePath("external.bin");
    QVERIFY(!LogIndexFiles::unchanged(selected));
#endif
}

void LogIndexFilesTest::discoveryCancellationAndDepthLimit()
{
    QTemporaryDir dir; QVERIFY(write(dir.filePath("one.bin"), "x"));
    int calls = 0;
    auto found = LogIndexFiles::discover(dir.path(), [&] { return ++calls == 3; });
    QVERIFY(found.cancelled); QVERIFY(!found.success);
    QString path = dir.path();
    for (int i = 0; i < 65; ++i) { path += "/d"; QVERIFY(QDir().mkdir(path)); }
    found = LogIndexFiles::discover(dir.path()); QVERIFY(!found.success); QVERIFY(found.error.contains("64"));
    QVERIFY(!LogIndexFiles::discover(dir.filePath("missing")).success);
}

void LogIndexFilesTest::unchangedChecksSourceOnly()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.tlog"); QVERIFY(write(path, "source"));
    const auto selected = entry(path); QVERIFY(LogIndexFiles::unchanged(selected));
    QVERIFY(write(path + ".jpg", "new thumbnail")); QVERIFY(LogIndexFiles::unchanged(selected));
    QVERIFY(write(path, "changed source")); QString error;
    QVERIFY(!LogIndexFiles::unchanged(selected, &error)); QVERIFY(error.contains("changed"));
}

void LogIndexFilesTest::fallbackAndRouteJpeg()
{
    QVERIFY(QImageReader::supportedImageFormats().contains("jpeg"));
    QTemporaryDir dir; const QString path = dir.filePath("flight.log"); QVERIFY(write(path, "source"));
    auto selected = entry(path);
    auto result = LogIndexFiles::thumbnail(selected, {});
    QVERIFY2(!result.jpeg.isEmpty(), qPrintable(result.warning)); QVERIFY(result.jpeg.startsWith(QByteArray::fromHex("ffd8")));
    QImage image = QImage::fromData(result.jpeg); QCOMPARE(image.size(), QSize(240, 140));
    QCOMPARE(read(path + ".jpg"), result.jpeg); QVERIFY(result.sidecar.exists);
    QCOMPARE(result.sidecar.modifiedUtc, selected.source.modifiedUtc);
    const QColor grid = image.pixelColor(10, 100); QVERIFY(qAbs(grid.red() - 36) < 20);
    QVERIFY(QFile::remove(path + ".jpg"));
    result = LogIndexFiles::thumbnail(selected, route()); image = QImage::fromData(result.jpeg);
    const QColor center = image.pixelColor(120, 70); QVERIFY(center.red() > 170); QVERIFY(center.green() < 110);
    int green = 0, white = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto c = image.pixelColor(x, y);
        green += c.green() > 130 && c.red() < 130 && c.blue() < 130;
        white += c.red() > 160 && c.green() > 160 && c.blue() > 160;
    }
    QVERIFY(green > 10); QVERIFY(white > 10);
}

void LogIndexFilesTest::cacheIsInjectedBoundedAndReadOnly()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.bin"); QVERIFY(write(path, "source"));
    int calls = 0; bool valid = true;
    const auto result = LogIndexFiles::thumbnail(entry(path), route(), [&](int x, int y, int z) {
        ++calls; valid &= z >= 1 && z <= 16 && x >= 0 && y >= 0 && x < (1 << z) && y < (1 << z);
        return solidImage(256, 256);
    });
    QVERIFY(!result.jpeg.isEmpty()); QVERIFY(calls > 0 && calls <= 12); QVERIFY(valid);
    QVERIFY(QImage::fromData(result.jpeg).pixelColor(120, 120).blue() > 100);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2); // Log and exact JPEG only.
}

void LogIndexFilesTest::freshSidecarReuseAndNormalization()
{
    QTemporaryDir dir; const QString path = dir.filePath("same.name.tlog"); QVERIFY(write(path, "source"));
    QVERIFY(date(path, QDateTime::currentDateTimeUtc().addSecs(-120)));
    const QByteArray original = solidImage(240, 140, "JPEG"); QVERIFY(write(path + ".jpg", original));
    int calls = 0;
    auto result = LogIndexFiles::thumbnail(entry(path), route(), [&](int, int, int) { ++calls; return QByteArray(); });
    QCOMPARE(result.jpeg, original); QCOMPARE(calls, 0); QCOMPARE(read(path + ".jpg"), original);
    QVERIFY(!QFileInfo(dir.filePath("same.name.jpg")).exists());
    const QByteArray larger = solidImage(1024, 768); QVERIFY(write(path + ".jpg", larger));
    result = LogIndexFiles::thumbnail(entry(path), {});
    QCOMPARE(QImage::fromData(result.jpeg).size(), QSize(240, 140));
    QVERIFY(result.jpeg.startsWith(QByteArray::fromHex("ffd8"))); QCOMPARE(read(path + ".jpg"), larger);
    QCOMPARE(result.sidecar.sizeBytes, qint64(larger.size()));
}

void LogIndexFilesTest::invalidFreshSidecarNeverOverwritten_data()
{
    QTest::addColumn<int>("kind"); QTest::newRow("invalid") << 0;
    QTest::newRow("pixel-bound") << 1; QTest::newRow("compressed-bound") << 2;
}
void LogIndexFilesTest::invalidFreshSidecarNeverOverwritten()
{
    QFETCH(int, kind); QTemporaryDir dir; const QString path = dir.filePath("flight.log");
    QVERIFY(write(path, "source")); QVERIFY(date(path, QDateTime::currentDateTimeUtc().addSecs(-120)));
    const QByteArray bytes = kind == 1 ? solidImage(4097, 1) : QByteArray("invalid fresh image");
    QVERIFY(write(path + ".jpg", bytes));
    if (kind == 2) { QFile file(path + ".jpg"); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(20 * 1024 * 1024 + 1)); }
    const auto before = entry(path).thumbnail;
    const auto result = LogIndexFiles::thumbnail(entry(path), {});
    QCOMPARE(QImage::fromData(result.jpeg).size(), QSize(240, 140)); QVERIFY(!result.warning.isEmpty());
    QCOMPARE(entry(path).thumbnail.sizeBytes, before.sizeBytes); QCOMPARE(result.sidecar.sizeBytes, before.sizeBytes);
    QFile file(path + ".jpg"); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.read(bytes.size()), bytes);
}

void LogIndexFilesTest::staleSidecarReplacedWithSourceTimestamp()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.log");
    QVERIFY(write(path, "source")); QVERIFY(write(path + ".jpg", "invalid stale"));
    QVERIFY(date(path + ".jpg", QDateTime::currentDateTimeUtc().addSecs(-3600)));
    const auto selected = entry(path); const auto result = LogIndexFiles::thumbnail(selected, {});
    QVERIFY2(!result.jpeg.isEmpty(), qPrintable(result.warning)); QCOMPARE(read(path + ".jpg"), result.jpeg);
    QCOMPARE(result.sidecar.modifiedUtc, selected.source.modifiedUtc);
}

void LogIndexFilesTest::thumbnailCallbackMutationAndCancellation_data()
{
    QTest::addColumn<int>("kind"); QTest::newRow("source-change-at-final-gate") << 0;
    QTest::newRow("sidecar-appears-at-final-gate") << 1; QTest::newRow("cancel-final-gate") << 2;
    QTest::newRow("caller-entry-retarget") << 3;
}
void LogIndexFilesTest::thumbnailCallbackMutationAndCancellation()
{
    QFETCH(int, kind); QTemporaryDir dir; const QString path = dir.filePath("flight.log");
    QVERIFY(write(path, "source")); auto selected = entry(path); int calls = 0;
    const auto result = LogIndexFiles::thumbnail(selected, {}, {}, [&] {
        ++calls;
        if (kind == 3 && calls == 1) { selected.fullPath = dir.filePath("other.log"); write(selected.fullPath, "other"); }
        if (calls != 4) return false;
        if (kind == 0) write(path, "changed source");
        if (kind == 1) write(path + ".jpg", "late companion");
        return kind == 2;
    });
    QVERIFY(calls >= 4);
    if (kind == 2) QVERIFY(result.cancelled);
    else if (kind != 3) QVERIFY(!result.warning.isEmpty());
    if (kind == 1) QCOMPARE(read(path + ".jpg"), QByteArray("late companion"));
    else if (kind == 3) { QVERIFY(QFileInfo(path + ".jpg").exists()); QVERIFY(!QFileInfo(selected.fullPath + ".jpg").exists()); }
    else QVERIFY(!QFileInfo(path + ".jpg").exists());
}

void LogIndexFilesTest::linkedSidecarPreserved()
{
#ifdef Q_OS_WIN
    QSKIP("Unix symbolic-link fixture.");
#else
    QTemporaryDir dir, outside; const QString path = dir.filePath("flight.log");
    QVERIFY(write(path, "source")); QVERIFY(write(outside.filePath("image"), "valuable"));
    QVERIFY(QFile::link(outside.filePath("image"), path + ".jpg"));
    const auto selected = entry(path); const auto result = LogIndexFiles::thumbnail(selected, {});
    QVERIFY(!result.jpeg.isEmpty()); QVERIFY(!result.warning.isEmpty()); QVERIFY(QFileInfo(path + ".jpg").isSymLink());
    QCOMPARE(read(outside.filePath("image")), QByteArray("valuable"));
    QVERIFY(!LogIndexFiles::prepareDelete(dir.path(), {selected}).success); QVERIFY(QFileInfo(path).exists());
#endif
}

void LogIndexFilesTest::exactDeleteAndImmutablePlan()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.tlog");
    for (const auto &name : {"flight.tlog", "flight.tlog.jpg", "flight.rlog", "flight.log", "flight.jpg", "flight.tlog.extra"})
        QVERIFY(write(dir.filePath(name), name));
    auto prepared = LogIndexFiles::prepareDelete(dir.path(), {entry(path)});
    QVERIFY2(prepared.success, qPrintable(prepared.error)); QCOMPARE(prepared.plan.paths().size(), 3);
    const auto result = LogIndexFiles::executeDelete(prepared.plan, {}, [&](int, int, const QString &) { prepared.plan = {}; });
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.deletedLogs, QStringList{path});
    QCOMPARE(result.deletedPaths.size(), 3); QCOMPARE(result.remaining, 0);
    QVERIFY(!QFileInfo(path).exists()); QVERIFY(!QFileInfo(path + ".jpg").exists()); QVERIFY(!QFileInfo(dir.filePath("flight.rlog")).exists());
    QVERIFY(QFileInfo(dir.filePath("flight.log")).exists()); QVERIFY(QFileInfo(dir.filePath("flight.jpg")).exists());
    QVERIFY(QFileInfo(dir.filePath("flight.tlog.extra")).exists());
}

void LogIndexFilesTest::preparationRefusesChangedOrLateCompanions_data()
{
    QTest::addColumn<QString>("relative");
    QTest::newRow("source") << QStringLiteral("flight.tlog"); QTest::newRow("late-rlog") << QStringLiteral("flight.rlog");
    QTest::newRow("late-jpeg") << QStringLiteral("flight.tlog.jpg");
}
void LogIndexFilesTest::preparationRefusesChangedOrLateCompanions()
{
    QFETCH(QString, relative); QTemporaryDir dir; const QString path = dir.filePath("flight.tlog");
    QVERIFY(write(path, "source")); const auto selected = entry(path);
    QVERIFY(write(dir.filePath(relative), "changed or newly created"));
    const auto result = LogIndexFiles::prepareDelete(dir.path(), {selected});
    QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(QFileInfo(path).exists());
}

void LogIndexFilesTest::executionContinuesAfterChangedSource()
{
    QTemporaryDir dir; const QString a = dir.filePath("a.log"), b = dir.filePath("b.bin");
    QVERIFY(write(a, "source")); QVERIFY(write(b, "safe"));
    const auto prepared = LogIndexFiles::prepareDelete(dir.path(), {entry(a), entry(b)}); QVERIFY(prepared.success);
    const auto result = LogIndexFiles::executeDelete(prepared.plan, {}, [&](int completed, int, const QString &path) {
        if (completed == 0 && path == a) write(a, "changed by progress callback");
    });
    QVERIFY(!result.success); QCOMPARE(result.deletedLogs, QStringList{b}); QCOMPARE(result.remaining, 1);
    QVERIFY(!result.warnings.isEmpty()); QVERIFY(QFileInfo(a).exists()); QVERIFY(!QFileInfo(b).exists());
}

void LogIndexFilesTest::cancellationKeepsPartialReceipts()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.tlog");
    QVERIFY(write(path, "source")); QVERIFY(write(path + ".jpg", "thumbnail")); QVERIFY(write(dir.filePath("flight.rlog"), "paired"));
    const auto prepared = LogIndexFiles::prepareDelete(dir.path(), {entry(path)}); QVERIFY(prepared.success);
    int calls = 0; const auto result = LogIndexFiles::executeDelete(prepared.plan, [&] { return ++calls == 2; });
    QVERIFY(result.cancelled); QVERIFY(!result.success); QCOMPARE(result.deletedLogs, QStringList{path});
    QCOMPARE(result.deletedPaths, QStringList{path}); QCOMPARE(result.remaining, 0);
    QVERIFY(QFileInfo(path + ".jpg").exists()); QVERIFY(QFileInfo(dir.filePath("flight.rlog")).exists());
    QVERIFY(!result.warnings.isEmpty());
}

void LogIndexFilesTest::companionMutationAfterSourceDeletion()
{
    QTemporaryDir dir; const QString path = dir.filePath("flight.tlog");
    QVERIFY(write(path, "source")); QVERIFY(write(path + ".jpg", "thumbnail")); QVERIFY(write(dir.filePath("flight.rlog"), "paired"));
    const auto prepared = LogIndexFiles::prepareDelete(dir.path(), {entry(path)}); QVERIFY(prepared.success);
    int calls = 0; const auto result = LogIndexFiles::executeDelete(prepared.plan, [&] {
        if (++calls == 2) write(path + ".jpg", "changed after source removal"); return false;
    });
    QVERIFY(!result.success); QCOMPARE(result.deletedLogs, QStringList{path}); QCOMPARE(result.deletedPaths.size(), 2);
    QVERIFY(!result.warnings.isEmpty()); QCOMPARE(read(path + ".jpg"), QByteArray("changed after source removal"));
    QVERIFY(!QFileInfo(dir.filePath("flight.rlog")).exists());
}

void LogIndexFilesTest::linkedAncestorAndRootSwapAfterConsent()
{
#ifdef Q_OS_WIN
    QSKIP("Unix symbolic-link fixture.");
#else
    QTemporaryDir dir, outside;
    QVERIFY(QDir(dir.path()).mkdir("logs"));
    const QString root = dir.filePath("logs"), path = root + "/flight.log";
    QVERIFY(write(path, "selected")); QVERIFY(write(outside.filePath("flight.log"), "outside"));
    const auto prepared = LogIndexFiles::prepareDelete(root, {entry(path)}); QVERIFY(prepared.success);
    bool swapped = false;
    const auto result = LogIndexFiles::executeDelete(prepared.plan, {}, [&](int, int, const QString &current) {
        if (current.isEmpty() || swapped) return;
        swapped = true;
        QVERIFY(QDir().rename(root, dir.filePath("original")));
        QVERIFY(QFile::link(outside.path(), root));
    });
    QVERIFY(swapped); QVERIFY(!result.success); QVERIFY(result.deletedPaths.isEmpty());
    QVERIFY(!result.error.isEmpty()); QCOMPARE(read(outside.filePath("flight.log")), QByteArray("outside"));
    QCOMPARE(read(dir.filePath("original/flight.log")), QByteArray("selected"));
#endif
}

void LogIndexFilesTest::ordinaryRootBelowAliasIsCanonicalAndPinned()
{
#ifdef Q_OS_WIN
    QSKIP("Unix ancestor-alias fixture, including the macOS /var convention.");
#else
    QTemporaryDir dir, original, replacement;
    QVERIFY(QDir(original.path()).mkdir("logs")); QVERIFY(QDir(replacement.path()).mkdir("logs"));
    const QString originalFile = original.filePath("logs/flight.log");
    const QString replacementFile = replacement.filePath("logs/flight.log");
    QVERIFY(write(originalFile, "original log")); QVERIFY(write(replacementFile, "replacement log"));
    const QString alias = dir.filePath("alias"); QVERIFY(QFile::link(original.path(), alias));
    const QString requestedRoot = alias + "/logs";
    const auto discovery = LogIndexFiles::discover(requestedRoot);
    QVERIFY2(discovery.success, qPrintable(discovery.error)); QCOMPARE(discovery.files.size(), 1);
    QCOMPARE(discovery.rootPath, QFileInfo(original.filePath("logs")).canonicalFilePath());
    QCOMPARE(discovery.files.first().fullPath, QFileInfo(originalFile).canonicalFilePath());
    QCOMPARE(discovery.files.first().rootPath, discovery.rootPath);
    const QString expectedDeleted = discovery.files.first().fullPath;
    const auto prepared = LogIndexFiles::prepareDelete(requestedRoot, discovery.files); QVERIFY(prepared.success);
    QVERIFY(QFile::remove(alias)); QVERIFY(QFile::link(replacement.path(), alias));
    // Changing only the admission spelling cannot redirect an already reviewed
    // plan. The original canonical target, not the new alias target, is removed.
    const auto result = LogIndexFiles::executeDelete(prepared.plan);
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.deletedLogs, QStringList{expectedDeleted});
    QVERIFY(!QFileInfo(originalFile).exists()); QCOMPARE(read(replacementFile), QByteArray("replacement log"));
#endif
}

QTEST_MAIN(LogIndexFilesTest)
#include "test_logindexfiles.moc"
