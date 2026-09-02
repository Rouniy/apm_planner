#include "WpRow.h"

#include "MissionCommandCatalog.h"
#include "comm/MissionItemProtocol.h"
#include "QGCMAVLink.h"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double kSemiMajorAxis = 6378137.0;
constexpr double kEccentricitySquared = 0.00669438;
constexpr double kUtmScale = 0.9996;
constexpr double kPi = 3.14159265358979323846;

double radians(double degrees)
{
    return degrees * kPi / 180.0;
}

double degrees(double radiansValue)
{
    return radiansValue * 180.0 / kPi;
}

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

int utmZoneFor(double latitude, double longitude)
{
    int zone = static_cast<int>(std::floor((longitude + 180.0) / 6.0)) + 1;
    zone = std::clamp(zone, 1, 60);
    if (latitude >= 56.0 && latitude < 64.0
        && longitude >= 3.0 && longitude < 12.0) {
        zone = 32;
    }
    if (latitude >= 72.0 && latitude < 84.0
        && longitude >= 0.0 && longitude < 42.0) {
        if (longitude < 9.0) zone = 31;
        else if (longitude < 21.0) zone = 33;
        else if (longitude < 33.0) zone = 35;
        else if (longitude < 42.0) zone = 37;
    }
    return zone;
}

bool toUtm(double latitude, double longitude, int *signedZone,
           double *easting, double *northing)
{
    if (!validCoordinate(latitude, longitude)
        || latitude < -80.0 || latitude > 84.0) {
        return false;
    }

    const int zone = utmZoneFor(latitude, longitude);
    const double longitudeOrigin = (zone - 1) * 6 - 180 + 3;
    const double latitudeRadians = radians(latitude);
    const double longitudeRadians = radians(longitude);
    const double longitudeOriginRadians = radians(longitudeOrigin);
    const double eccentricityPrimeSquared = kEccentricitySquared
        / (1.0 - kEccentricitySquared);
    const double n = kSemiMajorAxis
        / std::sqrt(1.0 - kEccentricitySquared
                    * std::sin(latitudeRadians) * std::sin(latitudeRadians));
    const double t = std::tan(latitudeRadians) * std::tan(latitudeRadians);
    const double c = eccentricityPrimeSquared
        * std::cos(latitudeRadians) * std::cos(latitudeRadians);
    const double a = std::cos(latitudeRadians)
        * (longitudeRadians - longitudeOriginRadians);
    const double m = kSemiMajorAxis
        * ((1.0 - kEccentricitySquared / 4.0
            - 3.0 * std::pow(kEccentricitySquared, 2) / 64.0
            - 5.0 * std::pow(kEccentricitySquared, 3) / 256.0)
               * latitudeRadians
           - (3.0 * kEccentricitySquared / 8.0
              + 3.0 * std::pow(kEccentricitySquared, 2) / 32.0
              + 45.0 * std::pow(kEccentricitySquared, 3) / 1024.0)
               * std::sin(2.0 * latitudeRadians)
           + (15.0 * std::pow(kEccentricitySquared, 2) / 256.0
              + 45.0 * std::pow(kEccentricitySquared, 3) / 1024.0)
               * std::sin(4.0 * latitudeRadians)
           - (35.0 * std::pow(kEccentricitySquared, 3) / 3072.0)
               * std::sin(6.0 * latitudeRadians));

    *easting = kUtmScale * n
            * (a + (1.0 - t + c) * std::pow(a, 3) / 6.0
               + (5.0 - 18.0 * t + t * t + 72.0 * c
                  - 58.0 * eccentricityPrimeSquared)
                   * std::pow(a, 5) / 120.0)
        + 500000.0;
    *northing = kUtmScale
        * (m + n * std::tan(latitudeRadians)
                    * (a * a / 2.0
                       + (5.0 - t + 9.0 * c + 4.0 * c * c)
                           * std::pow(a, 4) / 24.0
                       + (61.0 - 58.0 * t + t * t + 600.0 * c
                          - 330.0 * eccentricityPrimeSquared)
                           * std::pow(a, 6) / 720.0));
    if (latitude < 0.0) {
        *northing += 10000000.0;
    }
    *signedZone = latitude < 0.0 ? -zone : zone;
    return true;
}

