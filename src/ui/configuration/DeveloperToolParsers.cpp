// MAVLINK_USE_MESSAGE_INFO must precede the first MAVLink include so that
// mavlink_get_info.h (message names and per-field wire layout) is compiled
// into this translation unit. The helpers are `static inline`, so this does
// not conflict with other translation units.
#ifndef MAVLINK_USE_MESSAGE_INFO
#define MAVLINK_USE_MESSAGE_INFO
#endif
#include <cstddef>  // offsetof, used by the MAVLink field tables
#include <cstring>
#include <mavlink.h>

#include "DeveloperToolParsers.h"

#include <QStringList>
#include <QtEndian>
#include <QtNumeric>

namespace {

const char kSeparators[] = " \t\r\n,;:-";

bool isSeparator(QChar ch)
{
    const ushort u = ch.unicode();
    return u != 0 && u < 0x80 &&
           std::strchr(kSeparators, static_cast<char>(u)) != nullptr;
}

bool isHexDigit(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') ||
           (u >= 'A' && u <= 'F');
}

bool isDecimalDigit(QChar ch)
{
    const ushort u = ch.unicode();
    return u >= '0' && u <= '9';
}

bool allOf(const QString &text, bool (*predicate)(QChar))
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar ch : text) {
        if (!predicate(ch)) {
            return false;
        }
    }
    return true;
}

QString jsonString(const QString &value)
{
    QString out;
    out.reserve(value.size() + 2);
    out += QLatin1Char('"');
    for (const QChar ch : value) {
        switch (ch.unicode()) {
        case '"':
            out += QLatin1String("\\\"");
            break;
        case '\\':
            out += QLatin1String("\\\\");
            break;
        case '\n':
            out += QLatin1String("\\n");
            break;
        case '\r':
            out += QLatin1String("\\r");
            break;
        case '\t':
            out += QLatin1String("\\t");
            break;
        case '\b':
            out += QLatin1String("\\b");
            break;
        case '\f':
            out += QLatin1String("\\f");
            break;
        default:
            if (ch.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(
                    ch.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                out += ch;
            }
            break;
        }
    }
    out += QLatin1Char('"');
    return out;
}

// Shortest decimal representation that round-trips to the same value, the
// way .NET/Newtonsoft print `0.1f` as 0.1 and 100f as 100: the fewest
// significant digits win, fixed notation is kept for decimal exponents below
// 15, and the exponent marker is upper-case like .NET's "1E+15".
QString shortestRoundTrip(double value, int maxPrecision,
                          bool (*roundTrips)(const QString &, double))
{
    for (int precision = 1; precision <= maxPrecision; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general, value)) {
            continue;
        }
        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0) {
            return general;
        }
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < 15) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed, value)) {
                return fixed;
            }
        }
        return general.toUpper();
    }
    return QString::number(value, 'g', maxPrecision).toUpper();
}

bool floatRoundTrips(const QString &text, double value)
{
    return text.toFloat() == static_cast<float>(value);
}

bool doubleRoundTrips(const QString &text, double value)
{
    return text.toDouble() == value;
}

QString jsonFloat(float value)
{
    if (qIsNaN(value)) {
        return QStringLiteral("\"NaN\"");
    }
    if (qIsInf(value)) {
        return value > 0.0f ? QStringLiteral("\"Infinity\"")
                            : QStringLiteral("\"-Infinity\"");
    }
    return shortestRoundTrip(static_cast<double>(value), 9, floatRoundTrips);
}

QString jsonDouble(double value)
{
    if (qIsNaN(value)) {
        return QStringLiteral("\"NaN\"");
    }
    if (qIsInf(value)) {
        return value > 0.0 ? QStringLiteral("\"Infinity\"")
                           : QStringLiteral("\"-Infinity\"");
    }
    return shortestRoundTrip(value, 17, doubleRoundTrips);
}

unsigned wireElementSize(mavlink_message_type_t type)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR:
    case MAVLINK_TYPE_UINT8_T:
    case MAVLINK_TYPE_INT8_T:
        return 1;
    case MAVLINK_TYPE_UINT16_T:
    case MAVLINK_TYPE_INT16_T:
        return 2;
    case MAVLINK_TYPE_UINT32_T:
    case MAVLINK_TYPE_INT32_T:
    case MAVLINK_TYPE_FLOAT:
        return 4;
    case MAVLINK_TYPE_UINT64_T:
    case MAVLINK_TYPE_INT64_T:
    case MAVLINK_TYPE_DOUBLE:
        return 8;
    }
    return 0;
}

