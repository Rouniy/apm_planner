#include "SftpLogDownloadSupport.h"
#include "ui/Loghandling/DataFlashBinToLogConverter.h"
#include "ui/Loghandling/DataFlashKmlExporter.h"
#include "ui/Loghandling/DataFlashRawReader.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QUuid>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
using Support = SftpLogDownloadSupport;
bool cancelled(const Support::Cancel &cancel) { return cancel && cancel(); }
bool occupied(const QString &path) { const QFileInfo f(path); return f.exists() || f.isSymLink(); }

struct FileIdentity {
    QString canonicalPath;
    QDateTime born;
#ifdef Q_OS_UNIX
    quint64 device = 0;
    quint64 inode = 0;
#endif
    bool valid = false;

    static FileIdentity capture(const QString &path, bool directory = false) {
        FileIdentity result;
        const QFileInfo info(path);
        if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile())) return result;
        result.canonicalPath = info.canonicalFilePath();
        result.born = info.birthTime();
        if (result.canonicalPath != QDir::cleanPath(path)) return result;
#ifdef Q_OS_UNIX
        struct stat status {};
        const QByteArray encoded = QFile::encodeName(path);
        if (::lstat(encoded.constData(), &status) != 0
            || (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode))) return result;
        result.device = quint64(status.st_dev);
        result.inode = quint64(status.st_ino);
#else
        if (!result.born.isValid()) return result;
#endif
        result.valid = true;
        return result;
    }

    bool matches(const QString &path, bool directory = false) const {
        if (!valid) return false;
        const FileIdentity current = capture(path, directory);
        if (!current.valid || current.canonicalPath != canonicalPath) return false;
#ifdef Q_OS_UNIX
        return current.device == device && current.inode == inode
            && (!born.isValid() || !current.born.isValid() || current.born == born);
#else
        return current.born == born;
#endif
    }

    bool sameObject(const FileIdentity &other) const {
        if (!valid || !other.valid) return false;
#ifdef Q_OS_UNIX
        return device == other.device && inode == other.inode
            && (!born.isValid() || !other.born.isValid() || born == other.born);
#else
        return born.isValid() && born == other.born;
#endif
    }
};

struct FileSnapshot {
    FileIdentity identity;
    qint64 size = -1;
    QDateTime modifiedUtc;
    bool valid = false;

    static FileSnapshot capture(const QString &path) {
        FileSnapshot result;
        result.identity = FileIdentity::capture(path);
        if (!result.identity.valid) return result;
        const QFileInfo info(path);
        result.size = info.size();
        result.modifiedUtc = info.lastModified().toUTC();
        result.valid = result.size >= 0;
        return result;
    }

    bool matches(const QString &path) const {
        if (!valid || !identity.matches(path)) return false;
        const QFileInfo info(path);
        return info.size() == size && info.lastModified().toUTC() == modifiedUtc;
    }
};

struct Root {
    QString path;
    FileIdentity identity;
    bool current() const {
        return identity.matches(path, true);
    }
};

class NameAllocator final {
public:
    QString next(const Root &root, const QString &name, bool companions) {
        const QFileInfo file(name);
        const QString key = name.toCaseFolded() + (companions ? QStringLiteral("|bundle")
                                                              : QStringLiteral("|single"));
        int &suffix = m_nextSuffix[key];
        while (suffix < 10000) {
            const int candidateSuffix = suffix++;
            const QString stem = file.completeBaseName()
                + (candidateSuffix ? QLatin1Char('-') + QString::number(candidateSuffix)
                                   : QString());
            const QString result = QDir(root.path).filePath(
                stem + QLatin1Char('.') + file.suffix());
            if (!occupied(result) && (!companions
                || (!occupied(QDir(root.path).filePath(stem + QStringLiteral(".log")))
                    && !occupied(QDir(root.path).filePath(stem + QStringLiteral(".kml")))))) {
                return result;
            }
        }
        return {};
    }

private:
    QHash<QString, int> m_nextSuffix;
};