bool fromUtm(double easting, double northing, int signedZone,
             double *latitude, double *longitude)
{
    const int zone = std::abs(signedZone);
    if (zone < 1 || zone > 60 || !std::isfinite(easting)
        || !std::isfinite(northing) || easting < 100000.0
        || easting > 900000.0 || northing < 0.0
        || northing > 10000000.0) {
        return false;
    }

    const bool northern = signedZone > 0;
    const double x = easting - 500000.0;
    double y = northing;
    if (!northern) {
        y -= 10000000.0;
    }
    const double longitudeOrigin = (zone - 1) * 6 - 180 + 3;
    const double eccentricityPrimeSquared = kEccentricitySquared
        / (1.0 - kEccentricitySquared);
    const double m = y / kUtmScale;
    const double mu = m / (kSemiMajorAxis
        * (1.0 - kEccentricitySquared / 4.0
           - 3.0 * std::pow(kEccentricitySquared, 2) / 64.0
           - 5.0 * std::pow(kEccentricitySquared, 3) / 256.0));
    const double e1 = (1.0 - std::sqrt(1.0 - kEccentricitySquared))
        / (1.0 + std::sqrt(1.0 - kEccentricitySquared));
    const double phi1 = mu
        + (3.0 * e1 / 2.0 - 27.0 * std::pow(e1, 3) / 32.0)
            * std::sin(2.0 * mu)
        + (21.0 * e1 * e1 / 16.0 - 55.0 * std::pow(e1, 4) / 32.0)
            * std::sin(4.0 * mu)
        + (151.0 * std::pow(e1, 3) / 96.0) * std::sin(6.0 * mu)
        + (1097.0 * std::pow(e1, 4) / 512.0) * std::sin(8.0 * mu);
    const double n1 = kSemiMajorAxis
        / std::sqrt(1.0 - kEccentricitySquared
                    * std::sin(phi1) * std::sin(phi1));
    const double t1 = std::tan(phi1) * std::tan(phi1);
    const double c1 = eccentricityPrimeSquared
        * std::cos(phi1) * std::cos(phi1);
    const double r1 = kSemiMajorAxis * (1.0 - kEccentricitySquared)
        / std::pow(1.0 - kEccentricitySquared
                   * std::sin(phi1) * std::sin(phi1), 1.5);
    const double d = x / (n1 * kUtmScale);

    *latitude = degrees(phi1 - (n1 * std::tan(phi1) / r1)
        * (d * d / 2.0
           - (5.0 + 3.0 * t1 + 10.0 * c1 - 4.0 * c1 * c1
              - 9.0 * eccentricityPrimeSquared)
               * std::pow(d, 4) / 24.0
           + (61.0 + 90.0 * t1 + 298.0 * c1 + 45.0 * t1 * t1
              - 252.0 * eccentricityPrimeSquared - 3.0 * c1 * c1)
               * std::pow(d, 6) / 720.0));
    *longitude = longitudeOrigin + degrees(
        (d - (1.0 + 2.0 * t1 + c1) * std::pow(d, 3) / 6.0
         + (5.0 - 2.0 * c1 + 28.0 * t1 - 3.0 * c1 * c1
            + 8.0 * eccentricityPrimeSquared + 24.0 * t1 * t1)
             * std::pow(d, 5) / 120.0)
        / std::cos(phi1));
    return validCoordinate(*latitude, *longitude);
}

QChar latitudeBand(double latitude)
{
    static const QString bands = QStringLiteral("CDEFGHJKLMNPQRSTUVWX");
    if (latitude < -80.0 || latitude > 84.0) {
        return QChar();
    }
    const int index = std::min(19,
        static_cast<int>(std::floor((latitude + 80.0) / 8.0)));
    return bands.at(index);
}

