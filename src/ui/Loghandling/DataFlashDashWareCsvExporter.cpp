#include "DataFlashDashWareCsvExporter.h"
#include "DataFlashRawReader.h"
#include "DataFlashModeNames.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace {
using Result = DataFlashDashWareCsvExporter::Result;
using Definition = DataFlashRaw::Definition;

void warn(Result *result, const QString &text)
{
    if (!result->warnings.contains(text)) result->warnings.append(text);
}

bool cancelled(const DataFlashDashWareCsvExporter::CancelCheck &cancel, Result *result)
{
    if (cancel && cancel()) { result->cancelled = true; return true; }
    return false;
}

QByteArray number(double value, bool singlePrecision, const QString &field, Result *result)
{
    if (!std::isfinite(value)) {
        warn(result, QStringLiteral("Non-finite values in %1 are written explicitly as NaN/Infinity/-Infinity, not zero.").arg(field));
        if (std::isnan(value)) return "NaN";
        return value < 0 ? "-Infinity" : "Infinity";
    }
    if (value == 0 && std::signbit(value)) return "-0";
    // Use the shortest decimal that round-trips to the original scalar, rather
    // than displaying a float's binary rounding error as unnecessary digits.
    const int maximum = singlePrecision ? 9 : 17;
    for (int digits = 1; digits <= maximum; ++digits) {
        const QByteArray candidate = QByteArray::number(value, 'g', digits);
        bool ok = false;
        const double parsed = singlePrecision ? double(candidate.toFloat(&ok)) : candidate.toDouble(&ok);
        if (ok && parsed == value) return candidate;
    }
    return QByteArray::number(value, 'g', maximum);
}

double half(quint16 bits)
{
    const int exponent = (bits >> 10) & 31;
    const int fraction = bits & 1023;
    double value = 0;
    if (exponent == 31)
        value = fraction ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else if (exponent == 0)
        value = std::ldexp(double(fraction), -24);
    else
        value = std::ldexp(double(1024 + fraction), exponent - 25);
    return bits & 0x8000 ? -value : value;
}

QByteArray asciiValue(QByteArray bytes, const QString &field, Result *result)
{
    // Encoding.ASCII + Trim('\0') + DashWare Trim(), not metadata ascii(),
    // which intentionally stops at the first NUL instead of preserving it.
    while (!bytes.isEmpty() && bytes.front() == '\0') bytes.remove(0, 1);
    while (!bytes.isEmpty() && bytes.back() == '\0') bytes.chop(1);
    for (int index = 0; index < bytes.size(); ++index)
        if (quint8(bytes[index]) > 127) bytes[index] = '?';
    bytes = bytes.trimmed();
    if (bytes.contains('\0'))
        warn(result, QStringLiteral("Embedded NUL bytes in %1 were preserved; some CSV readers may not support them.").arg(field));
    return bytes;
}

