#include "FirmwareArchiveService.h"
#include "FirmwareArchiveManifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QMap>
#include <QSet>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
using namespace FirmwareArchive;
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif
bool samePath(const QString &a, const QString &b) { return a.compare(b, PathCase) == 0; }
bool present(const QString &p) { const QFileInfo f(p); return f.exists() || f.isSymLink(); }
QString singleLine(QString s) { return s.replace('\r', ' ').replace('\n', ' ').left(2048); }
struct Stamp {
    QDateTime birth, modified;
    qint64 size = 0;
    static Stamp of(const QString &p) {
        const QFileInfo f(p); return {f.birthTime(), f.lastModified(), f.size()};
    }
    bool identity(const QString &p, bool directory) const {
        const QFileInfo f(p);
        return f.exists() && !f.isSymLink() && (directory ? f.isDir() : f.isFile())
            && samePath(f.canonicalFilePath(), p)
            && (!birth.isValid() || f.birthTime() == birth);
    }
    bool matches(const QString &p) const {
        const QFileInfo f(p);
        return identity(p, false) && f.size() == size && f.lastModified() == modified;
    }
};
bool canonicalParent(const QString &requested, QString *out, QString *error) {
    const QFileInfo f(QDir::cleanPath(QFileInfo(requested).absoluteFilePath()));
    if (requested.trimmed().isEmpty() || requested.contains(QChar('\0'))
        || !f.exists() || !f.isDir() || f.isSymLink() || f.canonicalFilePath().isEmpty()) {
        if (error) *error = QStringLiteral("Choose an existing ordinary parent directory, not a symbolic link.");
        return false;
    }
    *out = QDir::cleanPath(f.canonicalFilePath()); // Pin native ancestor aliases once.
    return true;
}

// The two external callbacks share one gate. They are never concurrent, even
// though transports run concurrently. Copies survive reentrant owner deletion.
struct Coordinator {
    Cancel cancel;
    Progress progress;
    std::mutex callbacks;
    std::atomic<bool> stopped{false}, wasCancelled{false};
    int completed = 0;
    bool cancelled() {
        if (stopped.load()) return true;
        std::lock_guard<std::mutex> lock(callbacks);
        if (stopped.load()) return true;
        try {
            if (cancel && cancel()) { wasCancelled = true; stopped = true; }
        } catch (...) { wasCancelled = true; stopped = true; }
        return stopped.load();
    }
    void advance(int total, const QString &path) {
        std::lock_guard<std::mutex> lock(callbacks);
        if (stopped.load()) return;
        ++completed;
        try { if (progress) progress(completed, total, path); }
        catch (...) { wasCancelled = true; stopped = true; }
    }
};

