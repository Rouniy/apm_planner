#include <QtTest>
#include "services/FirmwareArchiveService.h"
#include "services/FirmwareArchiveManifest.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
using namespace FirmwareArchive;
const QUrl Manifest(QStringLiteral("https://manifest.example/firmware2.xml"));
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QByteArray xml(const QStringList &urls) {
    QByteArray result("<options><!--keep--><Firmware><name>Copter</name>");
    for (int i = 0; i < urls.size(); ++i)
        result += "<url" + QByteArray::number(i) + ">" + urls.at(i).toHtmlEscaped().toUtf8()
            + "</url" + QByteArray::number(i) + ">";
    return result + "<urlEmpty/></Firmware></options>";
}
FetchResult deliver(const ChunkSink &sink, const QByteArray &bytes) {
    if (!sink(bytes)) return {Failure::Policy, QStringLiteral("Sink refused data."), 0};
    return {Failure::None, {}, bytes.size()};
}
Fetch simple(const QByteArray &manifest, const QByteArray &payload = QByteArray("payload\0binary", 14)) {
    return [manifest, payload](const QUrl &uri, qint64, bool, const Cancel &, const ChunkSink &sink) {
        return deliver(sink, uri == Manifest ? manifest : payload);
    };
}
QString stageIn(const QString &parent) {
    const auto paths = QDir(parent).entryList({QStringLiteral("*.partial-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    return paths.size() == 1 ? QFileInfo(QDir(parent).filePath(paths.first())).canonicalFilePath() : QString();
}
QStringList directoryChildren(const QString &parent) {
    return QDir(parent).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}
QString firstFirmware(const QString &stage) {
    QDirIterator it(stage + "/files", QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}
}

class FirmwareArchiveServiceTest : public QObject {
    Q_OBJECT
private slots:
    void officialMirrorsAndNames() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto mirrors = FirmwareArchiveService::officialManifestUris();
        QCOMPARE(mirrors.size(), 2);
        QCOMPARE(mirrors.first().toString(), QString("https://github.com/ArduPilot/binary/raw/master/Firmware/firmware2.xml"));
        const auto utc = QDateTime(QDate(2026, 8, 23), QTime(10, 11, 12), Qt::UTC);
        QString error;
        const auto a = FirmwareArchiveService::nextDirectory(root.path(), utc, &error);
        QVERIFY(error.isEmpty()); QVERIFY(a.endsWith("MissionPlanner-Firmware-Archive-20260823-101112"));
        QVERIFY(QDir().mkdir(a));
        QCOMPARE(FirmwareArchiveService::nextDirectory(root.path(), utc), a + "-2");
        QVERIFY(writeFile(a + "-2", "existing file"));
        QCOMPARE(FirmwareArchiveService::nextDirectory(root.path(), utc), a + "-3");
        QVERIFY(FirmwareArchiveService::nextDirectory(root.path(), QDateTime(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void successfulMirrorsDedupHashAndParallelProgress() {
        QTemporaryDir root; QVERIFY(root.isValid());
        QStringList urls;
        for (int i = 0; i < 8; ++i) urls.append(QString("https://cdn.example/fw-%1.apj").arg(i));
        auto repeated = urls; repeated.append(urls.first());
        const auto manifest = xml(repeated);
        const QUrl first("https://first.example/firmware2.xml");
        std::mutex lock;
        QMap<QString, int> calls;
        std::atomic<int> active{0}, peak{0};
        QSemaphore allFirstFour;
        QVector<int> progress;
        std::atomic<int> inCallback{0}; std::atomic<bool> concurrentCallback{false}, bad{false};
        const auto fetch = [&](const QUrl &uri, qint64 limit, bool httpsOnly, const Cancel &cancel, const ChunkSink &sink) {
            { std::lock_guard<std::mutex> guard(lock); ++calls[uri.toString()]; }
            if (!httpsOnly) bad = true;
            if (uri == first) return FetchResult{Failure::Network, "mirror unavailable", 0};
            if (uri == Manifest) {
                if (limit != MaximumManifestBytes) bad = true;
                return deliver(sink, manifest);
            }
            if (limit != MaximumFirmwareBytes || cancel()) bad = true;
            const int current = ++active;
            int old = peak.load(); while (current > old && !peak.compare_exchange_weak(old, current)) {}
            if (uri.path().mid(4, 1).toInt() < 4) {
                allFirstFour.release();
                // Once all four workers have entered, let each proceed. The
                // bounded timeout keeps a broken pool from hanging the suite.
                const auto until = QDeadlineTimer(3000);
                while (allFirstFour.available() < 4 && !until.hasExpired()) QThread::yieldCurrentThread();
                if (allFirstFour.available() < 4) bad = true;
            }
            const QByteArray payload = "payload:" + uri.toEncoded();
            const auto got = deliver(sink, payload);
            --active; return got;
        };
        const auto result = FirmwareArchiveService::download({first, Manifest}, root.path() + "/archive", fetch,
            [&] {
                if (inCallback.fetch_add(1)) concurrentCallback = true;
                --inCallback; return false;
            }, [&](int completed, int total, const QString &item) {
                if (inCallback.fetch_add(1)) concurrentCallback = true;
                if (total != 8 || !item.startsWith("files/")) bad = true;
                progress.append(completed); --inCallback;
            });
        QVERIFY2(result.success, qPrintable(result.error));
        QVERIFY(!bad); QVERIFY(!concurrentCallback); QCOMPARE(peak.load(), 4);
        QCOMPARE(result.manifestSource, Manifest); QCOMPARE(result.fileCount, 8); QCOMPARE(result.failedFiles, 0);
        QCOMPARE(progress, QVector<int>({1,2,3,4,5,6,7,8}));
        qint64 bytes = 0;
        const auto local = readFile(result.directory + "/firmware2.xml");
        QVERIFY(local.contains("<!--keep-->"));
        QVERIFY(!local.contains("https://cdn.example/"));
        QStringList expectedChecksums;
        for (const QString &url : urls) {
            QCOMPARE(calls.value(url), 1);
            const QByteArray payload = "payload:" + QUrl(url).toEncoded(); bytes += payload.size();
            const QString relative = FirmwareArchiveManifest::relativePath(QUrl(url));
            QCOMPARE(readFile(result.directory + '/' + relative), payload);
            expectedChecksums.append(QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()) + "  " + relative);
        }
        std::sort(expectedChecksums.begin(), expectedChecksums.end(), [](const QString &a, const QString &b) { return a.mid(66) < b.mid(66); });
        QCOMPARE(readFile(result.directory + "/checksums.sha256"), (expectedChecksums.join('\n') + '\n').toUtf8());
        QCOMPARE(result.bytesDownloaded, bytes);
        QVERIFY(readFile(result.directory + "/archive-report.txt").contains("Unavailable: 0"));
        QCOMPARE(directoryChildren(root.path()), QStringList{"archive"});
    }

    void legacyFallbackAndPartialArchive() {
        QTemporaryDir root;
        const QString secure("http://legacy.example:8080/secure.apj");
        const QString fallback("http://legacy.example:8080/fallback.apj");
        const QString missing("https://cdn.example/missing.apj");
        const auto manifest = xml({secure, fallback, missing});
        std::mutex mutex; QMap<QString, bool> calls;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [&](const QUrl &uri, qint64, bool httpsOnly, const Cancel &, const ChunkSink &sink) {
                { std::lock_guard<std::mutex> guard(mutex); calls[uri.toString()] = httpsOnly; }
                if (uri == Manifest) return deliver(sink, manifest);
                if (uri == QUrl("https://legacy.example/secure.apj")) return deliver(sink, "123");
                if (uri == QUrl(fallback)) return deliver(sink, "45");
                if (!sink("discard this failed partial response")) return FetchResult{Failure::Policy, "sink failure", 0};
                return FetchResult{Failure::Network, "Unavailable\r\nserver", 36};
            });
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.fileCount, 2); QCOMPARE(result.failedFiles, 1); QCOMPARE(result.bytesDownloaded, qint64(5));
        QVERIFY(!calls.contains(secure));
        QVERIFY(calls.value("https://legacy.example/fallback.apj"));
        QVERIFY(calls.contains(fallback)); QVERIFY(!calls.value(fallback));
        const auto local = readFile(result.directory + "/firmware2.xml");
        QVERIFY(local.contains(missing.toUtf8())); QVERIFY(!local.contains(fallback.toUtf8()));
        const auto report = readFile(result.directory + "/archive-report.txt");
        QVERIFY(report.contains("Unavailable: 1")); QVERIFY(report.contains("Unavailable  server"));
        QVERIFY(!result.warnings.isEmpty());
    }

    void nonNetworkNeverFallsBack_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("policy") << int(Failure::Policy);
        QTest::newRow("limit") << int(Failure::Limit);
        QTest::newRow("disk") << int(Failure::LocalIo);
        QTest::newRow("cancel") << int(Failure::Cancelled);
    }
    void nonNetworkNeverFallsBack() {
        QFETCH(int, failure); QTemporaryDir root;
        std::atomic<int> legacy{0};
        const auto manifest = xml({"http://legacy.example/fw.apj"});
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [&](const QUrl &uri, qint64, bool, const Cancel &, const ChunkSink &sink) {
                if (uri == Manifest) return deliver(sink, manifest);
                if (uri.scheme() == "http") ++legacy;
                sink("partial"); return FetchResult{Failure(failure), "rejected", 7};
            });
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QCOMPARE(legacy.load(), 0);
        QCOMPARE(result.cancelled, failure == int(Failure::Cancelled));
        QCOMPARE(result.failedFiles, 1); QVERIFY(directoryChildren(root.path()).isEmpty());
    }

    void existingDestinationBeforeCallbacks() {
        QTemporaryDir root; QVERIFY(QDir().mkdir(root.path() + "/archive"));
        QVERIFY(writeFile(root.path() + "/archive/keep", "operator data"));
        int calls = 0;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [&](const QUrl &, qint64, bool, const Cancel &, const ChunkSink &) { ++calls; return FetchResult{}; },
            [&] { ++calls; return false; });
        QVERIFY(!result.success); QCOMPARE(calls, 0);
        QCOMPARE(readFile(root.path() + "/archive/keep"), QByteArray("operator data"));
    }

    void invalidManifestIsFatalAfterFirstSuccessfulMirror() {
        QTemporaryDir root; int calls = 0;
        const auto result = FirmwareArchiveService::download({QUrl("http://insecure.example/fw.xml"), Manifest, QUrl("https://other.example/fw.xml")},
            root.path() + "/archive", [&](const QUrl &uri, qint64, bool httpsOnly, const Cancel &, const ChunkSink &sink) {
                ++calls; if (uri != Manifest || !httpsOnly) return FetchResult{Failure::Policy, "wrong mirror", 0};
                return deliver(sink, "<broken");
            });
        QVERIFY(!result.success); QCOMPARE(calls, 1); QVERIFY(directoryChildren(root.path()).isEmpty());
    }

    void manifestLimitAndByteMismatch() {
        QTemporaryDir root; int calls = 0;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [&](const QUrl &, qint64, bool, const Cancel &, const ChunkSink &sink) {
                ++calls; const QByteArray chunk(1024 * 1024, 'x');
                for (int i = 0; i < 8; ++i) if (!sink(chunk)) return FetchResult{Failure::Policy, "early refusal", 0};
                const bool accepted = sink("x");
                return FetchResult{accepted ? Failure::None : Failure::Limit, "limit", MaximumManifestBytes + 1};
            });
        QVERIFY(!result.success); QCOMPARE(calls, 1); QVERIFY(directoryChildren(root.path()).isEmpty());
        const auto wrongCount = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [](const QUrl &, qint64, bool, const Cancel &, const ChunkSink &sink) {
                sink(xml({"https://cdn.example/fw.apj"})); return FetchResult{Failure::None, {}, 1};
            });
        QVERIFY(!wrongCount.success); QVERIFY(directoryChildren(root.path()).isEmpty());
    }

    void firmwareDeclaredOversizeNoFallback() {
        QTemporaryDir root; std::atomic<int> http{0};
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            [&](const QUrl &uri, qint64, bool, const Cancel &, const ChunkSink &sink) {
                if (uri == Manifest) return deliver(sink, xml({"http://cdn.example/fw.apj"}));
                if (uri.scheme() == "http") ++http;
                sink("small"); return FetchResult{Failure::None, {}, MaximumFirmwareBytes + 1};
            });
        QVERIFY(!result.success); QCOMPARE(http.load(), 0); QVERIFY(directoryChildren(root.path()).isEmpty());
    }

    void cancellationBeforePublicationAndForeignCleanup_data() {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("only owned removed") << false;
        QTest::newRow("foreign preserved") << true;
    }
    void cancellationBeforePublicationAndForeignCleanup() {
        QFETCH(bool, foreign); QTemporaryDir root; std::atomic<bool> cancel{false};
        QString stage; bool inserted = false;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            simple(xml({"https://cdn.example/fw.apj"})), [&] { return cancel.load(); },
            [&](int, int, const QString &) {
                stage = stageIn(root.path());
                if (foreign) inserted = writeFile(stage + "/foreign.txt", "do not delete");
                cancel = true;
            });
        QVERIFY(!result.success); QVERIFY(result.cancelled); QVERIFY(!QFileInfo::exists(root.path() + "/archive"));
        if (foreign) {
            QVERIFY(inserted); QCOMPARE(result.retainedStaging, stage);
            QCOMPARE(readFile(stage + "/foreign.txt"), QByteArray("do not delete"));
            QCOMPARE(directoryChildren(stage), QStringList{"foreign.txt"});
        } else { QVERIFY(result.retainedStaging.isEmpty()); QVERIFY(directoryChildren(root.path()).isEmpty()); }
    }

    void destinationCreatedByFinalProgressIsPreserved() {
        QTemporaryDir root; bool created = false;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            simple(xml({"https://cdn.example/fw.apj"})), {}, [&](int, int, const QString &) {
                created = QDir().mkdir(root.path() + "/archive") && writeFile(root.path() + "/archive/foreign", "keep");
            });
        QVERIFY(created); QVERIFY(!result.success); QVERIFY(!result.cancelled);
        QCOMPARE(readFile(root.path() + "/archive/foreign"), QByteArray("keep"));
        QCOMPARE(directoryChildren(root.path()), QStringList{"archive"});
    }

    void mutationByLastCancelCallbackNeverPublishes() {
        QTemporaryDir root; bool changed = false;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            simple(xml({"https://cdn.example/fw.apj"})), [&] {
                const QString stage = stageIn(root.path());
                // Once all metadata exists, callbacks still occur during the
                // final verification. A false cancellation answer is not trust.
                if (!changed && !stage.isEmpty() && QFileInfo::exists(stage + "/archive-report.txt")) {
                    changed = writeFile(stage + "/archive-report.txt", "foreign metadata");
                }
                return false;
            });
        QVERIFY(changed); QVERIFY(!result.success); QVERIFY(!result.cancelled);
        QVERIFY(!result.retainedStaging.isEmpty());
        QCOMPARE(readFile(result.retainedStaging + "/archive-report.txt"), QByteArray("foreign metadata"));
    }

    void firmwareMutatedByProgressIsNotBlessed() {
        QTemporaryDir root; bool changed = false;
        const auto result = FirmwareArchiveService::download({Manifest}, root.path() + "/archive",
            simple(xml({"https://cdn.example/fw.apj"})), {}, [&](int, int, const QString &) {
                changed = writeFile(firstFirmware(stageIn(root.path())), "different firmware");
            });
        QVERIFY(changed); QVERIFY(!result.success); QVERIFY(!result.retainedStaging.isEmpty());
        QCOMPARE(readFile(firstFirmware(result.retainedStaging)), QByteArray("different firmware"));
    }

    void canonicalAncestorAliasAdmissionAndRepoint() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link creates shortcuts rather than Unix directory symlinks on Windows.");
