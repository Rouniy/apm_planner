#include "ShapefileImportService.h"
#include "ShapefileGeometryReader.h"
#include "ShapefileProjection.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QSaveFile>
#include <QTextCodec>

#include <cmath>

namespace {
constexpr qint64 MaximumPreparedBytes = 128LL * 1024 * 1024;
constexpr int MaximumDirectoryEntries = 200000;
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif
bool samePath(const QString &a, const QString &b) { return a.compare(b, PathCase) == 0; }
bool present(const QString &path) { const QFileInfo f(path); return f.exists() || f.isSymLink(); }
struct Stamp {
    QString path;
    bool exists = false;
    qint64 size = 0;
    QDateTime modified, birth;
    QByteArray digest;
    static Stamp capture(const QString &path) {
        const QFileInfo f(path);
        return {path, present(path), f.size(), f.lastModified(), f.birthTime(), {}};
    }
    bool matches(bool directory = false) const {
        const QFileInfo f(path);
        if (!exists) return !present(path);
        return f.exists() && !f.isSymLink() && (directory ? f.isDir() : f.isFile())
            && samePath(f.canonicalFilePath(), path)
            && (!birth.isValid() || f.birthTime() == birth)
            && (directory || (f.size() == size && f.lastModified() == modified));
    }
};
struct Observer {
    Shapefile::Cancel cancel;
    Shapefile::Progress progress;
    bool cancelled = false;
    QString error;
    bool stopped() {
        if (cancelled || !error.isEmpty()) return true;
        try { cancelled = cancel && cancel(); }
        catch (...) { error = QStringLiteral("The conversion cancellation observer failed."); }
        return cancelled || !error.isEmpty();
    }
    bool report(qint64 done, qint64 total, const QString &phase) {
        if (stopped()) return false;
        try { if (progress) progress(done, total, phase); }
        catch (...) { error = QStringLiteral("The conversion progress observer failed."); }
        return !stopped();
    }
    QString failure() const {
        return error.isEmpty() ? QStringLiteral("Shapefile conversion cancelled.") : error;
    }
};
struct Sidecars {
    QMap<QString, QString> paths;
    QString error;
};
Sidecars discoverSidecars(const QString &source, Observer &observer) {
    Sidecars result;
    const QFileInfo sourceInfo(source);
    const QString stem = sourceInfo.completeBaseName();
    QDirIterator it(sourceInfo.absolutePath(), QDir::AllEntries | QDir::Hidden
                    | QDir::System | QDir::NoDotAndDotDot);
    int count = 0;
    while (it.hasNext()) {
        if (++count > MaximumDirectoryEntries) {
            result.error = QStringLiteral("Shapefile directory exceeds the 200000-entry safety limit."); return result;
        }
        if ((count % 256) == 1 && observer.stopped()) {
            result.error = observer.failure(); return result;
        }
        const QFileInfo f(it.next());
        if (f.completeBaseName().compare(stem, Qt::CaseInsensitive) != 0) continue;
        const QString extension = f.suffix().toLower();
        if (extension != QStringLiteral("prj") && extension != QStringLiteral("dbf")
            && extension != QStringLiteral("cpg")) continue;
        if (result.paths.contains(extension)) {
            result.error = QStringLiteral("Ambiguous case-insensitive .%1 sidecars; keep only the intended source file.").arg(extension);
            return result;
        }
        if (!f.isFile() || f.isSymLink() || !samePath(f.canonicalFilePath(), f.absoluteFilePath())) {
            result.error = QStringLiteral("Shapefile sidecar is not an ordinary unlinked file: %1").arg(f.absoluteFilePath());
            return result;
        }
        result.paths.insert(extension, f.absoluteFilePath());
    }
    return result;
}
bool readSnapshot(Stamp *stamp, qint64 limit, QByteArray *bytes,
                  const Stamp &parent, Observer &observer, QString *error) {
    if (!stamp || !parent.matches(true) || !stamp->matches() || !stamp->exists
        || stamp->size < 0 || stamp->size > limit) {
        *error = QStringLiteral("Source is unsafe, changed or exceeds its size limit: %1")
            .arg(stamp ? stamp->path : QString()); return false;
    }
    QFile file(stamp->path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read shapefile source: %1 — %2").arg(stamp->path, file.errorString()); return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (!file.atEnd()) {
        if (observer.stopped()) { *error = observer.failure(); return false; }
        if (!parent.matches(true) || !stamp->matches()) {
            *error = QStringLiteral("Shapefile source changed while reading: %1").arg(stamp->path); return false;
        }
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            *error = QStringLiteral("Reading shapefile source failed: %1").arg(stamp->path); return false;
        }
        if (chunk.size() > limit - read) {
            *error = QStringLiteral("Shapefile source grew beyond its size limit: %1").arg(stamp->path); return false;
        }
        read += chunk.size(); hash.addData(chunk);
        if (bytes) bytes->append(chunk);
    }
    if (!parent.matches(true) || !stamp->matches() || read != stamp->size) {
        *error = QStringLiteral("Shapefile source changed before reading completed: %1").arg(stamp->path); return false;
    }
    const QByteArray digest = hash.result();
    if (!stamp->digest.isEmpty() && stamp->digest != digest) {
        *error = QStringLiteral("Shapefile source content changed since the conversion plan: %1").arg(stamp->path); return false;
    }
    stamp->digest = digest;
    return true;
}
qint64 sourceLimit(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("prj")) return Shapefile::MaximumPrjBytes;
    if (suffix == QStringLiteral("dbf")) return Shapefile::MaximumDbfBytes;
    if (suffix == QStringLiteral("cpg")) return Shapefile::MaximumCpgBytes;
    return Shapefile::MaximumShpBytes;
}
QString readText(const QByteArray &bytes, QString *error) {
    QTextCodec::ConverterState state;
    const auto codec = QTextCodec::codecForUtfText(bytes, QTextCodec::codecForName("UTF-8"));
    QString text = codec->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars || text.contains(QChar('\0'))) {
        *error = QStringLiteral("Projection text is not valid UTF-8/UTF-16 or contains NUL bytes."); return {};
    }
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    return text.trimmed();
}
QString number(double value) {
    if (value == 0.0 && std::signbit(value)) return QStringLiteral("-0");
    QLocale invariant = QLocale::c();
    invariant.setNumberOptions(QLocale::OmitGroupSeparator);
    return invariant.toString(value, 'g', QLocale::FloatingPointShortest);
}
}

