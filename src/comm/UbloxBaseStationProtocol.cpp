#include "UbloxBaseStationProtocol.h"

#include <QtEndian>
#include <cmath>
#include <limits>

namespace {
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
template<typename T> T get(const QByteArray &p, int offset)
{ return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(p.constData() + offset)); }
template<typename T> void put(QByteArray &p, int offset, T value)
{ qToLittleEndian<T>(value, reinterpret_cast<uchar *>(p.data() + offset)); }
QString ascii(const QByteArray &p)
{ return QString::fromLatin1(p.left(p.indexOf('\0') < 0 ? p.size() : p.indexOf('\0'))).trimmed(); }
bool packetIs(const UbloxBaseStationProtocol::Packet &p, int cls, int id, int length,
              const void *out, QString *error)
{
    if (error) error->clear();
    return (out && p.messageClass == cls && p.messageId == id && p.payload.size() == length)
        || fail(error, QStringLiteral("Unexpected UBX message or payload length"));
}
bool accuracy(double value, double scale, quint32 *out, QString *error)
{
    const double scaled = value * scale;
    if (!std::isfinite(value) || value <= 0 || scaled < 1
        || scaled > std::numeric_limits<quint32>::max())
        return fail(error, QStringLiteral("Accuracy is outside the receiver's representable positive range"));
    *out = quint32(scaled);
    return true;
}
UbloxBaseStationProtocol::Command command(const QByteArray &bytes, const QString &description,
                                         int before = 0, int after = 0, int baud = 0)
{
    UbloxBaseStationProtocol::Command result;
    result.bytes = bytes; result.description = description; result.delayBeforeMs = before;
    result.delayAfterMs = after; result.baudRate = baud;
    return result;
}
}

void UbloxBaseStationProtocol::reset()
{ m_buffer.clear(); m_expected = 0; m_statistics = Statistics{}; }

QByteArray UbloxBaseStationProtocol::frame(quint8 cls, quint8 id, const QByteArray &payload)
{
    if (payload.size() > MaximumPayload) return {};
    QByteArray bytes = QByteArray::fromHex("b562");
    bytes.append(char(cls)); bytes.append(char(id));
    bytes.append(char(payload.size() & 255)); bytes.append(char(payload.size() >> 8));
    bytes.append(payload);
    quint8 a = 0, b = 0;
    for (int i = 2; i < bytes.size(); ++i) { a += quint8(bytes.at(i)); b += a; }
    bytes.append(char(a)); bytes.append(char(b));
    return bytes;
}

bool UbloxBaseStationProtocol::read(quint8 byte, Packet *completed)
{
    if (m_buffer.isEmpty()) {
        if (byte == 0xb5) m_buffer.append(char(byte));
        return false;
    }
    if (m_buffer.size() == 1 && byte != 0x62) {
        m_buffer.clear();
        if (byte == 0xb5) m_buffer.append(char(byte));
        return false;
    }
    m_buffer.append(char(byte));
    if (m_buffer.size() == 6) {
        const int length = get<quint16>(m_buffer, 4);
        if (length > MaximumPayload) {
            ++m_statistics.oversizedFrames;
            m_buffer.clear(); m_expected = 0;
            if (byte == 0xb5) m_buffer.append(char(byte));
            return false;
        }
        m_expected = length + 8;
    }
    if (!m_expected || m_buffer.size() < m_expected) return false;
    quint8 a = 0, b = 0;
    for (int i = 2; i < m_expected - 2; ++i) { a += quint8(m_buffer.at(i)); b += a; }
    const bool valid = a == quint8(m_buffer.at(m_expected - 2)) && b == byte;
    if (valid) {
        ++m_statistics.frames;
        if (completed) {
            completed->messageClass = quint8(m_buffer.at(2));
            completed->messageId = quint8(m_buffer.at(3));
            completed->payload = m_buffer.mid(6, m_expected - 8);
        }
    } else ++m_statistics.badChecksums;
    m_buffer.clear(); m_expected = 0;
    if (!valid && byte == 0xb5) m_buffer.append(char(byte));
    return valid;
}

bool UbloxBaseStationProtocol::decodeVersion(const Packet &p, Version *out, QString *error)
{
    if (error) error->clear();
    if (!out || p.messageClass != 0x0a || p.messageId != 4 || p.payload.size() < 40
        || p.payload.size() > MaximumPayload || (p.payload.size() - 40) % 30)
        return fail(error, QStringLiteral("Invalid UBX MON-VER payload"));
    Version result;
    result.software = ascii(p.payload.left(30)); result.hardware = ascii(p.payload.mid(30, 10));
    for (int offset = 40; offset < p.payload.size(); offset += 30)
        result.extensions.append(ascii(p.payload.mid(offset, 30)));
    *out = result; return true;
}

bool UbloxBaseStationProtocol::decodeAcknowledgement(const Packet &p, Acknowledgement *out,
                                                    QString *error)
{
    if (error) error->clear();
    if (!out || p.messageClass != 5 || p.messageId > 1 || p.payload.size() != 2)
        return fail(error, QStringLiteral("Invalid UBX ACK/NAK payload"));
    out->accepted = p.messageId == 1; out->messageClass = quint8(p.payload.at(0));
    out->messageId = quint8(p.payload.at(1)); return true;
}