bool publishNoReplace(const QString &source, const QString &target,
                      const FileIdentity &expected, QString *error)
{
#ifdef Q_OS_LINUX
    const QByteArray sourceName = QFile::encodeName(source);
    const int descriptor = ::open(sourceName.constData(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        *error = QStringLiteral("Could not pin the owned local source for publication.");
        return false;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)
        || quint64(status.st_dev) != expected.device
        || quint64(status.st_ino) != expected.inode) {
        ::close(descriptor);
        *error = QStringLiteral("The owned local source changed before publication.");
        return false;
    }
    const QByteArray descriptorName = QByteArrayLiteral("/proc/self/fd/")
        + QByteArray::number(descriptor);
    const QByteArray targetName = QFile::encodeName(target);
    const int linked = ::linkat(AT_FDCWD, descriptorName.constData(),
                                AT_FDCWD, targetName.constData(),
                                AT_SYMLINK_FOLLOW);
    ::close(descriptor);
    if (linked != 0) {
        if (!occupied(target)) *error = QStringLiteral("Atomic local-file publication failed.");
        return false;
    }
    return true;
#elif defined(Q_OS_UNIX)
    Q_UNUSED(expected)
    const QByteArray sourceName = QFile::encodeName(source);
    const QByteArray targetName = QFile::encodeName(target);
    if (::link(sourceName.constData(), targetName.constData()) != 0) {
        if (!occupied(target)) *error = QStringLiteral("Atomic local-file publication failed.");
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    Q_UNUSED(expected)
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(target.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        if (!occupied(target)) *error = QStringLiteral("Atomic local-file publication failed.");
        return false;
    }
    return true;
#else
    Q_UNUSED(source)
    Q_UNUSED(target)
    Q_UNUSED(expected)
    *error = QStringLiteral("Atomic no-overwrite publication is unavailable on this platform.");
    return false;
#endif
}

QString publish(const Root &root, const QString &source,
                FileSnapshot *ownedSource, const QString &name,
                bool companions, NameAllocator *allocator,
                bool *sourceRetired, QString *error)
{
    if (sourceRetired) *sourceRetired = false;
    if (!ownedSource || !ownedSource->matches(source) || !allocator) {
        *error = QStringLiteral("The owned local source changed before publication.");
        return {};
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!root.current() || !ownedSource->matches(source)) {
            *error = QStringLiteral("The selected output directory or owned source changed.");
            return {};
        }
        const QString target = allocator->next(root, name, companions);
        if (target.isEmpty()) break;
        QString publicationError;
        if (!publishNoReplace(source, target, ownedSource->identity,
                              &publicationError)) {
            if (occupied(target)) continue;
            *error = publicationError;
            return {};
        }
        const FileSnapshot targetSnapshot = FileSnapshot::capture(target);
        if (!targetSnapshot.valid
            || !targetSnapshot.identity.sameObject(ownedSource->identity)
            || targetSnapshot.size != ownedSource->size
            || targetSnapshot.modifiedUtc != ownedSource->modifiedUtc) {
            *error = QStringLiteral("Published local-file identity did not match its owned source.");
            return {};
        }
#ifdef Q_OS_UNIX
        if (ownedSource->matches(source) && QFile::remove(source)
            && sourceRetired) *sourceRetired = true;
#else
        if (sourceRetired) *sourceRetired = true;
#endif
        *ownedSource = targetSnapshot;
        return target;
    }
    *error = QStringLiteral("Could not publish the local file without overwriting another file.");
    return {};
}
double number(const QByteArray &raw, const DataFlashRaw::Definition &d, const QByteArray &label)
{
    const int index = d.columns.indexOf(label);
    if (index < 0) return std::numeric_limits<double>::quiet_NaN();
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + d.offsets.at(index));
    switch (d.format.at(index)) {
    case 'B': case 'M': return *at;
    case 'b': return qint8(*at);
    case 'H': return qFromLittleEndian<quint16>(at);
    case 'h': return qFromLittleEndian<qint16>(at);
    case 'I': return qFromLittleEndian<quint32>(at);
    case 'i': return qFromLittleEndian<qint32>(at);
    case 'Q': return double(qFromLittleEndian<quint64>(at));
    case 'q': return double(qFromLittleEndian<qint64>(at));
    case 'L': return qFromLittleEndian<qint32>(at) / 10000000.0;
    case 'e': return qFromLittleEndian<qint32>(at) / 100.0;
    case 'E': return qFromLittleEndian<quint32>(at) / 100.0;
    case 'c': return qFromLittleEndian<qint16>(at) / 100.0;
    case 'C': return qFromLittleEndian<quint16>(at) / 100.0;
    case 'f': { const auto bits = qFromLittleEndian<quint32>(at); float v; std::memcpy(&v, &bits, 4); return v; }
    case 'd': { const auto bits = qFromLittleEndian<quint64>(at); double v; std::memcpy(&v, &bits, 8); return v; }
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}
double firstNumber(const QByteArray &raw, const DataFlashRaw::Definition &d,
                   const QByteArray &first, const QByteArray &second)
{
    return number(raw, d, d.columns.contains(first) ? first : second);
}