struct ShapefileImportService::PlanData {
    QString source, directory, projection;
    Stamp parent;
    QMap<QString, QString> sidecars;
    QVector<Stamp> sources, destinations;
    QVector<Output> outputs;
    QVector<QByteArray> contents;
    qint64 pointCount = 0, discarded = 0;
    QStringList warnings;
};

bool ShapefileImportService::Plan::isValid() const { return !d.isNull(); }
QString ShapefileImportService::Plan::source() const { return d ? d->source : QString(); }
QString ShapefileImportService::Plan::directory() const { return d ? d->directory : QString(); }
QString ShapefileImportService::Plan::projectionName() const { return d ? d->projection : QString(); }
QVector<ShapefileImportService::Output> ShapefileImportService::Plan::outputs() const { return d ? d->outputs : QVector<Output>(); }
qint64 ShapefileImportService::Plan::pointCount() const { return d ? d->pointCount : 0; }
qint64 ShapefileImportService::Plan::discardedPoints() const { return d ? d->discarded : 0; }
QStringList ShapefileImportService::Plan::warnings() const { return d ? d->warnings : QStringList(); }

ShapefileImportService::Preparation ShapefileImportService::prepare(
    const QString &requestedInput, const Shapefile::Cancel &cancel, const Shapefile::Progress &progress)
{
    Observer observer{cancel, progress, false, {}};
    Preparation result;
    const auto fail = [&](const QString &error) {
        result.error = observer.error.isEmpty() ? error : observer.error;
        result.cancelled = observer.error.isEmpty() && (result.cancelled || observer.cancelled);
        return result;
    };
    if (requestedInput.trimmed().isEmpty() || requestedInput.contains(QChar('\0')))
        return fail(QStringLiteral("Select an existing .shp file."));
    const QFileInfo requested(QDir::cleanPath(QFileInfo(requestedInput).absoluteFilePath()));
    if (!requested.isFile() || requested.isSymLink()
        || requested.suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("Select an ordinary .shp file, not a symbolic link."));
    QSharedPointer<PlanData> data(new PlanData);
    data->source = requested.canonicalFilePath();
    if (data->source.isEmpty()) return fail(QStringLiteral("Cannot resolve the shapefile source."));
    data->directory = QFileInfo(data->source).absolutePath();
    data->parent = Stamp::capture(data->directory);
    if (!data->parent.matches(true)) return fail(QStringLiteral("The shapefile parent is unsafe."));
    const Sidecars sidecars = discoverSidecars(data->source, observer);
    if (!sidecars.error.isEmpty()) return fail(sidecars.error);
    data->sidecars = sidecars.paths;
    QMap<QString, QByteArray> inputs;
    auto readSource = [&](const QString &path, const QString &key) {
        Stamp stamp = Stamp::capture(path);
        if (!readSnapshot(&stamp, sourceLimit(path), &inputs[key], data->parent, observer, &result.error)) return false;
        data->sources.append(stamp); return true;
    };
    if (!readSource(data->source, QStringLiteral("shp"))) return fail(result.error);
    for (auto i = data->sidecars.cbegin(); i != data->sidecars.cend(); ++i)
        if (!readSource(i.value(), i.key())) return fail(result.error);
    const auto stopped = [&] { return observer.stopped(); };
    const auto report = [&](qint64 done, qint64 total, const QString &phase) { observer.report(done, total, phase); };
    if (!observer.report(0, 0, QStringLiteral("Reading shapefile geometry"))) return fail(observer.failure());
    auto geometry = ShapefileGeometryReader::read(inputs.value(QStringLiteral("shp")),
        data->sidecars.contains(QStringLiteral("dbf")), inputs.value(QStringLiteral("dbf")),
        inputs.value(QStringLiteral("cpg")), stopped, report);
    if (observer.stopped()) return fail(observer.failure());
    if (!geometry.success) {
        result.cancelled = geometry.cancelled;
        return fail(geometry.error.isEmpty() ? QStringLiteral("Shapefile geometry could not be read.") : geometry.error);
    }
    data->warnings = geometry.warnings;
    const QString wkt = readText(inputs.value(QStringLiteral("prj")), &result.error);
    if (!result.error.isEmpty()) return fail(result.error);
    inputs.clear(); // Raw snapshots are no longer needed; retain stamps/digests.
    auto converted = ShapefileProjection::transform(wkt, geometry.features, stopped, report);
    geometry.features.clear();
    if (observer.stopped()) return fail(observer.failure());
    if (!converted.success) {
        result.cancelled = converted.cancelled;
        return fail(converted.error.isEmpty()
            ? QStringLiteral("Shapefile projection failed.") : converted.error);
    }
    data->projection = converted.projectionName;
    data->discarded = converted.discardedPoints;
    data->warnings += converted.warnings;
    if (data->discarded)
        data->warnings.append(QStringLiteral("%1 non-finite/out-of-range WGS84 points were omitted.").arg(data->discarded));
    qint64 preparedBytes = 0;
    for (int feature = 0; feature < converted.features.size(); ++feature) {
        if (!observer.report(feature, converted.features.size(), QStringLiteral("Preparing exact POLY outputs")))
            return fail(observer.failure());
        const auto &points = converted.features.at(feature).points;
        if (points.isEmpty()) continue;
        if (data->outputs.size() >= Shapefile::MaximumFeatures
            || points.size() > Shapefile::MaximumPoints - data->pointCount)
            return fail(QStringLiteral("Conversion exceeds the output feature/point safety limits."));
        QByteArray bytes("#Shap to Poly - Mission Planner\r\n");
        for (int i = 0; i < points.size(); ++i) {
            if ((i % 2048) == 0 && observer.stopped()) return fail(observer.failure());
            const auto &point = points.at(i);
            if (!std::isfinite(point.x) || !std::isfinite(point.y)
                || point.x < -180 || point.x > 180 || point.y < -90 || point.y > 90)
                return fail(QStringLiteral("Projection returned an invalid coordinate after filtering."));
            bytes += number(point.y).toUtf8() + '\t' + number(point.x).toUtf8() + "\r\n";
            if (bytes.size() > MaximumPreparedBytes - preparedBytes)
                return fail(QStringLiteral("Prepared POLY output exceeds the 128 MiB safety limit."));
        }
        const QString path = QDir(data->directory).filePath(QStringLiteral("poly-%1.poly").arg(data->outputs.size() + 1));
        Stamp destination = Stamp::capture(path);
        if (destination.exists && !destination.matches())
            return fail(QStringLiteral("POLY output is not an ordinary unlinked file: %1").arg(path));
        data->destinations.append(destination);
        data->outputs.append({path, points.size(), destination.exists});
        data->contents.append(bytes);
        data->pointCount += points.size(); preparedBytes += bytes.size();
    }
    if (observer.stopped()) return fail(observer.failure());
    if (!data->parent.matches(true)) return fail(QStringLiteral("Source parent changed during preparation."));
    for (const auto &source : data->sources)
        if (!source.matches()) return fail(QStringLiteral("Source changed during preparation: %1").arg(source.path));
    const auto finalSidecars = discoverSidecars(data->source, observer);
    if (!finalSidecars.error.isEmpty()) return fail(finalSidecars.error);
    if (finalSidecars.paths != data->sidecars) return fail(QStringLiteral("Shapefile sidecars changed during preparation."));
    for (const auto &destination : data->destinations)
        if (!destination.matches()) return fail(QStringLiteral("Output changed during preparation: %1").arg(destination.path));
    if (observer.stopped()) return fail(observer.failure());
    result.plan.d = data;
    return result;
}