bool valuesFor(const QByteArray &raw, const Definition &d, bool text,
               QList<QByteArray> *values, Result *result, const QByteArray &firmware = {})
{
    values->clear();
    if (text) {
        *values = DataFlashRaw::textValues(raw, d);
        if (values->size() != d.columns.size()) { result->error = QStringLiteral("Text row does not match its declared schema."); return false; }
        for (int index = 0; index < values->size(); ++index) {
            (*values)[index] = values->at(index).trimmed();
            if (QByteArray("gfdcCeEL").contains(d.format[index])) {
                const QByteArray token = values->at(index).toLower();
                if (token == "nan" || token == "infinity" || token == "-infinity" || token == "inf" || token == "-inf")
                    warn(result, QStringLiteral("Non-finite values in %1_%2 were preserved explicitly, not replaced with zero.")
                        .arg(QString::fromLatin1(d.name), QString::fromLatin1(d.columns[index])));
            }
        }
        return true; // Text logs already contain rendered/scaled values.
    }
    for (int index = 0; index < d.format.size(); ++index) {
        const auto *at = reinterpret_cast<const uchar *>(raw.constData() + d.offsets[index]);
        const QString field = QString::fromLatin1(d.name + '_' + d.columns[index]);
        QByteArray value;
        switch (d.format[index]) {
        case 'b': value = QByteArray::number(qint8(*at)); break;
        case 'B': value = QByteArray::number(*at); break;
        case 'M': {
            const QString name = DataFlashModeNames::resolve(firmware, *at);
            value = name.isEmpty() ? QByteArray::number(*at) : name.toUtf8();
            break;
        }
        case 'h': value = QByteArray::number(qFromLittleEndian<qint16>(at)); break;
        case 'H': value = QByteArray::number(qFromLittleEndian<quint16>(at)); break;
        case 'i': value = QByteArray::number(qFromLittleEndian<qint32>(at)); break;
        case 'I': value = QByteArray::number(qFromLittleEndian<quint32>(at)); break;
        case 'q': value = QByteArray::number(qFromLittleEndian<qint64>(at)); break;
        case 'Q': value = QByteArray::number(qFromLittleEndian<quint64>(at)); break;
        case 'g': value = number(half(qFromLittleEndian<quint16>(at)), true, field, result); break;
        case 'f': {
            const quint32 bits = qFromLittleEndian<quint32>(at); float scalar = 0;
            std::memcpy(&scalar, &bits, sizeof(scalar)); value = number(scalar, true, field, result); break;
        }
        case 'd': {
            const quint64 bits = qFromLittleEndian<quint64>(at); double scalar = 0;
            std::memcpy(&scalar, &bits, sizeof(scalar)); value = number(scalar, false, field, result); break;
        }
        case 'c': value = number(qFromLittleEndian<qint16>(at) / 100.0, false, field, result); break;
        case 'C': value = number(qFromLittleEndian<quint16>(at) / 100.0, false, field, result); break;
        case 'e': value = number(qFromLittleEndian<qint32>(at) / 100.0, false, field, result); break;
        case 'E': value = number(qFromLittleEndian<quint32>(at) / 100.0, false, field, result); break;
        case 'L': value = number(qFromLittleEndian<qint32>(at) / 10000000.0, false, field, result); break;
        case 'n': case 'N': case 'Z':
            value = asciiValue(raw.mid(d.offsets[index], DataFlashRaw::width(d.format[index])), field, result); break;
        case 'a':
            value = "[";
            for (int element = 0; element < 32; ++element) {
                if (element) value += ' ';
                value += QByteArray::number(qFromLittleEndian<qint16>(at + element * 2));
            }
            value += ']'; break;
        default:
            result->error = QStringLiteral("Selected field %1 uses an unsupported DashWare value encoding.").arg(field); return false;
        }
        values->append(value);
    }
    return true;
}

void detectFirmware(const QByteArray &raw, const Definition &d, QByteArray *firmware)
{
    QList<QByteArray> values;
    Result ignored;
    if (!valuesFor(raw, d, false, &values, &ignored)) return;
    QByteArray line = d.name;
    for (int index = 0; index < values.size(); ++index) {
        QByteArray value = values[index];
        if (d.format[index] == 'Z') {
            value.replace("\\", "\\\\"); value.replace("\n", "\\n");
            value.replace("\r", "\\r"); value.replace("\t", "\\t");
            QByteArray escaped;
            for (const char byte : value) {
                if (quint8(byte) < 32 || quint8(byte) > 127)
                    escaped += "\\x" + QByteArray::number(quint8(byte), 16).rightJustified(2, '0').toUpper();
                else escaped += byte;
            }
            value = escaped;
        }
        line += ", " + value;
    }
    // Exact BinaryLog.ReadMessage predicates, applied to the first 100001
    // MSG/PARM records, matching DFLogBuffer's constructor prescan. Selection
    // filters do not affect firmware detection, and later evidence may win.
    if (line.contains("PARM, RATE_RLL_P") || line.contains("ArduCopter") || line.contains("Copter"))
        *firmware = "ArduCopter2";
    else if (line.contains("PARM, H_SWASH_PLATE") || line.contains("ArduCopter"))
        *firmware = "ArduCopter2";
    else if (line.contains("PARM, PTCH2SRV_P") || line.contains("ArduPlane") || line.contains("Plane"))
        *firmware = "ArduPlane";
    else if (line.contains("PARM, SKID_STEER_OUT") || line.contains("ArduRover") || line.contains("Rover"))
        *firmware = "ArduRover";
    else if (line.contains("AntennaTracker") || line.contains("Tracker"))
        *firmware = "ArduTracker";
}

bool timestamp(const Definition &d, const QList<QByteArray> &values, QByteArray *time, Result *result)
{
    int index = d.columns.indexOf("TimeMS");
    bool micros = false;
    if (index < 0) { index = d.columns.indexOf("TimeUS"); micros = index >= 0; }
    if (index < 0) index = d.columns.indexOf("T");
    if (index < 0) { *time = "0"; return true; }
    bool ok = false;
    const qint64 ticks = values[index].toLongLong(&ok);
    if (!ok) {
        result->error = QStringLiteral("%1_%2 is not a signed 64-bit integer timestamp; no CSV was published.")
            .arg(QString::fromLatin1(d.name), QString::fromLatin1(d.columns[index])); return false;
    }
    if (!micros) { *time = QByteArray::number(ticks); return true; }
    const quint64 magnitude = ticks < 0 ? quint64(-(ticks + 1)) + 1 : quint64(ticks);
    *time = (ticks < 0 ? QByteArray("-") : QByteArray()) + QByteArray::number(magnitude / 1000);
    if (magnitude % 1000) {
        QByteArray fraction = QByteArray::number(magnitude % 1000).rightJustified(3, '0');
        while (fraction.endsWith('0')) fraction.chop(1);
        *time += '.' + fraction;
    }
    return true;
}

