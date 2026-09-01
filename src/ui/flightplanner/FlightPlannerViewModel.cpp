#include "FlightPlannerViewModel.h"

#include "QGCMAVLink.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <cmath>

namespace {
bool parseInteger(const QString &text, qlonglong minimum, qlonglong maximum,
                  qlonglong *result)
{
    bool ok = false;
    const qlonglong value = QLocale::c().toLongLong(text, &ok);
    if (!ok || value < minimum || value > maximum) return false;
    *result = value;
    return true;
}

bool parseDouble(const QString &text, double *result)
{
    bool ok = false;
    const double value = QLocale::c().toDouble(text, &ok);
    if (!ok || !std::isfinite(value)) return false;
    *result = value;
    return true;
}

struct ParsedWplRow
{
    WpRowData row;
    bool current = false;
};

QString wplNumber(double value)
{
    return QLocale::c().toString(value, 'g', 16);
}

QString wplLine(const WpRowData &row, int sequence, bool current)
{
    return QStringList{
        QString::number(sequence), current ? QStringLiteral("1")
                                           : QStringLiteral("0"),
        QString::number(row.Frame), QString::number(row.Command),
        wplNumber(row.P1), wplNumber(row.P2), wplNumber(row.P3),
        wplNumber(row.P4), wplNumber(row.Lat), wplNumber(row.Lng),
        wplNumber(row.Alt), QStringLiteral("1")}.join(QLatin1Char('\t'));
}

bool parseWplRow(const QString &line, ParsedWplRow *result, QString *error)
{
    const QStringList fields = line.split(
        QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    if (fields.size() < 12) {
        *error = QStringLiteral("expected 12 fields");
        return false;
    }

    qlonglong sequence = 0;
    qlonglong current = 0;
    qlonglong frame = 0;
    qlonglong command = 0;
    if (!parseInteger(fields.at(0), 0, 65535, &sequence)
        || !parseInteger(fields.at(1), 0, 1, &current)
        || !parseInteger(fields.at(2), 0, 255, &frame)
        || !parseInteger(fields.at(3), 0, 65535, &command)) {
        *error = QStringLiteral("invalid sequence, frame, or command");
        return false;
    }

    WpRowData parsed;
    parsed.Seq = static_cast<int>(sequence);
    parsed.Frame = static_cast<quint8>(frame);
    parsed.Command = static_cast<quint16>(command);
    if (!parseDouble(fields.at(4), &parsed.P1)
        || !parseDouble(fields.at(5), &parsed.P2)
        || !parseDouble(fields.at(6), &parsed.P3)
        || !parseDouble(fields.at(7), &parsed.P4)
        || !parseDouble(fields.at(8), &parsed.Lat)
        || !parseDouble(fields.at(9), &parsed.Lng)
        || !parseDouble(fields.at(10), &parsed.Alt)) {
        *error = QStringLiteral("invalid numeric waypoint field");
        return false;
    }
    if (parsed.Lat < -90.0 || parsed.Lat > 90.0
        || parsed.Lng < -180.0 || parsed.Lng > 180.0) {
        *error = QStringLiteral("coordinate outside valid range");
        return false;
    }
    result->row = parsed;
    result->current = current != 0;
    return true;
}
}

FlightPlannerViewModel::FlightPlannerViewModel(QObject *parent)
    : QObject(parent)
    , m_waypoints(this)
{
    connect(&m_waypoints, &FlightPlannerMissionModel::missionTypeChanged,
            this, &FlightPlannerViewModel::missionTypeChanged);
}

FlightPlannerMissionModel *FlightPlannerViewModel::Waypoints()
{
    return &m_waypoints;
}

const FlightPlannerMissionModel *FlightPlannerViewModel::Waypoints() const
{
    return &m_waypoints;
}

QStringList FlightPlannerViewModel::MissionTypes() const
{
    return {QStringLiteral("Mission"), QStringLiteral("Fence"),
            QStringLiteral("Rally")};
}

QString FlightPlannerViewModel::MissionType() const
{
    return m_waypoints.MissionType();
}

QString FlightPlannerViewModel::Status() const { return m_status; }
bool FlightPlannerViewModel::UseMavFtp() const { return m_useMavFtp; }
double FlightPlannerViewModel::HomeLat() const { return m_homeLat; }
double FlightPlannerViewModel::HomeLng() const { return m_homeLng; }
double FlightPlannerViewModel::HomeAlt() const { return m_homeAlt; }
bool FlightPlannerViewModel::HomeValid() const { return m_homeValid; }
QString FlightPlannerViewModel::HomeAltLabel() const
{
    return QStringLiteral("ASL");
}
double FlightPlannerViewModel::DefaultAltitude() const
{
    return m_defaultAltitude;
}

void FlightPlannerViewModel::setMissionType(const QString &type)
{
    FlightPlannerMissionModel::MissionStore store;
    if (!FlightPlannerMissionModel::storeForName(type, &store)) {
        setStatus(tr("Unknown mission type: %1").arg(type));
        return;
    }
    m_waypoints.setMissionStore(store);
}

void FlightPlannerViewModel::setUseMavFtp(bool enabled)
{
    if (m_useMavFtp == enabled) return;
    m_useMavFtp = enabled;
    emit useMavFtpChanged(enabled);
}

void FlightPlannerViewModel::setHomeLat(double value)
{
    if (!std::isfinite(value) || value < -90.0 || value > 90.0) return;
    markHomeValid();
    if (m_homeLat == value) return;
    m_homeLat = value;
    emit homeLatChanged(value);
}

void FlightPlannerViewModel::setHomeLng(double value)
{
    if (!std::isfinite(value) || value < -180.0 || value > 180.0) return;
    markHomeValid();
    if (m_homeLng == value) return;
    m_homeLng = value;
    emit homeLngChanged(value);
}

void FlightPlannerViewModel::setHomeAlt(double value)
{
    if (!std::isfinite(value)) return;
    markHomeValid();
    if (m_homeAlt == value) return;
    m_homeAlt = value;
    emit homeAltChanged(value);
}

void FlightPlannerViewModel::setDefaultAltitude(double value)
{
    if (!std::isfinite(value) || m_defaultAltitude == value) return;
    m_defaultAltitude = value;
    emit defaultAltitudeChanged(value);
}

void FlightPlannerViewModel::SetHomeFromVehicle(double latitude,
                                                double longitude,
                                                double altitudeAsl)
{
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0 || !std::isfinite(altitudeAsl)) {
        setStatus(tr("Vehicle home position is invalid."));
        return;
    }
    setHomeLat(latitude);
    setHomeLng(longitude);
    setHomeAlt(altitudeAsl);
    setStatus(tr("Home location set from vehicle."));
}