// MAVLink payloads are little-endian on the wire regardless of host order.
template <typename T>
T readWire(const char *payload, unsigned offset)
{
    return qFromLittleEndian<T>(payload + offset);
}

float readWireFloat(const char *payload, unsigned offset)
{
    const quint32 bits = qFromLittleEndian<quint32>(payload + offset);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double readWireDouble(const char *payload, unsigned offset)
{
    const quint64 bits = qFromLittleEndian<quint64>(payload + offset);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString jsonScalar(mavlink_message_type_t type, const char *payload,
                   unsigned offset)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR:
    case MAVLINK_TYPE_UINT8_T:
        return QString::number(readWire<quint8>(payload, offset));
    case MAVLINK_TYPE_INT8_T:
        return QString::number(readWire<qint8>(payload, offset));
    case MAVLINK_TYPE_UINT16_T:
        return QString::number(readWire<quint16>(payload, offset));
    case MAVLINK_TYPE_INT16_T:
        return QString::number(readWire<qint16>(payload, offset));
    case MAVLINK_TYPE_UINT32_T:
        return QString::number(readWire<quint32>(payload, offset));
    case MAVLINK_TYPE_INT32_T:
        return QString::number(readWire<qint32>(payload, offset));
    case MAVLINK_TYPE_UINT64_T:
        return QString::number(readWire<quint64>(payload, offset));
    case MAVLINK_TYPE_INT64_T:
        return QString::number(readWire<qint64>(payload, offset));
    case MAVLINK_TYPE_FLOAT:
        return jsonFloat(readWireFloat(payload, offset));
    case MAVLINK_TYPE_DOUBLE:
        return jsonDouble(readWireDouble(payload, offset));
    }
    return QStringLiteral("null");
}

// Renders one payload field using the dialect's wire layout. The parser
// zero-fills a trimmed MAVLink 2 payload up to the message's maximum length,
// so fields beyond the transmitted bytes read as zero (MP10 decodes a trimmed
// payload into a zero-initialised structure with the same effect).
QString jsonField(const mavlink_field_info_t &field, const char *payload)
{
    const unsigned elementSize = wireElementSize(field.type);
    const unsigned count = field.array_length > 0 ? field.array_length : 1;
    if (elementSize == 0 ||
        field.wire_offset + count * elementSize > MAVLINK_MAX_PAYLOAD_LEN) {
        return QStringLiteral("null");
    }

    if (field.type == MAVLINK_TYPE_CHAR && field.array_length > 0) {
        QByteArray raw(payload + field.wire_offset,
                       static_cast<int>(field.array_length));
        const int terminator = raw.indexOf('\0');
        if (terminator >= 0) {
            raw.truncate(terminator);
        }
        return jsonString(QString::fromUtf8(raw));
    }

    if (field.array_length == 0) {
        return jsonScalar(field.type, payload, field.wire_offset);
    }

    QStringList items;
    items.reserve(static_cast<int>(count));
    for (unsigned index = 0; index < count; ++index) {
        items << jsonScalar(field.type, payload,
                            field.wire_offset + index * elementSize);
    }
    return QLatin1Char('[') + items.join(QStringLiteral(", ")) +
           QLatin1Char(']');
}

QString indentLines(const QStringList &entries, int depth)
{
    const QString pad(depth * 2, QLatin1Char(' '));
    QString out;
    for (int i = 0; i < entries.size(); ++i) {
        out += pad + entries.at(i);
        if (i + 1 < entries.size()) {
            out += QLatin1Char(',');
        }
        out += QLatin1Char('\n');
    }
    return out;
}

QString jsonEntry(const char *key, const QString &value)
{
    return jsonString(QString::fromLatin1(key)) + QStringLiteral(": ") +
           value;
}