// No recursive removal: only individually registered, still-matching files
// and empty owned directories may be removed. Foreign insertions are retained.
struct Tree {
    QString parent, stage, destination;
    Stamp parentStamp;
    QMap<QString, Stamp> directories, files;
    bool parentSafe() const { return parentStamp.identity(parent, true); }
    bool directorySafe(const QString &path) const {
        if (!parentSafe()) return false;
        if (samePath(path, parent)) return true;
        QString current = path;
        while (!samePath(current, parent)) {
            const auto i = directories.constFind(current);
            if (i == directories.cend() || !i->identity(current, true)) return false;
            const QString previous = current;
            current = QFileInfo(current).absolutePath();
            if (samePath(previous, current)) return false;
        }
        return true;
    }
    bool createDirectory(const QString &path) {
        if (directories.contains(path)) return directorySafe(path);
        const QString up = QFileInfo(path).absolutePath();
        if (!samePath(up, parent) && !createDirectory(up)) return false;
        if (!directorySafe(up) || present(path) || !QDir().mkdir(path)) return false;
        directories.insert(path, Stamp::of(path));
        return directorySafe(path);
    }
    bool openFile(QFile &file, const QString &path) {
        if (!directorySafe(QFileInfo(path).absolutePath()) || present(path)) return false;
        file.setFileName(path);
        if (!file.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return false;
        files.insert(path, Stamp::of(path));
        return files.value(path).identity(path, false);
    }
    bool fileSafe(const QString &path) const {
        const auto i = files.constFind(path);
        return i != files.cend() && directorySafe(QFileInfo(path).absolutePath()) && i->matches(path);
    }
    bool write(QFile &file, const QByteArray &bytes) {
        const QString path = file.fileName();
        if (!fileSafe(path) || file.write(bytes) != bytes.size() || !file.flush()) return false;
        // Flush each chunk before taking its stamp. This is not an fsync claim.
        if (!files.value(path).identity(path, false)) return false;
        files[path] = Stamp::of(path);
        return true;
    }
    bool removeFile(const QString &path) {
        if (!fileSafe(path) || !QFile::remove(path)) return false;
        files.remove(path); return true;
    }
    bool completeTree() const {
        if (!directorySafe(stage)) return false;
        int seenFiles = 0, seenDirectories = 1;
        QDirIterator it(stage, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next(); const QFileInfo f(p);
            if (f.isSymLink()) return false;
            if (f.isDir()) { if (!directorySafe(p)) return false; ++seenDirectories; }
            else { if (!fileSafe(p)) return false; ++seenFiles; }
            if (seenFiles > files.size() || seenDirectories > directories.size()) return false;
        }
        return seenFiles == files.size() && seenDirectories == directories.size();
    }
    void cleanup(Result *result) {
        for (const QString &p : files.keys()) removeFile(p);
        QStringList dirs = directories.keys();
        std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
        for (const QString &p : dirs) {
            if (directorySafe(p) && QDir().rmdir(p)) directories.remove(p);
        }
        if (!directories.isEmpty() || !files.isEmpty() || present(stage)) {
            result->retainedStaging = stage;
            result->warnings.append(QStringLiteral("Staging cleanup was unsafe or incomplete. Last known staging path: %1; it may have moved or become unsafe to access. No recursive deletion was attempted.").arg(stage));
        }
    }
};
struct Downloaded { QByteArray digest; qint64 bytes = 0; QString error; Failure failure = Failure::None; };
FetchResult invokeFetch(const Fetch &fetch, const QUrl &uri, qint64 limit, bool httpsOnly,
                        const Cancel &cancel, const ChunkSink &sink) {
    try { return fetch(uri, limit, httpsOnly, cancel, sink); }
    catch (...) { return {Failure::Policy, QStringLiteral("The download transport raised an exception."), 0}; }
}
}

QVector<QUrl> FirmwareArchiveService::officialManifestUris() {
    return {QUrl(QStringLiteral("https://github.com/ArduPilot/binary/raw/master/Firmware/firmware2.xml")),
            QUrl(QStringLiteral("https://firmware.ardupilot.org/Tools/MissionPlanner/Firmware/firmware2.xml"))};
}

