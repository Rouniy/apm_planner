#include "FlightLogClassifier.h"
#include "DataFlashRawReader.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/TlogReader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace {
using Result = FlightLogClassifier::Result;
using Cancel = FlightLogClassifier::Cancel;
using Progress = FlightLogClassifier::Progress;
using Disposition = FlightLogClassifier::Disposition;

bool fail(Result *result, const QString &error)
{
    result->error = error; result->success = false;
    result->disposition = Disposition::Leave; result->relativeDirectory.clear(); return false;
}
bool stopped(const Cancel &cancel, Result *result)
{
    if (cancel && cancel()) {
        result->cancelled = true; result->success = false;
        result->disposition = Disposition::Leave; result->relativeDirectory.clear(); return true;
    }
    return false;
}
void warn(Result *result, const QString &warning)
{
    if (!result->warnings.contains(warning)) result->warnings.append(warning);
}
QString typeName(int type)
{
    // Vendored minimal.xml plus MP10's later wire-compatible HEARTBEAT types.
    static const char *const names[] = {
        "GENERIC", "FIXED_WING", "QUADROTOR", "COAXIAL", "HELICOPTER",
        "ANTENNA_TRACKER", "GCS", "AIRSHIP", "FREE_BALLOON", "ROCKET",
        "GROUND_ROVER", "SURFACE_BOAT", "SUBMARINE", "HEXAROTOR", "OCTOROTOR",
        "TRICOPTER", "FLAPPING_WING", "KITE", "ONBOARD_CONTROLLER", "VTOL_DUOROTOR",
        "VTOL_QUADROTOR", "VTOL_TILTROTOR", "VTOL_RESERVED2", "VTOL_RESERVED3",
        "VTOL_RESERVED4", "VTOL_RESERVED5", "GIMBAL", "ADSB", "PARAFOIL",
        "DODECAROTOR", "CAMERA", "CHARGING_STATION", "FLARM", "SERVO", "ODID",
        "DECAROTOR", "BATTERY", "PARACHUTE", "LOG", "OSD", "IMU", "GPS", "WINCH",
        "GENERIC_MULTIROTOR", "ILLUMINATOR", "SPACECRAFT_ORBITER", "GROUND_QUADRUPED",
        "VTOL_GYRODYNE", "GRIPPER"};
    static_assert(MAV_TYPE_WINCH == 42 && MAV_TYPE_ENUM_END == 43,
                  "Update log classifier names when the vendored MAV_TYPE changes");
    return type >= 0 && type < int(sizeof(names) / sizeof(names[0]))
        ? QString::fromLatin1(names[type]) : QString::number(type);
}
void destination(Result *result, bool sitl, int type, int system, int serial)
{
    QStringList parts;
    if (sitl) parts.append(QStringLiteral("SITL"));
    parts.append(typeName(type)); parts.append(QString::number(system));
    if (serial) parts.append(QString::number(serial));
    result->disposition = Disposition::Move;
    result->relativeDirectory = parts.join('/');
}