QString mgrsFor(double latitude, double longitude)
{
    int signedZone = 0;
    double easting = 0.0;
    double northing = 0.0;
    if (!toUtm(latitude, longitude, &signedZone, &easting, &northing)) {
        return QString();
    }
    const int zone = std::abs(signedZone);
    const QChar band = latitudeBand(latitude);
    if (band.isNull()) {
        return QString();
    }
    static const std::array<QString, 3> columnSets{{
        QStringLiteral("ABCDEFGH"),
        QStringLiteral("JKLMNPQR"),
        QStringLiteral("STUVWXYZ"),
    }};
    static const QString rowLetters = QStringLiteral("ABCDEFGHJKLMNPQRSTUV");
    const int column = static_cast<int>(std::floor(easting / 100000.0));
    if (column < 1 || column > 8) {
        return QString();
    }
    const QChar columnLetter = columnSets.at((zone - 1) % 3).at(column - 1);
    const int row = static_cast<int>(std::floor(northing / 100000.0)) % 20;
    const int rowOffset = zone % 2 == 0 ? 5 : 0;
    const QChar rowLetter = rowLetters.at((row + rowOffset) % 20);
    const int eastingRemainder = static_cast<int>(std::floor(
        std::fmod(easting, 100000.0)));
    const int northingRemainder = static_cast<int>(std::floor(
        std::fmod(northing, 100000.0)));
    return QStringLiteral("%1%2%3%4 %5 %6")
        .arg(zone)
        .arg(band)
        .arg(columnLetter)
        .arg(rowLetter)
        .arg(eastingRemainder, 5, 10, QLatin1Char('0'))
        .arg(northingRemainder, 5, 10, QLatin1Char('0'));
}

double minimumNorthingForBand(QChar band)
{
    static const QString bands = QStringLiteral("CDEFGHJKLMNPQRSTUVWX");
    static const std::array<double, 20> minimums{{
        1100000, 2000000, 2800000, 3700000, 4600000,
        5500000, 6400000, 7300000, 8200000, 9100000,
        0, 800000, 1700000, 2600000, 3500000,
        4400000, 5300000, 6200000, 7000000, 7900000,
    }};
    const int index = bands.indexOf(band);
    return index >= 0 ? minimums.at(index) : -1.0;
}

bool fromMgrs(const QString &text, double *latitude, double *longitude)
{
    QString compact = text.toUpper();
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    static const QRegularExpression expression(QStringLiteral(
        "^(\\d{1,2})([C-HJ-NP-X])([A-HJ-NP-Z])([A-HJ-NP-V])(\\d{2,10})$"));
    const QRegularExpressionMatch match = expression.match(compact);
    if (!match.hasMatch()) {
        return false;
    }
    const int zone = match.captured(1).toInt();
    const QChar band = match.captured(2).at(0);
    const QChar columnLetter = match.captured(3).at(0);
    const QChar rowLetter = match.captured(4).at(0);
    const QString digits = match.captured(5);
    if (zone < 1 || zone > 60 || digits.size() % 2 != 0) {
        return false;
    }
    static const std::array<QString, 3> columnSets{{
        QStringLiteral("ABCDEFGH"),
        QStringLiteral("JKLMNPQR"),
        QStringLiteral("STUVWXYZ"),
    }};
    static const QString rowLetters = QStringLiteral("ABCDEFGHJKLMNPQRSTUV");
    const int columnIndex = columnSets.at((zone - 1) % 3).indexOf(columnLetter);
    const int rowLetterIndex = rowLetters.indexOf(rowLetter);
    if (columnIndex < 0 || rowLetterIndex < 0) {
        return false;
    }
    const int precision = digits.size() / 2;
    const double scale = std::pow(10.0, 5 - precision);
    const double easting = (columnIndex + 1) * 100000.0
        + digits.left(precision).toInt() * scale;
    const int rowOffset = zone % 2 == 0 ? 5 : 0;
    int row = rowLetterIndex - rowOffset;
    if (row < 0) row += 20;
    double northing = row * 100000.0
        + digits.mid(precision).toInt() * scale;
    const double minimum = minimumNorthingForBand(band);
    if (minimum < 0.0) {
        return false;
    }
    while (northing < minimum) {
        northing += 2000000.0;
    }
    const bool northern = band >= QLatin1Char('N');
    double candidateLatitude = 0.0;
    double candidateLongitude = 0.0;
    if (!fromUtm(easting, northing, northern ? zone : -zone,
                 &candidateLatitude, &candidateLongitude)
        || latitudeBand(candidateLatitude) != band
        || utmZoneFor(candidateLatitude, candidateLongitude) != zone) {
        return false;
    }
    *latitude = candidateLatitude;
    *longitude = candidateLongitude;
    return true;
}

