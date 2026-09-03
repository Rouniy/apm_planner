#ifndef NMEASENTENCEBUILDER_H
#define NMEASENTENCEBUILDER_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <mavlink.h>

/*
 * Mission Planner 10 TOOLS > NMEA Output formatter (ViewModels/SerialOutputNMEAViewModel.cs
 * MainLoop/Send/GetChecksum) and the slice of ExtLibs/ArduPilot/CurrentState.cs it reads.
 *
 * Pure code: no QObject, no I/O, no timers, no transport, no UI. Time is always passed in
 * (deterministic tests); the caller supplies the current UTC instant.
 *
 * Deliberate deviation (root decision, c37/c38): RMC field 9 uses the UTC date of the
 * supplied instant. MP10 formats DateTime.Now (the local date) while every time field is
 * UTC, which is a bug; here date and time come from the same UTC instant.
 */
struct NmeaVehicleState
{
    // CurrentState.useLocation: the last GLOBAL_POSITION_INT carried a usable position.
    // While true, GPS_RAW_INT no longer updates latitude/longitude/altitude.
    bool globalPositionSeen = false;
    bool hasPosition = false;     // latitude/longitude were assigned at least once
    double latitude = 0.0;        // degrees (cs.lat)
    double longitude = 0.0;       // degrees (cs.lng)
    double altitudeAmsl = 0.0;    // metres AMSL (cs.altasl with multiplieralt == 1)
    int fixType = 0;              // GPS_RAW_INT.fix_type (cs.gpsstatus)
    int satellites = 0;           // cs.satcount
    double hdop = 0.0;            // cs.gpshdop = round(eph / 100, 2)
    double groundSpeedMs = 0.0;   // cs.groundspeed, m/s
    double groundCourseDeg = 0.0; // cs.groundcourse, 0..360
    double yawDeg = 0.0;          // cs.yaw, 0..360
};

class NmeaSentenceBuilder
{
public:
    // Speed factors used by MP10 (knots and km/h from m/s).
    static constexpr double KnotsPerMetrePerSecond = 1.943844;
    static constexpr double KilometresPerHourPerMetrePerSecond = 3.6;

    // Applies one MAVLink message with CurrentState semantics. Returns true when the message
    // is one of GLOBAL_POSITION_INT, GPS_RAW_INT, VFR_HUD or ATTITUDE (state may still be
    // unchanged, e.g. when every field carried a sentinel); every other message is ignored.
    static bool Apply(NmeaVehicleState &state, const mavlink_message_t &message);

    // MP10 GetChecksum: XOR of every character after '$' up to (not including) '*', "X2".
    static QString Checksum(const QString &body);

    // Sentence bodies without checksum, exactly as MP10 formats them.
    static QString Gga(const NmeaVehicleState &state, const QDateTime &utcNow);
    static QString Gll(const NmeaVehicleState &state, const QDateTime &utcNow);
    static QString Hdg(const NmeaVehicleState &state);
    static QString Vtg(const NmeaVehicleState &state);
    static QString Rmc(const NmeaVehicleState &state, const QDateTime &utcNow);
    // MP10 order: GGA, GLL, HDG, VTG, RMC.
    static QStringList Tick(const NmeaVehicleState &state, const QDateTime &utcNow);

    // body + "*" + checksum + "\r\n" (MP10 appends "\r" and WriteLine adds "\n"), ASCII.
    static QByteArray Line(const QString &body);
    static QByteArray TickBytes(const NmeaVehicleState &state, const QDateTime &utcNow);

    // --- building blocks (public for tests) ---
    // MP10: (int)deg + (deg - (int)deg) * 0.6, i.e. degrees with the fraction expressed as
    // minutes / 100, then |value| * 100 -> ddmm.mmmm as a plain number.
    static double DegreesMinutes(double degrees);
    // C# custom numeric format "0...0.0...0": at least integerDigits before the point
    // (zero padded), exactly decimals after it. decimals == 0 -> no point.
    static QString FormatFixed(double value, int integerDigits, int decimals);
    // C# float.ToString(): shortest round-trip of a single-precision value.
    static QString FormatFloat(double value);
    static QString FormatTime(const QDateTime &utcNow);   // HHmmss.fff
    static QString FormatDate(const QDateTime &utcNow);   // ddMMyy (UTC, see header note)
    static double NormaliseDegrees(double degrees);      // CurrentState setter: value < 0 -> +360
};

#endif // NMEASENTENCEBUILDER_H
