#include "TlogMatlabExporter.h"
#include "MatFileWriter.h"
#include "TlogReader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
constexpr qint64 MaximumInput = 16LL * 1024 * 1024 * 1024;
constexpr int MaximumVariables = 4096;
constexpr int BufferBudget = 8 * 1024 * 1024;
struct Stamp {
    QString path;
    qint64 size = 0;
    QDateTime modified, born;
};
bool exists(const QString &path) { const QFileInfo info(path); return info.exists() || info.isSymLink(); }
Stamp stamp(const QString &path) {
    const QFileInfo info(path);
    return {info.canonicalFilePath(), info.size(), info.lastModified(), info.birthTime()};
}
bool same(const Stamp &before) {
    const QFileInfo info(before.path);
    return info.isFile() && !info.isSymLink() && info.canonicalFilePath() == before.path
        && info.size() == before.size && info.lastModified() == before.modified
        && (!before.born.isValid() || info.birthTime() == before.born);
}
bool parentCurrent(const QString &parent, const QDateTime &born) {
    const QFileInfo info(parent);
    return info.isDir() && !info.isSymLink() && info.canonicalFilePath() == parent
        && (!born.isValid() || info.birthTime() == born);
}
struct StageOwnership {
    QTemporaryFile &file;
    QString path;
    QDateTime born;
    bool current() const {
        const QFileInfo info(path);
        return info.isFile() && !info.isSymLink() && info.canonicalFilePath() == path
            && (!born.isValid() || info.birthTime() == born);
    }
    ~StageOwnership() {
        // Never remove a replacement installed at our staging pathname.
        if (exists(path) && !current()) file.setAutoRemove(false);
    }
};
bool publish(const QString &source, const QString &output, QString *error) {
#ifdef Q_OS_UNIX
    if (::link(QFile::encodeName(source).constData(), QFile::encodeName(output).constData()) == 0) {
        // QTemporaryFile removes only the private hard-link at destruction.
        return true;
    }
#elif defined(Q_OS_WIN)
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                    reinterpret_cast<LPCWSTR>(output.utf16()), MOVEFILE_WRITE_THROUGH)) return true;
#else
    Q_UNUSED(source)
    Q_UNUSED(output)
