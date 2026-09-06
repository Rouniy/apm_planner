#include "MicrodroneDownlinkEncoder.h"

#include <QtGlobal>

#include <charconv>
#include <cmath>

namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double DegreesToRadians = Pi / 180.0;

bool roundTrips(const QString &text, double value)
{
    const QByteArray bytes = text.toLatin1();
    double parsed = 0.0;
    const auto converted = std::from_chars(bytes.constData(),
        bytes.constData() + bytes.size(), parsed, std::chars_format::general);
    return converted.ec == std::errc()
        && converted.ptr == bytes.constData() + bytes.size()
        && parsed == value;
}

// .NET 10's default Double formatting is the shortest representation that
// round-trips. It retains fixed notation below exponent 17 and uses an
// upper-case exponent marker. QString's fixed/general conversions provide the
// same decimal candidates, so select the first candidate that round-trips.
QString dotNetGeneral(double value)
{
    if (qIsNaN(value)) {
        return QStringLiteral("NaN");
    }
    if (qIsInf(value)) {
        return value < 0.0 ? QStringLiteral("-Infinity")
                           : QStringLiteral("Infinity");
    }
    if (value == 0.0) {
        return std::signbit(value) ? QStringLiteral("-0")
                                   : QStringLiteral("0");
    }

    for (int precision = 1; precision <= 17; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general, value)) {
            continue;
        }

        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0) {
            return general;
        }
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < 17) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed, value)) {
                return fixed;
            }
        }
        return general.toUpper();
    }

    return QString::number(value, 'g', 17).toUpper();
}

QByteArray number(double value)
{
    return dotNetGeneral(value).toLatin1();
}

QByteArray record(const QByteArray &payload)
{
    QByteArray result = payload;
    result += QByteArray::number(MicrodroneDownlinkEncoder::Checksum(payload));
    result += "\r\n";
    return result;
}

} // namespace

quint8 MicrodroneDownlinkEncoder::Checksum(const QByteArray &payload)
{
    quint8 answer = 0;
    for (const char byte : payload) {
        answer = static_cast<quint8>(answer + static_cast<quint8>(byte));
    }
    return static_cast<quint8>(answer ^ 0xffu);
}

QByteArray MicrodroneDownlinkEncoder::EncodeFrame(
    const MicrodroneTelemetry &telemetry, const QDateTime &utcNow,
    qint64 sampleCounter, QString *error)
{
    if (error) {
        error->clear();
    }
    if (sampleCounter < 0) {
        if (error) {
            *error = QStringLiteral("The MicroDrone sample counter cannot be negative.");
        }
        return {};
    }
    if (!utcNow.isValid()) {
        if (error) {
            *error = QStringLiteral("The MicroDrone timestamp is invalid.");
        }
        return {};
    }

    const QDateTime timestamp = utcNow.toUTC();
    const QDateTime gpsEpoch(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC);
    const qint64 elapsedMilliseconds = gpsEpoch.msecsTo(timestamp);
    if (elapsedMilliseconds < 0) {
        if (error) {
            *error = QStringLiteral("GPS time predates 1980-01-06 UTC.");
        }
        return {};
    }
    constexpr qint64 WeekMilliseconds = 7LL * 24LL * 60LL * 60LL * 1000LL;
    const qint64 week = elapsedMilliseconds / WeekMilliseconds;
    const qint64 seconds = (elapsedMilliseconds % WeekMilliseconds) / 1000LL;

    constexpr double Wgs84A = 6378137.0;
    constexpr double Wgs84F = 1.0 / 298.257223563;
    const double latitudeRadians = telemetry.latitude * DegreesToRadians;
    const double longitudeRadians = telemetry.longitude * DegreesToRadians;
    const double clat = std::cos(latitudeRadians);
    const double slat = std::sin(latitudeRadians);
    const double clon = std::cos(longitudeRadians);
    const double slon = std::sin(longitudeRadians);
    const double eccentricity = std::sqrt(2.0 * Wgs84F - std::pow(Wgs84F, 2.0));
    const double eccentricitySquared = eccentricity * eccentricity;
    const double legacyAltitude = telemetry.altitude * 0.0001;
    const double x = (Wgs84A + legacyAltitude) * clat * clon;
    const double y = (Wgs84A + legacyAltitude) * clat * slon;
    const double z = ((1.0 - eccentricitySquared) * Wgs84A + legacyAltitude) * slat;
    const double courseRadians = telemetry.groundCourse * DegreesToRadians;

    QByteArray frame;
    frame.reserve(512);
    frame += record(QByteArrayLiteral("#1,28,07,2,1,1,1,2,16000,0,2,"));
    frame += record(QByteArrayLiteral("#4,")
        + QByteArray::number(sampleCounter / 10) + ','
        + QByteArray::number(seconds) + ',' + QByteArray::number(week)
        + QByteArrayLiteral(",25,"));
    frame += record(QByteArrayLiteral("#5,") + number(x * 100.0) + ','
        + number(y * 100.0) + ',' + number(z * 100.0) + ','
        + number(telemetry.gpsHdop + 0.01) + ','
        + number(telemetry.satelliteCount) + ',');
    frame += record(QByteArrayLiteral("#6,")
        + number(telemetry.groundSpeed * std::sin(courseRadians)) + ','
        + number(telemetry.groundSpeed * std::cos(courseRadians)) + ','
        + number(telemetry.verticalSpeed) + QByteArrayLiteral(",2,"));
    frame += record(QByteArrayLiteral("#7,")
        + number(telemetry.roll * DegreesToRadians) + ','
        + number(telemetry.pitch * DegreesToRadians) + ','
        + number(telemetry.yaw * DegreesToRadians) + ',');
    frame += record(QByteArrayLiteral("#8,") + number(telemetry.altitude) + ','
        + number(telemetry.altitude) + ','
        + number(telemetry.pressureTemperature) + ',');
    frame += record(QByteArrayLiteral("#9,") + number(telemetry.magnetometerX) + ','
        + number(telemetry.magnetometerY) + ','
        + number(telemetry.magnetometerZ) + ',');
    return frame;
}