ShapefileImportService::Result ShapefileImportService::exportPolyFiles(
    const Plan &requestedPlan, const Shapefile::Cancel &cancel, const Shapefile::Progress &progress)
{
    const auto data = requestedPlan.d; // Pin the immutable plan through observer callbacks.
    Observer observer{cancel, progress, false, {}};
    Result result;
    const auto fail = [&](const QString &error) {
        result.error = error; result.cancelled = observer.cancelled; return result;
    };
    if (!data) return fail(QStringLiteral("Analyze a shapefile before converting it."));
    result.projectionName = data->projection;
    result.warnings = data->warnings;
    if (data->outputs.size() != data->contents.size() || data->outputs.size() != data->destinations.size())
        return fail(QStringLiteral("Invalid immutable POLY output plan."));
    const auto sourceStampsMatch = [&] {
        if (!data->parent.matches(true)) return false;
        for (const auto &source : data->sources) if (!source.matches()) return false;
        return true;
    };
    if (!sourceStampsMatch()) return fail(QStringLiteral("Shapefile source or parent changed after the reviewed plan."));
    const auto sidecars = discoverSidecars(data->source, observer);
    if (!sidecars.error.isEmpty()) return fail(sidecars.error);
    if (sidecars.paths != data->sidecars) return fail(QStringLiteral("Shapefile sidecars changed after the reviewed plan."));
    // Confirm all inputs and output reservations before publishing the first file.
    for (const auto &source : data->sources) {
        Stamp copy = source;
        if (!readSnapshot(&copy, sourceLimit(copy.path), nullptr, data->parent, observer, &result.error)) return fail(result.error);
    }
    for (const auto &destination : data->destinations)
        if (!destination.matches()) return fail(QStringLiteral("POLY output changed after confirmation: %1").arg(destination.path));
    for (int index = 0; index < data->outputs.size(); ++index) {
        const auto &output = data->outputs.at(index);
        if (!observer.report(index, data->outputs.size(), QStringLiteral("Writing %1").arg(output.path))) return fail(observer.failure());
        if (!sourceStampsMatch() || !data->destinations.at(index).matches())
            return fail(QStringLiteral("Source or output changed before writing %1.").arg(output.path));
        QSaveFile file(output.path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Cannot stage POLY output %1: %2").arg(output.path, file.errorString()));
        const QByteArray &bytes = data->contents.at(index);
        for (qint64 at = 0; at < bytes.size(); at += 64 * 1024) {
            if (observer.stopped()) return fail(observer.failure());
            if (!sourceStampsMatch() || !data->destinations.at(index).matches())
                return fail(QStringLiteral("Source or output changed while staging %1.").arg(output.path));
            const qint64 count = qMin<qint64>(64 * 1024, bytes.size() - at);
            if (file.write(bytes.constData() + at, count) != count)
                return fail(QStringLiteral("Writing POLY output failed: %1").arg(output.path));
        }
        if (observer.stopped()) return fail(observer.failure());
        // No external callback between the final guard and atomic replacement.
        if (!sourceStampsMatch() || !data->destinations.at(index).matches())
            return fail(QStringLiteral("Source or output changed before publishing %1.").arg(output.path));
        if (!file.commit()) return fail(QStringLiteral("Cannot publish POLY output %1: %2").arg(output.path, file.errorString()));
        result.files.append(output.path); result.pointCount += output.pointCount;
    }
    // Completed files are definitive even if the final observer requests Cancel.
    observer.report(result.files.size(), data->outputs.size(), QStringLiteral("POLY conversion complete"));
    if (!observer.error.isEmpty()) result.warnings.append(observer.error);
    result.success = true;
    return result;
}
