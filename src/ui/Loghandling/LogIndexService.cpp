#include "LogIndexService.h"

#include "DataFlashRawReader.h"
#include "LogIndexFiles.h"
#include "comm/TlogReader.h"

#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace {
constexpr double EarthRadiusMeters = 6371000.0;
constexpr qint64 MaximumInMemoryThumbnailBytes = 64LL * 1024 * 1024;

enum class PassStatus { Complete, Cancelled, PartialError };

bool cancelled(const LogIndex::Cancel &cancel)
{
    return cancel && cancel();
}

void appendError(QString *target, const QString &message)
{
    if (message.isEmpty()) return;
    if (target->isEmpty()) *target = message;
    else if (!target->contains(message)) *target += QStringLiteral("; ") + message;
}

LogIndex::FileStamp stamp(const QString &path)
{
    const QFileInfo info(path);
    LogIndex::FileStamp result;
    result.exists = info.exists() && info.isFile() && !info.isSymLink();
    if (result.exists) {
        result.sizeBytes = info.size();
        result.modifiedUtc = info.lastModified().toUTC();
    }
    return result;
}

struct SampleTime
{
    bool valid = false;
    double seconds = 0.0;
    QDateTime utc;
};

double radians(double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2)
{
    const double dLat = radians(lat2 - lat1);
    const double dLon = radians(lon2 - lon1);
    const double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0)
        + std::cos(radians(lat1)) * std::cos(radians(lat2))
        * std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    return EarthRadiusMeters * 2.0
        * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && !(latitude == 0.0 && longitude == 0.0);
}

class FlightMetrics
{
public:
    explicit FlightMetrics(QVector<qint64> sampleIndices = {})
        : m_sampleIndices(std::move(sampleIndices))
        , m_sampling(!m_sampleIndices.isEmpty())
    {
        if (m_sampling) m_track.reserve(m_sampleIndices.size());
        else m_track.reserve(LogIndex::MaximumTrackPoints);
    }

    void add(double latitude, double longitude, double altitudeMeters,
             double speedMetersPerSecond, const SampleTime &time, bool armed)
    {
        if (!validCoordinate(latitude, longitude)
            || !std::isfinite(altitudeMeters)
            || !std::isfinite(speedMetersPerSecond)) return;
        Position current{latitude, longitude, altitudeMeters, time};
        if (m_hasLastAccepted) {
            const double jump = distanceMeters(
                m_lastAccepted.latitude, m_lastAccepted.longitude,
                latitude, longitude);
            const double seconds = delta(m_lastAccepted.time, time);
            const double maximumJump = seconds > 0.0
                ? 1000.0 + 300.0 * seconds : 100000.0;
            if (jump > maximumJump) return;
        }

        if (!m_home.valid) {
            m_home.valid = true;
            m_home.latitude = latitude;
            m_home.longitude = longitude;
            m_home.altitudeMeters = altitudeMeters;
        }
        const qint64 acceptedIndex = m_accepted++;
        if (m_sampling) {
            if (m_nextSample < m_sampleIndices.size()
                && m_sampleIndices.at(m_nextSample) == acceptedIndex) {
                m_track.append({latitude, longitude});
                ++m_nextSample;
            }
        } else if (!m_trackOverflow) {
            if (m_track.size() < LogIndex::MaximumTrackPoints) {
                m_track.append({latitude, longitude});
            } else {
                m_track.clear();
                m_track.squeeze();
                m_trackOverflow = true;
            }
        }

        if (time.valid) {
            if (!m_hasTime || time.seconds < m_startSeconds) {
                m_startSeconds = time.seconds;
            }
            if (!m_hasTime || time.seconds > m_endSeconds) {
                m_endSeconds = time.seconds;
            }
            m_hasTime = true;
        }
        if (time.utc.isValid()
            && (!m_dateUtc.isValid() || time.utc < m_dateUtc)) {
            m_dateUtc = time.utc;
        }

        if (m_hasLastDistance) {
            const double seconds = delta(m_lastDistance.time, time);
            if (seconds >= 1.0
                || !time.valid || !m_lastDistance.time.valid) {
                m_distanceMeters += distanceMeters(
                    m_lastDistance.latitude, m_lastDistance.longitude,
                    latitude, longitude);
                m_lastDistance = current;
            }
        } else {
            m_lastDistance = current;
            m_hasLastDistance = true;
        }

        if (m_hasLastAccepted) {
            const double seconds = delta(m_lastAccepted.time, time);
            if (seconds > 0.0 && seconds <= 10.0
                && (armed || speedMetersPerSecond > 0.2
                    || altitudeMeters > m_home.altitudeMeters + 2.0)) {
                m_timeInAirSeconds += seconds;
            }
        }
        m_lastAccepted = current;
        m_hasLastAccepted = true;
    }

