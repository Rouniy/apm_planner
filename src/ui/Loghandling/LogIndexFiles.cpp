#include "LogIndexFiles.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {
constexpr int MaximumDirectories = 20000, MaximumDepth = 64, MaximumEntries = 200000;
constexpr qint64 MaximumImageBytes = 20 * 1024 * 1024, MaximumPixels = 4 * 1024 * 1024;
constexpr int Width = 240, Height = 140;
constexpr double Pi = 3.14159265358979323846, World = 40075016.68557849;
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif
QString absolute(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
bool same(const QString &a, const QString &b) { return a.compare(b, PathCase) == 0; }
QString key(const QString &path) { return PathCase == Qt::CaseInsensitive ? path.toCaseFolded() : path; }
bool cancelled(const LogIndex::Cancel &cancel) { return cancel && cancel(); }
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
void warn(QStringList *warnings, const QString &text) {
    if (warnings->size() < 1000) warnings->append(text);
    else if (warnings->size() == 1000) warnings->append(QStringLiteral("Additional index warnings omitted."));
}
bool rootPath(const QString &requested, QString *result, QString *error) {
    if (requested.trimmed().isEmpty() || requested.contains(QChar('\0')))
        return fail(error, QStringLiteral("Choose an existing log directory."));
    const QString path = absolute(requested);
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!info.exists() || !info.isDir() || info.isSymLink() || canonical.isEmpty())
        return fail(error, QStringLiteral("The indexed root must be an existing ordinary directory, not a symbolic link: %1").arg(path));
    // A normal selected directory may have a native ancestor alias, e.g.
    // macOS /var -> /private/var. Resolve that spelling once at admission;
    // Entry/Plan paths and all subsequent containment checks stay canonical.
    if (result) *result = QDir::cleanPath(canonical);
    return true;
}
LogIndex::FileStamp stamp(const QFileInfo &info) {
    LogIndex::FileStamp result;
    result.exists = info.exists();
    if (result.exists) { result.sizeBytes = info.size(); result.modifiedUtc = info.lastModified().toUTC(); }
    return result;
}
// Existing ancestors must stay ordinary directories. Only the final leaf may
// be absent; dangling symlinks and directories masquerading as files fail.
bool inspect(const QString &root, const QString &requested, LogIndex::FileStamp *out, QString *error) {
    QString checked;
    if (!rootPath(root, &checked, error) || !same(checked, root)) return false;
    if (requested.isEmpty() || requested.contains(QChar('\0')))
        return fail(error, QStringLiteral("An indexed path is empty or invalid."));
    const QString path = absolute(requested);
    QString prefix = root; if (!prefix.endsWith('/')) prefix += '/';
    if (!path.startsWith(prefix, PathCase))
        return fail(error, QStringLiteral("Path escapes the indexed root: %1").arg(path));
    const QStringList parts = QDir(root).relativeFilePath(path).split('/');
    QString current = root;
    for (int i = 0; i < parts.size(); ++i) {
        current = QDir(current).filePath(parts.at(i));
        const QFileInfo info(current);
        const bool leaf = i == parts.size() - 1;
        if (info.isSymLink() || (info.exists() && !same(current, info.canonicalFilePath()))
            || (!leaf && (!info.exists() || !info.isDir()))
            || (leaf && info.exists() && !info.isFile()))
            return fail(error, QStringLiteral("Linked, missing-parent or non-file path refused: %1").arg(current));
        if (leaf && out) *out = stamp(info);
    }
    return true;
}
bool candidate(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "bin" || suffix == "log" || suffix == "tlog";
}
QString rlogPath(const QString &source) {
    return source.left(source.size() - QFileInfo(source).suffix().size()) + QStringLiteral("rlog");
}
struct Target { QString path; LogIndex::FileStamp expected; };
QVector<Target> targets(const LogIndex::Entry &entry) {
    QVector<Target> result{{entry.fullPath, entry.source}, {entry.fullPath + ".jpg", entry.thumbnail}};
    if (QFileInfo(entry.fullPath).suffix().compare("tlog", Qt::CaseInsensitive) == 0)
        result.append({rlogPath(entry.fullPath), entry.pairedRlog});
    return result;
}
bool matches(const QString &root, const Target &target, QString *error) {
    LogIndex::FileStamp current;
    if (!inspect(root, target.path, &current, error)) return false;
    if (current != target.expected)
        return fail(error, QStringLiteral("File changed, disappeared or appeared after indexing; rescan: %1").arg(target.path));
    return true;
}
bool entryMatches(const LogIndex::Entry &entry, bool companions, QString *error) {
    if (!entry.source.exists || !candidate(entry.fullPath)
        || !same(entry.fullPath, absolute(entry.fullPath)))
        return fail(error, QStringLiteral("Invalid indexed source: %1").arg(entry.fullPath));
    const auto files = targets(entry);
    for (int i = 0; i < (companions ? files.size() : 1); ++i)
        if (!matches(entry.rootPath, files.at(i), error)) return false;
    return true;
}
QImage decode(const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > MaximumImageBytes) return {};
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096
        || qint64(size.width()) * size.height() > MaximumPixels) return {};
    return reader.read();
}
QByteArray encode(const QImage &image) {
    QByteArray result; QBuffer buffer(&result); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", 86)) return {};
    return result;
}
QPointF project(const LogIndex::Point &point) {
    const double lat = qBound(-85.05112878, point.latitude, 85.05112878) * Pi / 180;
    return {point.longitude * World / 360, std::log(std::tan(Pi / 4 + lat / 2)) * World / (2 * Pi)};
}
bool validPoint(const LogIndex::Point &p) {
    return std::isfinite(p.latitude) && std::isfinite(p.longitude)
        && p.latitude >= -90 && p.latitude <= 90 && p.longitude >= -180 && p.longitude <= 180
        && (p.latitude != 0 || p.longitude != 0);
}
QByteArray render(const QVector<LogIndex::Point> &input, const LogIndex::TileReader &tiles,
                  const LogIndex::Cancel &cancel, bool *wasCancelled) {
    QVector<QPointF> points;
    const int count = qMin(input.size(), LogIndex::MaximumTrackPoints);
    for (int i = 0; i < count; ++i) {
        const int index = count < input.size() ? int(qint64(i) * (input.size() - 1) / (count - 1)) : i;
        if (validPoint(input.at(index))) points.append(project(input.at(index)));
    }
    QImage image(Width, Height, QImage::Format_RGB32); image.fill(QColor(36, 43, 48));
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(62, 72, 78));
    for (int x = 0; x < Width; x += 24) painter.drawLine(x, 0, x, Height);
    for (int y = 0; y < Height; y += 20) painter.drawLine(0, y, Width, y);
    if (points.size() < 2) {
        painter.setPen(QColor(235, 120, 100)); QFont font = painter.font(); font.setPixelSize(18); painter.setFont(font);
        painter.drawText(12, 28, QStringLiteral("No GPS data"));
    } else {
        double minX = points.first().x(), maxX = minX, minY = points.first().y(), maxY = minY;
        for (const auto &point : points) {
            minX = qMin(minX, point.x()); maxX = qMax(maxX, point.x());
            minY = qMin(minY, point.y()); maxY = qMax(maxY, point.y());
        }
        if (maxX - minX < 100) { minX -= 50; maxX += 50; }
        if (maxY - minY < 100) { minY -= 50; maxY += 50; }
        const double centerX = (minX + maxX) / 2, centerY = (minY + maxY) / 2;
        double width = (maxX - minX) * 1.24, height = (maxY - minY) * 1.24;
        if (width / height > double(Width) / Height) height = width * Height / Width;
        else width = height * Width / Height;
        minX = centerX - width / 2; maxY = centerY + height / 2;
        int zoom = 1;
        for (int z = 16; z >= 1; --z) {
            const double resolution = World / (256 * double(1 << z));
            if (width / resolution <= Width && height / resolution <= Height) { zoom = z; break; }
        }
        const int n = 1 << zoom; const double tileWidth = World / n;
        const int x0 = qBound(0, int(std::floor((minX + World / 2) / tileWidth)), n - 1);
        const int x1 = qBound(0, int(std::floor((minX + width + World / 2) / tileWidth)), n - 1);
        const int y0 = qBound(0, int(std::floor((World / 2 - maxY) / tileWidth)), n - 1);
        const int y1 = qBound(0, int(std::floor((World / 2 - maxY + height) / tileWidth)), n - 1);
        int calls = 0;
        for (int y = y0; tiles && y <= y1 && calls < 12; ++y) {
            for (int x = x0; x <= x1 && calls < 12; ++x) {
                if (cancelled(cancel)) { *wasCancelled = true; return {}; }
                QByteArray bytes;
                try { ++calls; bytes = tiles(x, y, zoom); } catch (...) { /* Cache miss, never fetch. */ }
                if (cancelled(cancel)) { *wasCancelled = true; return {}; }
                const QImage tile = decode(bytes);
                if (!tile.isNull()) painter.drawImage(QRectF(
                    (-World / 2 + x * tileWidth - minX) / width * Width,
                    (maxY - (World / 2 - y * tileWidth)) / height * Height,
                    tileWidth / width * Width, tileWidth / height * Height), tile);
            }
        }
        const auto screen = [&](const QPointF &point) {
            return QPointF((point.x() - minX) / width * Width, (maxY - point.y()) / height * Height);
        };
        QPainterPath path; path.moveTo(screen(points.first()));
        for (int i = 1; i < points.size(); ++i) path.lineTo(screen(points.at(i)));
        painter.setPen(QPen(QColor(255, 255, 255, 210), 5)); painter.drawPath(path);
        painter.setPen(QPen(QColor(230, 45, 45), 2.5)); painter.drawPath(path);
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(50, 205, 50)); painter.drawEllipse(screen(points.first()), 5, 5);
        painter.setBrush(Qt::red); painter.drawEllipse(screen(points.last()), 5, 5);
    }
    painter.end();
    if (cancelled(cancel)) { *wasCancelled = true; return {}; }
    return encode(image);
}
}

