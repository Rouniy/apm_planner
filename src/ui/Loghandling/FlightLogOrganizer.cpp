#include "FlightLogOrganizer.h"

#include "FlightLogClassifier.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

constexpr qint64 HashChunkSize = 1024 * 1024;

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanAbsolutePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString pathKey(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    return pathCaseSensitivity() == Qt::CaseInsensitive
        ? clean.toCaseFolded() : clean;
}

bool samePath(const QString &left, const QString &right)
{
    return QDir::cleanPath(left).compare(
               QDir::cleanPath(right), pathCaseSensitivity()) == 0;
}

bool isWithin(const QString &root, const QString &path)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanPath = QDir::cleanPath(path);
    if (samePath(cleanRoot, cleanPath)) return true;
    QString prefix = cleanRoot;
    // Qt's cleaned paths use '/' on every platform, including Windows.
    if (!prefix.endsWith(QLatin1Char('/'))) prefix += QLatin1Char('/');
    return cleanPath.startsWith(prefix, pathCaseSensitivity());
}

void appendWarning(QStringList *warnings, const QString &warning)
{
    if (!warnings) return;
    if (warnings->size() < FlightLogOrganizer::MaximumFiles - 1) {
        warnings->append(warning);
    } else if (warnings->size() == FlightLogOrganizer::MaximumFiles - 1) {
        warnings->append(QStringLiteral(
            "Additional organizer warnings were omitted at the safety limit."));
    }
}

bool isCandidateExtension(const QString &path)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    return extension == QStringLiteral("tlog")
        || extension == QStringLiteral("rlog")
        || extension == QStringLiteral("bin")
        || extension == QStringLiteral("log");
}

bool cancellationRequested(const FlightLogOrganizer::Cancel &cancel)
{
    return cancel && cancel();
}

bool validateRoot(const QString &requested, QString *canonical,
                  QString *error)
{
    if (canonical) canonical->clear();
    if (requested.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("A log directory is required.");
        return false;
    }
    const QString absolute = cleanAbsolutePath(requested);
    const QFileInfo info(absolute);
    if (info.isSymLink() || !info.exists() || !info.isDir()) {
        if (error) {
            *error = QStringLiteral(
                "The log root must be an existing ordinary directory, not a symbolic link.");
        }
        return false;
    }
    const QString resolved = QDir::cleanPath(info.canonicalFilePath());
    if (resolved.isEmpty() || !samePath(absolute, resolved)) {
        if (error) {
            *error = QStringLiteral(
                "The log root or one of its ancestors is a symbolic-link alias.");
        }
        return false;
    }
    const QString home = QDir::cleanPath(QDir::home().canonicalPath());
    if (QDir(resolved).isRoot()
        || (!home.isEmpty() && samePath(resolved, home))) {
        if (error) {
            *error = QStringLiteral(
                "Refusing to organize a filesystem root or the complete home directory.");
        }
        return false;
    }
    if (!info.isReadable()) {
        if (error) *error = QStringLiteral("The log root is not readable.");
        return false;
    }
    if (canonical) *canonical = resolved;
    return true;
}

// Checks every existing path component. Missing tail components are allowed
// only for future destination directories; symbolic-link aliases are never
// accepted.
bool validatePathChain(const QString &root, const QString &path,
                       bool allowMissingTail, QString *error)
{
    const QString absolute = cleanAbsolutePath(path);
    if (!isWithin(root, absolute)) {
        if (error) *error = QStringLiteral("A path escapes the selected log root: %1").arg(path);
        return false;
    }
    const QString relative = QDir(root).relativeFilePath(absolute);
    if (relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))) {
        if (error) *error = QStringLiteral("A path escapes the selected log root: %1").arg(path);
        return false;
    }
    QString current = root;
    bool missing = false;
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            if (error) *error = QStringLiteral("Unsafe relative path component in %1").arg(path);
            return false;
        }
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink()) {
            if (error) *error = QStringLiteral("Symbolic-link path component refused: %1").arg(current);
            return false;
        }
        if (!info.exists()) {
            missing = true;
            if (!allowMissingTail) {
                if (error) *error = QStringLiteral("Expected path no longer exists: %1").arg(current);
                return false;
            }
            continue;
        }
        if (missing) {
            if (error) *error = QStringLiteral("Inconsistent destination path: %1").arg(current);
            return false;
        }
        const QString resolved = QDir::cleanPath(info.canonicalFilePath());
        if (resolved.isEmpty() || !isWithin(root, resolved)
            || !samePath(cleanAbsolutePath(current), resolved)) {
            if (error) *error = QStringLiteral("Aliased path component refused: %1").arg(current);
            return false;
        }
    }
    return true;
}