    void apply(LogIndex::Entry *entry, bool preserveDateAndDuration) const
    {
        if (!preserveDateAndDuration || !entry->dateUtc.isValid()) {
            entry->dateUtc = m_dateUtc;
        }
        if (!preserveDateAndDuration || entry->durationSeconds == 0.0) {
            entry->durationSeconds = m_hasTime
                ? std::max(0.0, m_endSeconds - m_startSeconds) : 0.0;
        }
        entry->home = m_home;
        entry->timeInAirSeconds = std::max(0.0, m_timeInAirSeconds);
        entry->distanceMeters = std::max(0.0, m_distanceMeters);
    }

    qint64 acceptedCount() const { return m_accepted; }
    bool trackOverflow() const { return m_trackOverflow; }
    QVector<LogIndex::Point> track() const { return m_track; }

private:
    struct Position {
        double latitude = 0.0, longitude = 0.0, altitude = 0.0;
        SampleTime time;
    };

    static double delta(const SampleTime &first, const SampleTime &second)
    {
        return first.valid && second.valid
            ? second.seconds - first.seconds : 0.0;
    }

    QVector<qint64> m_sampleIndices;
    QVector<LogIndex::Point> m_track;
    int m_nextSample = 0;
    qint64 m_accepted = 0;
    bool m_sampling = false;
    bool m_trackOverflow = false;
    bool m_hasLastAccepted = false;
    bool m_hasLastDistance = false;
    bool m_hasTime = false;
    Position m_lastAccepted;
    Position m_lastDistance;
    LogIndex::Home m_home;
    QDateTime m_dateUtc;
    double m_startSeconds = 0.0;
    double m_endSeconds = 0.0;
    double m_timeInAirSeconds = 0.0;
    double m_distanceMeters = 0.0;
};

QVector<qint64> uniformSampleIndices(qint64 count)
{
    QVector<qint64> result;
    if (count <= LogIndex::MaximumTrackPoints) return result;
    result.reserve(LogIndex::MaximumTrackPoints);
    const qint64 denominator = LogIndex::MaximumTrackPoints - 1;
    const qint64 whole = (count - 1) / denominator;
    const qint64 remainder = (count - 1) % denominator;
    for (qint64 index = 0; index < LogIndex::MaximumTrackPoints; ++index) {
        // Exact integer equivalent of C# Math.Round's default midpoint-to-even
        // policy. Splitting count first keeps every intermediate in qint64.
        const qint64 residual = index * remainder;
        qint64 source = index * whole + residual / denominator;
        const qint64 fraction = residual % denominator;
        if (fraction * 2 > denominator
            || (fraction * 2 == denominator && (source & 1))) {
            ++source;
        }
        result.append(source);
    }
    return result;
}