struct LogIndexFiles::DeletePlanData {
    QString root;
    QDateTime rootCreated;
    QVector<LogIndex::Entry> entries;
    QStringList paths;
};
bool LogIndexFiles::DeletePlan::isValid() const { return d && !d->entries.isEmpty(); }
QString LogIndexFiles::DeletePlan::rootPath() const { return d ? d->root : QString(); }
QVector<LogIndex::Entry> LogIndexFiles::DeletePlan::entries() const { return d ? d->entries : QVector<LogIndex::Entry>(); }
QStringList LogIndexFiles::DeletePlan::paths() const { return d ? d->paths : QStringList(); }

LogIndex::Discovery LogIndexFiles::discover(const QString &requestedArg, const LogIndex::Cancel &cancelArg)
{
    const QString requested = requestedArg;
    const LogIndex::Cancel cancel = cancelArg;
    LogIndex::Discovery result;
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    if (!rootPath(requested, &result.rootPath, &result.error)) return result;
    const QDateTime rootCreated = QFileInfo(result.rootPath).birthTime();
    QVector<QPair<QString, int>> pending{{result.rootPath, 0}};
    int directories = 1, inspected = 0;
    while (!pending.isEmpty()) {
        if (cancelled(cancel)) { result.cancelled = true; return result; }
        const auto directory = pending.takeLast();
        QString currentRoot;
        if (!rootPath(result.rootPath, &currentRoot, &result.error)
            || QFileInfo(result.rootPath).birthTime() != rootCreated) {
            result.error = QStringLiteral("The indexed root changed during discovery."); return result;
        }
        const QFileInfo directoryInfo(directory.first);
        if (directoryInfo.isSymLink() || !directoryInfo.isDir()
            || !same(directory.first, directoryInfo.canonicalFilePath())) {
            warn(&result.warnings, QStringLiteral("Changed/linked directory skipped: %1").arg(directory.first)); continue;
        }
        if (!directoryInfo.isReadable()) {
            warn(&result.warnings, QStringLiteral("Unreadable directory skipped: %1").arg(directory.first)); continue;
        }
        QDirIterator iterator(directory.first, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            if (cancelled(cancel)) { result.cancelled = true; return result; }
            const QString path = iterator.next(); const QFileInfo info = iterator.fileInfo();
            if (++inspected > MaximumEntries) { result.error = QStringLiteral("Directory-entry safety limit (200000) exceeded."); return result; }
            if (info.isSymLink()) continue;
            if (info.isDir()) {
                if (++directories > MaximumDirectories || directory.second >= MaximumDepth) {
                    result.error = QStringLiteral("Directory count (20000) or depth (64) safety limit exceeded."); return result;
                }
                pending.append({absolute(path), directory.second + 1});
            } else if (info.isFile() && candidate(path)) {
                if (result.files.size() >= LogIndex::MaximumFiles) {
                    result.error = QStringLiteral("Log-file safety limit (20000) exceeded."); return result;
                }
                LogIndex::Entry entry; entry.fullPath = absolute(path); entry.rootPath = result.rootPath;
                if (!inspect(entry.rootPath, entry.fullPath, &entry.source, &entry.error) || !entry.source.exists) {
                    warn(&result.warnings, QStringLiteral("Changed/unavailable log skipped: %1").arg(path)); continue;
                }
                QString companionError;
                if (!inspect(entry.rootPath, entry.fullPath + ".jpg", &entry.thumbnail, &companionError))
                    warn(&result.warnings, companionError);
                if (QFileInfo(path).suffix().compare("tlog", Qt::CaseInsensitive) == 0
                    && !inspect(entry.rootPath, rlogPath(entry.fullPath), &entry.pairedRlog, &companionError))
                    warn(&result.warnings, companionError);
                result.files.append(entry);
            }
        }
    }
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    QString checked;
    if (!rootPath(result.rootPath, &checked, &result.error) || QFileInfo(result.rootPath).birthTime() != rootCreated) {
        result.error = QStringLiteral("The indexed root changed during discovery."); return result;
    }
    std::sort(result.files.begin(), result.files.end(), [](const LogIndex::Entry &a, const LogIndex::Entry &b) {
        return a.fullPath.compare(b.fullPath, PathCase) < 0;
    });
    result.success = true; return result;
}