QString jsonBool(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

bool parserKnowsLength(int parseState)
{
    return parseState >= MAVLINK_PARSE_STATE_GOT_LENGTH;
}

bool parserKnowsIncompatFlags(int parseState)
{
    return parseState >= MAVLINK_PARSE_STATE_GOT_INCOMPAT_FLAGS;
}

bool parserKnowsMessageId(int parseState)
{
    return parseState == MAVLINK_PARSE_STATE_GOT_MSGID3 ||
           parseState == MAVLINK_PARSE_STATE_GOT_PAYLOAD ||
           parseState == MAVLINK_PARSE_STATE_GOT_CRC1 ||
           parseState == MAVLINK_PARSE_STATE_GOT_BAD_CRC1 ||
           parseState == MAVLINK_PARSE_STATE_SIGNATURE_WAIT;
}

QString messageLabel(quint32 msgid)
{
    const mavlink_message_info_t *info = mavlink_get_message_info_by_id(msgid);
    if (info) {
        return QStringLiteral("%1 (message %2)")
            .arg(QString::fromLatin1(info->name))
            .arg(msgid);
    }
    return QStringLiteral("message %1").arg(msgid);
}

QString deviceTypeName(quint8 devtype, const char *const *names, int count)
{
    if (devtype < count && names[devtype]) {
        return QString::fromLatin1(names[devtype]);
    }
    return QString::number(devtype);
}

// AP_Compass_Backend.h DevTypes, as mirrored by Mission Planner Device.cs.
const char *const kCompassTypes[] = {
    nullptr,        // 0x00
    "HMC5883_OLD",  // 0x01
    "LSM303D",      // 0x02
    nullptr,        // 0x03
    "AK8963",       // 0x04
    "BMM150",       // 0x05
    "LSM9DS1",      // 0x06
    "HMC5883",      // 0x07
    "LIS3MDL",      // 0x08
    "AK09916",      // 0x09
    "IST8310",      // 0x0A
    "ICM20948",     // 0x0B
    "MMC3416",      // 0x0C
    "QMC5883L",     // 0x0D
    "MAG3110",      // 0x0E
    "SITL",         // 0x0F
    "IST8308",      // 0x10
    "RM3100",       // 0x11
    "RM3100_2",     // 0x12 (unused, past mistake)
    "MMC5883",      // 0x13
    "AK09918",      // 0x14
    "AK09915",      // 0x15
    "QMC5883P",     // 0x16
    "BMM350",       // 0x17
    "IIS2MDC",      // 0x18
    "LIS2MDL",      // 0x19
};

// AP_InertialSensor_Backend.h DevTypes, as mirrored by Mission Planner Device.cs.
const char *const kImuTypes[] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x00-0x07
    nullptr,          // 0x08
    "BMI160",         // 0x09
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,                   // 0x0A-0x0F
    "L3G4200D",       // 0x10
    "ACC_LSM303D",    // 0x11
    "ACC_BMA180",     // 0x12
    "ACC_MPU6000",    // 0x13
    nullptr,          // 0x14
    nullptr,          // 0x15
    "ACC_MPU9250",    // 0x16
    "ACC_IIS328DQ",   // 0x17
    "ACC_LSM9DS1",    // 0x18
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,          // 0x19-0x1F
    nullptr,          // 0x20
    "GYR_MPU6000",    // 0x21
    "GYR_L3GD20",     // 0x22
    nullptr,          // 0x23
    "GYR_MPU9250",    // 0x24
    "GYR_I3G4250D",   // 0x25
    "GYR_LSM9DS1",    // 0x26
    "INS_ICM20789",   // 0x27
    "INS_ICM20689",   // 0x28
    "INS_BMI055",     // 0x29
    "SITL",           // 0x2A
    "INS_BMI088",     // 0x2B
    "INS_ICM20948",   // 0x2C
    "INS_ICM20648",   // 0x2D
    "INS_ICM20649",   // 0x2E
    "INS_ICM20602",   // 0x2F
    "INS_ICM20601",   // 0x30
    "INS_ADIS1647X",  // 0x31
    "SERIAL",         // 0x32
    "INS_ICM40609",   // 0x33
    "INS_ICM42688",   // 0x34
    "INS_ICM42605",   // 0x35
    "INS_ICM40605",   // 0x36
    "INS_IIM42652",   // 0x37
    "BMI270",         // 0x38
    "INS_BMI085",     // 0x39
    "INS_ICM42670",   // 0x3A
    "INS_ICM45686",   // 0x3B
    "INS_SCHA63T",    // 0x3C
    "INS_IIM42653",   // 0x3D
    "INS_LSM6DSV",    // 0x3E
    "INS_ASM330",     // 0x3F
};