bool parseZone(const QString &text, int *signedZone)
{
    QString value = text.trimmed().toUpper();
    bool south = false;
    if (value.endsWith(QLatin1Char('S'))) {
        south = true;
        value.chop(1);
    } else if (value.endsWith(QLatin1Char('N'))) {
        value.chop(1);
    }
    bool ok = false;
    int zone = value.trimmed().toInt(&ok);
    if (!ok || zone == 0 || std::abs(zone) > 60) {
        return false;
    }
    if (zone < 0) {
        south = true;
        zone = -zone;
    }
    *signedZone = south ? -zone : zone;
    return true;
}
}

WpRow::WpRow(QObject *parent)
    : QObject(parent)
{
    connect(MissionCommandCatalog::instance(),
            &MissionCommandCatalog::catalogChanged, this, [this]() {
        emit commandNameChanged(CommandName());
    });
}

WpRow::WpRow(const WpRowData &data, QObject *parent)
    : QObject(parent)
{
    applyData(data);
    connect(MissionCommandCatalog::instance(),
            &MissionCommandCatalog::catalogChanged, this, [this]() {
        emit commandNameChanged(CommandName());
    });
}

int WpRow::Seq() const { return m_data.Seq; }
int WpRow::DisplayNumber() const { return m_data.Seq + 1; }
quint16 WpRow::Command() const { return m_data.Command; }
QString WpRow::CommandName() const { return CommandNameFor(m_data.Command); }
double WpRow::P1() const { return m_data.P1; }
double WpRow::P2() const { return m_data.P2; }
double WpRow::P3() const { return m_data.P3; }
double WpRow::P4() const { return m_data.P4; }
double WpRow::Lat() const { return m_data.Lat; }
double WpRow::Lng() const { return m_data.Lng; }
double WpRow::Alt() const { return m_data.Alt; }
double WpRow::AltDisplay() const { return m_data.Alt; }
QString WpRow::Grad() const { return m_data.Grad; }
QString WpRow::Angle() const { return m_data.Angle; }
QString WpRow::Dist() const { return m_data.Dist; }
QString WpRow::Az() const { return m_data.Az; }
quint8 WpRow::Frame() const { return m_data.Frame; }
QVariant WpRow::Tag() const { return m_data.Tag; }
QString WpRow::Zone() const { return m_data.Zone; }
QString WpRow::Easting() const { return m_data.Easting; }
QString WpRow::Northing() const { return m_data.Northing; }
QString WpRow::Mgrs() const { return m_data.Mgrs; }

QString WpRow::FrameName() const
{
    switch (m_data.Frame) {
    case MAV_FRAME_GLOBAL:
    case MAV_FRAME_GLOBAL_INT:
        return QStringLiteral("Absolute");
    case MAV_FRAME_GLOBAL_TERRAIN_ALT:
    case MAV_FRAME_GLOBAL_TERRAIN_ALT_INT:
        return QStringLiteral("Terrain");
    case MAV_FRAME_LOCAL_NED:
        return QStringLiteral("Local NED");
    case MAV_FRAME_LOCAL_ENU:
        return QStringLiteral("Local ENU");
    default:
        return QStringLiteral("Relative");
    }
}

WpRowData WpRow::toData() const
{
    return m_data;
}

QStringList WpRow::CommandList()
{
    return MissionCommandCatalog::instance()->Names();
}

QString WpRow::CommandNameFor(quint16 command)
{
    const QString name = MissionCommandCatalog::instance()->GetName(command);
    return name.isEmpty() ? QString::number(command) : name;
}

bool WpRow::commandForName(const QString &name, quint16 *command)
{
    if (!command) {
        return false;
    }
    const QString value = name.trimmed();
    if (MissionCommandCatalog::instance()->TryGetId(value, command)) {
        return true;
    }
    bool ok = false;
    const uint numeric = value.toUInt(&ok);
    if (!ok || numeric > 65535) {
        return false;
    }
    *command = static_cast<quint16>(numeric);
    return true;
}