#endif
    *error = QStringLiteral("Could not atomically publish MATLAB output without replacing an existing file.");
    return false;
}
// Exporter-private MP10 ExtLibs/Mavlink/Mavlink.cs scalar extensions. The
// compiled transport dialect already accepts these IDs/CRC-extras and retains
// the wire payload. No shared decoder or transport schema is changed here.
struct Supplement {
    quint32 messageId;
    const char *name;
    mavlink_message_type_t type;
    unsigned offset;
};
constexpr Supplement Supplements[] = {
    {286, "angular_velocity_z", MAVLINK_TYPE_FLOAT, 53},
    {259, "gimbal_device_id", MAVLINK_TYPE_UINT8_T, 235},
    {225, "fuel_pressure", MAVLINK_TYPE_FLOAT, 69},
    {285, "delta_yaw", MAVLINK_TYPE_FLOAT, 40},
    {285, "delta_yaw_velocity", MAVLINK_TYPE_FLOAT, 44},
    {285, "gimbal_device_id", MAVLINK_TYPE_UINT8_T, 48},
    {283, "gimbal_device_id", MAVLINK_TYPE_UINT8_T, 144},
    {69, "buttons2", MAVLINK_TYPE_UINT16_T, 11},
    {69, "enabled_extensions", MAVLINK_TYPE_UINT8_T, 13},
    {69, "s", MAVLINK_TYPE_INT16_T, 14},
    {69, "t", MAVLINK_TYPE_INT16_T, 16},
    {69, "aux1", MAVLINK_TYPE_INT16_T, 18},
    {69, "aux2", MAVLINK_TYPE_INT16_T, 20},
    {69, "aux3", MAVLINK_TYPE_INT16_T, 22},
    {69, "aux4", MAVLINK_TYPE_INT16_T, 24},
    {69, "aux5", MAVLINK_TYPE_INT16_T, 26},
    {69, "aux6", MAVLINK_TYPE_INT16_T, 28},
    {42, "total", MAVLINK_TYPE_UINT16_T, 2},
    {42, "mission_state", MAVLINK_TYPE_UINT8_T, 4},
    {42, "mission_mode", MAVLINK_TYPE_UINT8_T, 5},
    {331, "quality", MAVLINK_TYPE_INT8_T, 232},
    {108, "lat_int", MAVLINK_TYPE_INT32_T, 84},
    {108, "lon_int", MAVLINK_TYPE_INT32_T, 88},
    {269, "encoding", MAVLINK_TYPE_UINT8_T, 213}
};
static_assert(std::size(Supplements) == 24, "Review the MP10 offline extension inventory");
// A regenerated dialect must be reviewed instead of silently duplicating fields.
static_assert(MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE_LEN <= 53, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_CAMERA_INFORMATION_LEN <= 235, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_EFI_STATUS_LEN <= 69, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS_LEN <= 40, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION_LEN <= 144, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_MANUAL_CONTROL_LEN <= 11, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_MISSION_CURRENT_LEN <= 2, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_ODOMETRY_LEN <= 232, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_SIM_STATE_LEN <= 84, "Review offline supplement");
static_assert(MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION_LEN <= 213, "Review offline supplement");
constexpr bool sameName(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
struct SupplementalDefinitions {
    std::array<mavlink_message_info_t, 10> messages;
    bool valid = true;
};
constexpr auto SupplementalMetadata = [] {
    SupplementalDefinitions result{{{
        MAVLINK_MESSAGE_INFO_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE,
        MAVLINK_MESSAGE_INFO_CAMERA_INFORMATION,
        MAVLINK_MESSAGE_INFO_EFI_STATUS,
        MAVLINK_MESSAGE_INFO_GIMBAL_DEVICE_ATTITUDE_STATUS,
        MAVLINK_MESSAGE_INFO_GIMBAL_DEVICE_INFORMATION,
        MAVLINK_MESSAGE_INFO_MANUAL_CONTROL,
        MAVLINK_MESSAGE_INFO_MISSION_CURRENT,
        MAVLINK_MESSAGE_INFO_ODOMETRY,
        MAVLINK_MESSAGE_INFO_SIM_STATE,
        MAVLINK_MESSAGE_INFO_VIDEO_STREAM_INFORMATION
    }}};
    for (const auto &extension : Supplements) {
        bool found = false;
        for (auto &message : result.messages) {
            if (message.msgid != extension.messageId) continue;
            if (found || message.num_fields >= MAVLINK_MAX_FIELDS) { result.valid = false; return result; }
            found = true;
            for (unsigned i = 0; i < message.num_fields; ++i) {
                if (sameName(message.fields[i].name, extension.name)) { result.valid = false; return result; }
            }
            // structure_offset is unused: scalar() reads wire bytes, including
            // unaligned extensions and MAVLink's zero-trim padding, not C structs.
            message.fields[message.num_fields++] = {extension.name, nullptr,
                extension.type, 0, extension.offset, 0};
        }
        if (!found) { result.valid = false; return result; }
    }
    return result;
}();
static_assert(SupplementalMetadata.valid, "Missing, duplicate or overflowing offline MAVLink metadata");

const QHash<quint32, const mavlink_message_info_t *> &messageInfos() {
    static const auto table = [] {
        static const mavlink_message_info_t definitions[] = MAVLINK_MESSAGE_INFO;
        QHash<quint32, const mavlink_message_info_t *> result;
        for (const auto &definition : definitions) if (definition.name && *definition.name)
            result.insert(definition.msgid, &definition);
        for (const auto &definition : SupplementalMetadata.messages)
            result.insert(definition.msgid, &definition);
        return result;
    }();
    return table;
}
template<class T> T payload(const mavlink_message_t &message, int offset) {
    unsigned char bytes[sizeof(T)]{};
    if (offset < message.len)
        std::memcpy(bytes, _MAV_PAYLOAD(&message) + offset, qMin<int>(sizeof(T), message.len - offset));
    return qFromLittleEndian<T>(bytes);
}
double scalar(const mavlink_message_t &message, const mavlink_field_info_t &field) {
    const int at = field.wire_offset;
    switch (field.type) {
    case MAVLINK_TYPE_UINT8_T: return payload<quint8>(message, at);
    case MAVLINK_TYPE_INT8_T: return payload<qint8>(message, at);
    case MAVLINK_TYPE_UINT16_T: return payload<quint16>(message, at);
    case MAVLINK_TYPE_INT16_T: return payload<qint16>(message, at);
    case MAVLINK_TYPE_UINT32_T: return payload<quint32>(message, at);
    case MAVLINK_TYPE_INT32_T: return payload<qint32>(message, at);
    case MAVLINK_TYPE_UINT64_T: return double(payload<quint64>(message, at));
    case MAVLINK_TYPE_INT64_T: return double(payload<qint64>(message, at));
    case MAVLINK_TYPE_FLOAT: {
        const auto bits = payload<quint32>(message, at); float value;
        std::memcpy(&value, &bits, sizeof(value)); return double(value);
    }
    case MAVLINK_TYPE_DOUBLE: {
        const auto bits = payload<quint64>(message, at); double value;
        std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    default: return 0;
    }
}
bool scalarField(const mavlink_field_info_t &field) {
    return field.array_length == 0 && field.type >= MAVLINK_TYPE_UINT8_T && field.type <= MAVLINK_TYPE_DOUBLE;
}
bool skip(const mavlink_message_t &message) {
    return message.msgid == MAVLINK_MSG_ID_HEARTBEAT
        && mavlink_msg_heartbeat_get_type(&message) == MAV_TYPE_GCS;
}
struct Series {
    QByteArray name;
    mavlink_field_info_t field{};
    qint64 count = 0, filled = 0, flushed = 0;
    MatFileWriter::Matrix matrix;
    QVector<double> times, values;
};
}

double TlogMatlabExporter::mp10SerialDate(qint64 unixUsec)
{
    const quint64 prefix = quint64(unixUsec);
    if (prefix / 1000 / 1000 / 60 / 60 >= 9999999) return 367.0;
    const auto local = QDateTime::fromMSecsSinceEpoch(qint64(prefix / 1000), Qt::UTC).toLocalTime();
    if (!local.isValid()) return 367.0;
    const auto shifted = local.date().addYears(1).addDays(2);
    const qint64 ticks = QDate(1, 1, 1).daysTo(shifted) * 864000000000LL
        + qint64(local.time().msecsSinceStartOfDay()) * 10000;
    return double(ticks) / 864000000000.0;
}

TlogExportResult TlogMatlabExporter::Export(const QString &requestedInput,
    const QString &requestedOutput, Cancel cancel, Progress progress)
{
    const QString inputRequest = requestedInput, outputRequest = requestedOutput;
    TlogExportResult result;
    auto cancelled = [&] {
        if (cancel && cancel()) { result.cancelled = true; return true; }
        return false;
    };
    const QFileInfo inputInfo(inputRequest), outputInfo(outputRequest);
    const QFileInfo parentInfo(outputInfo.absolutePath());
    if (!inputInfo.isFile() || inputInfo.isSymLink() || inputInfo.size() > MaximumInput
        || inputInfo.size() < 0 || !parentInfo.isDir() || parentInfo.isSymLink()
        || outputRequest.isEmpty() || exists(outputInfo.absoluteFilePath())) {
        result.error = QStringLiteral("Choose a regular TLOG of at most 16 GiB and a new output file in an existing ordinary directory.");
        return result;
    }
    const Stamp source = stamp(inputInfo.canonicalFilePath());
    const QString parent = parentInfo.canonicalFilePath();
    const QDateTime parentBorn = QFileInfo(parent).birthTime();
    const QString output = QDir(parent).filePath(outputInfo.fileName());
    auto pathsCurrent = [&] {
        if (!same(source) || !parentCurrent(parent, parentBorn) || exists(output)) {
            result.error = QStringLiteral("The source, output directory or destination changed during MATLAB export.");
            return false;
        }
        return true;
    };
    auto notify = [&](qint64 amount) { if (progress) progress(amount, 1000); };
    auto hashSource = [&](int from, int to, QByteArray *digest) {
        QFile file(source.path);
        if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Cannot read the source TLOG for verification."); return false; }
        QCryptographicHash hash(QCryptographicHash::Sha256); qint64 done = 0;
        while (done < source.size) {
            if (cancelled() || !pathsCurrent()) return false;
            const QByteArray bytes = file.read(qMin<qint64>(1024 * 1024, source.size - done));
            if (bytes.isEmpty()) { result.error = QStringLiteral("Reading the source TLOG failed during verification."); return false; }
            hash.addData(bytes); done += bytes.size();
            notify(from + (to - from) * done / qMax<qint64>(1, source.size));
        }
        if (cancelled() || !pathsCurrent()) return false;
        if (file.error() != QFileDevice::NoError) {
            result.error = QStringLiteral("Reading the source TLOG failed during verification."); return false;
        }
        *digest = hash.result(); return true;
    };
    QByteArray originalHash, checkedHash;
    if (cancelled() || !pathsCurrent() || !hashSource(0, 100, &originalHash)) return result;

    QVector<Series> series;
    QHash<quint32, QVector<int>> byMessage;
    QHash<QByteArray, bool> names;
    auto scan = [&](bool countPass, MatFileWriter *writer, int bufferRows) {
        QFile file(source.path);
        if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Cannot open the source TLOG."); return false; }
        TlogReader reader(&file); reader.setCancelCheck(cancelled);
        reader.setProgress([&](qint64 done, qint64 total) {
            notify((countPass ? 100 : 450) + (countPass ? 250 : 350) * done / qMax<qint64>(1, total));
        });
        auto flush = [&](Series &item) {
            if (item.times.isEmpty()) return true;
            if (cancelled() || !pathsCurrent()) return false;
            if (!writer->writeDoubleColumn(item.matrix, 0, item.flushed, item.times, &result.error)
                || !writer->writeDoubleColumn(item.matrix, 1, item.flushed, item.values, &result.error)) return false;
            item.flushed += item.times.size(); item.times.clear(); item.values.clear(); return true;
        };
        TlogRecord record;
        while (reader.next(&record) == TlogReader::Status::Ok) {
            if (cancelled() || !pathsCurrent()) return false;
            const auto &message = record.message;
            if (skip(message)) continue;
            const auto info = messageInfos().value(message.msgid, nullptr);
            if (!info) continue;
            if (!byMessage.contains(message.msgid)) {
                if (!countPass) { result.error = QStringLiteral("The TLOG message schema changed between passes."); return false; }
                QVector<int> indices;
                for (unsigned fieldIndex = 0; fieldIndex < info->num_fields; ++fieldIndex) {
                    const auto &field = info->fields[fieldIndex];
                    if (!scalarField(field)) continue; // Includes every signing key byte array.
                    const QByteArray name = QByteArray(field.name) + "_mavlink_" + QByteArray(info->name).toLower() + "_t";
                    if (series.size() >= MaximumVariables || names.contains(name)) {
                        result.error = QStringLiteral("MATLAB export exceeds 4096 scalar variables or has ambiguous field names."); return false;
                    }
                    names.insert(name, true); indices.append(series.size());
                    Series item; item.name = name; item.field = field; series.append(std::move(item));
                }
                byMessage.insert(message.msgid, indices);
            }
            const double date = countPass ? 0 : mp10SerialDate(record.timestampUsec);
            for (int index : byMessage.value(message.msgid)) {
                auto &item = series[index];
                if (countPass) {
                    if (++item.count > (qint64(std::numeric_limits<quint32>::max()) - 1024) / 16) {
                        result.error = QStringLiteral("One MATLAB variable exceeds the Level-5 32-bit element-size limit."); return false;
                    }
                } else {
                    if (++item.filled > item.count) { result.error = QStringLiteral("The TLOG scalar counts changed between passes."); return false; }
                    item.times.append(date); item.values.append(scalar(message, item.field));
                    if (item.times.size() >= bufferRows && !flush(item)) return false;
                }
            }
        }
        if (reader.status() == TlogReader::Status::Cancelled) { result.cancelled = true; return false; }
        if (reader.status() == TlogReader::Status::Error) { result.error = reader.errorString(); return false; }
        if (countPass) {
            result.recordsRead = reader.recordCount(); result.skippedBytes = reader.skippedBytes();
        } else {
            if (reader.recordCount() != result.recordsRead || reader.skippedBytes() != result.skippedBytes) {
                result.error = QStringLiteral("The source TLOG changed between decoding passes."); return false;
            }
            for (auto &item : series) {
                if (item.filled != item.count) { result.error = QStringLiteral("The TLOG scalar counts changed between passes."); return false; }
                if (!flush(item)) return false;
            }
        }
        return !cancelled() && pathsCurrent();
    };
    if (!scan(true, nullptr, 0) || !hashSource(350, 450, &checkedHash)) return result;
    if (originalHash != checkedHash) { result.error = QStringLiteral("The source TLOG content changed after its first pass."); return result; }
    QTemporaryFile staged(QDir(parent).filePath(QStringLiteral(".tlog-matlab-XXXXXX.partial")));
    if (cancelled() || !pathsCurrent()) return result;
    if (!staged.open()) { result.error = QStringLiteral("Cannot create a private MATLAB staging file."); return result; }
    StageOwnership ownership{staged, staged.fileName(), QFileInfo(staged.fileName()).birthTime()};
    MatFileWriter writer(&staged);
    if (!writer.begin(&result.error)) return result;
    QVector<int> order; for (int index = 0; index < series.size(); ++index) order.append(index);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return series[a].name < series[b].name; });
    const int bufferRows = qMax(1, qMin(MatFileWriter::MaximumWriteChunkValues,
                                      BufferBudget / (16 * qMax(1, series.size()))));
    for (int index : order) {
        if (cancelled() || !pathsCurrent()) return result;
        if (!ownership.current()) { result.error = QStringLiteral("The private MATLAB stage was replaced."); return result; }
        auto &item = series[index];
        if (!writer.reserveDoubleMatrix(item.name, item.count, 2, &item.matrix, &result.error)) return result;
        item.times.reserve(bufferRows); item.values.reserve(bufferRows);
    }
    if (!scan(false, &writer, bufferRows) || !writer.finish(&result.error)) return result;
    if (!hashSource(800, 950, &checkedHash)) return result;
    if (checkedHash != originalHash) { result.error = QStringLiteral("The source TLOG content changed before publication."); return result; }
    if (!staged.flush()) { result.error = QStringLiteral("Flushing the staged MATLAB file failed."); return result; }
    staged.close(); notify(1000);
    if (cancelled() || !pathsCurrent()) return result;
    if (!ownership.current()) { result.error = QStringLiteral("The private MATLAB stage was replaced."); return result; }
    // No observer callbacks between final guards and atomic no-replace publication.
    if (!publish(ownership.path, output, &result.error)) return result;
    result.success = true; result.outputPaths.append(output); result.itemCount = series.size();
    result.message = QStringLiteral("Wrote %1 MATLAB scalar variables to %2.").arg(series.size()).arg(output);
    if (result.skippedBytes) result.message += QStringLiteral(" Skipped %1 unsupported, corrupt or trailing byte(s).").arg(result.skippedBytes);
    return result;
}