QByteArray stringField(const QByteArray &raw, const DataFlashRaw::Definition &d,
                       const QByteArray &name, bool text)
{
    const int index = d.columns.indexOf(name);
    if (index < 0) return {};
    if (text) return DataFlashRaw::textValues(raw, d).at(index).trimmed();
    if (!QByteArray("nNZ").contains(d.format[index])) return {};
    return DataFlashRaw::ascii(raw.mid(d.offsets[index], DataFlashRaw::width(d.format[index])));
}
double scalar(const QByteArray &raw, const DataFlashRaw::Definition &d, int index)
{
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + d.offsets[index]);
    switch (d.format[index]) {
    case 'b': return qint8(*at);
    case 'B': case 'M': return *at;
    case 'h': return qFromLittleEndian<qint16>(at);
    case 'H': return qFromLittleEndian<quint16>(at);
    case 'i': return qFromLittleEndian<qint32>(at);
    case 'I': return qFromLittleEndian<quint32>(at);
    case 'q': return double(qFromLittleEndian<qint64>(at));
    case 'Q': return double(qFromLittleEndian<quint64>(at));
    case 'c': return qFromLittleEndian<qint16>(at) / 100.0;
    case 'C': return qFromLittleEndian<quint16>(at) / 100.0;
    case 'e': return qFromLittleEndian<qint32>(at) / 100.0;
    case 'E': return qFromLittleEndian<quint32>(at) / 100.0;
    case 'L': return qFromLittleEndian<qint32>(at) / 10000000.0;
    case 'f': {
        const quint32 bits = qFromLittleEndian<quint32>(at); float value = 0;
        std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    case 'd': {
        const quint64 bits = qFromLittleEndian<quint64>(at); double value = 0;
        std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    case 'g': {
        const quint16 bits = qFromLittleEndian<quint16>(at);
        const int exponent = (bits >> 10) & 31, fraction = bits & 1023;
        if (exponent == 31) return std::numeric_limits<double>::quiet_NaN();
        const double value = exponent ? std::ldexp(double(1024 + fraction), exponent - 25)
                                      : std::ldexp(double(fraction), -24);
        return bits & 0x8000 ? -value : value;
    }
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}
bool intValue(const QByteArray &raw, const DataFlashRaw::Definition &d, bool text, int *out)
{
    const int index = d.columns.indexOf("Value");
    if (index < 0) return false;
    bool ok = true;
    const double value = text ? DataFlashRaw::textValues(raw, d).at(index).trimmed().toDouble(&ok)
                              : scalar(raw, d, index);
    if (!ok || !std::isfinite(value) || value != std::trunc(value)
        || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return false;
    *out = int(value); return true;
}

bool dataFlash(QFile *file, bool text, const Cancel &cancel,
               const Progress &progress, Result *result)
{
    DataFlashRaw::Reader reader(file, text);
    int kind = -1, system = 0, serial = 0;
    qint64 messages = 0;
    bool sawSystem = false, sawSerial = false, sitl = false;
    bool copter = false, plane = false, rover = false, sawFrame = false;
    QByteArray frame, raw;
    qint64 lastProgress = 0;
    while (reader.next(&raw, &kind)) {
        if (stopped(cancel, result)) return false;
        if (kind != 0 && kind != -2) {
            const auto &d = reader.currentDefinition;
            if (d.name == "SIM") sitl = true;
            else if (d.name == "PARM") {
                const QByteArray name = stringField(raw, d, "Name", text);
                if (name == "SYSID_THISMAV" && !sawSystem) {
                    sawSystem = true;
                    if (!intValue(raw, d, text, &system)) warn(result, QStringLiteral("First SYSID_THISMAV was not a usable integer; later values were not substituted."));
                } else if (name == "BRD_SERIAL_NUM" && !sawSerial) {
                    sawSerial = true;
                    if (!intValue(raw, d, text, &serial)) warn(result, QStringLiteral("First BRD_SERIAL_NUM was not a usable integer; later values were not substituted."));
                }
            } else if (d.name == "MSG" && messages++ < 100) {
                const QByteArray message = stringField(raw, d, "Message", text).toLower();
                // MP10 Select(predicate).Count() counts messages, not matches;
                // inspect the contents instead, including the first Frame text.
                copter = copter || message.contains("copter");
                plane = plane || message.contains("plane");
                rover = rover || message.contains("rover");
                if (!sawFrame && message.contains("frame:")) { sawFrame = true; frame = message; }
            }
        }
        if (file->pos() - lastProgress >= 64 * 1024) {
            lastProgress = file->pos(); if (progress) progress(lastProgress, file->size());
            if (stopped(cancel, result)) return false;
        }
    }
    if (!reader.error.isEmpty()) return fail(result, reader.error);
    if (file->error() != QFileDevice::NoError) return fail(result, file->errorString());
    if (reader.blankLines) warn(result, QStringLiteral("Ignored %1 blank DataFlash text lines.").arg(reader.blankLines));
    if (reader.trailingBytes) warn(result, QStringLiteral("Ignored %1 bytes of an incomplete final DataFlash record.").arg(reader.trailingBytes));
    int type = MAV_TYPE_GENERIC;
    if (copter) {
        type = MAV_TYPE_QUADROTOR;
        if (frame.contains("hexa")) type = MAV_TYPE_HEXAROTOR;
        if (frame.contains("octo") || frame.contains("octa")) type = MAV_TYPE_OCTOROTOR;
    }
    if (plane) type = MAV_TYPE_FIXED_WING;
    if (rover) type = MAV_TYPE_GROUND_ROVER;
    if (sitl || (system != 0 && type != MAV_TYPE_GENERIC)) destination(result, sitl, type, system, serial);
    else warn(result, QStringLiteral("DataFlash type or SYSID_THISMAV is unknown; leave this non-SITL log in place."));
    return true;
}

struct Heartbeat { quint16 endpoint = 0; int type = MAV_TYPE_GENERIC; };
struct Telemetry {
    QVector<Heartbeat> heartbeats;
    QHash<quint16, int> serials; // At most 256*256 exact wire endpoints.
    bool sitl = false;
    qint64 rejected = 0;

    void observe(const mavlink_message_t &message, Result *result)
    {
        if (message.compid == MAV_COMP_ID_MISSIONPLANNER) return;
        const quint16 endpoint = quint16(message.sysid) * 256 + message.compid;
        const auto sizeOk = [&](int minimum, int maximum) {
            return message.magic == MAVLINK_STX_MAVLINK1 ? message.len == minimum
                : message.len > 0 && message.len <= maximum;
        };
        if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
            if (!sizeOk(MAVLINK_MSG_ID_HEARTBEAT_MIN_LEN, MAVLINK_MSG_ID_HEARTBEAT_LEN)) { ++rejected; return; }
            mavlink_heartbeat_t hb{}; mavlink_msg_heartbeat_decode(&message, &hb);
            heartbeats.append({endpoint, hb.type});
        } else if (message.msgid == MAVLINK_MSG_ID_PARAM_VALUE) {
            if (!sizeOk(MAVLINK_MSG_ID_PARAM_VALUE_MIN_LEN, MAVLINK_MSG_ID_PARAM_VALUE_LEN)) { ++rejected; return; }
            mavlink_param_value_t parameter{}; mavlink_msg_param_value_decode(&message, &parameter);
            QByteArray name(parameter.param_id, sizeof(parameter.param_id));
            const int nul = name.indexOf('\0'); if (nul >= 0) name.truncate(nul);
            if (name != "BRD_SERIAL_NUM") return;
            const double value = parameter.param_value;
            if (!std::isfinite(value) || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
                warn(result, QStringLiteral("An invalid telemetry BRD_SERIAL_NUM was ignored.")); return;
            }
            serials.insert(endpoint, int(value)); // MP10 float-to-int truncation.
        } else if (message.msgid == MAVLINK_MSG_ID_SIMSTATE || message.msgid == MAVLINK_MSG_ID_HIL_CONTROLS) sitl = true;
    }
    void classify(Result *result) const
    {
        if (heartbeats.isEmpty()) {
            result->disposition = Disposition::Move; result->relativeDirectory = QStringLiteral("BAD");
            warn(result, QStringLiteral("No usable non-comp190 heartbeat was found; classified as BAD.")); return;
        }
        Heartbeat chosen = heartbeats.last();
        QSet<quint16> endpoints;
        for (const auto &hb : heartbeats) endpoints.insert(hb.endpoint);
        if (endpoints.size() > 1) {
            for (const auto &hb : heartbeats)
                if (hb.type != MAV_TYPE_GCS && hb.type != MAV_TYPE_ANTENNA_TRACKER) chosen = hb;
            warn(result, QStringLiteral("Multiple telemetry endpoints were present; MP10 heartbeat ordering selected one endpoint, not an authenticated identity."));
        }
        destination(result, sitl, chosen.type, chosen.endpoint >> 8, serials.value(chosen.endpoint));
        if (heartbeats.size() == 101)
            warn(result, QStringLiteral("Telemetry classification stopped at the MP10 limit of 101 heartbeats; later packets were not examined."));
    }
};

bool telemetry(QFile *file, bool timestamped, const Cancel &cancel,
               const Progress &progress, Result *result)
{
    Telemetry evidence;
    qint64 skipped = 0, lastProgress = 0;
    if (timestamped) {
        TlogReader reader(file);
        reader.setCancelCheck([&] {
            const qint64 position = reader.bytesProcessed();
            if (position - lastProgress >= 64 * 1024) {
                lastProgress = position; if (progress) progress(position, reader.bytesTotal());
            }
            return stopped(cancel, result);
        });
        TlogRecord record;
        while (evidence.heartbeats.size() < 101) {
            const auto status = reader.next(&record);
            if (status == TlogReader::Status::Cancelled) { result->cancelled = true; return false; }
            if (status == TlogReader::Status::Error) return fail(result, reader.errorString());
            if (status == TlogReader::Status::Truncated)
                warn(result, QStringLiteral("Truncated timestamped telemetry tail: %1").arg(reader.errorString()));
            if (status != TlogReader::Status::Ok) break;
            evidence.observe(record.message, result);
        }
        skipped = reader.skippedBytes(); evidence.rejected += reader.rejectedFrames();
    } else {
        MAVLinkFrameParser parser;
        qint64 consumed = 0, framed = 0;
        while (!file->atEnd() && evidence.heartbeats.size() < 101) {
            if (stopped(cancel, result)) return false;
            const QByteArray chunk = file->read(64 * 1024);
            if (chunk.isEmpty()) return fail(result, QStringLiteral("Could not read raw telemetry log."));
            for (int at = 0; at < chunk.size() && evidence.heartbeats.size() < 101; ++at) {
                if ((at & 1023) == 0 && stopped(cancel, result)) return false;
                ++consumed;
                mavlink_message_t message{};
                const auto state = parser.parseByte(quint8(chunk[at]), &message);
                if (state == MAVLINK_FRAMING_OK) {
                    framed += parser.lastFrame().size(); evidence.observe(message, result);
                } else if (state == MAVLINK_FRAMING_BAD_CRC || state == MAVLINK_FRAMING_BAD_SIGNATURE) {
                    framed += parser.lastFrame().size(); ++evidence.rejected;
                }
            }
            if (progress) progress(consumed, file->size());
        }
        skipped = qMax<qint64>(0, consumed - framed);
    }
    if (file->error() != QFileDevice::NoError) return fail(result, file->errorString());
    if (evidence.rejected) warn(result, QStringLiteral("Rejected %1 telemetry frames with invalid CRC/signature framing or payload length.").arg(evidence.rejected));
    if (skipped) warn(result, QStringLiteral("Skipped %1 unframed or truncated telemetry bytes.").arg(skipped));
    if (stopped(cancel, result)) return false;
    evidence.classify(result); return true;
}
}

FlightLogClassifier::Result FlightLogClassifier::Classify(
    const QString &path, const Cancel &cancel, const Progress &progress)
{
    Result result;
    try {
        if (stopped(cancel, &result)) return result;
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        if (!info.isFile() || (suffix != "bin" && suffix != "log" && suffix != "tlog" && suffix != "rlog")) {
            fail(&result, QStringLiteral("Select an existing .tlog, .rlog, .bin or .log file.")); return result;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { fail(&result, file.errorString()); return result; }
        const qint64 size = file.size();
        if (progress) progress(0, size);
        if (stopped(cancel, &result)) return result;
        if (size == 0) result.disposition = Disposition::DeleteEmpty;
        else if (size <= 1024) { result.disposition = Disposition::Move; result.relativeDirectory = QStringLiteral("SMALL"); }
        else if (suffix == "bin" || suffix == "log") {
            if (!dataFlash(&file, suffix == "log", cancel, progress, &result)) return result;
        } else if (!telemetry(&file, suffix == "tlog", cancel, progress, &result)) return result;
        if (file.size() != size) { fail(&result, QStringLiteral("Log size changed during classification.")); return result; }
        if (progress) progress(size, size);
        if (stopped(cancel, &result)) return result;
        if (file.size() != size) { fail(&result, QStringLiteral("Log size changed during classification.")); return result; }
        result.success = true;
    } catch (const std::exception &error) {
        fail(&result, QStringLiteral("Log classification failed: %1").arg(QString::fromUtf8(error.what())));
    } catch (...) { fail(&result, QStringLiteral("Unexpected log classification failure.")); }
    return result;
}