bool WpRow::CommandHasLocation(quint16 command)
{
    return MissionItemProtocol::commandHasLocation(command);
}

bool WpRow::CommandIsFlightPath(quint16 command)
{
    static constexpr std::array<quint16, 21> commands{{
        16, 17, 18, 19, 21, 22, 23, 24, 31, 81, 82,
        84, 85, 94, 192, 30001, 31000, 31001, 31002, 31003, 31004,
    }};
    return std::find(commands.cbegin(), commands.cend(), command)
        != commands.cend();
}

bool WpRow::FrameHasGlobalLocation(quint8 frame)
{
    return MissionItemProtocol::frameHasGlobalLocation(frame);
}

QStringList WpRow::FrameList()
{
    return {QStringLiteral("Relative"), QStringLiteral("Absolute"),
            QStringLiteral("Terrain")};
}

void WpRow::setSeq(int value)
{
    if (m_data.Seq == value) return;
    m_data.Seq = value;
    emit seqChanged(value);
    emit displayNumberChanged(DisplayNumber());
    emit changed();
}

void WpRow::setCommand(quint16 value)
{
    if (m_data.Command == value) return;
    m_data.Command = value;
    emit commandChanged(value);
    emit commandNameChanged(CommandName());
    emit changed();
}

void WpRow::setCommandName(const QString &value)
{
    quint16 command = 0;
    if (commandForName(value, &command)) {
        setCommand(command);
    }
}

#define WPROW_DOUBLE_SETTER(Name, Field, Signal) \
    void WpRow::set##Name(double value) \
    { \
        if (m_data.Field == value) return; \
        m_data.Field = value; \
        emit Signal(value); \
        emit changed(); \
    }

WPROW_DOUBLE_SETTER(P1, P1, p1Changed)
WPROW_DOUBLE_SETTER(P2, P2, p2Changed)
WPROW_DOUBLE_SETTER(P3, P3, p3Changed)
WPROW_DOUBLE_SETTER(P4, P4, p4Changed)

#undef WPROW_DOUBLE_SETTER

void WpRow::setLat(double value)
{
    if (m_data.Lat == value || !std::isfinite(value)
        || value < -90.0 || value > 90.0) return;
    m_data.Lat = value;
    emit latChanged(value);
    recomputeCoordinates();
    emit changed();
}

void WpRow::setLng(double value)
{
    if (m_data.Lng == value || !std::isfinite(value)
        || value < -180.0 || value > 180.0) return;
    m_data.Lng = value;
    emit lngChanged(value);
    recomputeCoordinates();
    emit changed();
}

void WpRow::setAlt(double value)
{
    if (m_data.Alt == value || !std::isfinite(value)) return;
    m_data.Alt = value;
    emit altChanged(value);
    emit altDisplayChanged(value);
    emit changed();
}

void WpRow::setAltDisplay(double value)
{
    setAlt(value);
}

#define WPROW_STRING_SETTER(Name, Field, Signal) \
    void WpRow::set##Name(const QString &value) \
    { \
        if (m_data.Field == value) return; \
        m_data.Field = value; \
        emit Signal(value); \
        emit changed(); \
    }

WPROW_STRING_SETTER(Grad, Grad, gradChanged)
WPROW_STRING_SETTER(Angle, Angle, angleChanged)
WPROW_STRING_SETTER(Dist, Dist, distChanged)
WPROW_STRING_SETTER(Az, Az, azChanged)

#undef WPROW_STRING_SETTER

void WpRow::setFrame(quint8 value)
{
    if (m_data.Frame == value) return;
    m_data.Frame = value;
    emit frameChanged(value);
    emit frameNameChanged(FrameName());
    emit changed();
}

void WpRow::setFrameName(const QString &value)
{
    if (value.compare(QStringLiteral("Absolute"), Qt::CaseInsensitive) == 0) {
        setFrame(MAV_FRAME_GLOBAL);
    } else if (value.compare(QStringLiteral("Terrain"), Qt::CaseInsensitive) == 0) {
        setFrame(MAV_FRAME_GLOBAL_TERRAIN_ALT);
    } else {
        setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
    }
}