struct FileSnapshot {
    QString path;
    QString canonical;
    qint64 bytes = -1;
    qint64 modifiedMs = 0;
    QByteArray sha256;
};

enum class SnapshotStatus { Ok, Cancelled, Error };

SnapshotStatus snapshotFile(const QString &root, const QString &path,
                            const FlightLogOrganizer::Cancel &cancel,
                            FileSnapshot *snapshot, QString *error)
{
    if (cancellationRequested(cancel)) return SnapshotStatus::Cancelled;
    if (!validatePathChain(root, path, false, error)) return SnapshotStatus::Error;
    const QString absolute = cleanAbsolutePath(path);
    QFileInfo before(absolute);
    if (before.isSymLink() || !before.exists() || !before.isFile()) {
        if (error) *error = QStringLiteral("Planned source is not an ordinary file: %1").arg(path);
        return SnapshotStatus::Error;
    }
    const QString canonical = QDir::cleanPath(before.canonicalFilePath());
    if (canonical.isEmpty() || !samePath(absolute, canonical)
        || !isWithin(root, canonical)) {
        if (error) *error = QStringLiteral("Planned source is aliased or outside the log root: %1").arg(path);
        return SnapshotStatus::Error;
    }
    // Force QFileInfo to cache the pre-read values before opening the file.
    const qint64 expectedBytes = before.size();
    const qint64 expectedModifiedMs = before.lastModified().toMSecsSinceEpoch();
    if (expectedBytes < 0) {
        if (error) *error = QStringLiteral("Cannot inspect the size of %1.").arg(path);
        return SnapshotStatus::Error;
    }

    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return SnapshotStatus::Error;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = expectedBytes;
    while (remaining > 0) {
        if (cancellationRequested(cancel)) return SnapshotStatus::Cancelled;
        const QByteArray bytes = file.read(qMin(HashChunkSize, remaining));
        if (bytes.isEmpty()) {
            if (error) {
                *error = file.error() == QFileDevice::NoError
                    ? QStringLiteral("Source became shorter while it was read: %1").arg(path)
                    : QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
            }
            return SnapshotStatus::Error;
        }
        hash.addData(bytes);
        remaining -= bytes.size();
    }
    const QByteArray unexpected = file.read(1);
    if (!unexpected.isEmpty()) {
        if (error) *error = QStringLiteral("Source grew while it was read: %1").arg(path);
        return SnapshotStatus::Error;
    }
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return SnapshotStatus::Error;
    }
    file.close();

    QString pathError;
    if (!validatePathChain(root, absolute, false, &pathError)) {
        if (error) *error = pathError;
        return SnapshotStatus::Error;
    }
    QFileInfo after(absolute);
    const QString afterCanonical = QDir::cleanPath(after.canonicalFilePath());
    if (after.isSymLink() || !after.exists() || !after.isFile()
        || !samePath(canonical, afterCanonical)
        || expectedBytes != after.size()
        || expectedModifiedMs != after.lastModified().toMSecsSinceEpoch()) {
        if (error) *error = QStringLiteral("Source changed while it was being inspected: %1").arg(path);
        return SnapshotStatus::Error;
    }
    if (snapshot) {
        snapshot->path = absolute;
        snapshot->canonical = canonical;
        snapshot->bytes = after.size();
        snapshot->modifiedMs = after.lastModified().toMSecsSinceEpoch();
        snapshot->sha256 = hash.result();
    }
    return SnapshotStatus::Ok;
}

QString deepestExistingAncestor(const QString &path)
{
    QString current = cleanAbsolutePath(path);
    for (;;) {
        if (QFileInfo::exists(current)) return current;
        const QString parent = QDir::cleanPath(QFileInfo(current).absolutePath());
        if (samePath(parent, current)) return QString();
        current = parent;
    }
}

bool storageIsKnownAndDifferent(const QString &source,
                                const QString &destinationAncestor)
{
    const QStorageInfo sourceStorage(QFileInfo(source).absolutePath());
    const QStorageInfo destinationStorage(destinationAncestor);
    if (!sourceStorage.isValid() || !sourceStorage.isReady()
        || !destinationStorage.isValid() || !destinationStorage.isReady()) {
        return false;
    }
    if (!sourceStorage.device().isEmpty()
        && !destinationStorage.device().isEmpty()) {
        return sourceStorage.device() != destinationStorage.device();
    }
    return !samePath(sourceStorage.rootPath(), destinationStorage.rootPath());
}

bool sameSnapshot(const FileSnapshot &left, const FileSnapshot &right)
{
    return samePath(left.path, right.path)
        && samePath(left.canonical, right.canonical)
        && left.bytes == right.bytes
        && left.modifiedMs == right.modifiedMs
        && left.sha256 == right.sha256;
}

