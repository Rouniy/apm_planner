#include "DataFlashLogSplitter.h"
#include "DataFlashRawReader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace {
constexpr qint64 MetadataLimit = 16 * 1024 * 1024;
using namespace DataFlashRaw;
bool exists(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() || file.isSymLink();
}

bool write(QFile *file, const QByteArray &bytes, QString *error)
{
    qint64 position = 0;
    while (position < bytes.size()) {
        const qint64 count = file->write(bytes.constData() + position, bytes.size() - position);
        if (count <= 0) { *error = QStringLiteral("Could not write staged part: %1").arg(file->errorString()); return false; }
        position += count;
    }
    return true;
}

bool cancelled(const DataFlashLogSplitter::CancelCheck &cancel, DataFlashLogSplitter::Result *result)
{
    if (cancel && cancel()) { result->cancelled = true; return true; }
    return false;
}

bool hashFile(const QString &path, QByteArray *digest, const DataFlashLogSplitter::CancelCheck &cancel,
              DataFlashLogSplitter::Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result->error = QStringLiteral("Could not verify file: %1").arg(file.errorString()); return false; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled(cancel, result)) return false;
        const QByteArray bytes = file.read(64 * 1024);
        if (bytes.isEmpty()) { result->error = QStringLiteral("Could not read file during verification."); return false; }
        hash.addData(bytes);
    }
    *digest = hash.result();
    return true;
}
}

QStringList DataFlashLogSplitter::OutputPaths(const QString &inputPath, int pieces)
{
    const QFileInfo input(inputPath);
    const QString suffix = input.suffix().toLower();
    if (inputPath.isEmpty() || pieces < 2 || pieces > 1000 || (suffix != "bin" && suffix != "log")) return {};
    const QString parent = QFileInfo(input.absolutePath()).canonicalFilePath();
    if (parent.isEmpty()) return {};
    const QString base = QDir(parent).filePath(input.fileName());
    QStringList paths;
    for (int piece = 0; piece < pieces; ++piece)
        paths.append(base + QStringLiteral("_split%1.%2").arg(piece).arg(suffix));
    return paths;
}