void WpRow::setTag(const QVariant &value)
{
    if (m_data.Tag == value) return;
    m_data.Tag = value;
    emit tagChanged(value);
    emit changed();
}

void WpRow::setZone(const QString &value)
{
    if (m_data.Zone == value) return;
    m_data.Zone = value;
    emit zoneChanged(value);
    reverseFromUtm();
    emit changed();
}

void WpRow::setEasting(const QString &value)
{
    if (m_data.Easting == value) return;
    m_data.Easting = value;
    emit eastingChanged(value);
    reverseFromUtm();
    emit changed();
}

void WpRow::setNorthing(const QString &value)
{
    if (m_data.Northing == value) return;
    m_data.Northing = value;
    emit northingChanged(value);
    reverseFromUtm();
    emit changed();
}

void WpRow::setMgrs(const QString &value)
{
    if (m_data.Mgrs == value) return;
    m_data.Mgrs = value;
    emit mgrsChanged(value);
    if (!m_coordinateGuard && !value.trimmed().isEmpty()) {
        double latitude = 0.0;
        double longitude = 0.0;
        if (fromMgrs(value, &latitude, &longitude)) {
            m_coordinateGuard = true;
            if (m_data.Lat != latitude) {
                m_data.Lat = latitude;
                emit latChanged(latitude);
            }
            if (m_data.Lng != longitude) {
                m_data.Lng = longitude;
                emit lngChanged(longitude);
            }
            m_coordinateGuard = false;
            recomputeCoordinates();
        }
    }
    emit changed();
}

void WpRow::applyData(const WpRowData &data)
{
    m_data = data;
    if (m_data.Lat != 0.0 || m_data.Lng != 0.0) {
        recomputeCoordinates();
    }
}

void WpRow::recomputeCoordinates()
{
    if (m_coordinateGuard) return;
    m_coordinateGuard = true;

    QString zoneText;
    QString eastingText;
    QString northingText;
    QString mgrsText;
    if (m_data.Lat != 0.0 || m_data.Lng != 0.0) {
        int zone = 0;
        double easting = 0.0;
        double northing = 0.0;
        if (toUtm(m_data.Lat, m_data.Lng, &zone, &easting, &northing)) {
            zoneText = QString::number(zone);
            eastingText = QString::number(easting, 'f', 1);
            northingText = QString::number(northing, 'f', 1);
            mgrsText = mgrsFor(m_data.Lat, m_data.Lng);
        }
    }
    const auto update = [](QString &target, const QString &value,
                           const auto &notify) {
        if (target != value) {
            target = value;
            notify(value);
        }
    };
    update(m_data.Zone, zoneText,
           [this](const QString &value) { emit zoneChanged(value); });
    update(m_data.Easting, eastingText,
           [this](const QString &value) { emit eastingChanged(value); });
    update(m_data.Northing, northingText,
           [this](const QString &value) { emit northingChanged(value); });
    update(m_data.Mgrs, mgrsText,
           [this](const QString &value) { emit mgrsChanged(value); });
    m_coordinateGuard = false;
}

void WpRow::reverseFromUtm()
{
    if (m_coordinateGuard) return;
    int zone = 0;
    bool eastingOk = false;
    bool northingOk = false;
    const double easting = m_data.Easting.toDouble(&eastingOk);
    const double northing = m_data.Northing.toDouble(&northingOk);
    if (!parseZone(m_data.Zone, &zone) || !eastingOk || !northingOk) {
        return;
    }
    double latitude = 0.0;
    double longitude = 0.0;
    if (!fromUtm(easting, northing, zone, &latitude, &longitude)) {
        return;
    }
    m_coordinateGuard = true;
    if (m_data.Lat != latitude) {
        m_data.Lat = latitude;
        emit latChanged(latitude);
    }
    if (m_data.Lng != longitude) {
        m_data.Lng = longitude;
        emit lngChanged(longitude);
    }
    m_coordinateGuard = false;
    recomputeCoordinates();
}