double binaryScalar(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, int index)
{
    if (index < 0 || index >= definition.format.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const int offset = definition.offsets.at(index);
    const int width = DataFlashRaw::width(definition.format.at(index));
    if (offset < 0 || width <= 0 || offset + width > raw.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + offset);
    switch (definition.format.at(index)) {
    case 'b': return qint8(*at);
    case 'B': case 'M': return *at;
    case 'h': return qFromLittleEndian<qint16>(at);
    case 'H': return qFromLittleEndian<quint16>(at);
    case 'i': return qFromLittleEndian<qint32>(at);
    case 'I': return qFromLittleEndian<quint32>(at);
    case 'q': return static_cast<double>(qFromLittleEndian<qint64>(at));
    case 'Q': return static_cast<double>(qFromLittleEndian<quint64>(at));
    case 'c': return qFromLittleEndian<qint16>(at) / 100.0;
    case 'C': return qFromLittleEndian<quint16>(at) / 100.0;
    case 'e': return qFromLittleEndian<qint32>(at) / 100.0;
    case 'E': return qFromLittleEndian<quint32>(at) / 100.0;
    case 'L': return qFromLittleEndian<qint32>(at) / 10000000.0;
    case 'f': {
        const quint32 bits = qFromLittleEndian<quint32>(at);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case 'd': {
        const quint64 bits = qFromLittleEndian<quint64>(at);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case 'g': {
        const quint16 bits = qFromLittleEndian<quint16>(at);
        const int exponent = (bits >> 10) & 31;
        const int fraction = bits & 1023;
        if (exponent == 31) return std::numeric_limits<double>::quiet_NaN();
        const double value = exponent
            ? std::ldexp(double(1024 + fraction), exponent - 25)
            : std::ldexp(double(fraction), -24);
        return bits & 0x8000 ? -value : value;
    }
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

bool fieldNumber(const QByteArray &raw,
                 const DataFlashRaw::Definition &definition, bool text,
                 const QList<QByteArray> &names, double *value)
{
    int index = -1;
    for (const QByteArray &name : names) {
        index = definition.columns.indexOf(name);
        if (index >= 0) break;
    }
    if (index < 0) return false;
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (text) {
        const auto values = DataFlashRaw::textValues(raw, definition);
        if (index >= values.size()) return false;
        bool ok = false;
        parsed = values.at(index).trimmed().toDouble(&ok);
        if (!ok) return false;
    } else {
        parsed = binaryScalar(raw, definition, index);
    }
    if (!std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

QByteArray fieldText(const QByteArray &raw,
                     const DataFlashRaw::Definition &definition, bool text,
                     const QList<QByteArray> &names)
{
    int index = -1;
    for (const QByteArray &name : names) {
        index = definition.columns.indexOf(name);
        if (index >= 0) break;
    }
    if (index < 0) return {};
    if (text) {
        const auto values = DataFlashRaw::textValues(raw, definition);
        return index < values.size() ? values.at(index).trimmed() : QByteArray();
    }
    const char encoding = definition.format.at(index);
    if (!QByteArray("nNZaA").contains(encoding)) return {};
    return DataFlashRaw::ascii(raw.mid(
        definition.offsets.at(index), DataFlashRaw::width(encoding)));
}

QDateTime gpsWeekUtc(double weekValue, double millisecondsValue)
{
    if (!std::isfinite(weekValue) || !std::isfinite(millisecondsValue)
        || weekValue != std::trunc(weekValue)
        || weekValue < 0.0 || weekValue > 5000.0
        || millisecondsValue < 0.0
        || millisecondsValue > 7.0 * 24.0 * 60.0 * 60.0 * 1000.0) {
        return {};
    }
    const QDateTime epoch(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC);
    QDateTime gps = epoch.addDays(static_cast<qint64>(weekValue) * 7)
        .addMSecs(static_cast<qint64>(std::llround(millisecondsValue)));
    if (!gps.isValid()) return {};
    struct Leap { int year, month, day, total; };
    static const Leap leaps[] = {
        {1981, 7, 1, 1}, {1982, 7, 1, 2}, {1983, 7, 1, 3},
        {1985, 7, 1, 4}, {1988, 1, 1, 5}, {1990, 1, 1, 6},
        {1991, 1, 1, 7}, {1992, 7, 1, 8}, {1993, 7, 1, 9},
        {1994, 7, 1, 10}, {1996, 1, 1, 11}, {1997, 7, 1, 12},
        {1999, 1, 1, 13}, {2006, 1, 1, 14}, {2009, 1, 1, 15},
        {2012, 7, 1, 16}, {2015, 7, 1, 17}, {2017, 1, 1, 18},
    };
    int offset = 0;
    for (const Leap &leap : leaps) {
        const QDateTime effective(
            QDate(leap.year, leap.month, leap.day), QTime(0, 0), Qt::UTC);
        if (gps >= effective.addSecs(leap.total)) offset = leap.total;
        else break;
    }
    return gps.addSecs(-offset);
}

bool fieldIntegral(const QByteArray &raw,
                   const DataFlashRaw::Definition &definition, bool text,
                   const QList<QByteArray> &names, qint64 *value)
{
    int index = -1;
    for (const QByteArray &name : names) {
        index = definition.columns.indexOf(name);
        if (index >= 0) break;
    }
    if (index < 0) return false;
    if (text) {
        const auto values = DataFlashRaw::textValues(raw, definition);
        if (index >= values.size()) return false;
        bool ok = false;
        const qint64 parsed = values.at(index).trimmed().toLongLong(&ok, 10);
        if (!ok) return false;
        *value = parsed;
        return true;
    }
    double number = 0.0;
    if (!fieldNumber(raw, definition, false, names, &number)
        || number != std::trunc(number)
        || number < -9223372036854775808.0
        || number >= 9223372036854775808.0) {
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

bool messageMilliseconds(const QByteArray &raw,
                         const DataFlashRaw::Definition &definition,
                         bool text, double *milliseconds)
{
    qint64 value = 0;
    if (definition.columns.contains("TimeMS")) {
        if (!fieldIntegral(raw, definition, text, {"TimeMS"}, &value)
            || value < 0) return false;
        *milliseconds = static_cast<double>(value);
        return true;
    }
    if (definition.columns.contains("TimeUS")) {
        if (!fieldIntegral(raw, definition, text, {"TimeUS"}, &value)
            || value < 0) return false;
        *milliseconds = static_cast<double>(value) / 1000.0;
        return true;
    }
    if (definition.columns.contains("T")) {
        if (!fieldIntegral(raw, definition, text, {"T"}, &value)
            || value < 0) return false;
        *milliseconds = static_cast<double>(value);
        return true;
    }
    return false;
}

QDateTime absoluteGpsTime(const QByteArray &raw,
                          const DataFlashRaw::Definition &definition,
                          bool text)
{
    qint64 week = 0, milliseconds = 0;
    if (!fieldIntegral(raw, definition, text, {"Week", "GWk"}, &week)
        || !fieldIntegral(raw, definition, text,
                          {"TimeMS", "GMS"}, &milliseconds)) {
        return {};
    }
    return gpsWeekUtc(static_cast<double>(week),
                      static_cast<double>(milliseconds));
}

class DataFlashClock final
{
public:
    bool anchored() const { return m_anchorUtc.isValid(); }

    bool tryAnchor(const QByteArray &raw,
                   const DataFlashRaw::Definition &definition, bool text)
    {
        if (anchored()) return true;
        double status = 0.0;
        if (!fieldNumber(raw, definition, text, {"Status"}, &status)
            || status < 3.0) return false;
        const QDateTime utc = absoluteGpsTime(raw, definition, text);
        if (!utc.isValid()) return false;

        qint64 offset = 0;
        qint64 candidate = 0;
        if (definition.columns.contains("T")) {
            if (!fieldIntegral(raw, definition, text, {"T"}, &candidate)
                || candidate < std::numeric_limits<int>::min()
                || candidate > std::numeric_limits<int>::max()) {
                return false;
            }
            offset = candidate;
        }
        // This intentionally mirrors DFItem's integer division: the first
        // valid GPS TimeUS establishes msoffset at whole-millisecond precision.
        if (definition.columns.contains("TimeUS")) {
            if (!fieldIntegral(raw, definition, text,
                               {"TimeUS"}, &candidate)) {
                return false;
            }
            offset = candidate / 1000;
        }
        m_anchorUtc = utc;
        m_offsetMilliseconds = offset;
        return true;
    }

    SampleTime sample(const QByteArray &raw,
                      const DataFlashRaw::Definition &definition,
                      bool text) const
    {
        double milliseconds = 0.0;
        const bool hasMessageTime = messageMilliseconds(
            raw, definition, text, &milliseconds);
        if (!hasMessageTime && !anchored()) return {};

        SampleTime result;
        result.valid = true;
        // Keep flight deltas near the boot-time origin. Converting an epoch to
        // double seconds before subtraction loses the sub-millisecond detail
        // that MP10 retains in DateTime ticks.
        result.seconds = (hasMessageTime ? milliseconds : 0.0) / 1000.0;
        if (anchored()) {
            const double deltaMilliseconds =
                (hasMessageTime ? milliseconds : 0.0)
                - static_cast<double>(m_offsetMilliseconds);
            result.utc = m_anchorUtc.addMSecs(
                static_cast<qint64>(deltaMilliseconds));
        }
        return result;
    }

private:
    QDateTime m_anchorUtc;
    qint64 m_offsetMilliseconds = 0;
};

bool isGpsClockSource(const QByteArray &name)
{
    return name == "GPS" || name == "GPS2" || name == "GPSB";
}

QString detectFrame(const QByteArray &message)
{
    static const char *const frames[] = {
        "ArduCopter", "ArduPlane", "ArduRover", "ArduSub",
        "AntennaTracker", "ArduTracker",
    };
    const QString text = QString::fromUtf8(message);
    for (const char *frame : frames) {
        if (text.contains(QString::fromLatin1(frame), Qt::CaseInsensitive)) {
            return QString::fromLatin1(frame);
        }
    }
    return {};
}

PassStatus discoverDataFlashClock(const QString &path, bool text,
                                  const LogIndex::Cancel &cancel,
                                  DataFlashClock *clock)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return PassStatus::Complete;
    const qint64 snapshotLength = file.size();
    DataFlashRaw::Reader reader(&file, text);
    QByteArray raw;
    int kind = -1;
    while (file.pos() < snapshotLength) {
        if (cancelled(cancel)) return PassStatus::Cancelled;
        if (!reader.next(&raw, &kind)) break;
        if (kind == -1 && isGpsClockSource(reader.currentDefinition.name)
            && clock->tryAnchor(raw, reader.currentDefinition, text)) {
            break;
        }
    }
    return cancelled(cancel) ? PassStatus::Cancelled : PassStatus::Complete;
}

PassStatus parseDataFlash(const QString &path, bool text,
                          const LogIndex::Cancel &cancel,
                          bool metadata, FlightMetrics *flight,
                          LogIndex::Entry *entry, QString *error,
                          const DataFlashClock &clock)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to open DataFlash log: %1")
            .arg(file.errorString());
        return PassStatus::PartialError;
    }
    const qint64 snapshotLength = file.size();
    DataFlashRaw::Reader reader(&file, text);
    QByteArray raw;
    int kind = -1;
    while (file.pos() < snapshotLength) {
        if (cancelled(cancel)) return PassStatus::Cancelled;
        if (!reader.next(&raw, &kind)) break;
        if (kind >= 0 || kind == -2) continue;
        const auto &definition = reader.currentDefinition;
        if (definition.name == "GPS") {
            double status = 0.0, latitude = 0.0, longitude = 0.0;
            double altitude = 0.0, speed = 0.0;
            if (!fieldNumber(raw, definition, text, {"Status"}, &status)
                || status < 3.0
                || !fieldNumber(raw, definition, text, {"Lat"}, &latitude)
                || !fieldNumber(raw, definition, text, {"Lng"}, &longitude)
                || !fieldNumber(raw, definition, text, {"Alt"}, &altitude)) {
                continue;
            }
            if (!fieldNumber(raw, definition, text, {"Spd"}, &speed)) {
                speed = 0.0;
            }
            flight->add(latitude, longitude, altitude, speed,
                        clock.sample(raw, definition, text), false);
        } else if (metadata && definition.name == "CAM") {
            ++entry->cameraMessages;
        } else if (metadata && definition.name == "MSG") {
            const QString frame = detectFrame(fieldText(
                raw, definition, text, {"Message", "Msg", "Text"}));
            if (!frame.isEmpty()) entry->frame = frame;
        } else if (metadata && definition.name == "PARM") {
            const QByteArray name = fieldText(
                raw, definition, text, {"Name"});
            if (name.compare("SYSID_THISMAV", Qt::CaseInsensitive) == 0) {
                double value = 0.0;
                if (fieldNumber(raw, definition, text, {"Value"}, &value)
                    && value == std::trunc(value)
                    && value >= std::numeric_limits<int>::min()
                    && value <= std::numeric_limits<int>::max()) {
                    entry->systemId = static_cast<int>(value);
                }
            }
        }
    }
    if (cancelled(cancel)) return PassStatus::Cancelled;
    if (!reader.error.isEmpty()) {
        *error = reader.error;
        return PassStatus::PartialError;
    }
    if (file.error() != QFileDevice::NoError) {
        *error = file.errorString();
        return PassStatus::PartialError;
    }
    if (metadata && reader.blankLines > 0) {
        appendError(&entry->error, QStringLiteral(
            "Ignored %1 blank DataFlash text line(s).").arg(reader.blankLines));
    }
    if (metadata && reader.trailingBytes > 0) {
        appendError(&entry->error, QStringLiteral(
            "Ignored %1 byte(s) of an incomplete final DataFlash record.")
            .arg(reader.trailingBytes));
    }
    return PassStatus::Complete;
}

QString mavTypeName(int type)
{
    static const char *const names[] = {
        "GENERIC", "FIXED_WING", "QUADROTOR", "COAXIAL", "HELICOPTER",
        "ANTENNA_TRACKER", "GCS", "AIRSHIP", "FREE_BALLOON", "ROCKET",
        "GROUND_ROVER", "SURFACE_BOAT", "SUBMARINE", "HEXAROTOR", "OCTOROTOR",
        "TRICOPTER", "FLAPPING_WING", "KITE", "ONBOARD_CONTROLLER", "VTOL_DUOROTOR",
        "VTOL_QUADROTOR", "VTOL_TILTROTOR", "VTOL_RESERVED2", "VTOL_RESERVED3",
        "VTOL_RESERVED4", "VTOL_RESERVED5", "GIMBAL", "ADSB", "PARAFOIL",
        "DODECAROTOR", "CAMERA", "CHARGING_STATION", "FLARM", "SERVO", "ODID",
        "DECAROTOR", "BATTERY", "PARACHUTE", "LOG", "OSD", "IMU", "GPS", "WINCH",
    };
    return type >= 0 && type < int(sizeof(names) / sizeof(names[0]))
        ? QString::fromLatin1(names[type]) : QString::number(type);
}

PassStatus parseTlog(const QString &path, const LogIndex::Cancel &cancel,
                     bool metadata, FlightMetrics *flight,
                     LogIndex::Entry *entry, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to open telemetry log: %1")
            .arg(file.errorString());
        return PassStatus::PartialError;
    }
    TlogReader reader(&file);
    reader.setCancelCheck(cancel);
    QHash<quint8, quint8> fixes;
    QHash<quint8, bool> armed;
    quint8 primarySystem = 0;
    QDateTime packetStart;
    qint64 packetStartUsec = 0, packetEndUsec = 0;
    bool hasPacketTime = false;
    bool sitl = false;
    TlogRecord record;
    TlogReader::Status status = TlogReader::Status::Ok;
    while ((status = reader.next(&record)) == TlogReader::Status::Ok) {
        const mavlink_message_t &message = record.message;
        if (message.sysid == 255) continue;
        const QDateTime timestamp = record.timestampUsec >= 0
            ? record.timestampUtc() : QDateTime();
        if (timestamp.isValid()) {
            if (!packetStart.isValid() || timestamp < packetStart) {
                packetStart = timestamp;
            }
            if (!hasPacketTime || record.timestampUsec < packetStartUsec) {
                packetStartUsec = record.timestampUsec;
            }
            if (!hasPacketTime || record.timestampUsec > packetEndUsec) {
                packetEndUsec = record.timestampUsec;
            }
            hasPacketTime = true;
        }
        switch (message.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT: {
            mavlink_heartbeat_t heartbeat{};
            mavlink_msg_heartbeat_decode(&message, &heartbeat);
            if (!primarySystem) primarySystem = message.sysid;
            armed.insert(message.sysid,
                (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0);
            if (metadata && message.sysid == primarySystem) {
                entry->frame = mavTypeName(heartbeat.type);
            }
            break;
        }
        case MAVLINK_MSG_ID_GPS_RAW_INT: {
            mavlink_gps_raw_int_t gps{};
            mavlink_msg_gps_raw_int_decode(&message, &gps);
            fixes.insert(message.sysid, gps.fix_type);
            break;
        }
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
            mavlink_global_position_int_t position{};
            mavlink_msg_global_position_int_decode(&message, &position);
            if (!primarySystem) primarySystem = message.sysid;
            if (message.sysid != primarySystem
                || (fixes.contains(message.sysid)
                    && fixes.value(message.sysid) < 3)) break;
            const double speed = std::hypot(
                static_cast<double>(position.vx),
                static_cast<double>(position.vy)) / 100.0;
            SampleTime sampleTime;
            if (timestamp.isValid()) {
                sampleTime.valid = true;
                sampleTime.seconds = record.timestampUsec / 1000000.0;
                sampleTime.utc = timestamp;
            }
            flight->add(position.lat / 1e7, position.lon / 1e7,
                        position.alt / 1000.0, speed, sampleTime,
                        armed.value(message.sysid, false));
            break;
        }
        case MAVLINK_MSG_ID_CAMERA_FEEDBACK:
            if (metadata) ++entry->cameraMessages;
            break;
        case MAVLINK_MSG_ID_SIM_STATE:
        case MAVLINK_MSG_ID_SIMSTATE:
            sitl = true;
            break;
        default:
            break;
        }
    }
    if (status == TlogReader::Status::Cancelled) {
        return PassStatus::Cancelled;
    }
    if (metadata) {
        entry->systemId = primarySystem;
        entry->dateUtc = packetStart;
        entry->durationSeconds = hasPacketTime
            ? std::max(0.0, (packetEndUsec - packetStartUsec) / 1000000.0)
            : 0.0;
        if (sitl) entry->frame += QStringLiteral(" (SITL)");
    }
    if (status == TlogReader::Status::Error) {
        *error = reader.errorString();
        return PassStatus::PartialError;
    }
    if (status == TlogReader::Status::Truncated) {
        *error = reader.errorString();
        return PassStatus::PartialError;
    }
    if (reader.rejectedFrames() > 0 || reader.skippedBytes() > 0) {
        *error = QStringLiteral(
            "Recovered after %1 rejected frame(s) and %2 skipped byte(s).")
            .arg(reader.rejectedFrames()).arg(reader.skippedBytes());
        return PassStatus::PartialError;
    }
    return PassStatus::Complete;
}

bool sourcePreflight(LogIndex::Entry *entry, LogIndex::FileStamp *before,
                     QString *error)
{
    if (entry->fullPath.trimmed().isEmpty()) {
        *error = QStringLiteral("No log path was supplied.");
        return false;
    }
    const QFileInfo info(entry->fullPath);
    if (info.isSymLink()) {
        *error = QStringLiteral("A symbolic-link log cannot be indexed.");
        return false;
    }
    *before = stamp(entry->fullPath);
    if (!before->exists) {
        *error = QStringLiteral("The log does not exist or is not a regular file.");
        return false;
    }
    if (entry->source.exists && entry->source != *before) {
        *error = QStringLiteral("The log changed before analysis; rescan the directory.");
        return false;
    }
    entry->source = *before;
    return true;
}

void sortEntries(QVector<LogIndex::Entry> *entries)
{
    std::sort(entries->begin(), entries->end(),
              [](const LogIndex::Entry &left, const LogIndex::Entry &right) {
        const QDateTime leftDate = left.dateUtc.isValid()
            ? left.dateUtc : left.source.modifiedUtc;
        const QDateTime rightDate = right.dateUtc.isValid()
            ? right.dateUtc : right.source.modifiedUtc;
        if (leftDate != rightDate) return leftDate > rightDate;
#ifdef Q_OS_WIN
        return QString::compare(left.fullPath, right.fullPath,
                                Qt::CaseInsensitive) < 0;
#else
        return left.fullPath < right.fullPath;
#endif
    });
}

class SerializedCancel final
{
public:
    explicit SerializedCancel(LogIndex::Cancel callback)
        : m_callback(std::move(callback)) {}

    bool check()
    {
        if (m_stopped.load(std::memory_order_acquire)) return true;
        if (!m_callback) return false;
        QMutexLocker locker(&m_mutex);
        if (!m_stopped.load(std::memory_order_relaxed)
            && m_callback && m_callback()) {
            m_stopped.store(true, std::memory_order_release);
        }
        return m_stopped.load(std::memory_order_relaxed);
    }

    bool stopped() const
    {
        return m_stopped.load(std::memory_order_acquire);
    }

private:
    LogIndex::Cancel m_callback;
    mutable QMutex m_mutex;
    std::atomic_bool m_stopped{false};
};

struct ScanWork
{
    LogIndex::Entry entry;
    bool cancelled = false;
};

ScanWork scanOne(LogIndex::Entry source, const LogIndex::TileReader &tiles,
                 const std::shared_ptr<SerializedCancel> &stop)
{
    ScanWork result;
    result.entry = source;
    const LogIndex::Cancel cancel = [stop] { return stop->check(); };
    try {
        LogIndex::Analysis analysis = LogIndexService::analyzeFile(source, cancel);
        if (analysis.cancelled) {
            result.cancelled = true;
            return result;
        }
        result.entry = analysis.entry;
        const LogIndex::ThumbnailResult thumbnail = LogIndexFiles::thumbnail(
            result.entry, analysis.track, tiles, cancel);
        analysis.track.clear();
        analysis.track.squeeze();
        if (thumbnail.cancelled) {
            result.cancelled = true;
            return result;
        }
        result.entry.thumbnail = thumbnail.sidecar;
        result.entry.thumbnailJpeg = thumbnail.jpeg;
        appendError(&result.entry.error, thumbnail.warning);
        QString unchangedError;
        if (!LogIndexFiles::unchanged(result.entry, &unchangedError)) {
            appendError(&result.entry.error, unchangedError.isEmpty()
                ? QStringLiteral("The log changed during indexing; rescan it.")
                : unchangedError);
        }
    } catch (const std::exception &exception) {
        appendError(&result.entry.error,
                    QStringLiteral("Log analysis failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        appendError(&result.entry.error,
                    QStringLiteral("Log analysis failed unexpectedly."));
    }
    return result;
}
}

LogIndex::Analysis LogIndexService::analyzeFile(
    const LogIndex::Entry &source, const LogIndex::Cancel &cancel)
{
    LogIndex::Analysis result;
    result.entry = source;
    result.entry.dateUtc = {};
    result.entry.frame = source.fullPath.endsWith(
        QStringLiteral(".tlog"), Qt::CaseInsensitive)
        ? QStringLiteral("Unknown") : QStringLiteral("DFLog Unknown");
    result.entry.systemId = 0;
    result.entry.durationSeconds = 0.0;
    result.entry.timeInAirSeconds = 0.0;
    result.entry.distanceMeters = 0.0;
    result.entry.cameraMessages = 0;
    result.entry.home = {};
    result.entry.error.clear();
    result.entry.thumbnailJpeg.clear();
    if (cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }

    LogIndex::FileStamp before;
    if (!sourcePreflight(&result.entry, &before, &result.entry.error)) {
        return result;
    }
    const bool telemetry = result.entry.fullPath.endsWith(
        QStringLiteral(".tlog"), Qt::CaseInsensitive);
    const bool text = result.entry.fullPath.endsWith(
        QStringLiteral(".log"), Qt::CaseInsensitive);
    if (!telemetry && !text
        && !result.entry.fullPath.endsWith(
            QStringLiteral(".bin"), Qt::CaseInsensitive)) {
        result.entry.error = QStringLiteral("Unsupported log extension.");
        return result;
    }

    DataFlashClock dataFlashClock;
    // A late or absent GPS UTC anchor can require a complete extra pass, but
    // every pass remains streaming and bounded by the snapshotted file size.
    if (!telemetry
        && discoverDataFlashClock(result.entry.fullPath, text, cancel,
                                  &dataFlashClock) == PassStatus::Cancelled) {
        result.cancelled = true;
        return result;
    }
    FlightMetrics metrics;
    QString parseError;
    const PassStatus first = telemetry
        ? parseTlog(result.entry.fullPath, cancel, true,
                    &metrics, &result.entry, &parseError)
        : parseDataFlash(result.entry.fullPath, text, cancel, true,
                         &metrics, &result.entry, &parseError,
                         dataFlashClock);
    if (first == PassStatus::Cancelled) {
        result.cancelled = true;
        return result;
    }
    appendError(&result.entry.error, parseError);
    metrics.apply(&result.entry, telemetry);

    if (!metrics.trackOverflow()) {
        result.track = metrics.track();
    } else if (!cancelled(cancel)) {
        if (stamp(result.entry.fullPath) != before) {
            appendError(&result.entry.error, QStringLiteral(
                "The log changed before the bounded track sampling pass; rescan the directory."));
        } else {
            FlightMetrics sampler(uniformSampleIndices(metrics.acceptedCount()));
            LogIndex::Entry ignored;
            ignored.frame = result.entry.frame;
            QString secondError;
            const PassStatus second = telemetry
                ? parseTlog(result.entry.fullPath, cancel, false,
                            &sampler, &ignored, &secondError)
                : parseDataFlash(result.entry.fullPath, text, cancel, false,
                                 &sampler, &ignored, &secondError,
                                 dataFlashClock);
            if (second == PassStatus::Cancelled) {
                result.cancelled = true;
                return result;
            }
            appendError(&result.entry.error, secondError.isEmpty()
                ? QString() : QStringLiteral("Track sampling: %1").arg(secondError));
            result.track = sampler.track();
        }
    }

    if (cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }
    if (stamp(result.entry.fullPath) != before) {
        appendError(&result.entry.error, QStringLiteral(
            "The log changed during analysis; displayed metrics may be partial. Rescan before using file actions."));
    }
    return result;
}

LogIndex::ScanResult LogIndexService::scan(
    const QString &root, const LogIndex::Cancel &cancel,
    const LogIndex::Progress &progress, const LogIndex::TileReader &tiles)
{
    LogIndex::ScanResult result;
    const LogIndex::Discovery discovery = LogIndexFiles::discover(root, cancel);
    result.rootPath = discovery.rootPath;
    result.warnings = discovery.warnings;
    if (!discovery.success) {
        result.cancelled = discovery.cancelled;
        result.error = discovery.error;
        return result;
    }

    result.entries.reserve(discovery.files.size());
    const auto stop = std::make_shared<SerializedCancel>(cancel);
    const int ideal = QThread::idealThreadCount();
    const int parallelism = qBound(1, ideal > 0 ? ideal / 2 : 1, 4);
    QThreadPool pool;
    pool.setMaxThreadCount(parallelism);
    pool.setExpiryTimeout(-1);
    qint64 thumbnailBudget = MaximumInMemoryThumbnailBytes;
    const int total = discovery.files.size();
    int next = 0;
    int completed = 0;
    while (next < total && !stop->check()) {
        QVector<QFuture<ScanWork>> futures;
        futures.reserve(qMin(parallelism, total - next));
        while (next < total && futures.size() < parallelism
               && !stop->stopped()) {
            const LogIndex::Entry source = discovery.files.at(next++);
            futures.append(QtConcurrent::run(
                &pool, [source, tiles, stop] { return scanOne(source, tiles, stop); }));
        }
        for (QFuture<ScanWork> &future : futures) {
            future.waitForFinished();
            ScanWork work = future.result();
            if (work.cancelled) {
                result.cancelled = true;
                continue;
            }
            if (work.entry.thumbnailJpeg.size() <= thumbnailBudget) {
                thumbnailBudget -= work.entry.thumbnailJpeg.size();
            } else {
                work.entry.thumbnailJpeg.clear();
                if (!work.entry.thumbnail.exists) {
                    appendError(&work.entry.error, QStringLiteral(
                        "The in-memory thumbnail budget was reached and no sidecar was available."));
                }
            }
            result.entries.append(std::move(work.entry));
            ++completed;
            if (progress) {
                progress(completed, total, result.entries.last().fullPath);
            }
            // The callback is external code and may synchronously replace a
            // file. Detect that before returning an apparently current row.
            QString callbackChange;
            if (!LogIndexFiles::unchanged(result.entries.last(),
                                          &callbackChange)) {
                appendError(&result.entries.last().error,
                            callbackChange.isEmpty()
                    ? QStringLiteral("The log changed during progress notification; rescan it.")
                    : callbackChange);
            }
            if (stop->check()) result.cancelled = true;
        }
        if (result.cancelled || stop->stopped()) break;
    }
    pool.waitForDone();
    if (stop->stopped()) result.cancelled = true;
    sortEntries(&result.entries);
    result.success = !result.cancelled;
    if (result.cancelled && result.error.isEmpty()) {
        result.error = QStringLiteral("Flight log indexing was cancelled.");
    }
    return result;
}