bool LogIndexFiles::unchanged(const LogIndex::Entry &entry, QString *error)
{
    if (error) error->clear();
    return entryMatches(entry, false, error);
}

LogIndex::ThumbnailResult LogIndexFiles::thumbnail(
    const LogIndex::Entry &entryArg, const QVector<LogIndex::Point> &trackArg,
    const LogIndex::TileReader &tilesArg, const LogIndex::Cancel &cancelArg)
{
    const LogIndex::Entry entry = entryArg;
    const QVector<LogIndex::Point> track = trackArg;
    const LogIndex::TileReader tiles = tilesArg;
    const LogIndex::Cancel cancel = cancelArg;
    LogIndex::ThumbnailResult result;
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    if (!unchanged(entry, &result.warning)) {
        result.jpeg = render(track, {}, cancel, &result.cancelled);
        return result;
    }
    const QDateTime rootCreated = QFileInfo(entry.rootPath).birthTime();
    const QString sidecar = entry.fullPath + ".jpg";
    LogIndex::FileStamp before;
    const bool safe = inspect(entry.rootPath, sidecar, &before, &result.warning);
    result.sidecar = before;
    const bool fresh = safe && before.exists && before.modifiedUtc >= entry.source.modifiedUtc;
    if (fresh && before.sizeBytes > 0 && before.sizeBytes <= MaximumImageBytes) {
        QFile file(sidecar);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.read(MaximumImageBytes + 1);
            const QImage cached = decode(bytes);
            if (bytes.size() == before.sizeBytes && file.error() == QFileDevice::NoError
                && !cached.isNull()) {
                if (cancelled(cancel)) { result.cancelled = true; return result; }
                if (unchanged(entry, &result.warning) && matches(entry.rootPath, {sidecar, before}, &result.warning)
                    && QFileInfo(entry.rootPath).birthTime() == rootCreated) {
                    // Preserve fresh disk bytes, but never retain an oversized
                    // cached image per row across a large archive scan.
                    result.jpeg = cached.size() == QSize(Width, Height) && bytes.size() <= 128 * 1024
                        && bytes.startsWith(QByteArray::fromHex("ffd8")) ? bytes
                        : encode(cached.scaled(Width, Height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                    if (result.jpeg.isEmpty()) result.warning = QStringLiteral("Cached thumbnail could not be encoded as JPEG.");
                    return result;
                }
            }
        }
    }
    result.jpeg = render(track, tiles, cancel, &result.cancelled);
    if (result.cancelled) return result;
    if (result.jpeg.isEmpty()) { result.warning = QStringLiteral("JPEG thumbnail encoding failed; verify the Qt JPEG image plugin."); return result; }
    QString sourceError;
    if (!unchanged(entry, &sourceError) || QFileInfo(entry.rootPath).birthTime() != rootCreated) {
        result.warning = sourceError.isEmpty() ? QStringLiteral("Root changed; thumbnail remains in memory only.") : sourceError;
        return result;
    }
    if (!safe) return result;
    if (fresh) {
        result.warning = QStringLiteral("Fresh thumbnail could not be decoded within image limits; existing sidecar preserved.");
        return result;
    }
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    if (!unchanged(entry, &result.warning) || !matches(entry.rootPath, {sidecar, before}, &result.warning)
        || QFileInfo(entry.rootPath).birthTime() != rootCreated) {
        if (result.warning.isEmpty()) result.warning = QStringLiteral("Root changed; thumbnail remains in memory only.");
        return result;
    }
    QSaveFile output(sidecar); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(result.jpeg) != result.jpeg.size() || !output.flush()
        || !output.setFileTime(entry.source.modifiedUtc, QFileDevice::FileModificationTime)) {
        result.warning = QStringLiteral("Thumbnail remains in memory; sidecar could not be staged: %1").arg(output.errorString()); return result;
    }
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    if (!unchanged(entry, &result.warning) || !matches(entry.rootPath, {sidecar, before}, &result.warning)
        || QFileInfo(entry.rootPath).birthTime() != rootCreated) {
        if (result.warning.isEmpty()) result.warning = QStringLiteral("Root changed before thumbnail publication."); return result;
    }
    if (!output.commit()) {
        result.warning = QStringLiteral("Thumbnail remains in memory; sidecar publication failed: %1").arg(output.errorString()); return result;
    }
    if (!inspect(entry.rootPath, sidecar, &result.sidecar, &result.warning)) result.sidecar = {};
    return result;
}