QString FirmwareArchiveService::nextDirectory(const QString &requested, const QDateTime &now, QString *error) {
    if (error) error->clear();
    QString parent;
    if (!canonicalParent(requested, &parent, error)) return {};
    if (!now.isValid()) { if (error) *error = QStringLiteral("An archive timestamp is required."); return {}; }
    const QString base = QDir(parent).filePath(QStringLiteral("MissionPlanner-Firmware-Archive-")
                                             + now.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    for (int n = 1; n <= 1000; ++n) {
        const QString path = n == 1 ? base : base + '-' + QString::number(n);
        if (!present(path)) return path;
    }
    if (error) *error = QStringLiteral("All 1000 archive names for this timestamp already exist.");
    return {};
}

FirmwareArchive::Result FirmwareArchiveService::download(
    const QVector<QUrl> &requestedManifests, const QString &requestedDestination,
    const Fetch &requestedFetch, const Cancel &requestedCancel, const Progress &requestedProgress) {
    const auto manifests = requestedManifests;
    const Fetch fetch = requestedFetch;
    Coordinator coordinator;
    coordinator.cancel = requestedCancel; coordinator.progress = requestedProgress;
    const Cancel cancel = [&coordinator] { return coordinator.cancelled(); };
    Result result;
    Tree tree;
    if (!fetch || manifests.isEmpty() || manifests.size() > 1000) {
        result.error = QStringLiteral("Provide a transport and 1–1000 HTTPS manifest mirrors."); return result;
    }
    if (requestedDestination.trimmed().isEmpty() || requestedDestination.contains(QChar('\0'))) {
        result.error = QStringLiteral("An archive destination is required."); return result;
    }
    const QFileInfo requested(QDir::cleanPath(QFileInfo(requestedDestination).absoluteFilePath()));
    if (!canonicalParent(requested.absolutePath(), &tree.parent, &result.error)) return result;
    tree.parentStamp = Stamp::of(tree.parent);
    tree.destination = QDir(tree.parent).filePath(requested.fileName());
    if (present(tree.destination)) { result.error = QStringLiteral("The archive destination already exists."); return result; }

    QByteArray xml;
    QStringList mirrorErrors;
    for (const QUrl &uri : manifests) {
        if (cancel()) break;
        if (!FirmwareArchiveManifest::allowedUrl(uri, true)) {
            mirrorErrors.append(QStringLiteral("Only absolute HTTPS manifest URLs are accepted.")); continue;
        }
        QByteArray bytes;
        Failure sinkFailure = Failure::None;
        const auto got = invokeFetch(fetch, uri, MaximumManifestBytes, true, cancel, [&](const QByteArray &chunk) {
            if (cancel()) { sinkFailure = Failure::Cancelled; return false; }
            if (sinkFailure != Failure::None) return false;
            if (chunk.size() > MaximumManifestBytes - bytes.size()) { sinkFailure = Failure::Limit; return false; }
            bytes += chunk; return true;
        });
        if (sinkFailure == Failure::Cancelled || got.failure == Failure::Cancelled) {
            coordinator.wasCancelled = true; coordinator.stopped = true; break;
        }
        if (got.success() && sinkFailure == Failure::None && got.bytes == bytes.size()) {
            xml = bytes; result.manifestSource = uri; break;
        }
        mirrorErrors.append(uri.toString() + QStringLiteral(": ")
                            + (sinkFailure == Failure::Limit ? QStringLiteral("Manifest size limit exceeded.")
                               : singleLine(got.error.isEmpty() ? QStringLiteral("Incomplete manifest response.") : got.error)));
    }
    if (coordinator.wasCancelled) { result.cancelled = true; result.error = QStringLiteral("Archive cancelled."); return result; }
    if (result.manifestSource.isEmpty()) {
        result.error = QStringLiteral("No firmware manifest mirror succeeded. ") + mirrorErrors.join(QStringLiteral(" | ")); return result;
    }
    const auto plan = FirmwareArchiveManifest::parse(xml); // A downloaded invalid manifest is fatal, not a mirror retry.
    if (!plan.success || plan.downloads.isEmpty() || plan.downloads.size() > MaximumDownloads) {
        result.error = plan.error.isEmpty() ? QStringLiteral("Manifest must contain 1–20000 distinct firmware URLs.") : plan.error;
        return result;
    }
    if (cancel()) { result.cancelled = true; result.error = QStringLiteral("Archive cancelled."); return result; }
    if (!tree.parentSafe() || present(tree.destination)) {
        result.error = QStringLiteral("Archive parent or destination changed while downloading the manifest."); return result;
    }
    tree.stage = tree.destination + QStringLiteral(".partial-") + QUuid::createUuid().toString(QUuid::Id128);
    if (!tree.createDirectory(tree.stage)) {
        result.error = QStringLiteral("Cannot create a private archive staging directory.");
        if (!tree.directories.isEmpty()) tree.cleanup(&result);
        return result;
    }
    auto fail = [&](const QString &message) {
        result.error = message; result.cancelled = coordinator.wasCancelled.load();
        tree.cleanup(&result); return result;
    };
    for (const auto &d : plan.downloads) {
        if (cancel()) return fail(QStringLiteral("Archive cancelled."));
        const QString full = QDir(tree.stage).filePath(d.relativePath);
        if (QDir::isAbsolutePath(d.relativePath) || d.relativePath.contains('\\')
            || d.relativePath.split('/').contains(QStringLiteral(".."))
            || !d.relativePath.startsWith(QStringLiteral("files/"))
            || !tree.createDirectory(QFileInfo(full).absolutePath()))
            return fail(QStringLiteral("Unsafe firmware output path or changed staging directory."));
    }
    QVector<Downloaded> downloaded(plan.downloads.size());
    // Detach before concurrent writes to distinct value slots.
    Downloaded *outcomes = downloaded.data();
    std::atomic<int> next{0};
    std::atomic<bool> fatal{false};
    auto worker = [&] {
        while (!cancel()) {
            const int index = next.fetch_add(1);
            if (index >= plan.downloads.size()) return;
            const auto &d = plan.downloads.at(index);
            Downloaded &out = outcomes[index];
            const QString path = QDir(tree.stage).filePath(d.relativePath);
            auto attempt = [&](const QUrl &uri, bool httpsOnly) {
                out = Downloaded{};
                QFile file;
                {
                    std::lock_guard<std::mutex> lock(coordinator.callbacks);
                    if (!tree.openFile(file, path)) { out.failure = Failure::LocalIo; out.error = QStringLiteral("Cannot safely create firmware output."); return; }
                }
                QCryptographicHash hash(QCryptographicHash::Sha256);
                const auto got = invokeFetch(fetch, uri, MaximumFirmwareBytes, httpsOnly, cancel, [&](const QByteArray &chunk) {
                    if (cancel()) { out.failure = Failure::Cancelled; return false; }
                    if (out.failure != Failure::None) return false;
                    if (chunk.size() > MaximumFirmwareBytes - out.bytes) {
                        out.failure = Failure::Limit; out.error = QStringLiteral("Firmware exceeds the 256 MiB safety limit."); return false;
                    }
                    std::lock_guard<std::mutex> lock(coordinator.callbacks);
                    if (!tree.write(file, chunk)) {
                        out.failure = Failure::LocalIo; out.error = QStringLiteral("Firmware write failed or its path changed."); return false;
                    }
                    hash.addData(chunk); out.bytes += chunk.size(); return true;
                });
                if (out.failure == Failure::None && !got.success()) { out.failure = got.failure; out.error = singleLine(got.error); }
                if (out.failure == Failure::None && got.bytes > MaximumFirmwareBytes) {
                    out.failure = Failure::Limit; out.error = QStringLiteral("Transport reported firmware beyond the 256 MiB safety limit.");
                }
                if (out.failure == Failure::None && got.bytes != out.bytes) {
                    out.failure = Failure::Network; out.error = QStringLiteral("Transport byte count does not match stored firmware.");
                }
                {
                    std::lock_guard<std::mutex> lock(coordinator.callbacks);
                    if (out.failure == Failure::None && (!file.flush() || !tree.fileSafe(path))) {
                        out.failure = Failure::LocalIo; out.error = QStringLiteral("Firmware changed before final flush.");
                    }
                    file.close();
                    if (out.failure != Failure::None && !tree.removeFile(path)) {
                        out.failure = Failure::LocalIo; out.error = QStringLiteral("Cannot safely remove a failed firmware download.");
                    }
                }
                if (out.failure == Failure::None) out.digest = hash.result().toHex();
            };
            const bool legacy = d.uri.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0;
            QUrl secure = d.uri;
            if (legacy) { secure.setScheme(QStringLiteral("https")); secure.setPort(-1); }
            attempt(secure, true);
            if (legacy && out.failure == Failure::Network && !cancel()) attempt(d.uri, false);
            if (out.failure == Failure::LocalIo) { fatal = true; coordinator.stopped = true; }
            if (out.failure == Failure::Cancelled) {
                if (!fatal.load()) coordinator.wasCancelled = true;
                coordinator.stopped = true;
            }
            coordinator.advance(plan.downloads.size(), d.relativePath);
        }
    };
    QThreadPool pool;
    pool.setMaxThreadCount(MaximumParallelDownloads);
    QVector<QFuture<void>> futures;
    for (int n = 0; n < std::min(MaximumParallelDownloads, plan.downloads.size()); ++n)
        futures.append(QtConcurrent::run(&pool, worker));
    for (auto &future : futures) future.waitForFinished();
    pool.waitForDone();
    QHash<QString, QString> successes;
    QStringList checksums, unavailable;
    for (int i = 0; i < downloaded.size(); ++i) {
        const auto &out = downloaded.at(i); const auto &d = plan.downloads.at(i);
        if (out.failure == Failure::None && !out.digest.isEmpty()) {
            successes.insert(d.uri.toString(QUrl::FullyEncoded), d.relativePath);
            checksums.append(QString::fromLatin1(out.digest) + QStringLiteral("  ") + d.relativePath);
            ++result.fileCount; result.bytesDownloaded += out.bytes;
        } else {
            ++result.failedFiles;
            unavailable.append(d.uri.toString(QUrl::FullyEncoded) + QStringLiteral(" | ")
                               + (out.error.isEmpty() ? QStringLiteral("Download unavailable.") : singleLine(out.error)));
        }
    }
    if (fatal || coordinator.wasCancelled || successes.isEmpty()) {
        for (int i = 0; i < std::min(20, unavailable.size()); ++i) result.warnings.append(unavailable.at(i));
        if (unavailable.size() > 20)
            result.warnings.append(QStringLiteral("%1 additional unavailable firmware URLs omitted from this error summary.").arg(unavailable.size() - 20));
    }
    if (fatal) return fail(QStringLiteral("Local I/O or archive path integrity failed; archive was not published."));
    if (cancel()) return fail(QStringLiteral("Archive cancelled."));
    if (successes.isEmpty()) return fail(QStringLiteral("None of the firmware URLs could be downloaded."));
    QString rewriteError;
    const QByteArray localManifest = FirmwareArchiveManifest::rewrite(plan, successes, &rewriteError);
    if (!rewriteError.isEmpty() || localManifest.isEmpty()) return fail(QStringLiteral("Cannot rewrite archive manifest: ") + rewriteError);
    // Reference checksum ordering is by relative file path, not by digest.
    std::sort(checksums.begin(), checksums.end(), [](const QString &a, const QString &b) { return a.mid(66) < b.mid(66); });
    std::sort(unavailable.begin(), unavailable.end());
    QStringList report{QStringLiteral("Mission Planner firmware archive"),
        QStringLiteral("Manifest: ") + result.manifestSource.toString(QUrl::FullyEncoded),
        QStringLiteral("Downloaded: %1").arg(result.fileCount), QStringLiteral("Unavailable: %1").arg(result.failedFiles),
        QStringLiteral("Bytes: %1").arg(result.bytesDownloaded), QString(),
        QStringLiteral("Unavailable URLs remain unchanged in firmware2.xml:")};
    report += unavailable;
    const QVector<QPair<QString, QByteArray>> metadata{
        {QStringLiteral("firmware2.xml"), localManifest},
        {QStringLiteral("checksums.sha256"), (checksums.join('\n') + '\n').toUtf8()},
        {QStringLiteral("archive-report.txt"), (report.join('\n') + '\n').toUtf8()}};
    for (const auto &item : metadata) {
        if (cancel()) return fail(QStringLiteral("Archive cancelled."));
        QFile file;
        if (!tree.openFile(file, QDir(tree.stage).filePath(item.first)) || !tree.write(file, item.second)) {
            file.close();
            return fail(QStringLiteral("Cannot safely write archive metadata."));
        }
        file.close();
    }
    // Re-read successful output, independently of transport promises. Progress
    // callbacks can edit files; such edits must not be blessed by checksums.
    for (int i = 0; i < downloaded.size(); ++i) {
        const auto &out = downloaded.at(i);
        if (out.digest.isEmpty()) continue;
        const QString path = QDir(tree.stage).filePath(plan.downloads.at(i).relativePath);
        if (cancel()) return fail(QStringLiteral("Archive cancelled."));
        if (!tree.fileSafe(path)) return fail(QStringLiteral("Stored firmware changed before publication."));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot verify stored firmware."));
        auto verificationFailed = [&](const QString &error) {
            file.close(); return fail(error);
        };
        QCryptographicHash digest(QCryptographicHash::Sha256);
        qint64 bytes = 0;
        while (!file.atEnd()) {
            if (cancel()) return verificationFailed(QStringLiteral("Archive cancelled."));
            if (!tree.fileSafe(path)) return verificationFailed(QStringLiteral("Stored firmware changed during verification."));
            const QByteArray chunk = file.read(64 * 1024);
            if (chunk.isEmpty() && file.error() != QFile::NoError) return verificationFailed(QStringLiteral("Firmware verification read failed."));
            bytes += chunk.size(); digest.addData(chunk);
            if (bytes > MaximumFirmwareBytes) return verificationFailed(QStringLiteral("Stored firmware grew during verification."));
        }
        file.close();
        if (bytes != out.bytes || digest.result().toHex() != out.digest)
            return fail(QStringLiteral("Stored firmware checksum changed before publication."));
    }
    if (cancel()) return fail(QStringLiteral("Archive cancelled."));
    // No external callback between the final full-tree guard and rename.
    if (!tree.completeTree() || present(tree.destination))
        return fail(QStringLiteral("Archive staging, parent or destination changed before publication."));
    if (!QDir().rename(tree.stage, tree.destination)) return fail(QStringLiteral("Cannot publish archive without overwriting a destination."));
    result.success = true; result.directory = tree.destination;
    if (result.failedFiles) result.warnings.append(QStringLiteral("%1 firmware downloads were unavailable; their original network URLs remain in the manifest. See archive-report.txt.").arg(result.failedFiles));
    return result;
}