bool UbloxBaseStationProtocol::decodeSurveyIn(const Packet &p, SurveyIn *out, QString *error)
{
    if (!packetIs(p, 1, 0x3b, 40, out, error)) return false;
    const QByteArray &v = p.payload;
    if (v.at(0) != 0 || quint8(v.at(36)) > 1 || quint8(v.at(37)) > 1)
        return fail(error, QStringLiteral("Unsupported NAV-SVIN version or flags"));
    for (int i = 24; i <= 26; ++i)
        if (std::abs(int(qint8(v.at(i)))) > 99)
            return fail(error, QStringLiteral("Invalid NAV-SVIN high precision component"));
    SurveyIn result;
    result.timeOfWeekMs = get<quint32>(v, 4); result.durationSeconds = get<quint32>(v, 8);
    result.xMeters = get<qint32>(v, 12) * .01 + qint8(v.at(24)) * .0001;
    result.yMeters = get<qint32>(v, 16) * .01 + qint8(v.at(25)) * .0001;
    result.zMeters = get<qint32>(v, 20) * .01 + qint8(v.at(26)) * .0001;
    result.accuracyMeters = get<quint32>(v, 28) * .0001;
    result.observations = get<quint32>(v, 32);
    result.valid = v.at(36) == 1; result.active = v.at(37) == 1;
    // WGS84 ECEF -> geodetic. At the Earth's centre no geographic position exists.
    constexpr double a = 6378137.0, b = 6356752.3142451793;
    constexpr double e2 = 1.0 - b * b / (a * a), ep2 = a * a / (b * b) - 1.0;
    const double radial = std::hypot(result.xMeters, result.yMeters);
    if (std::hypot(radial, result.zMeters) > 1) {
        const double theta = std::atan2(result.zMeters * a, radial * b);
        const double st = std::sin(theta), ct = std::cos(theta);
        const double lat = std::atan2(result.zMeters + ep2 * b * st * st * st,
                                      radial - e2 * a * ct * ct * ct);
        const double sinLat = std::sin(lat), cosLat = std::cos(lat);
        const double n = a / std::sqrt(1 - e2 * sinLat * sinLat);
        result.latitude = lat * (180.0 / std::acos(-1.0));
        result.longitude = std::atan2(result.yMeters, result.xMeters) * (180.0 / std::acos(-1.0));
        result.altitudeMeters = radial > 1e-6 ? radial / cosLat - n : std::abs(result.zMeters) - b;
        result.hasPosition = std::isfinite(result.altitudeMeters);
    }
    *out = result; return true;
}

bool UbloxBaseStationProtocol::decodePosition(const Packet &p, Position *out, QString *error)
{
    if (!packetIs(p, 1, 7, 92, out, error)) return false;
    const QByteArray &v = p.payload;
    Position result;
    result.timeOfWeekMs = get<quint32>(v, 0); result.fixType = quint8(v.at(20));
    const quint8 flags = quint8(v.at(21));
    result.fixOk = flags & 1; result.differential = flags & 2; result.carrierSolution = flags >> 6;
    result.satellites = quint8(v.at(23));
    result.longitude = get<qint32>(v, 24) * 1e-7; result.latitude = get<qint32>(v, 28) * 1e-7;
    result.altitudeMeters = get<qint32>(v, 32) * .001;
    result.mslAltitudeMeters = get<qint32>(v, 36) * .001;
    result.horizontalAccuracyMeters = get<quint32>(v, 40) * .001;
    result.verticalAccuracyMeters = get<quint32>(v, 44) * .001;
    if (result.fixType > 5 || std::abs(result.latitude) > 90 || std::abs(result.longitude) > 180)
        return fail(error, QStringLiteral("Invalid NAV-PVT fix or coordinates"));
    if ((quint8(v.at(11)) & 3) == 3) {
        const int second = quint8(v.at(10));
        if (second <= 60) {
            result.utc = QDateTime(QDate(get<quint16>(v, 4), quint8(v.at(6)), quint8(v.at(7))),
                                  QTime(quint8(v.at(8)), quint8(v.at(9)), qMin(second, 59)), Qt::UTC);
            if (result.utc.isValid()) result.utc = result.utc.addMSecs(
                get<qint32>(v, 16) / 1000000 + (second == 60 ? 1000 : 0));
        }
    }
    *out = result; return true;
}