bool safeRelativeDirectory(const QString &relative)
{
    if (QDir::isAbsolutePath(relative)) return false;
    QString normalized = relative;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString clean = QDir::cleanPath(normalized);
    if (clean.isEmpty() || clean == QStringLiteral(".")) return true;
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return clean == normalized && !parts.contains(QStringLiteral("."))
        && !parts.contains(QStringLiteral(".."));
}

QString describeRelative(const QString &root, const QString &path)
{
    return QDir::toNativeSeparators(QDir(root).relativeFilePath(path));
}

} // namespace

struct FlightLogOrganizer::PlanData {
    QString root;
    int candidates = 0;
    QVector<Entry> entries;
    QStringList warnings;
    QHash<QString, FileSnapshot> snapshots;
};

bool FlightLogOrganizer::Plan::isValid() const { return bool(d); }
QString FlightLogOrganizer::Plan::root() const { return d ? d->root : QString(); }
int FlightLogOrganizer::Plan::candidateCount() const { return d ? d->candidates : 0; }

const QVector<FlightLogOrganizer::Entry> &FlightLogOrganizer::Plan::entries() const
{
    static const QVector<Entry> empty;
    return d ? d->entries : empty;
}

const QStringList &FlightLogOrganizer::Plan::warnings() const
{
    static const QStringList empty;
    return d ? d->warnings : empty;
}