// AP_Baro_Backend.h DevTypes, as mirrored by Mission Planner Device.cs.
const char *const kBaroTypes[] = {
    nullptr,             // 0x00
    "BARO_SITL",         // 0x01
    "BARO_BMP085",       // 0x02
    "BARO_BMP280",       // 0x03
    "BARO_BMP388",       // 0x04
    "BARO_DPS280",       // 0x05
    "BARO_DPS310",       // 0x06
    "BARO_FBM320",       // 0x07
    "BARO_ICM20789",     // 0x08
    "BARO_KELLERLD",     // 0x09
    "BARO_LPS2XH",       // 0x0A
    "BARO_MS5611",       // 0x0B
    "BARO_SPL06",        // 0x0C
    "BARO_DRONECAN",     // 0x0D
    "BARO_MSP",          // 0x0E
    "BARO_ICP101XX",     // 0x0F
    "BARO_ICP201XX",     // 0x10
    "BARO_MS5607",       // 0x11
    "BARO_MS5837_30BA",  // 0x12
    "BARO_MS5637",       // 0x13
    "BARO_BMP390",       // 0x14
    "BARO_BMP581",       // 0x15
    "BARO_SPA06",        // 0x16
    "BARO_AUAV",         // 0x17
    "BARO_MS5837_02BA",  // 0x18
};

// AP_Airspeed_Backend.h DevTypes, as mirrored by Mission Planner Device.cs.
const char *const kAirspeedTypes[] = {
    nullptr,              // 0x00
    "AIRSPEED_SITL",      // 0x01
    "AIRSPEED_MS4525",    // 0x02
    "AIRSPEED_MS5525",    // 0x03
    "AIRSPEED_DLVR",      // 0x04
    "AIRSPEED_MSP",       // 0x05
    "AIRSPEED_SDP3X",     // 0x06
    "AIRSPEED_DRONECAN",  // 0x07
    "AIRSPEED_ANALOG",    // 0x08
    "AIRSPEED_NMEA",      // 0x09
    "AIRSPEED_ASP5033",   // 0x0A
};

template <typename T, int N>
constexpr int arraySize(const T (&)[N])
{
    return N;
}

} // namespace

QString DecodedMavlinkPacket::text() const
{
    QString out = summary + QLatin1Char('\n') + json;
    if (trailingBytes > 0) {
        out += QLatin1Char('\n') +
               DeveloperToolParsers::tr(
                   "Note: %n trailing byte(s) after the packet were ignored.",
                   nullptr, trailingBytes);
    }
    if (signaturePresent && !signatureVerified) {
        out += QLatin1Char('\n') +
               DeveloperToolParsers::tr(
                   "Note: the packet carries a MAVLink 2 signature block; it "
                   "was NOT verified because no signing key is configured.");
    }
    return out;
}