LogIndexFiles::DeletePreparation LogIndexFiles::prepareDelete(
    const QString &root, const QVector<LogIndex::Entry> &entries, const LogIndex::Cancel &cancelArg)
{
    DeletePreparation result;
    // Pin caller-owned data before the first injected cancellation callback.
    const QVector<LogIndex::Entry> pinned = entries;
    const QString requested = root;
    const LogIndex::Cancel cancel = cancelArg;
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    auto data = std::make_shared<DeletePlanData>();
    if (!rootPath(requested, &data->root, &result.error)) return result;
    data->rootCreated = QFileInfo(data->root).birthTime();
    if (pinned.isEmpty() || pinned.size() > LogIndex::MaximumFiles) {
        result.error = QStringLiteral("Select between 1 and 20000 indexed logs."); return result;
    }
    QSet<QString> unique, uniquePaths;
    for (const auto &entry : pinned) {
        if (cancelled(cancel)) { result.cancelled = true; return result; }
        if (!same(entry.rootPath, data->root) || !entryMatches(entry, true, &result.error)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("Selected log belongs to another indexed root."); return result;
        }
        if (unique.contains(key(entry.fullPath))) continue;
        unique.insert(key(entry.fullPath)); data->entries.append(entry);
        for (const auto &target : targets(entry)) {
            const QString pathKey = key(target.path);
            if (target.expected.exists && !uniquePaths.contains(pathKey)) {
                uniquePaths.insert(pathKey); data->paths.append(target.path);
            }
        }
    }
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    for (const auto &entry : data->entries)
        if (!entryMatches(entry, true, &result.error)) return result;
    if (QFileInfo(data->root).birthTime() != data->rootCreated) {
        result.error = QStringLiteral("Indexed root changed during delete preparation."); return result;
    }
    result.plan.d = data; result.success = true; return result;
}

