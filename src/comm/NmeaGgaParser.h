#ifndef NMEAGGAPARSER_H
#define NMEAGGAPARSER_H

#include <QMetaType>
#include <QString>

struct NmeaGgaFix
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeM = 0.0;
    int satellites = 0;
    double hdop = 0.0;
    int fixQuality = 0;
    bool hasGeoidSeparation = false;
    double geoidSeparationM = 0.0;

    double geodeticAltitudeM() const noexcept
    {
        return hasGeoidSeparation
            ? altitudeM + geoidSeparationM : -1000.0;
    }
};

enum class NmeaGgaParseError
{
    None,
    EmptySentence,
    InvalidEncoding,
    MissingChecksum,
    NotGga,
    ChecksumMismatch,
    TooFewFields,
    NoPositionFix,
    InvalidCoordinate,
    InvalidAltitude,
    InvalidGeoidSeparation
};

struct NmeaGgaParseResult
{
    NmeaGgaFix fix;
    NmeaGgaParseError errorCode = NmeaGgaParseError::None;
    QString error;
    bool accepted = false;

    bool isValid() const noexcept { return accepted; }
};

/**
 * Parser for the GP/GN GGA subset consumed by Mission Planner 10 Follow Me.
 * Coordinates and numeric fields always use the C locale.  The GGA altitude
 * is retained for other consumers, although Follow Me commands its separately
 * configured relative altitude.
 */
class NmeaGgaParser final
{
public:
    static NmeaGgaParseResult parse(const QString &sentence);
    static QString checksum(const QString &sentence);

private:
    static bool parseCoordinate(const QString &value,
                                const QString &hemisphere,
                                bool latitude,
                                double *coordinate);
    static NmeaGgaParseResult failure(NmeaGgaParseError errorCode,
                                      const QString &error);
};

Q_DECLARE_METATYPE(NmeaGgaFix)
Q_DECLARE_METATYPE(NmeaGgaParseError)
Q_DECLARE_METATYPE(NmeaGgaParseResult)

#endif // NMEAGGAPARSER_H