FlightLogOrganizer::Analysis FlightLogOrganizer::Analyze(
    const QString &requestedRoot, const Cancel &cancel,
    const Progress &progress)
{
    Analysis analysis;
    try {
        QString root;
        if (!validateRoot(requestedRoot, &root, &analysis.error)) return analysis;
        if (cancellationRequested(cancel)) {
            analysis.cancelled = true;
            return analysis;
        }

        QStringList discovered;
        QStringList candidates;
        QStringList warnings;
        int visited = 0;
        QDirIterator iterator(root,
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                | QDir::System,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            iterator.next();
            if (++visited > MaximumFiles) {
                analysis.error = QStringLiteral(
                    "The directory exceeds the %1-entry organizer limit.")
                                         .arg(MaximumFiles);
                return analysis;
            }
            const QFileInfo info = iterator.fileInfo();
            const QString absolute = cleanAbsolutePath(info.filePath());
            if (info.isSymLink()) {
                appendWarning(&warnings, QStringLiteral("Skipped symbolic link: %1")
                    .arg(describeRelative(root, absolute)));
                continue;
            }
            if (info.isDir()) {
                if (!info.isReadable()) {
                    appendWarning(&warnings,
                        QStringLiteral("Skipped unreadable directory: %1")
                            .arg(describeRelative(root, absolute)));
                }
                continue;
            }
            if (!info.isFile()) continue;
            QString pathError;
            if (!validatePathChain(root, absolute, false, &pathError)) {
                appendWarning(&warnings,
                    QStringLiteral("Skipped unsafe file: %1 (%2)")
                        .arg(describeRelative(root, absolute), pathError));
                continue;
            }
            discovered.append(absolute);
            if (isCandidateExtension(absolute)) candidates.append(absolute);
            if (discovered.size() > MaximumFiles
                || candidates.size() > MaximumFiles) {
                analysis.error = QStringLiteral(
                    "The directory exceeds the %1-file organizer limit.")
                                         .arg(MaximumFiles);
                return analysis;
            }
        }
        discovered.sort(pathCaseSensitivity());
        candidates.sort(pathCaseSensitivity());
        QHash<QString, QStringList> filesByDirectory;
        QHash<QString, QStringList> candidatesByStem;
        QHash<QString, QStringList> stemsByDirectory;
        QSet<QString> indexedStems;
        for (const QString &path : discovered) {
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            filesByDirectory[pathKey(QFileInfo(path).absolutePath())].append(path);
        }
        for (const QString &candidate : candidates) {
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            const QFileInfo info(candidate);
            const QString directoryKey = pathKey(info.absolutePath());
            const QString stemKey = pathKey(QDir(info.absolutePath())
                .filePath(info.completeBaseName()));
            candidatesByStem[stemKey].append(candidate);
            if (!indexedStems.contains(stemKey)) {
                indexedStems.insert(stemKey);
                stemsByDirectory[directoryKey].append(stemKey);
            }
        }

        auto data = std::make_shared<PlanData>();
        data->root = root;
        data->candidates = candidates.size();
        data->warnings = warnings;
        QHash<QString, int> sourceEntries;
        QHash<QString, QString> destinationSources;
        QSet<QString> secondaryCandidates;
        QSet<QString> suppressedCandidates;
        QSet<QString> conflictingStemGroups;
        QSet<QString> handledStemGroups;
        int companionMatches = 0;

        QStringList directoryKeys = stemsByDirectory.keys();
        directoryKeys.sort(pathCaseSensitivity());
        for (const QString &directoryKey : directoryKeys) {
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            QStringList stems = stemsByDirectory.value(directoryKey);
            stems.sort(pathCaseSensitivity());
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            for (int first = 0; first < stems.size();) {
                if (cancellationRequested(cancel)) {
                    analysis.cancelled = true;
                    return analysis;
                }
                int after = first + 1;
                while (after < stems.size()
                       && stems.at(after).startsWith(stems.at(first),
                                                    pathCaseSensitivity())) {
                    ++after;
                }
                if (after - first > 1) {
                    for (int at = first; at < after; ++at) {
                        conflictingStemGroups.insert(stems.at(at));
                        const QStringList members = candidatesByStem.value(stems.at(at));
                        for (const QString &member : members)
                            suppressedCandidates.insert(pathKey(member));
                    }
                    appendWarning(&data->warnings, QStringLiteral(
                        "Overlapping basename-prefix candidate groups were left wholly untouched: %1")
                            .arg(describeRelative(root, stems.at(first))));
                }
                first = after;
            }
        }

        // MP10's basename* companion rule makes same-stem logs one connected
        // group. Choose a stable primary instead of letting enumeration order
        // assign the group to unrelated classifications. A group mixing empty
        // and non-empty candidates is left wholly untouched: empty candidates
        // may only ever be explicit DeleteEmpty operations.
        const auto formatPriority = [](const QString &path) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("tlog")) return 0;
            if (suffix == QStringLiteral("rlog")) return 1;
            if (suffix == QStringLiteral("bin")) return 2;
            return 3;
        };
        for (const QString &candidate : candidates) {
            const QFileInfo info(candidate);
            const QString stemKey = pathKey(QDir(info.absolutePath())
                .filePath(info.completeBaseName()));
            if (handledStemGroups.contains(stemKey)) continue;
            handledStemGroups.insert(stemKey);
            if (conflictingStemGroups.contains(stemKey)) continue;
            QStringList group = candidatesByStem.value(stemKey);
            if (group.size() < 2) continue;
            bool hasEmpty = false;
            bool hasNonEmpty = false;
            for (const QString &member : group) {
                if (QFileInfo(member).size() == 0) hasEmpty = true;
                else hasNonEmpty = true;
            }
            if (hasEmpty && hasNonEmpty) {
                for (const QString &member : group)
                    suppressedCandidates.insert(pathKey(member));
                appendWarning(&data->warnings, QStringLiteral(
                    "Left mixed empty/non-empty basename group untouched to avoid deleting or relocating only part of it: %1")
                        .arg(describeRelative(root, candidate)));
                continue;
            }
            if (!hasNonEmpty) continue;
            std::sort(group.begin(), group.end(), [&](const QString &left,
                                                       const QString &right) {
                const int leftPriority = formatPriority(left);
                const int rightPriority = formatPriority(right);
                return leftPriority == rightPriority
                    ? left.compare(right, pathCaseSensitivity()) < 0
                    : leftPriority < rightPriority;
            });
            const QString primary = group.constFirst();
            for (int index = 1; index < group.size(); ++index)
                secondaryCandidates.insert(pathKey(group.at(index)));
            appendWarning(&data->warnings, QStringLiteral(
                "Multiple log candidates share one basename-prefix group; %1 is the deterministic primary and all listed companions follow its classification.")
                    .arg(describeRelative(root, primary)));
        }

        const auto addEntry = [&](Operation operation, const QString &source,
                                  const QString &destination,
                                  const FileSnapshot &snapshot,
                                  QString *error) -> bool {
            Entry entry;
            entry.operation = operation;
            entry.source = cleanAbsolutePath(source);
            entry.destination = destination.isEmpty()
                ? QString() : cleanAbsolutePath(destination);
            entry.bytes = snapshot.bytes;
            const QString sourceKey = pathKey(entry.source);
            if (sourceEntries.contains(sourceKey)) {
                const Entry &existing = data->entries.at(sourceEntries.value(sourceKey));
                if (existing.operation != entry.operation
                    || !samePath(existing.destination, entry.destination)) {
                    *error = QStringLiteral(
                        "Overlapping candidate groups assign conflicting operations to %1.")
                                     .arg(entry.source);
                    return false;
                }
                return true;
            }
            if (data->entries.size() >= MaximumFiles) {
                *error = QStringLiteral("The plan exceeds the %1-entry limit.")
                                     .arg(MaximumFiles);
                return false;
            }
            if (operation == Operation::Move) {
                if (entry.destination.isEmpty()
                    || !isWithin(root, entry.destination)) {
                    *error = QStringLiteral("Unsafe organizer destination for %1.")
                                     .arg(entry.source);
                    return false;
                }
                if (samePath(entry.source, entry.destination)) return true;
                const QString destinationKey = pathKey(entry.destination);
                if (destinationSources.contains(destinationKey)
                    && destinationSources.value(destinationKey) != sourceKey) {
                    *error = QStringLiteral("Two sources would overwrite the same destination: %1")
                                     .arg(entry.destination);
                    return false;
                }
                destinationSources.insert(destinationKey, sourceKey);
            }
            sourceEntries.insert(sourceKey, data->entries.size());
            data->entries.append(entry);
            data->snapshots.insert(sourceKey, snapshot);
            return true;
        };

        for (int index = 0; index < candidates.size(); ++index) {
            if (cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }
            if (progress) progress(qint64(index) * 1000,
                                   qint64(candidates.size()) * 1000);
            const QString candidate = candidates.at(index);
            if (suppressedCandidates.contains(pathKey(candidate))
                || secondaryCandidates.contains(pathKey(candidate))) {
                continue;
            }
            FileSnapshot before;
            QString snapshotError;
            SnapshotStatus status = snapshotFile(
                root, candidate, cancel, &before, &snapshotError);
            if (status == SnapshotStatus::Cancelled) {
                analysis.cancelled = true;
                return analysis;
            }
            if (status != SnapshotStatus::Ok) {
                analysis.error = snapshotError;
                return analysis;
            }

            // Keep one scale for the complete directory; a classifier reports
            // bytes, whereas this loop counts files. Reserve the end of the
            // current slot for its snapshot and companion checks.
            const Progress classifierProgress = progress ? Progress(
                [&](qint64 done, qint64 total) {
                    const qint64 fraction = total > 0 ? qint64(qBound(
                        0.0L, 999.0L * done / total, 999.0L)) : 0;
                    progress(qint64(index) * 1000 + fraction,
                             qint64(candidates.size()) * 1000);
                }) : Progress();
            const FlightLogClassifier::Result classified =
                FlightLogClassifier::Classify(candidate, cancel, classifierProgress);
            if (classified.cancelled || cancellationRequested(cancel)) {
                analysis.cancelled = true;
                return analysis;
            }

            FileSnapshot after;
            status = snapshotFile(root, candidate, cancel, &after,
                                  &snapshotError);
            if (status == SnapshotStatus::Cancelled) {
                analysis.cancelled = true;
                return analysis;
            }
            if (status != SnapshotStatus::Ok || !sameSnapshot(before, after)) {
                analysis.error = status == SnapshotStatus::Ok
                    ? QStringLiteral("Source changed during classification: %1")
                          .arg(candidate)
                    : snapshotError;
                return analysis;
            }

            // A malformed individual log does not make the whole read-only
            // inventory unusable. It remains untouched and is called out in
            // the explicit plan warnings. Snapshot/IO failures still abort.
            if (!classified.success) {
                appendWarning(&data->warnings,
                    QStringLiteral("No action was derived from unclassified file: %1 (%2)")
                        .arg(describeRelative(root, candidate),
                             classified.error.isEmpty()
                                 ? QStringLiteral("classification failed")
                                 : classified.error));
                continue;
            }
            for (const QString &warning : classified.warnings) {
                appendWarning(&data->warnings, QStringLiteral("%1: %2")
                    .arg(describeRelative(root, candidate), warning));
            }

            if (classified.disposition
                == FlightLogClassifier::Disposition::Leave) {
                continue;
            }
            if (classified.disposition
                == FlightLogClassifier::Disposition::DeleteEmpty) {
                if (after.bytes != 0) {
                    analysis.error = QStringLiteral(
                        "Classifier requested deletion of a non-empty file: %1")
                                             .arg(candidate);
                    return analysis;
                }
                if (!addEntry(Operation::DeleteEmpty, candidate, QString(),
                              after, &analysis.error)) {
                    return analysis;
                }
                continue;
            }
            if (!safeRelativeDirectory(classified.relativeDirectory)) {
                analysis.error = QStringLiteral(
                    "Classifier returned an unsafe destination directory for %1.")
                                         .arg(candidate);
                return analysis;
            }
            const QString destinationDirectory = classified.relativeDirectory.isEmpty()
                ? root : QDir(root).filePath(classified.relativeDirectory);
            QString destinationError;
            if (!validatePathChain(root, destinationDirectory, true,
                                   &destinationError)) {
                analysis.error = destinationError;
                return analysis;
            }

            const QFileInfo candidateInfo(candidate);
            const QString prefix = candidateInfo.completeBaseName();
            const QString absolutePrefix = QDir(candidateInfo.absolutePath())
                .filePath(prefix);
            const QStringList siblings = filesByDirectory.value(
                pathKey(candidateInfo.absolutePath()));
            const auto lessPath = [](const QString &left,
                                     const QString &right) {
                return left.compare(right, pathCaseSensitivity()) < 0;
            };
            auto sibling = std::lower_bound(siblings.cbegin(), siblings.cend(),
                                             absolutePrefix, lessPath);
            for (; sibling != siblings.cend()
                   && sibling->startsWith(absolutePrefix,
                                          pathCaseSensitivity()); ++sibling) {
                if (cancellationRequested(cancel)) {
                    analysis.cancelled = true;
                    return analysis;
                }
                if (++companionMatches > MaximumFiles) {
                    analysis.error = QStringLiteral(
                        "Overlapping basename-prefix groups exceed the %1-match safety limit.")
                                             .arg(MaximumFiles);
                    return analysis;
                }
                const QString &source = *sibling;
                const QFileInfo sourceInfo(source);
                FileSnapshot companion;
                if (samePath(source, candidate)) {
                    companion = after;
                } else {
                    status = snapshotFile(root, source, cancel, &companion,
                                          &snapshotError);
                    if (status == SnapshotStatus::Cancelled) {
                        analysis.cancelled = true;
                        return analysis;
                    }
                    if (status != SnapshotStatus::Ok) {
                        analysis.error = snapshotError;
                        return analysis;
                    }
                    appendWarning(&data->warnings, QStringLiteral(
                        "Reference basename-prefix companion included: %1 (from %2)")
                            .arg(describeRelative(root, source),
                                 describeRelative(root, candidate)));
                }
                const QString destination = QDir(destinationDirectory)
                    .filePath(sourceInfo.fileName());
                if (!addEntry(Operation::Move, source, destination, companion,
                              &analysis.error)) {
                    return analysis;
                }
            }
        }
        if (progress) progress(qint64(candidates.size()) * 1000,
                               qint64(candidates.size()) * 1000);
        if (cancellationRequested(cancel)) {
            analysis.cancelled = true;
            return analysis;
        }

        // Freeze every source after all classifiers and progress callbacks have
        // run, and reject collisions before exposing a valid plan.
        QSet<QString> sourceKeys;
        for (const Entry &entry : data->entries)
            sourceKeys.insert(pathKey(entry.source));
        for (const Entry &entry : data->entries) {
            const QString sourceKey = pathKey(entry.source);
            FileSnapshot finalSnapshot;
            QString snapshotError;
            const SnapshotStatus status = snapshotFile(
                root, entry.source, cancel, &finalSnapshot, &snapshotError);
            if (status == SnapshotStatus::Cancelled) {
                analysis.cancelled = true;
                return analysis;
            }
            if (status != SnapshotStatus::Ok
                || !sameSnapshot(data->snapshots.value(sourceKey),
                                 finalSnapshot)) {
                analysis.error = status == SnapshotStatus::Ok
                    ? QStringLiteral("Source changed before the plan was sealed: %1")
                          .arg(entry.source)
                    : snapshotError;
                return analysis;
            }
            if (entry.operation == Operation::Move) {
                const QFileInfo destination(entry.destination);
                if (destination.isSymLink() || destination.exists()) {
                    analysis.error = QStringLiteral(
                        "Destination already exists; nothing was changed: %1")
                                             .arg(entry.destination);
                    return analysis;
                }
                QString destinationError;
                if (!validatePathChain(root, destination.absolutePath(), true,
                                       &destinationError)) {
                    analysis.error = destinationError;
                    return analysis;
                }
                if (sourceKeys.contains(pathKey(entry.destination))) {
                    analysis.error = QStringLiteral(
                        "A planned destination is also a different planned source: %1")
                                             .arg(entry.destination);
                    return analysis;
                }
            }
        }

        analysis.plan.d = std::move(data);
        analysis.success = true;
        return analysis;
    } catch (const std::exception &exception) {
        analysis.error = QString::fromUtf8(exception.what());
    } catch (...) {
        analysis.error = QStringLiteral("Unexpected log organizer analysis error.");
    }
    return analysis;
}