DataFlashLogSplitter::Result DataFlashLogSplitter::Split(
    const QString &inputPath, int pieces, CancelCheck cancel, Progress progress)
{
    Result result;
    try {
        const QStringList destinations = OutputPaths(inputPath, pieces);
        if (destinations.isEmpty()) { result.error = QStringLiteral("Select a .bin/.log file and between 2 and 1000 pieces."); return result; }
        for (const auto &path : destinations)
            if (exists(path)) { result.error = QStringLiteral("Output already exists; nothing was overwritten: %1").arg(path); return result; }
        const QString sourcePath = QFileInfo(inputPath).canonicalFilePath();
        QFile source(sourcePath);
        if (sourcePath.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not open the input DataFlash file."); return result;
        }
        const qint64 sourceSize = source.size();
        const bool text = QFileInfo(inputPath).suffix().compare("log", Qt::CaseInsensitive) == 0;
        if (progress) progress(0, 1000);
        Reader first(&source, text);
        QByteArray metadata[4];
        qint64 metadataBytes = 0, dataBytes = 0, lastProgress = -65536;
        QByteArray raw;
        int kind = -1;
        while (first.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            if (kind != -2) ++result.recordsRead;
            if (kind >= 0) {
                QByteArray prefixRecord = raw;
                if (text && !prefixRecord.endsWith('\n')) prefixRecord += '\n';
                metadataBytes += prefixRecord.size();
                if (metadataBytes > MetadataLimit) { result.error = QStringLiteral("Metadata exceeds the 16 MiB safety limit."); return result; }
                metadata[kind] += prefixRecord;
            } else if (kind == -1) {
                ++result.dataRecords; dataBytes += raw.size();
            }
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(sourceSize ? qint64(250.0L * source.pos() / sourceSize) : 250, 1000);
            }
        }
        if (!first.error.isEmpty()) { result.error = first.error; return result; }
        if (first.blankLines)
            result.warnings.append(QStringLiteral("Ignored %1 blank text line(s).").arg(first.blankLines));
        if (first.trailingBytes)
            result.warnings.append(QStringLiteral("Dropped incomplete final binary %1 record (type %2): %3 trailing bytes.")
                .arg(QString::fromLatin1(first.definitions.value(first.trailingType).name))
                .arg(first.trailingType).arg(first.trailingBytes));
        if (source.size() != sourceSize || source.pos() != sourceSize) { result.error = QStringLiteral("Input changed during the metadata pass."); return result; }
        if (result.dataRecords < pieces) { result.error = QStringLiteral("There are fewer complete data records than requested pieces."); return result; }
        const QByteArray sourceHash = first.hash.result();
        source.close();
        QByteArray prefix;
        for (const auto &group : metadata) prefix += group;
        result.warnings.append(QStringLiteral("Parts preserve ordinary records once, but are not complete flight-state snapshots. Publication is not group-atomic; cancellation stops before publication, not between published files."));
        if (progress) progress(250, 1000);
        if (cancelled(cancel, &result)) return result;
        if (QFileInfo(inputPath).canonicalFilePath() != sourcePath || !source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Input path changed before the splitting pass."); return result;
        }
        QTemporaryDir staging(QFileInfo(destinations.first()).absolutePath() + QStringLiteral("/.apm-dataflash-split-XXXXXX"));
        if (!staging.isValid()) { result.error = QStringLiteral("Could not create private sibling staging directory."); return result; }
        QStringList staged;
        QList<QByteArray> stageHashes;
        QList<qint64> stageSizes;
        QFile part;
        QCryptographicHash partHash(QCryptographicHash::Sha256);
        int index = 0;
        qint64 currentBytes = 0, writtenBytes = 0, writtenRecords = 0;
        long double targetBytes = static_cast<long double>(dataBytes) / pieces;
        const auto openPart = [&]() {
            const QString path = staging.filePath(QStringLiteral("part-%1").arg(index));
            part.setFileName(path);
            if (!part.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { result.error = QStringLiteral("Could not create a staged part."); return false; }
            staged.append(path); partHash.reset(); partHash.addData(prefix);
            return write(&part, prefix, &result.error);
        };
        const auto closePart = [&]() {
            if (!part.flush()) { result.error = QStringLiteral("Could not flush a staged part."); return false; }
            stageSizes.append(part.size()); stageHashes.append(partHash.result()); part.close(); return true;
        };
        if (!openPart()) return result;
        Reader second(&source, text);
        lastProgress = -65536;
        while (second.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            if (kind == -1) {
                const qint64 recordsLeft = result.dataRecords - writtenRecords;
                const int partsLeft = pieces - index;
                const bool closerBefore = std::abs(currentBytes - targetBytes)
                    <= std::abs(currentBytes + raw.size() - targetBytes);
                if (currentBytes > 0 && partsLeft > 1 && recordsLeft >= partsLeft - 1
                    && (recordsLeft == partsLeft - 1 || closerBefore)) {
                    if (!closePart()) return result;
                    ++index; currentBytes = 0;
                    targetBytes = static_cast<long double>(dataBytes - writtenBytes) / (pieces - index);
                    if (!openPart()) return result;
                }
                if (!write(&part, raw, &result.error)) return result;
                partHash.addData(raw); currentBytes += raw.size(); writtenBytes += raw.size(); ++writtenRecords;
            }
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(250 + qint64(250.0L * source.pos() / sourceSize), 1000);
            }
        }
        if (!second.error.isEmpty()) { result.error = second.error; return result; }
        if (second.hash.result() != sourceHash || source.size() != sourceSize
            || writtenRecords != result.dataRecords || index != pieces - 1) {
            result.error = QStringLiteral("Input changed between splitting passes."); return result;
        }
        source.close();
        if (!closePart()) return result;
        for (int piece = 0; piece < staged.size(); ++piece) {
            if (progress) progress(500 + (250 * piece) / pieces, 1000);
            QByteArray digest;
            if (!hashFile(staged[piece], &digest, cancel, &result)) return result;
            if (digest != stageHashes[piece] || QFileInfo(staged[piece]).size() != stageSizes[piece]) {
                result.error = QStringLiteral("A staged part changed before publication."); return result;
            }
        }
        if (progress) progress(750, 1000);
        if (cancelled(cancel, &result)) return result;
        QByteArray finalHash;
        if (QFileInfo(inputPath).canonicalFilePath() != sourcePath || !hashFile(sourcePath, &finalHash, cancel, &result)) {
            if (result.error.isEmpty() && !result.cancelled) result.error = QStringLiteral("Input path changed before publication.");
            return result;
        }
        if (finalHash != sourceHash) { result.error = QStringLiteral("Input changed before publication."); return result; }
        for (const auto &path : destinations)
            if (exists(path)) { result.error = QStringLiteral("Output appeared before publication: %1").arg(path); return result; }
        if (cancelled(cancel, &result)) return result;
        // Deliberate cancellation boundary. Never roll back or delete a published
        // file: another actor might already have opened or modified it.
        for (int piece = 0; piece < staged.size(); ++piece) {
            if (exists(destinations[piece]) || !QFile::rename(staged[piece], destinations[piece])) {
                result.error = QStringLiteral("Could not publish %1 without overwriting. %2 earlier part(s) remain published.")
                    .arg(destinations[piece]).arg(result.outputs.size()); return result;
            }
            result.outputs.append(destinations[piece]); result.bytesWritten += stageSizes[piece];
            if (progress) progress(750 + (250 * (piece + 1)) / pieces, 1000);
        }
        result.success = true;
    } catch (const std::exception &error) {
        result.error = QStringLiteral("DataFlash split failed: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected DataFlash split failure.");
    }
    return result;
}
