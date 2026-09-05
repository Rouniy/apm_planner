#include <QtTest>
#include "ui/Loghandling/LogAnonymizer.h"
#include "ui/Loghandling/LogAnonymizeService.h"
#include <QFile>
#include <QTemporaryDir>
#include <limits>

namespace {
const QByteArray sample = "FMT, 42, 11, GPS, LL, Lat,Lng\r\nGPS,10,20\r\n";
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
}

class LogAnonymizerTest : public QObject
{
    Q_OBJECT
private slots:
    void offsetsAndTypes()
    {
        double value = 0;
        QVERIFY(LogAnonymizer::parseOffset(QStringLiteral(" -1.25 "), &value));
        QCOMPARE(value, -1.25);
        QVERIFY(!LogAnonymizer::parseOffset(QStringLiteral("1,250"), &value));
        QVERIFY(!LogAnonymizer::parseOffset(QStringLiteral("nan"), &value));
        QVERIFY(!LogAnonymizer::parseOffset(QStringLiteral("inf"), &value));
        QVERIFY(!LogAnonymizer::parseOffset(QStringLiteral("1e999"), &value));
        for (int i = 0; i != 100; ++i) {
            QVERIFY(LogAnonymizer::parseOffset({}, &value));
            QVERIFY(qAbs(value) >= 0.5 && qAbs(value) <= 2.0);
        }
        LogAnonymizer::Format format;
        QVERIFY(LogAnonymizer::formatForPath(QStringLiteral("A.TLOG"), &format));
        QCOMPARE(format, LogAnonymizer::Format::Telemetry);
        QVERIFY(!LogAnonymizer::formatForPath(QStringLiteral("log.csv"), &format));
    }
    void atomicPublicationAndInputGuard()
    {
        QTemporaryDir dir;
        const QString input = dir.filePath(QStringLiteral("полёт.log"));
        const QString output = dir.filePath(QStringLiteral("полёт-anon.log"));
        QVERIFY(writeFile(input, sample));
        QVERIFY(writeFile(output, "PREVIOUS"));
        auto result = LogAnonymizer::anonymizeFile(input, input, {1.25, -0.5});
        QVERIFY(!result.success);
        QCOMPARE(readFile(input), sample);
        const QString alias = dir.filePath(QStringLiteral("alias.log"));
        if (QFile::link(input, alias)) {
            result = LogAnonymizer::anonymizeFile(input, alias, {1.25, -0.5});
            QVERIFY(!result.success);
            QCOMPARE(readFile(input), sample);
        }
        result = LogAnonymizer::anonymizeFile(input, output, {1.25, -0.5});
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.patchedValues, qint64(2));
        QVERIFY(readFile(output).contains("11.25"));
        QVERIFY(readFile(output).contains("19.5"));
        QCOMPARE(readFile(input), sample);
        QVERIFY(!result.warnings.isEmpty());
        const QByteArray previous = readFile(output);
        QVERIFY(writeFile(input, "not a DataFlash log\n"));
        result = LogAnonymizer::anonymizeFile(input, output, {1, 2});
        QVERIFY(!result.success);
        QCOMPARE(readFile(output), previous);
    }
    void cancelBeforeCommitAndChangingInput()
    {
        QTemporaryDir dir;
        const QString input = dir.filePath(QStringLiteral("source.log"));
        const QString output = dir.filePath(QStringLiteral("output.log"));
        QVERIFY(writeFile(input, sample));
        QVERIFY(writeFile(output, "PREVIOUS"));
        bool cancel = false;
        auto result = LogAnonymizer::anonymizeFile(input, output, {1, 2},
            [&]() { return cancel; }, [&](qint64 done, qint64 total) {
                if (done == total) cancel = true;
            });
        QVERIFY(result.cancelled);
        QVERIFY(!result.success);
        QCOMPARE(readFile(output), QByteArray("PREVIOUS"));
        bool changed = false;
        result = LogAnonymizer::anonymizeFile(input, output, {1, 2}, {},
            [&](qint64 done, qint64 total) {
                if (!changed && done >= total / 3 && done < total * 2 / 3) {
                    changed = true;
                    QByteArray other = sample;
                    other.replace("10,20", "11,21");
                    QVERIFY(writeFile(input, other));
                }
            });
        QVERIFY(changed);
        QVERIFY(!result.success);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(readFile(output), QByteArray("PREVIOUS"));
    }
    void asynchronousLifecycleAndTokenIsolation()
    {
        QTemporaryDir dir;
        const QString input = dir.filePath(QStringLiteral("source.log"));
        const QString output = dir.filePath(QStringLiteral("output.log"));
        QVERIFY(writeFile(input, sample));
        LogAnonymizeService service;
        QVERIFY(service.start(input, output, {1, 2}));
        const quint64 first = service.token();
        QVERIFY(!service.start(input, output, {1, 2}));
        service.cancel(first + 1);
        QVERIFY(!service.cancellationRequested());
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
        QVERIFY2(service.result().success, qPrintable(service.result().error));
        const QByteArray previous = readFile(output);
        QByteArray large = sample.left(sample.indexOf("GPS,10,20"));
        for (int i = 0; i < 500000; ++i) large += "GPS,10,20\r\n";
        QVERIFY(writeFile(input, large));
        QVERIFY(service.start(input, output, {2, 3}));
        QVERIFY(service.token() > first);
        service.cancel(first);
        QVERIFY(!service.cancellationRequested());
        service.cancel(service.token());
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
        QVERIFY(service.result().cancelled);
        QCOMPARE(readFile(output), previous);
        QVERIFY(service.start(input, output, {2, 3}));
        service.shutdown();
        QVERIFY(!service.busy());
        QVERIFY(!service.start(input, output, {2, 3}));
        QCOMPARE(readFile(output), previous);
    }
    void missingOutputDirectoryAndRemovedSourceFailUnpublished()
    {
        QTemporaryDir dir;
        const QString input = dir.filePath(QStringLiteral("source.log"));
        const QString output = dir.filePath(QStringLiteral("output.log"));
        QVERIFY(writeFile(input, sample));
        auto result = LogAnonymizer::anonymizeFile(input,
            dir.filePath(QStringLiteral("missing/output.log")), {1, 2});
        QVERIFY(!result.success);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(readFile(input), sample);
        QVERIFY(writeFile(output, "PREVIOUS"));
        bool renamed = false;
        result = LogAnonymizer::anonymizeFile(input, output, {1, 2}, {},
            [&](qint64 done, qint64 total) {
                if (!renamed && done >= total * 2 / 3) {
                    renamed = QFile::rename(input, dir.filePath(QStringLiteral("moved.log")));
                }
            });
        // Some platforms prevent renaming an open file; that itself preserves
        // the pinned source, so only assert retirement on successful rename.
        if (renamed) {
            QVERIFY(!result.success);
            QCOMPARE(readFile(output), QByteArray("PREVIOUS"));
        }
    }
};
QTEST_GUILESS_MAIN(LogAnonymizerTest)
#include "test_loganonymizer.moc"