QVector<UbloxBaseStationProtocol::Command> UbloxBaseStationProtocol::autoConfigure(int initialBaud,
                                                                                bool modern)
{
    if (initialBaud <= 0) return {};
    QVector<Command> result;
    const QByteArray uart = frame(6, 0, QByteArray::fromHex("01000000d0080000000807002300230000000000"));
    const int bauds[] = {initialBaud, 9600, 38400, 57600, 115200, 230400, 460800};
    for (int baud : bauds)
        result.append(command(QByteArrayLiteral("UU") + uart, QStringLiteral("UART baud discovery"), 50, 100, baud));
    result.append(command(frame(6, 0, QByteArray::fromHex("0300000000000000000000002300230000000000")),
                          QStringLiteral("USB protocols"), 0, 300));
    result.append(command(frame(6, 8, QByteArray::fromHex("e80301000100")), QStringLiteral("1 Hz navigation"), 0, 200));
    result.append(command(frame(6, 0x24, QByteArray::fromHex(
        "ffff020300000000102700000f00fa00fa0064002c010000002310270000000000000000")),
                          QStringLiteral("Stationary navigation model"), 0, 200));
    const auto rate = [&result](quint8 cls, quint8 id, quint8 interval) {
        QByteArray payload(8, '\0'); payload[0] = char(cls); payload[1] = char(id);
        payload[3] = payload[5] = char(interval);
        result.append(command(frame(6, 1, payload), QStringLiteral("Message rate %1/%2 = %3")
            .arg(cls, 2, 16, QLatin1Char('0')).arg(id, 2, 16, QLatin1Char('0')).arg(interval), 0, 10));
    };
    for (int i = 0; i <= 15; ++i) if (i != 11 && i != 12 && i != 14) rate(0xf0, quint8(i), 0);
    result.append(command(frame(0x0a, 4), QStringLiteral("Poll receiver version"), 0, 10));
    rate(1, 0x3b, 1); rate(1, 7, 1); rate(0xf5, 5, 5);
    const quint8 msm4 = modern ? 1 : 0, msm7 = modern ? 0 : 1;
    rate(0xf5, 0x4a, msm4); rate(0xf5, 0x4d, msm7);
    rate(0xf5, 0x54, msm4); rate(0xf5, 0x57, msm7);
    rate(0xf5, 0x5e, msm4); rate(0xf5, 0x61, msm7);
    rate(0xf5, 0x7c, msm4); rate(0xf5, 0x7f, msm7);
    rate(0xf5, 0xfe, 0); rate(0xf5, 0xe6, 5); rate(1, 0x12, 1);
    rate(2, 0x15, 1); rate(2, 0x10, 1); rate(2, 0x13, 2); rate(2, 0x11, 2); rate(0x0a, 9, 2);
    result.last().delayAfterMs += 100;
    return result;
}

QVector<UbloxBaseStationProtocol::Command> UbloxBaseStationProtocol::disableBase()
{
    return {command(frame(6, 0x71, QByteArray(40, '\0')), QStringLiteral("Disable time mode"), 200),
            command(frame(6, 9, QByteArray::fromHex("00000000ffff00000000000001")), QStringLiteral("Save receiver BBR"), 0, 1000),
            command(frame(6, 4, QByteArray::fromHex("14ff0200")), QStringLiteral("Reset receiver"), 0, 3000)};
}

QByteArray UbloxBaseStationProtocol::surveyIn(quint32 duration, double metres, QString *error)
{
    if (error) error->clear();
    if (!duration) duration = 60;
    if (metres == 0) metres = 2;
    quint32 limit = 0;
    if (!accuracy(metres, 10000, &limit, error)) return {};
    QByteArray payload(40, '\0'); put<quint16>(payload, 2, 1);
    put<quint32>(payload, 24, duration); put<quint32>(payload, 28, limit);
    return frame(6, 0x71, payload);
}

QByteArray UbloxBaseStationProtocol::fixedLla(double lat, double lon, double alt, double metres,
                                           QString *error)
{
    if (error) error->clear();
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(alt)
        || std::abs(lat) > 90 || std::abs(lon) > 180
        || alt * 100 < std::numeric_limits<qint32>::min() || alt * 100 > std::numeric_limits<qint32>::max()) {
        fail(error, QStringLiteral("Fixed LLA is outside the receiver's coordinate range")); return {};
    }
    quint32 limit = 0;
    if (!accuracy(metres, 1000, &limit, error)) return {};
    QByteArray payload(40, '\0'); put<quint16>(payload, 2, 258);
    const double values[] = {lat * 1e7, lon * 1e7, alt * 100};
    for (int i = 0; i < 3; ++i) {
        const qint32 coarse = qint32(values[i]);
        put<qint32>(payload, 4 + i * 4, coarse);
        payload[16 + i] = char(qint8((values[i] - coarse) * 100));
    }
    put<quint32>(payload, 20, limit); put<quint32>(payload, 24, 60); put<quint32>(payload, 28, 2000);
    return frame(6, 0x71, payload);
}

QVector<UbloxBaseStationProtocol::Command> UbloxBaseStationProtocol::restartSurvey(
    int initialBaud, quint32 duration, double metres, bool modern, QString *error)
{
    const QByteArray survey = surveyIn(duration, metres, error);
    if (survey.isEmpty()) return {};
    if (initialBaud <= 0) { fail(error, QStringLiteral("Invalid receiver baud rate")); return {}; }
    QVector<Command> result = disableBase(); result += autoConfigure(initialBaud, modern);
    result.append(command(survey, QStringLiteral("Start survey-in"), 200));
    return result;
}