DeveloperToolBytes DeveloperToolParsers::ParseBytes(const QString &input)
{
    DeveloperToolBytes result;
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        result.error = tr("Enter at least one byte.");
        return result;
    }

    bool hasSeparators = false;
    for (const QChar ch : trimmed) {
        if (isSeparator(ch)) {
            hasSeparators = true;
            break;
        }
    }

    if (!hasSeparators) {
        QString compact = trimmed;
        if (compact.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
            compact.remove(0, 2);
        }
        if (compact.isEmpty() || compact.size() % 2 != 0) {
            result.error = tr("Compact hexadecimal input must contain an even "
                              "number of hex digits.");
            return result;
        }
        for (int i = 0; i < compact.size(); ++i) {
            if (!isHexDigit(compact.at(i))) {
                result.error =
                    tr("Compact hexadecimal input contains '%1' at position "
                       "%2, which is not a hex digit. Separate decimal bytes "
                       "with spaces or commas.")
                        .arg(compact.at(i))
                        .arg(i + 1);
                return result;
            }
        }
        result.bytes = QByteArray::fromHex(compact.toLatin1());
        return result;
    }

    QStringList tokens;
    QString current;
    for (const QChar ch : trimmed) {
        if (isSeparator(ch)) {
            if (!current.isEmpty()) {
                tokens << current;
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    if (!current.isEmpty()) {
        tokens << current;
    }

    QByteArray bytes; // published only when every token is valid
    for (const QString &token : tokens) {
        const bool explicitHex =
            token.startsWith(QLatin1String("0x"), Qt::CaseInsensitive);
        QString digits = explicitHex ? token.mid(2) : token;
        bool hasLetter = false;
        for (const QChar ch : digits) {
            if (ch.isLetter()) {
                hasLetter = true;
                break;
            }
        }
        const bool hex = explicitHex || hasLetter;
        if (!hex && digits.startsWith(QLatin1Char('+'))) {
            digits.remove(0, 1); // NumberStyles.Integer accepts a leading sign
        }

        bool valid = hex ? allOf(digits, isHexDigit) : allOf(digits, isDecimalDigit);
        uint value = 0;
        if (valid) {
            value = digits.toUInt(&valid, hex ? 16 : 10);
        }
        if (!valid || value > 255) {
            result.error =
                tr("'%1' is not a byte (0..255 or 00..FF).").arg(token);
            return result;
        }
        bytes.append(static_cast<char>(static_cast<quint8>(value)));
    }

    if (bytes.isEmpty()) {
        result.error = tr("Enter at least one byte.");
        return result;
    }
    result.bytes = bytes;
    return result;
}

DecodedMavlinkPacket DeveloperToolParsers::DecodeMavlinkPacket(
    const QString &input)
{
    const DeveloperToolBytes bytes = ParseBytes(input);
    if (!bytes.ok()) {
        DecodedMavlinkPacket result;
        result.error = bytes.error;
        return result;
    }
    return DecodeMavlinkPacket(bytes.bytes);
}

DecodedMavlinkPacket DeveloperToolParsers::DecodeMavlinkPacket(
    const QByteArray &bytes)
{
    DecodedMavlinkPacket result;
    if (bytes.isEmpty()) {
        result.error = tr("Enter at least one byte.");
        return result;
    }

    // One-shot parse with private buffers and no channel globals. No signing
    // key is configured, so a signed packet is accepted on framing and CRC
    // alone; the signature block is reported but never verified by this
    // diagnostic tool.
    mavlink_message_t rxmsg;
    mavlink_status_t status;
    mavlink_message_t decoded;
    mavlink_status_t decodedStatus;
    std::memset(&rxmsg, 0, sizeof(rxmsg));
    std::memset(&status, 0, sizeof(status));
    std::memset(&decoded, 0, sizeof(decoded));
    std::memset(&decodedStatus, 0, sizeof(decodedStatus));

    int start = -1;
    int consumed = -1;
    unsigned framing = MAVLINK_FRAMING_INCOMPLETE;
    for (int i = 0; i < bytes.size(); ++i) {
        const quint8 byte = static_cast<quint8>(bytes.at(i));
        const bool wasIdle = status.parse_state == MAVLINK_PARSE_STATE_UNINIT ||
                             status.parse_state == MAVLINK_PARSE_STATE_IDLE;
        // The C parser silently drops a MAVLink 2 frame whose incompatibility
        // flags it does not understand; diagnose that explicitly instead of
        // reporting a truncated packet afterwards.
        if (status.parse_state == MAVLINK_PARSE_STATE_GOT_LENGTH &&
            rxmsg.magic == MAVLINK_STX && (byte & ~MAVLINK_IFLAG_MASK) != 0) {
            result.error =
                tr("MAVLink 2 packet declares incompatibility flags 0x%1; only "
                   "0x%2 (signed) is understood, so the packet cannot be "
                   "decoded.")
                    .arg(byte, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<int>(MAVLINK_IFLAG_MASK), 2, 16,
                         QLatin1Char('0'));
            return result;
        }
        framing = mavlink_frame_char_buffer(
            &rxmsg, &status, byte, &decoded, &decodedStatus);
        if (wasIdle && status.parse_state == MAVLINK_PARSE_STATE_GOT_STX) {
            start = i;
        }
        if (framing != MAVLINK_FRAMING_INCOMPLETE) {
            consumed = i + 1;
            break;
        }
    }

    if (consumed < 0) {
        if (start < 0) {
            result.error =
                tr("No MAVLink start byte (0xFD for MAVLink 2, 0xFE for "
                   "MAVLink 1) was found in the %n byte(s).",
                   nullptr, bytes.size());
            return result;
        }
        const int provided = bytes.size() - start;
        const bool mavlink2 = rxmsg.magic == MAVLINK_STX;
        if (!parserKnowsLength(status.parse_state)) {
            result.error =
                tr("Packet is incomplete: only the start byte was provided. "
                   "A MAVLink 2 packet needs at least 12 bytes and a MAVLink "
                   "1 packet at least 8.");
            return result;
        }
        int expected = (mavlink2 ? MAVLINK_CORE_HEADER_LEN
                                 : MAVLINK_CORE_HEADER_MAVLINK1_LEN) +
                       1 + rxmsg.len + MAVLINK_NUM_CHECKSUM_BYTES;
        const bool signedFlag = mavlink2 &&
                                parserKnowsIncompatFlags(status.parse_state) &&
                                (rxmsg.incompat_flags & MAVLINK_IFLAG_SIGNED);
        if (signedFlag) {
            expected += MAVLINK_SIGNATURE_BLOCK_LEN;
        }
        const QString what = parserKnowsMessageId(status.parse_state)
                                 ? messageLabel(rxmsg.msgid)
                                 : tr("an unidentified message");
        if (status.parse_state == MAVLINK_PARSE_STATE_SIGNATURE_WAIT) {
            result.error =
                tr("Packet is truncated: the signed MAVLink 2 %1 needs %2 "
                   "signature bytes but only %3 were provided; %4 more "
                   "byte(s) are required.")
                    .arg(what)
                    .arg(MAVLINK_SIGNATURE_BLOCK_LEN)
                    .arg(MAVLINK_SIGNATURE_BLOCK_LEN - status.signature_wait)
                    .arg(status.signature_wait);
            return result;
        }
        result.error =
            tr("Packet is truncated: MAVLink %1 %2 with payload length %3 "
               "needs %4 bytes%5 but only %6 were provided from the start "
               "byte; %7 more byte(s) are required.")
                .arg(mavlink2 ? 2 : 1)
                .arg(what)
                .arg(rxmsg.len)
                .arg(expected)
                .arg(signedFlag ? tr(" (including the signature)") : QString())
                .arg(provided)
                .arg(expected - provided);
        return result;
    }

    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(rxmsg.msgid);
    const mavlink_message_info_t *info =
        mavlink_get_message_info_by_id(rxmsg.msgid);
    if (!entry || !info) {
        result.error =
            tr("Message id %1 is not defined in the ardupilotmega MAVLink "
               "dialect, so its checksum and fields cannot be verified.")
                .arg(rxmsg.msgid);
        return result;
    }

    const quint16 carriedCrc =
        static_cast<quint16>(rxmsg.ck[0] | (rxmsg.ck[1] << 8));
    if (framing == MAVLINK_FRAMING_BAD_CRC) {
        result.error =
            tr("CRC mismatch: %1 with %2 payload byte(s) computes checksum "
               "0x%3 but the bytes carry 0x%4. Check that every byte was "
               "entered correctly and that the packet was produced for the "
               "ardupilotmega dialect.")
                .arg(messageLabel(rxmsg.msgid))
                .arg(rxmsg.len)
                .arg(rxmsg.checksum, 4, 16, QLatin1Char('0'))
                .arg(carriedCrc, 4, 16, QLatin1Char('0'));
        return result;
    }
    if (framing != MAVLINK_FRAMING_OK) {
        result.error = tr("%1 was rejected by the MAVLink parser (status %2).")
                           .arg(messageLabel(rxmsg.msgid))
                           .arg(framing);
        return result;
    }

    const bool mavlink2 = rxmsg.magic == MAVLINK_STX;
    result.name = QString::fromLatin1(info->name);
    result.msgid = rxmsg.msgid;
    result.sysid = rxmsg.sysid;
    result.compid = rxmsg.compid;
    result.seq = rxmsg.seq;
    result.payloadLength = rxmsg.len;
    result.incompatFlags = mavlink2 ? rxmsg.incompat_flags : 0;
    result.compatFlags = mavlink2 ? rxmsg.compat_flags : 0;
    result.crc16 = carriedCrc;
    result.mavlink2 = mavlink2;
    result.signaturePresent =
        mavlink2 && (rxmsg.incompat_flags & MAVLINK_IFLAG_SIGNED) != 0;
    result.signatureVerified = false; // no signing key: never verified
    result.packet = bytes.mid(start, consumed - start);
    result.leadingBytesSkipped = start;
    result.trailingBytes = bytes.size() - consumed;

    result.summary =
        QStringLiteral("%1 (message %2, system %3, component %4, %5 bytes)")
            .arg(result.name)
            .arg(result.msgid)
            .arg(result.sysid)
            .arg(result.compid)
            .arg(result.packet.size());

    // Stable diagnostic JSON. Property names are borrowed from MP10's
    // MAVLinkMessage so the output reads familiarly, but the content is
    // APM Planner specific: hex buffer string, JSON lists for arrays, JSON
    // strings for char arrays, explicit signature state.
    const char *payload = _MAV_PAYLOAD(&rxmsg);
    QStringList data;
    for (unsigned fieldIndex = 0; fieldIndex < info->num_fields; ++fieldIndex) {
        const mavlink_field_info_t &field = info->fields[fieldIndex];
        data << jsonEntry(field.name, jsonField(field, payload));
    }

    QString signature = QStringLiteral("null");
    quint8 signatureLinkId = 0;
    quint64 signatureTimestamp = 0;
    if (result.signaturePresent) {
        QStringList signatureBytes;
        for (int i = 0; i < MAVLINK_SIGNATURE_BLOCK_LEN; ++i) {
            signatureBytes << QString::number(rxmsg.signature[i]);
        }
        signature = QLatin1Char('[') + signatureBytes.join(QStringLiteral(", ")) +
                    QLatin1Char(']');
        signatureLinkId = rxmsg.signature[0];
        for (int i = 0; i < 6; ++i) {
            signatureTimestamp |= static_cast<quint64>(rxmsg.signature[1 + i])
                                  << (8 * i);
        }
    }

    QStringList entries;
    entries << jsonEntry("buffer",
                         jsonString(QString::fromLatin1(result.packet.toHex())));
    entries << jsonEntry("header", QString::number(rxmsg.magic));
    entries << jsonEntry("payloadlength", QString::number(rxmsg.len));
    entries << jsonEntry("incompat_flags", QString::number(result.incompatFlags));
    entries << jsonEntry("compat_flags", QString::number(result.compatFlags));
    entries << jsonEntry("seq", QString::number(rxmsg.seq));
    entries << jsonEntry("sysid", QString::number(rxmsg.sysid));
    entries << jsonEntry("compid", QString::number(rxmsg.compid));
    entries << jsonEntry("msgid", QString::number(rxmsg.msgid));
    entries << jsonEntry("ismavlink2", jsonBool(mavlink2));
    entries << jsonEntry("msgtypename", jsonString(result.name));
    entries << jsonEntry("data", QStringLiteral("{\n") + indentLines(data, 2) +
                                     QStringLiteral("  }"));
    entries << jsonEntry("crc16", QString::number(carriedCrc));
    entries << jsonEntry("sig", signature);
    entries << jsonEntry("sigLinkid", QString::number(signatureLinkId));
    entries << jsonEntry("sigTimestamp", QString::number(signatureTimestamp));
    entries << jsonEntry("signaturePresent", jsonBool(result.signaturePresent));
    entries << jsonEntry("signatureVerified", jsonBool(result.signatureVerified));
    entries << jsonEntry("Length", QString::number(result.packet.size()));

    result.json = QStringLiteral("{\n") + indentLines(entries, 1) +
                  QLatin1Char('}');
    return result;
}

DecodedHardwareId DeveloperToolParsers::DecodeHardwareId(
    const QString &input, const QString &parameterName)
{
    const QString trimmed = input.trimmed();
    quint32 value = 0;
    if (trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const QString digits = trimmed.mid(2);
        bool valid = allOf(digits, isHexDigit);
        if (valid) {
            value = digits.toUInt(&valid, 16);
        }
        if (!valid) {
            DecodedHardwareId result;
            result.error =
                tr("The hardware ID is not a valid hexadecimal UInt32 value.");
            return result;
        }
    } else {
        QString digits = trimmed;
        if (digits.startsWith(QLatin1Char('+'))) {
            digits.remove(0, 1); // NumberStyles.Integer accepts a leading sign
        }
        bool valid = allOf(digits, isDecimalDigit);
        if (valid) {
            value = digits.toUInt(&valid, 10);
        }
        if (!valid) {
            DecodedHardwareId result;
            result.error = tr("The hardware ID is not a valid UInt32 value.");
            return result;
        }
    }
    return DecodeHardwareId(value, parameterName);
}

DecodedHardwareId DeveloperToolParsers::DecodeHardwareId(
    quint32 devid, const QString &parameterName)
{
    DecodedHardwareId result;
    result.parameterName = parameterName.trimmed().toUpper();
    result.devid = devid;
    result.busType = static_cast<quint8>(devid & 0x7);
    result.bus = static_cast<quint8>((devid >> 3) & 0x1f);
    result.address = static_cast<quint8>((devid >> 8) & 0xff);
    result.devtype = static_cast<quint8>((devid >> 16) & 0xff);
    result.busTypeName = BusTypeName(result.busType);

    const QString &name = result.parameterName;
    if (name.contains(QLatin1String("COMPASS"))) {
        result.devtypeName = CompassDeviceTypeName(result.devtype);
    } else if (name.contains(QLatin1String("BARO"))) {
        result.devtypeName = BaroDeviceTypeName(result.devtype);
    } else if (name.contains(QLatin1String("ASP"))) {
        result.devtypeName = AirspeedDeviceTypeName(result.devtype);
    } else if (name.contains(QLatin1String("INS"))) {
        result.devtypeName = ImuDeviceTypeName(result.devtype);
    } else {
        // Deliberate deviation: MP10 lists compass/imu/baro/imu here (the
        // repeated IMU entry is an upstream slip); this port offers the
        // airspeed table as the fourth alternative.
        result.devtypeName = QStringLiteral("%1 or %2 or %3 or %4 ")
                                 .arg(CompassDeviceTypeName(result.devtype))
                                 .arg(ImuDeviceTypeName(result.devtype))
                                 .arg(BaroDeviceTypeName(result.devtype))
                                 .arg(AirspeedDeviceTypeName(result.devtype));
    }

    // Device.DeviceStructure.ToString() layout (trailing space included):
    // "{name} devid {devid} bus type {bus_type} bus {bus} address {address} devtype {devtype} "
    result.text = QStringLiteral("%1 devid %2 bus type %3 bus %4 address %5 devtype %6 ")
                      .arg(result.parameterName)
                      .arg(result.devid)
                      .arg(result.busTypeName)
                      .arg(result.bus)
                      .arg(result.address)
                      .arg(result.devtypeName);
    return result;
}

QString DeveloperToolParsers::BusTypeName(quint8 busType)
{
    switch (busType) {
    case 0:
        return QStringLiteral("UNKNOWN");
    case 1:
        return QStringLiteral("I2C");
    case 2:
        return QStringLiteral("SPI");
    case 3:
        return QStringLiteral("UAVCAN");
    case 4:
        return QStringLiteral("SITL");
    case 5:
        return QStringLiteral("MSP");
    case 6:
        return QStringLiteral("SERIAL");
    default:
        return QString::number(busType);
    }
}

QString DeveloperToolParsers::CompassDeviceTypeName(quint8 devtype)
{
    return deviceTypeName(devtype, kCompassTypes, arraySize(kCompassTypes));
}

QString DeveloperToolParsers::ImuDeviceTypeName(quint8 devtype)
{
    return deviceTypeName(devtype, kImuTypes, arraySize(kImuTypes));
}

QString DeveloperToolParsers::BaroDeviceTypeName(quint8 devtype)
{
    return deviceTypeName(devtype, kBaroTypes, arraySize(kBaroTypes));
}

QString DeveloperToolParsers::AirspeedDeviceTypeName(quint8 devtype)
{
    return deviceTypeName(devtype, kAirspeedTypes, arraySize(kAirspeedTypes));
}