WpRow *FlightPlannerViewModel::AddWaypointAt(double latitude, double longitude)
{
    return AddWaypointAt(latitude, longitude, m_defaultAltitude);
}

WpRow *FlightPlannerViewModel::AddWaypointAt(double latitude, double longitude,
                                             double altitude)
{
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0 || !std::isfinite(altitude)) {
        setStatus(tr("Waypoint coordinate is invalid."));
        return nullptr;
    }

    WpRowData row;
    row.Lat = latitude;
    row.Lng = longitude;
    switch (m_waypoints.missionStore()) {
    case FlightPlannerMissionModel::MissionStore::Fence:
        row.Command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
        row.Frame = MAV_FRAME_GLOBAL;
        row.Alt = 0.0;
        break;
    case FlightPlannerMissionModel::MissionStore::Rally:
        row.Command = MAV_CMD_NAV_RALLY_POINT;
        row.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        row.Alt = altitude;
        break;
    case FlightPlannerMissionModel::MissionStore::Mission:
    default:
        row.Command = MAV_CMD_NAV_WAYPOINT;
        row.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        row.Alt = altitude;
        break;
    }
    WpRow *result = m_waypoints.appendRow(row);
    setStatus(tr("Added %1 item %2.")
                  .arg(MissionType()).arg(result->DisplayNumber()));
    return result;
}

bool FlightPlannerViewModel::DeleteWaypoint(int row)
{
    const bool removed = m_waypoints.removeRow(row);
    if (removed) setStatus(tr("Removed item %1.").arg(row + 1));
    return removed;
}

bool FlightPlannerViewModel::MoveWaypointUp(int row)
{
    return m_waypoints.moveWaypointUp(row);
}

bool FlightPlannerViewModel::MoveWaypointDown(int row)
{
    return m_waypoints.moveWaypointDown(row);
}

void FlightPlannerViewModel::ClearWaypoints()
{
    m_waypoints.clearActiveStore();
    setStatus(tr("Cleared local %1 items.").arg(MissionType().toLower()));
}