double boardTimeMs(const QByteArray &raw, const DataFlashRaw::Definition &d)
{
    if (d.columns.contains("TimeUS")) {
        const double timeUs = number(raw, d, "TimeUS");
        return std::isfinite(timeUs) ? timeUs / 1000.0
                                     : std::numeric_limits<double>::quiet_NaN();
    }
    if (d.columns.contains("T")) return number(raw, d, "T");
    return std::numeric_limits<double>::quiet_NaN();
}

QDateTime firstGpsTime(const QString &path, const Support::Cancel &cancel, QString *warning)
{
    if (warning) warning->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (warning) *warning = file.errorString();
        return {};
    }
    DataFlashRaw::Reader reader(&file, false);
    QByteArray raw; int kind = -1;
    QDateTime anchor;
    double anchorBoardMs = std::numeric_limits<double>::quiet_NaN();
    while (reader.next(&raw, &kind)) {
        if (cancelled(cancel)) return {};
        const auto &d = reader.currentDefinition;
        if (kind != -1 || !d.name.startsWith("GPS")) continue;
        const double status = number(raw, d, "Status");
        if (!std::isfinite(status) || status < 3) continue;
        if (!anchor.isValid()) {
            const double week = firstNumber(raw, d, "Week", "GWk");
            const double gpsWeekMs = firstNumber(raw, d, "TimeMS", "GMS");
            if (std::isfinite(week) && week >= 0 && week <= 5000 && week == std::trunc(week)
                && std::isfinite(gpsWeekMs) && gpsWeekMs >= 0
                && gpsWeekMs <= 604800000) {
                // MP10's DFLog uses the current GPS leap offset, not the date
                // of an old log. Eighteen seconds is its contemporary value.
                anchor = QDateTime(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC)
                    .addDays(qint64(week) * 7)
                    .addMSecs(qint64(gpsWeekMs) - 18000);
                anchorBoardMs = boardTimeMs(raw, d);
            }
        }
        if (d.name != "GPS") continue;
        const double lat = number(raw, d, "Lat"), lon = number(raw, d, "Lng"), alt = number(raw, d, "Alt");
        if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(alt)
            || lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) continue;
        const double timeMs = boardTimeMs(raw, d);
        if (anchor.isValid() && anchor.date().year() >= 1980
            && std::isfinite(timeMs) && std::isfinite(anchorBoardMs)
            && std::abs(timeMs - anchorBoardMs) < 315576000000.0) {
            const QDateTime result = anchor.addMSecs(qint64(timeMs - anchorBoardMs));
            if (result.isValid() && result.date().year() >= 1980) return result;
        }
        if (warning) {
            *warning = QStringLiteral(
                "First GPS track point has no valid post-1980 UTC anchor and board-time delta; retained remote filename.");
        }
        return {};
    }
    if (!reader.error.isEmpty() && warning) *warning = reader.error;
    return {};
}
}

