#ifndef DEVELOPERTOOLPARSERS_H
#define DEVELOPERTOOLPARSERS_H

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QtGlobal>

/*
 * Port of Mission Planner 10 `Services/DeveloperToolParsers.cs` used by the
 * Developer Tools page ("Decode MAVLink Packet" and "Decode Hardware ID").
 *
 * Mission Planner throws FormatException/InvalidDataException; this port
 * returns result structs whose `error` is empty on success so the caller can
 * log `"MAVLink decode failed: " + error` like the MP10 view model.
 *
 * The packet JSON is a stable APM Planner diagnostic format. Its property
 * names are borrowed from MP10's MAVLinkMessage for familiarity, but the
 * content intentionally differs from Newtonsoft output: the raw buffer is a
 * hex string, arrays are JSON lists, char arrays are JSON strings, and
 * signature state is reported explicitly.
 */

struct DeveloperToolBytes
{
    QByteArray bytes;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

struct DecodedMavlinkPacket
{
    QString error;

    bool ok() const { return error.isEmpty(); }

    QString name;                  // MAVLink message name, e.g. "HEARTBEAT"
    quint32 msgid = 0;
    quint8 sysid = 0;
    quint8 compid = 0;
    quint8 seq = 0;
    quint8 payloadLength = 0;      // wire payload length (MAVLink 2 trims trailing zeros)
    quint8 incompatFlags = 0;      // 0 for MAVLink 1
    quint8 compatFlags = 0;        // 0 for MAVLink 1
    quint16 crc16 = 0;             // checksum carried by the packet
    bool mavlink2 = false;
    bool signaturePresent = false;  // MAVLink 2 packet carries a signature block
    bool signatureVerified = false; // always false: no signing key is configured,
                                    // the signature is carried through unchecked
    QByteArray packet;             // exact wire bytes of the decoded packet
    int leadingBytesSkipped = 0;   // bytes before the start marker
    int trailingBytes = 0;         // bytes after the packet that were ignored
    QString summary;               // "HEARTBEAT (message 0, system 1, component 1, 21 bytes)"
    QString json;                  // indented diagnostic JSON (see file comment)

    // Log text: summary, newline, JSON, then notes about ignored trailing
    // bytes and an unverified signature when applicable.
    QString text() const;
};

struct DecodedHardwareId
{
    QString error;

    bool ok() const { return error.isEmpty(); }

    QString parameterName;         // trimmed and upper-cased caller name
    quint32 devid = 0;
    quint8 busType = 0;            // devid & 0x7
    quint8 bus = 0;                // (devid >> 3) & 0x1f
    quint8 address = 0;            // (devid >> 8) & 0xff
    quint8 devtype = 0;            // (devid >> 16) & 0xff
    QString busTypeName;           // "I2C", "SPI", ... or the raw number when unknown
    QString devtypeName;           // resolved from the parameter family, or the "A or B or C or D" form
    QString text;                  // MP10 Device.DeviceStructure.ToString() layout; when no
                                   // family matches, the fourth alternative is the airspeed
                                   // table (MP10 repeats the IMU table there)
};

class DeveloperToolParsers final
{
    Q_DECLARE_TR_FUNCTIONS(DeveloperToolParsers)

public:
    // Accepts compact hex ("0xfd0500a1" / "fd0500a1") or bytes separated by
    // space, tab, newline, ',', ';', ':' or '-'. MP10 token rule, kept as is:
    // a token is hexadecimal when it has a "0x" prefix OR contains any letter,
    // otherwise it is decimal. So "10" is ten, "0x10" and "1a" are sixteen and
    // twenty-six, and one list may mix both ("253 fd 0x05 10").
    static DeveloperToolBytes ParseBytes(const QString &input);

    // Parses the bytes and decodes exactly one MAVLink 1/2 packet from them
    // with the ardupilotmega dialect (leading garbage is skipped, trailing
    // bytes are reported, CRC and dialect membership are verified, unknown
    // incompatibility flags are rejected, signatures are reported but never
    // verified).
    static DecodedMavlinkPacket DecodeMavlinkPacket(const QString &input);
    static DecodedMavlinkPacket DecodeMavlinkPacket(const QByteArray &bytes);

    // Decodes an ArduPilot *_DEV_ID value given as decimal or "0x" hex.
    static DecodedHardwareId DecodeHardwareId(
        const QString &input, const QString &parameterName = QString());
    static DecodedHardwareId DecodeHardwareId(
        quint32 devid, const QString &parameterName = QString());

    // ArduPilot device-id tables (Device.cs); unknown values return the number.
    static QString BusTypeName(quint8 busType);
    static QString CompassDeviceTypeName(quint8 devtype);
    static QString ImuDeviceTypeName(quint8 devtype);
    static QString BaroDeviceTypeName(quint8 devtype);
    static QString AirspeedDeviceTypeName(quint8 devtype);
};

#endif