QByteArray csvField(QByteArray value)
{
    if (value.contains(',') || value.contains('"') || value.contains('\r')
        || value.contains('\n') || value.contains('\0')) {
        value.replace("\"", "\"\""); return '"' + value + '"';
    }
    return value;
}

bool writeRow(QSaveFile *file, const QList<QByteArray> &fields, Result *result)
{
    QByteArray row;
    for (const auto &field : fields) { row += csvField(field); row += ','; }
    row += '\n'; // Deliberate trailing blank column matches MP10.
    qint64 position = 0;
    while (position < row.size()) {
        const qint64 count = file->write(row.constData() + position, row.size() - position);
        if (count <= 0) { result->error = QStringLiteral("Could not write staged CSV: %1").arg(file->errorString()); return false; }
        position += count;
    }
    return true;
}

bool paths(const QString &input, const QString &output, QString *source, QString *destination, Result *result)
{
    QFileInfo in(input), out(output);
    const QString suffix = in.suffix().toLower();
    if (input.isEmpty() || output.isEmpty() || (suffix != "bin" && suffix != "log")
        || !in.isFile() || !in.isReadable() || out.isSymLink() || (out.exists() && !out.isFile())) {
        result->error = QStringLiteral("Select a readable .bin/.log input and a regular, non-symlink CSV output."); return false;
    }
    const QString parent = QFileInfo(out.absolutePath()).canonicalFilePath();
    *source = in.canonicalFilePath();
    *destination = parent.isEmpty() ? QString() : QDir(parent).filePath(out.fileName());
    if (source->isEmpty() || destination->isEmpty() || *source == *destination) {
        result->error = QStringLiteral("Input/output paths must resolve to different files in existing directories."); return false;
    }
    return true;
}
}