#else
        QTemporaryDir root;
        QVERIFY(QDir().mkpath(root.path() + "/real/child"));
        QVERIFY(QDir().mkpath(root.path() + "/other/child"));
        const QString alias = root.path() + "/alias";
        QVERIFY(QFile::link(root.path() + "/real", alias));
        bool repointed = false;
        const auto result = FirmwareArchiveService::download({Manifest}, alias + "/child/archive",
            simple(xml({"https://cdn.example/fw.apj"})), {}, [&](int, int, const QString &) {
                repointed = QFile::remove(alias) && QFile::link(root.path() + "/other", alias);
            });
        QVERIFY(repointed); QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.directory, QFileInfo(root.path() + "/real/child/archive").canonicalFilePath());
        QVERIFY(directoryChildren(root.path() + "/other/child").isEmpty());
        int fetches = 0;
        const auto refused = FirmwareArchiveService::download({Manifest}, alias + "/archive",
            [&](const QUrl &, qint64, bool, const Cancel &, const ChunkSink &) { ++fetches; return FetchResult{}; });
        QVERIFY(!refused.success); QCOMPARE(fetches, 0);
#endif
    }

    void pinnedParentReplacementIsNeverFollowed() {
#ifdef Q_OS_WIN
        QSKIP("Uses Unix directory symlink replacement.");
#else
        QTemporaryDir root;
        const QString parent = root.path() + "/parent";
        const QString moved = root.path() + "/original";
        const QString foreign = root.path() + "/foreign";
        QVERIFY(QDir().mkdir(parent)); QVERIFY(QDir().mkdir(foreign));
        QVERIFY(writeFile(foreign + "/keep", "foreign"));
        bool replaced = false;
        const auto result = FirmwareArchiveService::download({Manifest}, parent + "/archive",
            simple(xml({"https://cdn.example/fw.apj"})), {}, [&](int, int, const QString &) {
                replaced = QDir().rename(parent, moved) && QFile::link(foreign, parent);
            });
        QVERIFY(replaced); QVERIFY(!result.success); QVERIFY(!result.retainedStaging.isEmpty());
        QCOMPARE(directoryChildren(foreign), QStringList{"keep"});
        QCOMPARE(readFile(foreign + "/keep"), QByteArray("foreign"));
        QVERIFY(!stageIn(moved).isEmpty());
#endif
    }

    void callbackInputsPinnedAndNoUnexpectedTreePublication() {
        QTemporaryDir root;
        QVector<QUrl> mirrors{Manifest};
        Fetch fetch = simple(xml({"https://cdn.example/fw.apj"}));
        bool modified = false;
        Progress progress = [&](int, int, const QString &) {
            mirrors.clear(); fetch = {}; progress = {};
            const auto stage = stageIn(root.path());
            modified = QDir().mkdir(stage + "/unowned");
        };
        const auto result = FirmwareArchiveService::download(mirrors, root.path() + "/archive", fetch, {}, progress);
        QVERIFY(modified); QVERIFY(!result.success); QVERIFY(!result.retainedStaging.isEmpty());
        QVERIFY(QFileInfo(result.retainedStaging + "/unowned").isDir());
    }
};
QTEST_GUILESS_MAIN(FirmwareArchiveServiceTest)
#include "test_firmwarearchiveservice.moc"