bool FlightPlannerViewModel::LoadFile(const QString &path, bool append)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(tr("Load failed: %1").arg(file.errorString()));
        return false;
    }
    QTextStream stream(&file);
    stream.setLocale(QLocale::c());
    const QString header = stream.readLine().trimmed();
    const QStringList version = header.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool versionOk = false;
    const int versionNumber = version.size() == 3
        ? version.at(2).toInt(&versionOk) : 0;
    if (version.size() != 3
        || version.at(0).compare(QStringLiteral("QGC"), Qt::CaseInsensitive) != 0
        || version.at(1).compare(QStringLiteral("WPL"), Qt::CaseInsensitive) != 0
        || !versionOk || versionNumber < 110) {
        setStatus(tr("Load failed: unsupported mission file header."));
        return false;
    }

    QVector<ParsedWplRow> parsedRows;
    int lineNumber = 1;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        ++lineNumber;
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        ParsedWplRow row;
        QString error;
        if (!parseWplRow(line, &row, &error)) {
            setStatus(tr("Load failed at line %1: %2.")
                          .arg(lineNumber).arg(error));
            return false;
        }
        parsedRows.append(row);
    }

    bool hasHome = false;
    WpRowData home;
    if (!parsedRows.isEmpty() && parsedRows.first().row.Seq == 0
        && parsedRows.first().current
        && parsedRows.first().row.Frame == MAV_FRAME_GLOBAL
        && parsedRows.first().row.Command == MAV_CMD_NAV_WAYPOINT) {
        home = parsedRows.takeFirst().row;
        hasHome = true;
    }

    QVector<WpRowData> missionRows;
    missionRows.reserve(parsedRows.size());
    for (const ParsedWplRow &parsed : parsedRows) {
        missionRows.append(parsed.row);
    }

    setMissionType(QStringLiteral("Mission"));
    if (append) {
        m_waypoints.appendStore(
            FlightPlannerMissionModel::MissionStore::Mission, missionRows);
    } else {
        m_waypoints.replaceStore(
            FlightPlannerMissionModel::MissionStore::Mission, missionRows);
        if (hasHome) {
            setHomeLat(home.Lat);
            setHomeLng(home.Lng);
            setHomeAlt(home.Alt);
        } else {
            clearHome();
        }
    }
    setStatus(tr("%1 %2 waypoint(s) from %3.")
                  .arg(append ? tr("Appended") : tr("Loaded"))
                  .arg(missionRows.size()).arg(QFileInfo(path).fileName()));
    return true;
}

bool FlightPlannerViewModel::LoadAndAppend(const QString &path)
{
    return LoadFile(path, true);
}

bool FlightPlannerViewModel::SaveFile(const QString &path)
{
    if (m_waypoints.missionStore()
        != FlightPlannerMissionModel::MissionStore::Mission) {
        setStatus(tr("Save failed: Fence and Rally require their typed "
                     "file codecs, which are not implemented yet."));
        return false;
    }
    if (!m_homeValid) {
        setStatus(tr("Save failed: set a valid Home location first."));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setStatus(tr("Save failed: %1").arg(file.errorString()));
        return false;
    }
    QTextStream stream(&file);
    stream.setLocale(QLocale::c());
    stream << "QGC WPL 110\n";

    const auto store = m_waypoints.missionStore();
    const QVector<WpRowData> rows = m_waypoints.rows(store);
    int sequenceOffset = 0;
    if (store == FlightPlannerMissionModel::MissionStore::Mission) {
        WpRowData home;
        home.Command = MAV_CMD_NAV_WAYPOINT;
        home.Frame = MAV_FRAME_GLOBAL;
        home.Lat = m_homeLat;
        home.Lng = m_homeLng;
        home.Alt = m_homeAlt;
        stream << wplLine(home, 0, true) << '\n';
        sequenceOffset = 1;
    }
    for (int index = 0; index < rows.size(); ++index) {
        stream << wplLine(rows.at(index), index + sequenceOffset, false)
               << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit()) {
        setStatus(tr("Save failed: %1").arg(file.errorString()));
        return false;
    }
    setStatus(tr("Saved %1 item(s) to %2.")
                  .arg(rows.size()).arg(QFileInfo(path).fileName()));
    return true;
}

void FlightPlannerViewModel::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(status);
}

void FlightPlannerViewModel::markHomeValid()
{
    if (m_homeValid) return;
    m_homeValid = true;
    emit homeValidChanged(true);
}

void FlightPlannerViewModel::clearHome()
{
    const bool wasValid = m_homeValid;
    const bool latitudeChanged = m_homeLat != 0.0;
    const bool longitudeChanged = m_homeLng != 0.0;
    const bool altitudeChanged = m_homeAlt != 0.0;
    m_homeValid = false;
    m_homeLat = 0.0;
    m_homeLng = 0.0;
    m_homeAlt = 0.0;
    if (wasValid) emit homeValidChanged(false);
    if (latitudeChanged) emit homeLatChanged(0.0);
    if (longitudeChanged) emit homeLngChanged(0.0);
    if (altitudeChanged) emit homeAltChanged(0.0);
}