FlightLogOrganizer::Result FlightLogOrganizer::Execute(
    const Plan &plan, const Cancel &cancel, const Progress &progress)
{
    Result result;
    // User callbacks are allowed to reset or replace the caller's Plan while
    // Execute is running. Keep the immutable backing store alive independently
    // of that const-reference alias for the complete operation.
    const std::shared_ptr<const PlanData> data = plan.d;
    if (!data) {
        result.error = QStringLiteral("A valid analyzed organizer plan is required.");
        return result;
    }
    result.warnings = data->warnings;
    result.remaining = data->entries.size();
    try {
        // No allocation for completion accounting may first occur after a
        // filesystem operation has already succeeded.
        result.completed.reserve(data->entries.size());
        QString currentRoot;
        if (!validateRoot(data->root, &currentRoot, &result.error)
            || !samePath(currentRoot, data->root)) {
            if (result.error.isEmpty())
                result.error = QStringLiteral("The analyzed log root changed.");
            return result;
        }
        if (data->entries.size() > MaximumFiles) {
            result.error = QStringLiteral("The organizer plan exceeds its safety limit.");
            return result;
        }
        if (cancellationRequested(cancel)) {
            result.cancelled = true;
            return result;
        }

        QSet<QString> sourceKeys;
        QSet<QString> destinationKeys;
        for (const Entry &entry : data->entries) {
            const QString sourceKey = pathKey(entry.source);
            if (sourceKeys.contains(sourceKey)
                || !data->snapshots.contains(sourceKey)) {
                result.error = QStringLiteral("Organizer plan contains a duplicate or unknown source.");
                return result;
            }
            sourceKeys.insert(sourceKey);
        }

        // Complete preflight: every source hash and every destination is
        // validated before the first directory or file is mutated.
        for (const Entry &entry : data->entries) {
            if (cancellationRequested(cancel)) {
                result.cancelled = true;
                return result;
            }
            const QString sourceKey = pathKey(entry.source);
            FileSnapshot current;
            QString snapshotError;
            const SnapshotStatus status = snapshotFile(
                currentRoot, entry.source, cancel, &current, &snapshotError);
            if (status == SnapshotStatus::Cancelled) {
                result.cancelled = true;
                return result;
            }
            if (status != SnapshotStatus::Ok
                || !sameSnapshot(data->snapshots.value(sourceKey), current)) {
                result.error = status == SnapshotStatus::Ok
                    ? QStringLiteral("Planned source changed; nothing was changed: %1")
                          .arg(entry.source)
                    : snapshotError;
                return result;
            }
            if (entry.operation == Operation::DeleteEmpty) {
                if (!entry.destination.isEmpty() || current.bytes != 0) {
                    result.error = QStringLiteral(
                        "Empty-file deletion precondition changed: %1")
                                             .arg(entry.source);
                    return result;
                }
                continue;
            }
            if (entry.operation != Operation::Move
                || entry.destination.isEmpty()
                || !isWithin(currentRoot, entry.destination)) {
                result.error = QStringLiteral("Organizer plan contains an unsafe move.");
                return result;
            }
            const QString destinationKey = pathKey(entry.destination);
            if (destinationKeys.contains(destinationKey)
                || sourceKeys.contains(destinationKey)) {
                result.error = QStringLiteral("Organizer destination collision: %1")
                                     .arg(entry.destination);
                return result;
            }
            destinationKeys.insert(destinationKey);
            const QFileInfo destination(entry.destination);
            if (destination.isSymLink() || destination.exists()) {
                result.error = QStringLiteral(
                    "Destination appeared after analysis; nothing was changed: %1")
                                         .arg(entry.destination);
                return result;
            }
            QString pathError;
            if (!validatePathChain(currentRoot, destination.absolutePath(),
                                   true, &pathError)) {
                result.error = pathError;
                return result;
            }
            const QString existingAncestor = deepestExistingAncestor(
                destination.absolutePath());
            if (existingAncestor.isEmpty()) {
                result.error = QStringLiteral(
                    "Cannot identify the destination filesystem for %1.")
                                     .arg(entry.destination);
                return result;
            }
            if (storageIsKnownAndDifferent(entry.source, existingAncestor)) {
                result.error = QStringLiteral(
                    "Cross-volume moves are refused before any changes: %1")
                                     .arg(entry.destination);
                return result;
            }
        }

        if (progress) progress(0, data->entries.size());
        for (const Entry &entry : data->entries) {
            if (cancellationRequested(cancel)) {
                result.cancelled = true;
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }
            QString rootError;
            QString verifiedRoot;
            if (!validateRoot(currentRoot, &verifiedRoot, &rootError)
                || !samePath(verifiedRoot, currentRoot)) {
                result.error = rootError.isEmpty()
                    ? QStringLiteral("The log root changed during execution.")
                    : rootError;
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }
            const QString sourceKey = pathKey(entry.source);
            FileSnapshot current;
            QString snapshotError;
            const SnapshotStatus status = snapshotFile(
                currentRoot, entry.source, cancel, &current, &snapshotError);
            if (status == SnapshotStatus::Cancelled) {
                result.cancelled = true;
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }
            if (status != SnapshotStatus::Ok
                || !sameSnapshot(data->snapshots.value(sourceKey), current)) {
                result.error = status == SnapshotStatus::Ok
                    ? QStringLiteral("Planned source changed during execution: %1")
                          .arg(entry.source)
                    : snapshotError;
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }
            if (cancellationRequested(cancel)) {
                result.cancelled = true;
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }

            bool applied = false;
            if (entry.operation == Operation::DeleteEmpty) {
                // The final cancellation callback may itself change the file.
                // Recheck the empty-file precondition after every callback;
                // none may run between this fresh check and remove(). This
                // does not claim exclusion against an external filesystem race.
                QString pathError;
                const bool safePath = validatePathChain(
                    currentRoot, entry.source, false, &pathError);
                const QFileInfo fresh(entry.source);
                if (!safePath || current.bytes != 0 || !fresh.isFile()
                    || fresh.isSymLink() || fresh.size() != 0
                    || !samePath(fresh.canonicalFilePath(), current.canonical)
                    || fresh.lastModified().toMSecsSinceEpoch() != current.modifiedMs) {
                    result.error = QStringLiteral("Refusing to delete a changed or non-empty file: %1")
                                         .arg(entry.source);
                    result.remaining = data->entries.size()
                        - result.completed.size();
                    return result;
                }
                applied = QFile::remove(entry.source);
            } else {
                const QFileInfo destination(entry.destination);
                QString pathError;
                if (destination.isSymLink() || destination.exists()
                    || !validatePathChain(currentRoot,
                                          destination.absolutePath(), true,
                                          &pathError)) {
                    result.error = pathError.isEmpty()
                        ? QStringLiteral("Move destination is no longer available: %1")
                              .arg(entry.destination)
                        : pathError;
                    result.remaining = data->entries.size()
                        - result.completed.size();
                    return result;
                }
                if (!QDir().mkpath(destination.absolutePath())
                    || !validatePathChain(currentRoot,
                                          destination.absolutePath(), false,
                                          &pathError)) {
                    result.error = pathError.isEmpty()
                        ? QStringLiteral("Cannot create a safe destination directory: %1")
                              .arg(destination.absolutePath())
                        : pathError;
                    result.remaining = data->entries.size()
                        - result.completed.size();
                    return result;
                }
                if (storageIsKnownAndDifferent(entry.source,
                                               destination.absolutePath())) {
                    result.error = QStringLiteral(
                        "Destination storage changed; cross-volume move refused: %1")
                                             .arg(entry.destination);
                    result.remaining = data->entries.size()
                        - result.completed.size();
                    return result;
                }
                if (cancellationRequested(cancel)) {
                    result.cancelled = true;
                    result.remaining = data->entries.size()
                        - result.completed.size();
                    return result;
                }
                // Re-stat after the final callback just as for deletion. The
                // source must still match the verified snapshot immediately
                // before the rename (external filesystem races stay unlocked).
                const bool safeSource = validatePathChain(
                    currentRoot, entry.source, false, &pathError);
                const QFileInfo freshSource(entry.source);
                if (!safeSource || !freshSource.isFile() || freshSource.isSymLink()
                    || freshSource.size() != current.bytes
                    || !samePath(freshSource.canonicalFilePath(), current.canonical)
                    || freshSource.lastModified().toMSecsSinceEpoch() != current.modifiedMs) {
                    result.error = QStringLiteral("Refusing to move a changed source file: %1")
                                       .arg(entry.source);
                    return result;
                }
                applied = QFile::rename(entry.source, entry.destination);
            }
            if (!applied) {
                result.error = entry.operation == Operation::DeleteEmpty
                    ? QStringLiteral("Cannot delete the confirmed empty file: %1")
                          .arg(entry.source)
                    : QStringLiteral("Cannot move %1 to %2 without overwriting.")
                          .arg(entry.source, entry.destination);
                result.remaining = data->entries.size()
                    - result.completed.size();
                return result;
            }
            result.completed.append(entry);
            result.remaining = data->entries.size()
                - result.completed.size();
            if (progress)
                progress(result.completed.size(), data->entries.size());
        }
        result.success = true;
        result.remaining = 0;
        return result;
    } catch (const std::exception &exception) {
        result.error = QString::fromUtf8(exception.what());
    } catch (...) {
        result.error = QStringLiteral("Unexpected log organizer execution error.");
    }
    result.remaining = data->entries.size() - result.completed.size();
    return result;
}