DataFlashDashWareCsvExporter::Result DataFlashDashWareCsvExporter::Export(
    QString input, QString output, QStringList types, CancelCheck cancel, Progress progress)
{
    Result result;
    try {
        QString sourcePath, outputPath;
        if (!paths(input, output, &sourcePath, &outputPath, &result)) return result;
        if (cancelled(cancel, &result)) return result;
        QSet<QByteArray> selected;
        for (const auto &type : types)
            if (!type.trimmed().isEmpty()) selected.insert(type.trimmed().toUpper().toLatin1());
        const bool text = QFileInfo(input).suffix().compare("log", Qt::CaseInsensitive) == 0;
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly)) { result.error = source.errorString(); return result; }
        const qint64 size = source.size();
        DataFlashRaw::Reader first(&source, text);
        QByteArray raw;
        int kind = -1;
        QByteArray firmware;
        qint64 modeEvidenceRecords = 0;
        qint64 lastProgress = -65536;
        if (progress) progress(0, 1000);
        while (first.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            if (!text && kind != 0 && kind != -2
                && (first.currentDefinition.name == "MSG" || first.currentDefinition.name == "PARM")
                && modeEvidenceRecords++ < 100001)
                detectFirmware(raw, first.currentDefinition, &firmware);
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(size ? qint64(400.0L * source.pos() / size) : 400, 1000);
            }
        }
        if (!first.error.isEmpty()) { result.error = first.error; return result; }
        if (source.size() != size || source.pos() != size) { result.error = QStringLiteral("Input changed during schema scan."); return result; }
        const QByteArray originalHash = first.hash.result();
        if (first.blankLines) warn(&result, QStringLiteral("Ignored %1 blank text line(s).").arg(first.blankLines));
        if (first.trailingBytes) warn(&result, QStringLiteral("Dropped incomplete final %1 record (type %2): %3 trailing bytes.")
            .arg(QString::fromLatin1(first.definitions.value(first.trailingType).name)).arg(first.trailingType).arg(first.trailingBytes));
        QList<QByteArray> columns{"GLOBAL_TimeMS"};
        QSet<QByteArray> columnNames{"GLOBAL_TimeMS"};
        QHash<int, int> starts;
        QList<int> schemaOrder = first.definitionOrder;
        // DFLog.FMTLine seeds FMT immediately after the first actual schema.
        // A later real FMT definition updates its fields without moving its slot.
        if (!schemaOrder.isEmpty() && schemaOrder.first() != DataFlashRaw::FmtId) {
            schemaOrder.removeAll(DataFlashRaw::FmtId);
            schemaOrder.insert(1, DataFlashRaw::FmtId);
        }
        Definition syntheticFmt;
        syntheticFmt.id = DataFlashRaw::FmtId; syntheticFmt.name = "FMT";
        syntheticFmt.format = "BBnNZ";
        syntheticFmt.columns = {"Type", "Length", "Name", "Format", "Columns"};
        for (int id : schemaOrder) {
            const Definition d = id == DataFlashRaw::FmtId && !first.definitions.contains(id)
                ? syntheticFmt : first.definitions.value(id);
            if (!selected.isEmpty() && !selected.contains(d.name.toUpper())) continue;
            if (!text && d.format.contains('A')) {
                result.error = QStringLiteral("Selected type %1 uses unsupported binary DashWare encoding A.").arg(QString::fromLatin1(d.name)); return result;
            }
            starts.insert(id, columns.size());
            for (const auto &column : d.columns) {
                const QByteArray name = d.name + '_' + column;
                if (columnNames.contains(name)) {
                    result.error = QStringLiteral("Ambiguous duplicate CSV column: %1.").arg(QString::fromLatin1(name)); return result;
                }
                columnNames.insert(name);
                columns.append(name);
            }
        }
        result.columns = columns.size();
        source.close();
        if (progress) progress(400, 1000);
        if (cancelled(cancel, &result)) return result;
        if (QFileInfo(input).canonicalFilePath() != sourcePath || !source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Input changed before CSV generation."); return result;
        }
        QSaveFile destination(outputPath);
        destination.setDirectWriteFallback(false);
        if (!destination.open(QIODevice::WriteOnly)) { result.error = destination.errorString(); return result; }
        if (!writeRow(&destination, columns, &result)) return result;
        DataFlashRaw::Reader second(&source, text);
        qint64 rows = 0;
        lastProgress = -65536;
        while (second.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            const Definition d = second.currentDefinition;
            if (kind != -2 && (!first.definitions.contains(d.id) || !first.definitions.value(d.id).same(d))) {
                result.error = QStringLiteral("Input schema changed before row conversion; no CSV was published."); return result;
            }
            if (kind != 0 && kind != -2 && starts.contains(d.id)) {
                QList<QByteArray> values;
                if (!valuesFor(raw, d, text, &values, &result, firmware)) return result;
                if (starts.value(d.id) > columns.size() - values.size()) {
                    result.error = QStringLiteral("A row exceeds the captured CSV schema."); return result;
                }
                QList<QByteArray> row;
                row.reserve(columns.size());
                for (int index = 0; index < columns.size(); ++index) row.append(QByteArray());
                if (!timestamp(d, values, &row[0], &result)) return result;
                for (int index = 0; index < values.size(); ++index) row[starts.value(d.id) + index] = values[index];
                if (!writeRow(&destination, row, &result)) return result;
                ++rows;
            }
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(400 + (size ? qint64(400.0L * source.pos() / size) : 400), 1000);
            }
        }
        if (!second.error.isEmpty()) { result.error = second.error; return result; }
        if (second.hash.result() != originalHash || source.size() != size || source.pos() != size) {
            result.error = QStringLiteral("Input changed between CSV passes."); return result;
        }
        source.close();
        if (!destination.flush()) { result.error = destination.errorString(); return result; }
        if (progress) progress(800, 1000);
        if (cancelled(cancel, &result)) return result;
        if (!source.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Input unavailable for final verification."); return result; }
        QCryptographicHash finalHash(QCryptographicHash::Sha256);
        while (!source.atEnd()) {
            if (cancelled(cancel, &result)) return result;
            const QByteArray bytes = source.read(64 * 1024);
            if (bytes.isEmpty()) { result.error = QStringLiteral("Input read failed during final verification."); return result; }
            finalHash.addData(bytes);
        }
        if (finalHash.result() != originalHash || QFileInfo(input).canonicalFilePath() != sourcePath) {
            result.error = QStringLiteral("Input changed before CSV publication."); return result;
        }
        if (cancelled(cancel, &result)) return result;
        QString checkedSource, checkedOutput;
        if (!paths(input, output, &checkedSource, &checkedOutput, &result) || checkedOutput != outputPath || checkedSource != sourcePath) {
            if (result.error.isEmpty()) result.error = QStringLiteral("Selected paths changed before CSV publication.");
            return result;
        }
        const qint64 bytes = destination.pos();
        if (!destination.commit()) { result.error = destination.errorString(); return result; }
        result.rowsWritten = rows; result.bytesWritten = bytes; result.success = true;
        // No caller callback after publication: cancellation/error can no longer
        // honestly retract the atomically committed file.
    } catch (const std::exception &error) {
        result.error = QStringLiteral("DashWare CSV export failed: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected DashWare CSV export failure.");
    }
    return result;
}