LogIndex::DeleteResult LogIndexFiles::executeDelete(
    const DeletePlan &plan, const LogIndex::Cancel &cancelArg, const LogIndex::Progress &progressArg)
{
    const auto data = plan.d; // Callback may replace or clear the caller's plan.
    const LogIndex::Cancel cancel = cancelArg;
    const LogIndex::Progress progress = progressArg;
    LogIndex::DeleteResult result;
    if (!data || data->entries.isEmpty()) { result.error = QStringLiteral("No valid confirmed delete plan."); return result; }
    result.remaining = data->entries.size();
    const auto rootCurrent = [&] {
        QString checked;
        return rootPath(data->root, &checked, &result.error) && same(checked, data->root)
            && QFileInfo(data->root).birthTime() == data->rootCreated;
    };
    int completed = 0;
    for (const auto &entry : data->entries) {
        if (progress) progress(completed, data->entries.size(), entry.fullPath);
        if (cancelled(cancel)) { result.cancelled = true; break; }
        if (!rootCurrent()) { result.error = QStringLiteral("Indexed root changed before deletion."); break; }
        QString error;
        if (!entryMatches(entry, true, &error)) { warn(&result.warnings, error); ++completed; continue; }
        // No user callback between this final path/stamp validation and removal.
        if (!QFile::remove(entry.fullPath)) {
            warn(&result.warnings, QStringLiteral("Could not delete selected log: %1").arg(entry.fullPath)); ++completed; continue;
        }
        result.deletedLogs.append(entry.fullPath); result.deletedPaths.append(entry.fullPath); --result.remaining;
        const auto companions = targets(entry);
        for (int i = 1; i < companions.size(); ++i) {
            const auto &target = companions.at(i);
            if (!target.expected.exists) continue;
            if (cancelled(cancel)) { result.cancelled = true; break; }
            const QFileInfo sourceNow(entry.fullPath);
            if (sourceNow.exists() || sourceNow.isSymLink()) {
                warn(&result.warnings, QStringLiteral("Source reappeared after deletion; companion preserved: %1").arg(target.path)); continue;
            }
            if (!rootCurrent() || !matches(data->root, target, &error)) {
                if (error.isEmpty()) error = QStringLiteral("Indexed root changed.");
                warn(&result.warnings, QStringLiteral("%1 companion preserved: %2").arg(entry.fullPath, error)); continue;
            }
            if (QFile::remove(target.path)) result.deletedPaths.append(target.path);
            else warn(&result.warnings, QStringLiteral("Could not delete companion: %1").arg(target.path));
        }
        ++completed;
        if (result.cancelled) {
            warn(&result.warnings, QStringLiteral("Deletion cancelled after removing a source; some confirmed companions remain.")); break;
        }
    }
    if (progress) progress(completed, data->entries.size(), QString());
    result.success = !result.cancelled && result.error.isEmpty() && result.warnings.isEmpty() && result.remaining == 0;
    return result;
}
