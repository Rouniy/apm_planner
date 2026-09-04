#include "NmeaGgaParser.h"

#include <QLocale>
#include <QStringList>

#include <cmath>

NmeaGgaParseResult NmeaGgaParser::parse(const QString &sentence)
{
    if (sentence.trimmed().isEmpty()) {
        return failure(NmeaGgaParseError::EmptySentence,
                       QStringLiteral("Invalid GGA sentence."));
    }

    const QString value = sentence.trimmed();
    for (const QChar character : value) {
        if (character.unicode() < 0x20U
            || character.unicode() > 0x7eU) {
            return failure(NmeaGgaParseError::InvalidEncoding,
                           QStringLiteral(
                               "NMEA sentence is not printable ASCII."));
        }
    }
    const int checksumSeparator = value.indexOf(QLatin1Char('*'));
    if (checksumSeparator <= 1
        || checksumSeparator + 2 >= value.size()) {
        return failure(NmeaGgaParseError::MissingChecksum,
                       QStringLiteral("NMEA checksum is missing."));
    }

    const QString kind = value.left(6);
    if (kind.compare(QStringLiteral("$GPGGA"),
                     Qt::CaseInsensitive) != 0
        && kind.compare(QStringLiteral("$GNGGA"),
                        Qt::CaseInsensitive) != 0) {
        return failure(NmeaGgaParseError::NotGga,
                       QStringLiteral("Not a GGA sentence."));
    }

    const QString expected = value.mid(checksumSeparator + 1, 2);
    if (expected.compare(checksum(value), Qt::CaseInsensitive) != 0) {
        return failure(NmeaGgaParseError::ChecksumMismatch,
                       QStringLiteral("NMEA checksum mismatch."));
    }

    const QStringList fields = value.left(checksumSeparator).split(
        QLatin1Char(','), Qt::KeepEmptyParts);
    if (fields.size() < 10) {
        return failure(NmeaGgaParseError::TooFewFields,
                       QStringLiteral("GGA sentence has too few fields."));
    }

    bool qualityOk = false;
    const int quality = fields.at(6).toInt(&qualityOk, 10);
    if (!qualityOk || quality <= 0) {
        return failure(NmeaGgaParseError::NoPositionFix,
                       QStringLiteral("GPS has no position fix."));
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (!parseCoordinate(fields.at(2), fields.at(3), true, &latitude)
        || !parseCoordinate(fields.at(4), fields.at(5), false,
                            &longitude)) {
        return failure(NmeaGgaParseError::InvalidCoordinate,
                       QStringLiteral(
                           "GGA latitude or longitude is invalid."));
    }

    bool altitudeOk = false;
    const double altitude = QLocale::c().toDouble(fields.at(9),
                                                   &altitudeOk);
    if (!altitudeOk || !std::isfinite(altitude)) {
        return failure(NmeaGgaParseError::InvalidAltitude,
                       QStringLiteral("GGA altitude is invalid."));
    }

    bool satellitesOk = false;
    int satellites = fields.at(7).toInt(&satellitesOk, 10);
    if (!satellitesOk) {
        satellites = 0;
    }

    bool hdopOk = false;
    double hdop = QLocale::c().toDouble(fields.at(8), &hdopOk);
    if (!hdopOk || !std::isfinite(hdop)) {
        hdop = 0.0;
    }

    bool hasGeoidSeparation = false;
    double geoidSeparation = 0.0;
    if (fields.size() > 11 && !fields.at(11).trimmed().isEmpty()) {
        bool geoidOk = false;
        geoidSeparation = QLocale::c().toDouble(fields.at(11), &geoidOk);
        if (!geoidOk || !std::isfinite(geoidSeparation)) {
            return failure(
                NmeaGgaParseError::InvalidGeoidSeparation,
                QStringLiteral("GGA geoid separation is invalid."));
        }
        hasGeoidSeparation = true;
    }

    NmeaGgaParseResult result;
    result.fix.latitude = latitude;
    result.fix.longitude = longitude;
    result.fix.altitudeM = altitude;
    result.fix.satellites = satellites;
    result.fix.hdop = hdop;
    result.fix.fixQuality = quality;
    result.fix.hasGeoidSeparation = hasGeoidSeparation;
    result.fix.geoidSeparationM = geoidSeparation;
    result.accepted = true;
    return result;
}

QString NmeaGgaParser::checksum(const QString &sentence)
{
    quint8 value = 0;
    for (const QChar character : sentence) {
        if (character == QLatin1Char('$')) {
            continue;
        }
        if (character == QLatin1Char('*')) {
            break;
        }
        value ^= static_cast<quint8>(character.unicode() & 0xffU);
    }
    return QStringLiteral("%1").arg(value, 2, 16,
                                    QLatin1Char('0')).toUpper();
}

bool NmeaGgaParser::parseCoordinate(const QString &value,
                                    const QString &hemisphere,
                                    bool latitude,
                                    double *coordinate)
{
    if (!coordinate) {
        return false;
    }

    bool valueOk = false;
    const double raw = QLocale::c().toDouble(value, &valueOk);
    if (!valueOk || !std::isfinite(raw) || raw < 0.0) {
        return false;
    }

    const double degrees = std::trunc(raw / 100.0);
    const double minutes = raw - degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }

    double parsed = degrees + minutes / 60.0;
    const QString negative = latitude
        ? QStringLiteral("S") : QStringLiteral("W");
    const QString positive = latitude
        ? QStringLiteral("N") : QStringLiteral("E");
    if (hemisphere.compare(negative, Qt::CaseInsensitive) == 0) {
        parsed *= -1.0;
    } else if (hemisphere.compare(positive, Qt::CaseInsensitive) != 0) {
        return false;
    }

    const double limit = latitude ? 90.0 : 180.0;
    if (parsed < -limit || parsed > limit) {
        return false;
    }
    *coordinate = parsed;
    return true;
}

NmeaGgaParseResult NmeaGgaParser::failure(
    NmeaGgaParseError errorCode, const QString &error)
{
    NmeaGgaParseResult result;
    result.errorCode = errorCode;
    result.error = error;
    return result;
}