QString SftpLogDownloadSupport::safeBinName(const QString &remoteName, int fallbackIndex)
{
    QString name = remoteName;
    name.replace(QChar(0), '_'); name.replace('\\', '/'); name = name.section('/', -1);
    for (int i = 0; i < name.size(); ++i) {
        const auto c = name.at(i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) name[i] = '_';
    }
    if (name.isEmpty() || name == "." || name == "..") name = "log_" + QString::number(qMax(0, fallbackIndex)) + ".bin";
    if (name.startsWith('.') || name.startsWith('-')) {
        while (name.startsWith('.') || name.startsWith('-')) name.remove(0, 1);
        name.prepend("log_");
    }
    if (!name.endsWith(".bin", Qt::CaseInsensitive)) name += ".bin";
    if (name.size() > 100) name = name.left(96) + ".bin";
    const QString stem = name.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
        || (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
            && stem.at(3) >= '1' && stem.at(3) <= '9')) name = "log_" + name;
    return name;
}

SftpLogDownloadSupport::Result SftpLogDownloadSupport::download(
    SftpLogSession &session, const QVector<SftpLogEntry> &entries,
    const QString &destination, bool createKml, Cancel cancel, Progress progress)
{
    Result result;
    const auto stopped = [&] {
        if (!cancelled(cancel)) return false;
        result.cancelled = true; result.error = QStringLiteral("SFTP download cancelled; completed files retained.");
        return true;
    };
    if (stopped()) return result;
    const QFileInfo chosen(destination);
    if (!chosen.isDir() || chosen.isSymLink()) {
        result.error = QStringLiteral("Select an existing ordinary local destination directory."); return result;
    }
    const QString canonicalRoot = chosen.canonicalFilePath();
    Root root{canonicalRoot, FileIdentity::capture(canonicalRoot, true)};
    if (!root.current() || entries.isEmpty() || entries.size() > SftpLogSession::MaximumEntries) {
        result.error = QStringLiteral("Invalid destination or remote selection."); return result;
    }
    qint64 total = 0;
    for (const auto &entry : entries) {
        if (!SftpLogSession::isSafeName(entry.name) || !entry.name.endsWith(".bin", Qt::CaseInsensitive)
            || SftpLogSession::normalizeDirectory(entry.remoteDirectory).isEmpty()
            || entry.length < 0 || entry.length > SftpLogSession::MaximumFileBytes
            || total > std::numeric_limits<qint64>::max() - entry.length) {
            result.error = QStringLiteral("Unsafe or oversized remote log selection."); return result;
        }
        total += entry.length;
    }
    QTemporaryDir staging(QDir(root.path).filePath(QStringLiteral(".apm-sftp-XXXXXX")));
    staging.setAutoRemove(false); // Never recursively delete foreign additions.
    if (!staging.isValid()) { result.error = QStringLiteral("Cannot create local staging directory."); return result; }
    const Root stage{staging.path(), FileIdentity::capture(staging.path(), true)};
    struct Cleanup {
        struct OwnedFile {
            FileIdentity identity;
#ifdef Q_OS_LINUX
            int descriptor = -1;
#endif
            bool matches(const QString &path) const {
                if (!identity.matches(path)) return false;
#ifdef Q_OS_LINUX
                struct stat status {};
                return descriptor >= 0 && ::fstat(descriptor, &status) == 0
                    && S_ISREG(status.st_mode)
                    && quint64(status.st_dev) == identity.device
                    && quint64(status.st_ino) == identity.inode;
#else
                return true;
#endif
            }
            void closePin() {
#ifdef Q_OS_LINUX
                if (descriptor >= 0) ::close(descriptor);
                descriptor = -1;
#endif
            }
        };
        Root stage; Result *result; QHash<QString, OwnedFile> files; bool done = false;
        bool track(const QString &path) {
            OwnedFile owned;
            owned.identity = FileIdentity::capture(path);
            if (!owned.identity.valid) {
                result->warnings.append("Could not pin cleanup ownership for: " + path);
                return false;
            }
#ifdef Q_OS_LINUX
            const QByteArray encoded = QFile::encodeName(path);
            owned.descriptor = ::open(encoded.constData(),
                                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat status {};
            if (owned.descriptor < 0 || ::fstat(owned.descriptor, &status) != 0
                || !S_ISREG(status.st_mode)
                || quint64(status.st_dev) != owned.identity.device
                || quint64(status.st_ino) != owned.identity.inode) {
                owned.closePin();
                result->warnings.append("Could not pin cleanup ownership for: " + path);
                return false;
            }
#endif
            files.insert(path, owned);
            return true;
        }
        void release(const QString &path) {
            auto iterator = files.find(path);
            if (iterator == files.end()) return;
            iterator.value().closePin();
            files.erase(iterator);
        }
        void run() {
            if (done) return;
            done = true;
            if (!stage.current()) {
                for (auto iterator = files.begin(); iterator != files.end(); ++iterator)
                    iterator.value().closePin();
                result->warnings.append("Staging directory changed; cleanup refused.");
                return;
            }
            for (auto iterator = files.begin(); iterator != files.end(); ++iterator) {
                if (iterator.value().matches(iterator.key())) {
                    if (!QFile::remove(iterator.key()))
                        result->warnings.append("Could not remove owned partial file: " + iterator.key());
                } else if (occupied(iterator.key())) {
                    result->warnings.append("Partial-file identity changed; cleanup refused: " + iterator.key());
                }
                iterator.value().closePin();
            }
            if (!stage.current()) {
                result->warnings.append("Staging directory identity changed before removal; cleanup refused.");
            } else if (!QDir().rmdir(stage.path)) {
                result->warnings.append("Staging directory retained: " + stage.path);
            }
        }
        ~Cleanup() { run(); }
    } cleanup{stage, &result};
    NameAllocator allocator;
    qint64 completed = 0;
    const auto perform = [&]() -> Result {
    for (int i = 0; i < entries.size(); ++i) {
        if (stopped()) return result;
        const auto &entry = entries.at(i);
        if (!root.current() || !stage.current()) { result.error = QStringLiteral("Local output directory changed."); return result; }
        const QStorageInfo storage(root.path);
        if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0
            && entry.length > storage.bytesAvailable()) {
            result.error = QStringLiteral("Insufficient local disk space for the listed BIN log."); return result;
        }
        const QString binStage = staging.filePath(QString::number(i) + ".bin");
        QFile output(binStage);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { result.error = output.errorString(); return result; }
        if (!cleanup.track(binStage)) {
            output.close();
            result.error = QStringLiteral("Could not establish ownership of the local partial file.");
            return result;
        }
        qint64 copied = 0;
        const bool received = session.download(entry, &output, &copied, &result.error, cancel,
            [&](qint64 bytes) { if (progress) progress(completed + bytes, total, "Downloading " + entry.name); });
        const bool flushed = output.flush(); output.close();
        if (stopped()) return result;
        if (!received || !flushed) {
            if (result.error.isEmpty()) result.error = output.errorString();
            return result;
        }
        if (copied < 0 || copied > SftpLogSession::MaximumFileBytes || QFileInfo(binStage).size() != copied) {
            result.error = QStringLiteral("Downloaded byte count does not match the local BIN file."); return result;
        }
        FileSnapshot binSnapshot = FileSnapshot::capture(binStage);
        bool binStageRetired = false;
        QString bin = publish(root, binStage, &binSnapshot,
                              safeBinName(entry.name, i + 1), true,
                              &allocator, &binStageRetired, &result.error);
        if (bin.isEmpty()) return result;
        if (binStageRetired) cleanup.release(binStage);
        ++result.savedLogs; result.publishedPaths.append(bin);
        if (copied != entry.length) result.warnings.append(entry.name + ": listed/received sizes differ; remote log may still be active.");
        if (progress) progress(completed + copied, total, "Processing " + entry.name);
        if (stopped()) return result;
        QString timeWarning;
        const auto date = firstGpsTime(bin, cancel, &timeWarning);
        if (!timeWarning.isEmpty()) result.warnings.append(entry.name + ": GPS timestamp: " + timeWarning);
        if (stopped()) return result;
        if (!binSnapshot.matches(bin)) {
            result.error = QStringLiteral(
                "A completed local BIN changed during timestamp inspection; derived files were not created.");
            return result;
        }
        if (date.isValid()) {
            const QString dated = date.toLocalTime().toString("yyyy-MM-dd HH-mm-ss") + ".bin";
            if (QFileInfo(bin).fileName() != dated) {
                const QString previousBin = bin;
                bool previousRetired = false;
                const auto renamed = publish(root, bin, &binSnapshot, dated,
                                             true, &allocator, &previousRetired,
                                             &result.error);
                if (renamed.isEmpty()) return result;
                if (previousRetired) result.publishedPaths.removeAll(previousBin);
                else result.warnings.append(
                    entry.name + ": timestamped BIN was published, but its previous owned name could not be retired.");
                bin = renamed; result.publishedPaths.append(bin);
            }
        }
        const QString stem = QFileInfo(bin).completeBaseName();
        const QString logStage = staging.filePath(QString::number(i) + ".log");
        const auto converted = DataFlashBinToLogConverter::Convert(bin, logStage, cancel);
        if (converted.success) {
            if (!cleanup.track(logStage)) {
                result.error = QStringLiteral("Could not establish ownership of the converted LOG staging file.");
                return result;
            }
            if (!binSnapshot.matches(bin)) {
                result.error = QStringLiteral(
                    "A completed local BIN changed during LOG conversion; the derived file was not published.");
                return result;
            }
            FileSnapshot logSnapshot = FileSnapshot::capture(logStage);
            bool logStageRetired = false;
            const auto log = publish(root, logStage, &logSnapshot,
                                     stem + ".log", false, &allocator,
                                     &logStageRetired, &result.error);
            if (log.isEmpty()) return result;
            if (logStageRetired) cleanup.release(logStage);
            result.publishedPaths.append(log);
        } else if (converted.cancelled) {
            result.cancelled = true;
            result.error = QStringLiteral("SFTP download cancelled; completed files retained.");
            return result;
        }
        else result.warnings.append(entry.name + ": BIN-to-LOG failed: " + converted.error);
        result.warnings.append(converted.warnings);
        if (stopped()) return result;
        if (createKml) {
            if (!binSnapshot.matches(bin)) {
                result.error = QStringLiteral(
                    "A completed local BIN changed before KML export; no KML was created.");
                return result;
            }
            QString kmlStage;
            for (int attempt = 0; attempt < 100 && kmlStage.isEmpty(); ++attempt) {
                const QString candidate = staging.filePath(
                    QStringLiteral(".kml-%1.kml").arg(
                        QUuid::createUuid().toString(QUuid::WithoutBraces)));
                if (!occupied(candidate)) kmlStage = candidate;
            }
            if (kmlStage.isEmpty()) {
                result.error = QStringLiteral("Could not allocate a private KML staging path.");
                return result;
            }
            const auto kml = DataFlashKmlExporter::Export(bin, kmlStage, cancel);
            if (kml.succeeded) {
                if (!cleanup.track(kmlStage)) {
                    result.error = QStringLiteral("Could not establish ownership of the KML staging file.");
                    return result;
                }
                if (!binSnapshot.matches(bin)) {
                    result.error = QStringLiteral(
                        "A completed local BIN changed during KML export; the derived file was not published.");
                    return result;
                }
                FileSnapshot kmlSnapshot = FileSnapshot::capture(kmlStage);
                bool kmlStageRetired = false;
                const auto published = publish(root, kmlStage, &kmlSnapshot,
                                               stem + ".kml", false,
                                               &allocator, &kmlStageRetired,
                                               &result.error);
                if (published.isEmpty()) return result;
                if (kmlStageRetired) cleanup.release(kmlStage);
                result.publishedPaths.append(published);
            } else if (kml.cancelled) { result.cancelled = true; result.error = kml.error; return result; }
            else result.warnings.append(entry.name + ": KML failed: " + kml.error);
        }
        completed += copied;
        if (progress) progress(completed, total, "Saved " + entry.name);
        if (stopped()) return result;
    }
    result.success = true;
    return result;
    };
    result = perform();
    cleanup.run();
    return result;
}
