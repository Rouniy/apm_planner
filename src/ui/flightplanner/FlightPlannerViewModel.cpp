#include "FlightPlannerViewModel.h"

#include "FlightPlannerMissionCodec.h"
#include "FlightPlannerRouteMetrics.h"
#include "QgcPlanFileCodec.h"
#include "QGCMAVLink.h"
#include "comm/MissionTransferController.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

constexpr int MissionUndoLimit = 40;
constexpr double FeetPerMeter = 3.280839895013123;

bool isCopterVehicleType(int mavType)
{
    switch (mavType) {
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
        return true;
    default:
        return false;
    }
}

bool validMissionPosition(double latitude, double longitude, double altitude)
{
    return std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0
            && std::isfinite(longitude) && longitude >= -180.0
            && longitude <= 180.0 && std::isfinite(altitude);
}

bool remapJumpTargets(
        QVector<WpRowData> *rows, int oldRowCount,
        const std::function<int(int)> &newIndexForOldIndex, QString *error)
{
    if (!rows || oldRowCount < 0 || !newIndexForOldIndex) return false;
    for (WpRowData &row : *rows) {
        if (row.Command != MAV_CMD_DO_JUMP) continue;
        if (!std::isfinite(row.P1)) {
            if (error) *error = QStringLiteral("DO_JUMP target is not finite");
            return false;
        }
        const double rounded = std::round(row.P1);
        if (std::abs(row.P1 - rounded) > 0.000001
            || rounded < 1.0 || rounded > oldRowCount) {
            if (error) {
                *error = QStringLiteral("DO_JUMP target %1 is outside mission items 1..%2")
                        .arg(row.P1, 0, 'g', 16).arg(oldRowCount);
            }
            return false;
        }
        const int oldTargetIndex = static_cast<int>(rounded) - 1;
        const int newTargetIndex = newIndexForOldIndex(oldTargetIndex);
        if (newTargetIndex < 0) {
            if (error) {
                *error = QStringLiteral("DO_JUMP targets item %1, which would be removed")
                        .arg(oldTargetIndex + 1);
            }
            return false;
        }
        row.P1 = newTargetIndex + 1;
    }
    return true;
}

QVector<WpRowData> remapMissionJumpTargets(
        const QVector<WpRowData> &source, int sequenceOffset)
{
    QMap<int, int> targetMap;
    for (int index = 0; index < source.size(); ++index) {
        if (source.at(index).Seq >= 0) {
            // QGC Plan doJumpId/MAVLink mission sequence includes Home at 0,
            // while the local model stores only command rows starting at 0.
            targetMap.insert(source.at(index).Seq + 1,
                             sequenceOffset + index + 1);
        }
    }

    QVector<WpRowData> rows = source;
    for (int index = 0; index < rows.size(); ++index) {
        WpRowData &row = rows[index];
        if (row.Command == MAV_CMD_DO_JUMP && std::isfinite(row.P1)) {
            const double rounded = std::round(row.P1);
            if (std::abs(row.P1 - rounded) <= 0.000001
                && rounded >= 1.0
                && rounded <= std::numeric_limits<int>::max()) {
                const int oldTarget = static_cast<int>(rounded);
                row.P1 = targetMap.value(
                    oldTarget,
                    oldTarget <= source.size()
                        ? sequenceOffset + oldTarget : oldTarget);
            }
        }
        row.Seq = sequenceOffset + index;
    }
    return rows;
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
    , m_drawnPolygon(this)
{
    connect(&m_waypoints, &FlightPlannerMissionModel::missionTypeChanged,
            this, &FlightPlannerViewModel::missionTypeChanged);
    connect(&m_waypoints, &FlightPlannerMissionModel::missionTypeChanged,
            this, [this](const QString &) { recomputeRouteMetrics(); });
    connect(&m_waypoints, &FlightPlannerMissionModel::rowsChanged,
            this, [this](FlightPlannerMissionModel::MissionStore store) {
        if (store == m_waypoints.missionStore())
            recomputeRouteMetrics();
    });
    connect(this, &FlightPlannerViewModel::homeLatChanged,
            this, [this](double) { recomputeRouteMetrics(); });
    connect(this, &FlightPlannerViewModel::homeLngChanged,
            this, [this](double) { recomputeRouteMetrics(); });
    connect(this, &FlightPlannerViewModel::homeAltChanged,
            this, [this](double) { recomputeRouteMetrics(); });
    connect(&m_drawnPolygon, &FlightPlannerPolygonModel::statusChanged,
            this, &FlightPlannerViewModel::setStatus);
}

FlightPlannerViewModel::~FlightPlannerViewModel()
{
    MissionTransferController *controller =
            m_activeTransferController.data();
    const bool ownsTransfer = controller
            && controller == m_missionTransferController.data()
            && m_activeTransferId != 0
            && controller->transferId() == m_activeTransferId
            && controller->busy();
    if (m_missionTransferController)
        disconnect(m_missionTransferController.data(), nullptr, this, nullptr);
    if (ownsTransfer) {
        controller->cancel(
                tr("Flight planner closed during mission transfer."));
    }
}

FlightPlannerMissionModel *FlightPlannerViewModel::Waypoints()
{
    return &m_waypoints;
}

const FlightPlannerMissionModel *FlightPlannerViewModel::Waypoints() const
{
    return &m_waypoints;
}

FlightPlannerPolygonModel *FlightPlannerViewModel::DrawnPolygon()
{
    return &m_drawnPolygon;
}

const FlightPlannerPolygonModel *FlightPlannerViewModel::DrawnPolygon() const
{
    return &m_drawnPolygon;
}

bool FlightPlannerViewModel::PolygonDrawMode() const
{
    return m_polygonDrawMode;
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
double FlightPlannerViewModel::HomeAltDisplay() const
{
    return m_homeAlt * m_altitudeMultiplier;
}
bool FlightPlannerViewModel::HomeValid() const { return m_homeValid; }
QString FlightPlannerViewModel::HomeAltLabel() const
{
    return tr("%1 ASL").arg(AltUnit());
}
double FlightPlannerViewModel::DefaultAltitude() const
{
    return m_defaultAltitude;
}
double FlightPlannerViewModel::DefaultAltitudeDisplay() const
{
    return m_defaultAltitude * m_altitudeMultiplier;
}
QStringList FlightPlannerViewModel::DefaultFrames() const
{
    return WpRow::FrameList();
}
QString FlightPlannerViewModel::DefaultFrame() const
{
    return m_defaultFrame;
}
quint8 FlightPlannerViewModel::DefaultFrameId() const
{
    if (m_defaultFrame == QStringLiteral("Absolute"))
        return MAV_FRAME_GLOBAL;
    if (m_defaultFrame == QStringLiteral("Terrain"))
        return MAV_FRAME_GLOBAL_TERRAIN_ALT;
    return MAV_FRAME_GLOBAL_RELATIVE_ALT;
}
double FlightPlannerViewModel::WpRadius() const { return m_wpRadius; }
double FlightPlannerViewModel::WpRadiusDisplay() const
{
    return m_wpRadius * m_distanceMultiplier;
}
double FlightPlannerViewModel::LoiterRadius() const { return m_loiterRadius; }
double FlightPlannerViewModel::LoiterRadiusDisplay() const
{
    return m_loiterRadius * m_distanceMultiplier;
}
double FlightPlannerViewModel::AltWarn() const { return m_altWarn; }
double FlightPlannerViewModel::AltWarnDisplay() const
{
    return m_altWarn * m_altitudeMultiplier;
}
QStringList FlightPlannerViewModel::AltitudeUnits() const
{
    return {QStringLiteral("Meters"), QStringLiteral("Feet")};
}
QString FlightPlannerViewModel::AltUnits() const { return m_altUnits; }
QString FlightPlannerViewModel::AltUnit() const
{
    return m_altUnits == QStringLiteral("Feet")
        ? QStringLiteral("ft") : QStringLiteral("m");
}
double FlightPlannerViewModel::AltitudeMultiplier() const
{
    return m_altitudeMultiplier;
}
QStringList FlightPlannerViewModel::DistanceUnits() const
{
    return {QStringLiteral("Meters"), QStringLiteral("Feet")};
}
QString FlightPlannerViewModel::DistUnits() const { return m_distUnits; }
QString FlightPlannerViewModel::DistanceUnit() const
{
    return m_distUnits == QStringLiteral("Feet")
        ? QStringLiteral("ft") : QStringLiteral("m");
}
double FlightPlannerViewModel::DistanceMultiplier() const
{
    return m_distanceMultiplier;
}
bool FlightPlannerViewModel::SplineDefault() const { return m_splineDefault; }
bool FlightPlannerViewModel::VerifyHeight() const { return m_verifyHeight; }
bool FlightPlannerViewModel::ShowWpRadius() const
{
    return !isCopterVehicleType(m_vehicleType)
        && m_vehicleType != MAV_TYPE_GROUND_ROVER;
}
bool FlightPlannerViewModel::ShowLoiterRadius() const
{
    return ShowWpRadius();
}
QString FlightPlannerViewModel::TotalDist() const { return m_totalDist; }
QString FlightPlannerViewModel::HomeDist() const { return m_homeDist; }
QString FlightPlannerViewModel::PrevDist() const { return m_prevDist; }
double FlightPlannerViewModel::WpAccelerationCms() const
{
    return m_navigationParameters.wpAccelerationCms;
}
double FlightPlannerViewModel::WpSpeedCms() const
{
    return m_navigationParameters.wpSpeedCms;
}
FlightPlannerNavigationParameters
FlightPlannerViewModel::NavigationParameters() const
{
    return m_navigationParameters;
}

MissionElevationProfileResult
FlightPlannerViewModel::BuildElevationProfile(
        double sampleSpacingMeters, int maximumSamples) const
{
    MissionElevationHome home;
    home.valid = m_homeValid;
    home.latitude = m_homeLat;
    home.longitude = m_homeLng;
    home.altitudeAmslMeters = m_homeAlt;
    return MissionElevationProfile::Build(
        m_waypoints.rows(FlightPlannerMissionModel::MissionStore::Mission),
        home,
        [this](double latitude, double longitude,
               double *altitudeAmslMeters) {
            return sampleTerrainAltitude(
                latitude, longitude, altitudeAmslMeters);
        },
        sampleSpacingMeters, maximumSamples);
}

bool FlightPlannerViewModel::TransferBusy() const
{
    return m_transferBusy;
}

int FlightPlannerViewModel::TransferProgress() const
{
    return m_transferProgress;
}

bool FlightPlannerViewModel::CanReadWaypoints() const
{
    return m_missionTransferController
            && m_activeTransferId == 0
            && !m_transferBusy
            && !m_missionTransferController->busy();
}

bool FlightPlannerViewModel::CanWriteWaypoints() const
{
    return CanReadWaypoints();
}

bool FlightPlannerViewModel::CanCancelTransfer() const
{
    return m_activeTransferId != 0
            && m_activeTransferController
            && m_activeTransferController.data()
                    == m_missionTransferController.data()
            && m_activeTransferController->transferId() == m_activeTransferId
            && m_activeTransferController->busy();
}

bool FlightPlannerViewModel::CanSetHomeFromVehicle() const
{
    return static_cast<bool>(m_vehicleHomeProvider);
}

bool FlightPlannerViewModel::CanUndo() const
{
    return !m_undoHistory.isEmpty();
}

bool FlightPlannerViewModel::SetHome(double latitude, double longitude)
{
    if (m_transferBusy) {
        setStatus(tr("Set Home failed: wait for the active mission transfer."));
        return false;
    }
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0) {
        setStatus(tr("Set Home failed: coordinate is invalid."));
        return false;
    }

    double altitude = m_homeValid ? m_homeAlt : 0.0;
    const bool sampledTerrain = sampleTerrainAltitude(
        latitude, longitude, &altitude);
    if (sampledTerrain)
        altitude = std::round(altitude * 100.0) / 100.0;
    setHomeLat(latitude);
    setHomeLng(longitude);
    setHomeAlt(altitude);
    setStatus(sampledTerrain
        ? tr("Home location set to %1, %2 at %3 %4 ASL from terrain data.")
              .arg(latitude, 0, 'f', 7)
              .arg(longitude, 0, 'f', 7)
              .arg(altitude * m_altitudeMultiplier, 0, 'f', 1)
              .arg(AltUnit())
        : tr("Home location set to %1, %2; altitude kept at %3 %4 ASL.")
              .arg(latitude, 0, 'f', 7)
              .arg(longitude, 0, 'f', 7)
              .arg(altitude * m_altitudeMultiplier, 0, 'f', 1)
              .arg(AltUnit()));
    return true;
}

bool FlightPlannerViewModel::SetFenceReturn(double latitude,
                                             double longitude)
{
    if (m_transferBusy) {
        setStatus(tr("Set Fence Return failed: wait for the active mission transfer."));
        return false;
    }
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0) {
        setStatus(tr("Set Fence Return failed: coordinate is invalid."));
        return false;
    }

    const auto fenceStore = FlightPlannerMissionModel::MissionStore::Fence;
    QVector<WpRowData> rows = m_waypoints.rows(fenceStore);
    for (int index = rows.size() - 1; index >= 0; --index) {
        if (rows.at(index).Command == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            rows.removeAt(index);
        }
    }

    WpRowData returnPoint;
    returnPoint.Command = MAV_CMD_NAV_FENCE_RETURN_POINT;
    returnPoint.Frame = MAV_FRAME_GLOBAL;
    returnPoint.Lat = latitude;
    returnPoint.Lng = longitude;
    returnPoint.Alt = 0.0;
    rows.append(returnPoint);

    captureStoreUndo(fenceStore);
    m_waypoints.replaceStore(fenceStore, rows);
    setMissionType(FlightPlannerMissionModel::storeName(fenceStore));
    setStatus(tr("Fence return location set to %1, %2.")
                  .arg(latitude, 0, 'f', 7)
                  .arg(longitude, 0, 'f', 7));
    return true;
}

void FlightPlannerViewModel::setMissionTransferController(
        MissionTransferController *controller)
{
    if (m_missionTransferController.data() == controller)
        return;

    ++m_controllerBindingGeneration;
    if (m_controllerBindingGeneration == 0)
        ++m_controllerBindingGeneration;
    const quint64 bindingGeneration = m_controllerBindingGeneration;

    MissionTransferController *previous = m_missionTransferController.data();
    const bool cancelledForVehicleChange = previous
            && previous == m_activeTransferController.data()
            && m_activeTransferId != 0
            && previous->transferId() == m_activeTransferId
            && previous->busy();
    if (previous) {
        // Invalidate and disconnect first so the synchronous cancellation
        // result from the old vehicle cannot mutate the newly selected store.
        disconnect(previous, nullptr, this, nullptr);
    }
    clearActiveTransfer();
    if (cancelledForVehicleChange) {
        previous->cancel(
                tr("Active vehicle changed during mission transfer."));
    }

    m_missionTransferController = nullptr;
    setTransferBusy(false);
    setTransferProgress(0);

    if (controller && controller->thread() != thread()) {
        setStatus(tr("Mission transfer controller is on the wrong thread."));
        emit transferAvailabilityChanged();
        return;
    }

    m_missionTransferController = controller;
    if (controller) {
        connect(controller, &MissionTransferController::busyChanged,
                this, [this, controller, bindingGeneration](bool busy) {
            if (m_controllerBindingGeneration != bindingGeneration
                || m_missionTransferController.data() != controller)
                return;
            setTransferBusy(busy);
        });
        connect(controller, &MissionTransferController::progressChanged,
                this, [this, controller, bindingGeneration](int progress) {
            if (m_controllerBindingGeneration != bindingGeneration
                || m_missionTransferController.data() != controller)
                return;
            setTransferProgress(progress);
        });
        connect(controller, &MissionTransferController::transferFinished,
                this, [this, controller, bindingGeneration](
                        const MissionTransferResult &result) {
            handleTransferFinished(controller, bindingGeneration, result);
        });
        connect(controller, &QObject::destroyed,
                this, [this, bindingGeneration]() {
            if (m_controllerBindingGeneration != bindingGeneration)
                return;
            const bool transferWasActive = m_activeTransferId != 0;
            m_missionTransferController = nullptr;
            clearActiveTransfer();
            setTransferBusy(false);
            setTransferProgress(0);
            emit transferAvailabilityChanged();
            if (transferWasActive) {
                setStatus(tr("Mission transfer failed: vehicle became unavailable."));
            }
        });
        setTransferBusy(controller->busy());
        setTransferProgress(controller->busy() ? controller->progress() : 0);
    }
    emit transferAvailabilityChanged();

    if (cancelledForVehicleChange) {
        setStatus(tr("Mission transfer cancelled: active vehicle changed."));
    }
}

void FlightPlannerViewModel::setVehicleHomeProvider(
        VehicleHomeProvider provider)
{
    const bool wasAvailable = CanSetHomeFromVehicle();
    m_vehicleHomeProvider = std::move(provider);
    const bool isAvailable = CanSetHomeFromVehicle();
    if (wasAvailable != isAvailable)
        emit vehicleHomeProviderChanged(isAvailable);
}

void FlightPlannerViewModel::setVehiclePositionProvider(
        VehiclePositionProvider provider)
{
    m_vehiclePositionProvider = std::move(provider);
}

void FlightPlannerViewModel::setTerrainAltitudeProvider(
        TerrainAltitudeProvider provider)
{
    m_terrainAltitudeProvider = std::move(provider);
}

void FlightPlannerViewModel::setVehicleType(int mavType)
{
    if (m_vehicleType == mavType) return;
    m_vehicleType = mavType;
    emit vehicleTypeChanged(m_vehicleType);
    recomputeRouteMetrics();
}

void FlightPlannerViewModel::setVehicleParameterAccess(
        VehicleParameterReader reader, VehicleParameterWriter writer)
{
    m_vehicleParameterReader = std::move(reader);
    m_vehicleParameterWriter = std::move(writer);
    RefreshVehicleParameters();
}

void FlightPlannerViewModel::RefreshVehicleParameters()
{
    m_vehicleParameters.clear();
    if (m_vehicleParameterReader) {
        for (const QString &name : FlightPlannerNavigation::ParameterNames()) {
            double value = 0.0;
            if (m_vehicleParameterReader(name, &value)
                && std::isfinite(value)) {
                m_vehicleParameters.insert(name, value);
            }
        }
    }
    applyVehicleParameters();
}

void FlightPlannerViewModel::applyVehicleParameters()
{
    const FlightPlannerNavigationParameters resolved =
        FlightPlannerNavigation::ResolveParameters(
            m_vehicleParameters, m_wpRadius);
    const bool radiusChanged = m_wpRadius != resolved.wpRadiusMeters;
    const bool navigationChanged =
        m_navigationParameters.wpAccelerationCms
                != resolved.wpAccelerationCms
        || m_navigationParameters.wpSpeedCms != resolved.wpSpeedCms
        || m_navigationParameters.wpRadiusMeters != resolved.wpRadiusMeters
        || m_navigationParameters.wpAccelerationSource
                != resolved.wpAccelerationSource
        || m_navigationParameters.wpSpeedSource != resolved.wpSpeedSource
        || m_navigationParameters.wpRadiusSource != resolved.wpRadiusSource
        || m_navigationParameters.availableParameters
                != resolved.availableParameters;
    m_wpRadius = resolved.wpRadiusMeters;
    m_navigationParameters = resolved;
    if (radiusChanged) {
        emit wpRadiusChanged(m_wpRadius);
    }
    if (navigationChanged) {
        emit plannerNavigationChanged();
    }
}

void FlightPlannerViewModel::UpdateVehicleParameter(
        const QString &name, const QVariant &value)
{
    if (!FlightPlannerNavigation::ParameterNames().contains(name)) {
        return;
    }
    bool ok = false;
    const double converted = value.toDouble(&ok);
    if (!ok || !std::isfinite(converted)) {
        return;
    }
    m_vehicleParameters.insert(name, converted);
    applyVehicleParameters();
}

int FlightPlannerViewModel::WriteRadiusParams()
{
    if (!m_vehicleParameterWriter) {
        return 0;
    }
    int written = 0;
    const auto writes = FlightPlannerNavigation::WaypointRadiusWrites(
        m_navigationParameters.availableParameters, m_wpRadius);
    for (const auto &write : writes) {
        if (m_vehicleParameterWriter(write.first, write.second)) {
            ++written;
        }
    }
    return written;
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

void FlightPlannerViewModel::setPolygonDrawMode(bool enabled)
{
    if (m_polygonDrawMode == enabled)
        return;
    m_polygonDrawMode = enabled;
    emit polygonDrawModeChanged(enabled);
    setStatus(enabled
        ? tr("Polygon draw: click the map to add vertices.")
        : tr("Polygon draw off."));
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
    emit homeAltDisplayChanged(HomeAltDisplay());
}

void FlightPlannerViewModel::setHomeAltDisplay(double value)
{
    if (!std::isfinite(value) || m_altitudeMultiplier <= 0.0) return;
    setHomeAlt(value / m_altitudeMultiplier);
}

void FlightPlannerViewModel::setDefaultAltitude(double value)
{
    if (!std::isfinite(value) || m_defaultAltitude == value) return;
    m_defaultAltitude = value;
    emit defaultAltitudeChanged(value);
    emit defaultAltitudeDisplayChanged(DefaultAltitudeDisplay());
}

void FlightPlannerViewModel::setDefaultAltitudeDisplay(double value)
{
    if (!std::isfinite(value) || m_altitudeMultiplier <= 0.0) return;
    setDefaultAltitude(value / m_altitudeMultiplier);
}

void FlightPlannerViewModel::setDefaultFrame(const QString &value)
{
    const QString candidate = value.trimmed();
    QString canonical;
    for (const QString &frame : WpRow::FrameList()) {
        if (candidate.compare(frame, Qt::CaseInsensitive) == 0) {
            canonical = frame;
            break;
        }
    }
    if (canonical.isEmpty() || m_defaultFrame == canonical) return;
    m_defaultFrame = canonical;
    emit defaultFrameChanged(m_defaultFrame);
}

void FlightPlannerViewModel::setWpRadius(double value)
{
    if (!std::isfinite(value) || value < 0.0 || m_wpRadius == value) {
        return;
    }
    m_wpRadius = value;
    m_navigationParameters.wpRadiusMeters = value;
    emit wpRadiusChanged(value);
    emit wpRadiusDisplayChanged(WpRadiusDisplay());
    emit plannerNavigationChanged();
}

void FlightPlannerViewModel::setWpRadiusDisplay(double value)
{
    if (!std::isfinite(value) || value < 0.0
        || m_distanceMultiplier <= 0.0) {
        return;
    }
    setWpRadius(value / m_distanceMultiplier);
}

void FlightPlannerViewModel::setLoiterRadius(double value)
{
    if (!std::isfinite(value) || m_loiterRadius == value)
        return;
    m_loiterRadius = value;
    emit loiterRadiusChanged(value);
    emit loiterRadiusDisplayChanged(LoiterRadiusDisplay());
    recomputeRouteMetrics();
}

void FlightPlannerViewModel::setLoiterRadiusDisplay(double value)
{
    if (!std::isfinite(value) || m_distanceMultiplier <= 0.0) return;
    setLoiterRadius(value / m_distanceMultiplier);
}

void FlightPlannerViewModel::setAltWarn(double value)
{
    if (!std::isfinite(value) || value < 0.0 || m_altWarn == value) return;
    m_altWarn = value;
    emit altWarnChanged(value);
    emit altWarnDisplayChanged(AltWarnDisplay());
}

void FlightPlannerViewModel::setAltWarnDisplay(double value)
{
    if (!std::isfinite(value) || value < 0.0
        || m_altitudeMultiplier <= 0.0) {
        return;
    }
    setAltWarn(value / m_altitudeMultiplier);
}

void FlightPlannerViewModel::setAltUnits(const QString &value)
{
    const QString candidate = value.trimmed();
    QString canonical;
    for (const QString &unit : AltitudeUnits()) {
        if (candidate.compare(unit, Qt::CaseInsensitive) == 0) {
            canonical = unit;
            break;
        }
    }
    if (canonical.isEmpty() || canonical == m_altUnits) return;
    m_altUnits = canonical;
    m_altitudeMultiplier = canonical == QStringLiteral("Feet")
        ? FeetPerMeter : 1.0;
    m_waypoints.setAltitudePresentation(m_altitudeMultiplier, AltUnit());
    emit altUnitsChanged(m_altUnits);
    emit altitudePresentationChanged();
    emit homeAltDisplayChanged(HomeAltDisplay());
    emit defaultAltitudeDisplayChanged(DefaultAltitudeDisplay());
    emit altWarnDisplayChanged(AltWarnDisplay());
}

void FlightPlannerViewModel::setDistUnits(const QString &value)
{
    const QString candidate = value.trimmed();
    QString canonical;
    for (const QString &unit : DistanceUnits()) {
        if (candidate.compare(unit, Qt::CaseInsensitive) == 0) {
            canonical = unit;
            break;
        }
    }
    if (canonical.isEmpty() || canonical == m_distUnits) return;
    m_distUnits = canonical;
    m_distanceMultiplier = canonical == QStringLiteral("Feet")
        ? FeetPerMeter : 1.0;
    m_waypoints.setDistancePresentation(
        m_distanceMultiplier, DistanceUnit());
    emit distUnitsChanged(m_distUnits);
    emit distancePresentationChanged();
    emit wpRadiusDisplayChanged(WpRadiusDisplay());
    emit loiterRadiusDisplayChanged(LoiterRadiusDisplay());
    recomputeRouteMetrics();
}

void FlightPlannerViewModel::setSplineDefault(bool enabled)
{
    if (m_splineDefault == enabled) return;
    m_splineDefault = enabled;
    emit splineDefaultChanged(enabled);
}

void FlightPlannerViewModel::setVerifyHeight(bool enabled)
{
    if (m_verifyHeight == enabled) return;
    m_verifyHeight = enabled;
    emit verifyHeightChanged(enabled);
}

void FlightPlannerViewModel::SetHomeFromVehicle()
{
    if (!m_vehicleHomeProvider) {
        setStatus(tr("Home location is unavailable: no active vehicle."));
        return;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeAsl = 0.0;
    bool available = false;
    try {
        available = m_vehicleHomeProvider(
                &latitude, &longitude, &altitudeAsl);
    } catch (...) {
        setStatus(tr("Home location is unavailable from the active vehicle."));
        return;
    }
    if (!available) {
        setStatus(tr("Home location is unavailable from the active vehicle."));
        return;
    }
    SetHomeFromVehicle(latitude, longitude, altitudeAsl);
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

void FlightPlannerViewModel::ReadWaypoints()
{
    MissionTransferController *controller =
            m_missionTransferController.data();
    if (!controller) {
        setStatus(tr("Read failed: no active vehicle mission controller."));
        return;
    }
    if (controller->busy() || m_activeTransferId != 0) {
        setStatus(tr("Read failed: another mission transfer is active."));
        return;
    }

    const auto store = m_waypoints.missionStore();
    const MAV_MISSION_TYPE type = FlightPlannerMissionCodec::missionType(store);
    const quint64 previousId = controller->transferId();
    if (previousId == std::numeric_limits<quint64>::max()) {
        setStatus(tr("Read failed: mission transfer identifier is exhausted."));
        return;
    }

    m_activeTransferController = controller;
    m_activeTransferId = previousId + 1;
    m_activeMissionType = static_cast<quint8>(type);
    m_activeTransferStore = store;
    m_activeTransferRowCount = 0;
    m_activeTransferIsDownload = true;
    emit transferAvailabilityChanged();
    setStatus(tr("Reading %1...")
              .arg(FlightPlannerMissionModel::storeName(store).toLower()));

    if (!controller->startDownload(type)) {
        if (m_activeTransferController.data() == controller
            && m_activeTransferId == previousId + 1) {
            clearActiveTransfer();
            setTransferBusy(controller->busy());
            setTransferProgress(controller->progress());
            setStatus(tr("Read failed: mission transfer could not be started."));
        }
        return;
    }

    // startDownload may synchronously finish with a test or loopback
    // transport. Only touch state if this operation is still pending.
    if (m_activeTransferController.data() == controller
        && m_activeTransferId == previousId + 1) {
        if (controller->transferId() != m_activeTransferId) {
            clearActiveTransfer();
            setStatus(tr("Read failed: mission transfer identifier changed."));
            return;
        }
        setTransferBusy(controller->busy());
        setTransferProgress(controller->progress());
    }
}

void FlightPlannerViewModel::WriteWaypoints()
{
    MissionTransferController *controller =
            m_missionTransferController.data();
    if (!controller) {
        setStatus(tr("Write failed: no active vehicle mission controller."));
        return;
    }
    if (controller->busy() || m_activeTransferId != 0) {
        setStatus(tr("Write failed: another mission transfer is active."));
        return;
    }

    const auto store = m_waypoints.missionStore();
    const QVector<WpRowData> rows = m_waypoints.rows(store);
    if (store == FlightPlannerMissionModel::MissionStore::Mission
        && m_altWarn > 0.0) {
        for (int index = 0; index < rows.size(); ++index) {
            const WpRowData &row = rows.at(index);
            if (WpRow::CommandIsFlightPath(row.Command)
                && row.Command != MAV_CMD_NAV_TAKEOFF
                && row.Command != MAV_CMD_NAV_LAND
                && row.Command != MAV_CMD_NAV_RETURN_TO_LAUNCH
                && row.Alt < m_altWarn) {
                setStatus(tr("Write blocked: mission item %1 altitude %2 %4 "
                             "is below Alt Warn %3 %4.")
                          .arg(index + 1)
                          .arg(row.Alt * m_altitudeMultiplier, 0, 'f', 1)
                          .arg(m_altWarn * m_altitudeMultiplier, 0, 'f', 1)
                          .arg(AltUnit()));
                return;
            }
        }
    }
    const FlightPlannerMissionCodec::EncodeResult encoded =
            FlightPlannerMissionCodec::encode(
                    store, rows, m_homeValid,
                    m_homeLat, m_homeLng, m_homeAlt);
    if (!encoded.ok) {
        setStatus(tr("Write failed: %1").arg(encoded.error));
        return;
    }

    const MAV_MISSION_TYPE type = FlightPlannerMissionCodec::missionType(store);
    const quint64 previousId = controller->transferId();
    if (previousId == std::numeric_limits<quint64>::max()) {
        setStatus(tr("Write failed: mission transfer identifier is exhausted."));
        return;
    }

    m_activeTransferController = controller;
    m_activeTransferId = previousId + 1;
    m_activeMissionType = static_cast<quint8>(type);
    m_activeTransferStore = store;
    m_activeTransferRowCount = rows.size();
    m_activeTransferIsDownload = false;
    emit transferAvailabilityChanged();
    setStatus(tr("Writing %1...")
              .arg(FlightPlannerMissionModel::storeName(store).toLower()));

    if (!controller->startUpload(type, encoded.items)) {
        if (m_activeTransferController.data() == controller
            && m_activeTransferId == previousId + 1) {
            clearActiveTransfer();
            setTransferBusy(controller->busy());
            setTransferProgress(controller->progress());
            setStatus(tr("Write failed: mission transfer could not be started."));
        }
        return;
    }

    if (m_activeTransferController.data() == controller
        && m_activeTransferId == previousId + 1) {
        if (controller->transferId() != m_activeTransferId) {
            clearActiveTransfer();
            setStatus(tr("Write failed: mission transfer identifier changed."));
            return;
        }
        setTransferBusy(controller->busy());
        setTransferProgress(controller->progress());
    }
}

void FlightPlannerViewModel::CancelTransfer()
{
    MissionTransferController *controller =
            m_activeTransferController.data();
    if (!controller || controller != m_missionTransferController.data()
        || !controller->busy() || m_activeTransferId == 0
        || controller->transferId() != m_activeTransferId) {
        setStatus(tr("No flight-plan transfer is active."));
        return;
    }

    setStatus(tr("Cancelling mission transfer..."));
    if (!controller->cancel(tr("Cancelled by user"))
        && m_activeTransferId != 0) {
        setStatus(tr("Mission transfer could not be cancelled."));
    }
}

bool FlightPlannerViewModel::requireEditableMission(const QString &operation)
{
    if (m_transferBusy) {
        setStatus(tr("%1 failed: wait for the active mission transfer.")
                  .arg(operation));
        return false;
    }
    if (m_waypoints.missionStore()
            != FlightPlannerMissionModel::MissionStore::Mission) {
        setStatus(tr("%1 is available only for Mission items.")
                  .arg(operation));
        return false;
    }
    return true;
}

void FlightPlannerViewModel::captureStoreUndo(
        FlightPlannerMissionModel::MissionStore store)
{
    const bool wasAvailable = CanUndo();
    UndoEntry entry;
    entry.changedStore = store;
    entry.previousActiveStore = m_waypoints.missionStore();
    entry.rows = m_waypoints.rows(store);
    m_undoHistory.append(std::move(entry));
    while (m_undoHistory.size() > MissionUndoLimit)
        m_undoHistory.removeFirst();
    if (wasAvailable != CanUndo()) emit canUndoChanged(CanUndo());
}

void FlightPlannerViewModel::captureMissionUndo()
{
    captureStoreUndo(FlightPlannerMissionModel::MissionStore::Mission);
}

bool FlightPlannerViewModel::replaceMissionRows(
        const QVector<WpRowData> &rows, const QString &status)
{
    captureMissionUndo();
    m_waypoints.replaceStore(
        FlightPlannerMissionModel::MissionStore::Mission, rows);
    setStatus(status);
    return true;
}

bool FlightPlannerViewModel::appendMissionCommand(
        quint16 command, quint8 frame, double latitude, double longitude,
        double altitude, double param1, double param2,
        double param3, double param4)
{
    if (!requireEditableMission(tr("Add command"))) return false;
    if (!validMissionPosition(latitude, longitude, altitude)
        || !std::isfinite(param1) || !std::isfinite(param2)
        || !std::isfinite(param3) || !std::isfinite(param4)) {
        setStatus(tr("Add command failed: mission position is invalid."));
        return false;
    }

    WpRowData row;
    row.Command = command;
    row.Frame = frame;
    row.Lat = latitude;
    row.Lng = longitude;
    row.Alt = altitude;
    row.P1 = param1;
    row.P2 = param2;
    row.P3 = param3;
    row.P4 = param4;
    captureMissionUndo();
    WpRow *added = m_waypoints.appendRow(row);
    setStatus(tr("Added %1 as mission item %2.")
              .arg(WpRow::CommandNameFor(command))
              .arg(added ? added->DisplayNumber() : 0));
    return added != nullptr;
}

bool FlightPlannerViewModel::sampleTerrainAltitude(
        double latitude, double longitude, double *altitudeAmslMeters) const
{
    if (!altitudeAmslMeters || !m_terrainAltitudeProvider
        || !std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0) {
        return false;
    }
    double sampled = 0.0;
    bool available = false;
    try {
        available = m_terrainAltitudeProvider(
            latitude, longitude, &sampled);
    } catch (...) {
        return false;
    }
    if (!available || !std::isfinite(sampled)) return false;
    *altitudeAmslMeters = sampled;
    return true;
}

double FlightPlannerViewModel::verifyPlaceAltitude(
        double latitude, double longitude, double baseAltitude,
        quint8 frame) const
{
    if (!m_verifyHeight || !std::isfinite(baseAltitude))
        return baseAltitude;

    double terrain = 0.0;
    if (!sampleTerrainAltitude(latitude, longitude, &terrain))
        return baseAltitude;
    if (frame == MAV_FRAME_GLOBAL || frame == MAV_FRAME_GLOBAL_INT)
        return terrain + baseAltitude;
    if (frame == MAV_FRAME_GLOBAL_TERRAIN_ALT
        || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT_INT) {
        return baseAltitude;
    }

    double homeTerrain = 0.0;
    if (!m_homeValid
        || !sampleTerrainAltitude(m_homeLat, m_homeLng, &homeTerrain)) {
        return baseAltitude;
    }
    return terrain + baseAltitude - homeTerrain;
}

bool FlightPlannerViewModel::InsertWaypointAt(
        int index, double latitude, double longitude, double altitude)
{
    return insertNavigationWaypointAt(
        index,
        m_splineDefault ? MAV_CMD_NAV_SPLINE_WAYPOINT
                        : MAV_CMD_NAV_WAYPOINT,
        latitude, longitude, altitude);
}

bool FlightPlannerViewModel::InsertRegularWaypointAt(
        int index, double latitude, double longitude, double altitude)
{
    return insertNavigationWaypointAt(
        index, MAV_CMD_NAV_WAYPOINT, latitude, longitude, altitude);
}

bool FlightPlannerViewModel::InsertSplineWaypointAt(
        int index, double latitude, double longitude, double altitude)
{
    return insertNavigationWaypointAt(
        index, MAV_CMD_NAV_SPLINE_WAYPOINT,
        latitude, longitude, altitude);
}

bool FlightPlannerViewModel::AddWaypointAtCurrentPosition()
{
    if (!requireEditableMission(tr("Add waypoint at current position")))
        return false;
    if (!m_vehiclePositionProvider) {
        setStatus(tr("Add waypoint at current position failed: no vehicle is connected."));
        return false;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeRelative = 0.0;
    bool available = false;
    try {
        available = m_vehiclePositionProvider(
            &latitude, &longitude, &altitudeRelative);
    } catch (...) {
        available = false;
    }
    if (!available || !validMissionPosition(
            latitude, longitude, altitudeRelative)) {
        setStatus(tr("Add waypoint at current position failed: vehicle position is unavailable."));
        return false;
    }

    // Mission Planner uses the live relative altitude when Verify Height is
    // off. With Verify Height on, setfromMap replaces it with the ordinary
    // default-clearance terrain calculation for the selected frame.
    const quint8 frame = DefaultFrameId();
    const double liveAltitude = std::trunc(altitudeRelative);
    const double originalAltitude = qFuzzyIsNull(liveAltitude)
        ? m_defaultAltitude : liveAltitude;
    const double altitude = m_verifyHeight
        ? verifyPlaceAltitude(latitude, longitude, m_defaultAltitude, frame)
        : originalAltitude;
    WpRow *row = AddWaypointAt(latitude, longitude, altitude);
    if (!row) return false;
    setStatus(tr("Added waypoint %1 at the current vehicle position.")
                  .arg(row->DisplayNumber()));
    return true;
}

bool FlightPlannerViewModel::insertNavigationWaypointAt(
        int index, quint16 command, double latitude, double longitude,
        double altitude)
{
    if (!requireEditableMission(tr("Insert waypoint"))) return false;
    if (command != MAV_CMD_NAV_WAYPOINT
        && command != MAV_CMD_NAV_SPLINE_WAYPOINT) {
        setStatus(tr("Insert waypoint failed: command is not a waypoint."));
        return false;
    }
    QVector<WpRowData> rows = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    if (index < 0 || index > rows.size()) {
        setStatus(tr("Insert waypoint failed: index %1 is outside 0..%2.")
                  .arg(index).arg(rows.size()));
        return false;
    }
    if (!validMissionPosition(latitude, longitude, altitude)) {
        setStatus(tr("Insert waypoint failed: coordinate is invalid."));
        return false;
    }

    QString jumpError;
    if (!remapJumpTargets(
            &rows, rows.size(), [index](int oldIndex) {
                return oldIndex >= index ? oldIndex + 1 : oldIndex;
            }, &jumpError)) {
        setStatus(tr("Insert waypoint refused: %1.").arg(jumpError));
        return false;
    }

    WpRowData waypoint;
    waypoint.Command = command;
    waypoint.Frame = DefaultFrameId();
    waypoint.Lat = latitude;
    waypoint.Lng = longitude;
    waypoint.Alt = verifyPlaceAltitude(
        latitude, longitude, altitude, waypoint.Frame);
    rows.insert(index, waypoint);
    return replaceMissionRows(
        rows, tr("Inserted waypoint as mission item %1.").arg(index + 1));
}

bool FlightPlannerViewModel::MoveWaypoint(
        int row, double latitude, double longitude)
{
    if (!requireEditableMission(tr("Move waypoint"))) return false;
    if (!validMissionPosition(latitude, longitude, 0.0)) {
        setStatus(tr("Move waypoint failed: coordinate is invalid."));
        return false;
    }
    QVector<WpRowData> rows = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    if (row < 0 || row >= rows.size()) {
        setStatus(tr("Move waypoint failed: row %1 is outside the mission.")
                  .arg(row + 1));
        return false;
    }
    WpRowData &waypoint = rows[row];
    if (!WpRow::CommandHasLocation(waypoint.Command)
        || !WpRow::FrameHasGlobalLocation(waypoint.Frame)) {
        setStatus(tr("Move waypoint failed: mission item %1 has no global position.")
                  .arg(row + 1));
        return false;
    }
    if (m_verifyHeight
        && waypoint.Frame != MAV_FRAME_GLOBAL_TERRAIN_ALT
        && waypoint.Frame != MAV_FRAME_GLOBAL_TERRAIN_ALT_INT) {
        double oldTerrain = 0.0;
        double newTerrain = 0.0;
        if (sampleTerrainAltitude(waypoint.Lat, waypoint.Lng, &oldTerrain)
            && sampleTerrainAltitude(latitude, longitude, &newTerrain)) {
            waypoint.Alt += newTerrain - oldTerrain;
        }
    }
    waypoint.Lat = latitude;
    waypoint.Lng = longitude;
    return replaceMissionRows(
        rows, tr("Moved mission item %1 to %2, %3.")
                  .arg(row + 1)
                  .arg(latitude, 0, 'f', 7)
                  .arg(longitude, 0, 'f', 7));
}

bool FlightPlannerViewModel::AddTakeoff(
        double latitude, double longitude, double altitude)
{
    return appendMissionCommand(MAV_CMD_NAV_TAKEOFF,
                                DefaultFrameId(),
                                latitude, longitude, altitude);
}

bool FlightPlannerViewModel::AddLand(double latitude, double longitude)
{
    return appendMissionCommand(MAV_CMD_NAV_LAND,
                                DefaultFrameId(),
                                latitude, longitude, 0.0);
}

bool FlightPlannerViewModel::AddRtl()
{
    return appendMissionCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH,
                                MAV_FRAME_GLOBAL_RELATIVE_ALT,
                                0.0, 0.0, 0.0);
}

bool FlightPlannerViewModel::AddRoi(double latitude, double longitude)
{
    if (!requireEditableMission(tr("Add ROI"))) return false;
    const quint8 frame = DefaultFrameId();
    return appendMissionCommand(MAV_CMD_DO_SET_ROI,
                                frame, latitude, longitude,
                                verifyPlaceAltitude(
                                    latitude, longitude,
                                    m_defaultAltitude, frame));
}

bool FlightPlannerViewModel::AddLoiterForever(
        double latitude, double longitude)
{
    if (!requireEditableMission(tr("Add loiter"))) return false;
    const quint8 frame = DefaultFrameId();
    return appendMissionCommand(MAV_CMD_NAV_LOITER_UNLIM,
                                frame, latitude, longitude,
                                verifyPlaceAltitude(
                                    latitude, longitude,
                                    m_defaultAltitude, frame));
}

bool FlightPlannerViewModel::AddLoiterTime(
        double latitude, double longitude, double seconds)
{
    if (!requireEditableMission(tr("Add loiter time"))) return false;
    if (!std::isfinite(seconds) || seconds < 0.0) {
        setStatus(tr("Add loiter time failed: duration must be zero or greater."));
        return false;
    }
    const quint8 frame = DefaultFrameId();
    return appendMissionCommand(
        MAV_CMD_NAV_LOITER_TIME, frame, latitude, longitude,
        verifyPlaceAltitude(latitude, longitude, m_defaultAltitude, frame),
        seconds);
}

bool FlightPlannerViewModel::AddLoiterTurns(
        double latitude, double longitude, double turns)
{
    if (!requireEditableMission(tr("Add loiter turns"))) return false;
    if (!std::isfinite(turns) || turns <= 0.0) {
        setStatus(tr("Add loiter turns failed: turns must be greater than zero."));
        return false;
    }
    const quint8 frame = DefaultFrameId();
    return appendMissionCommand(
        MAV_CMD_NAV_LOITER_TURNS, frame, latitude, longitude,
        verifyPlaceAltitude(latitude, longitude, m_defaultAltitude, frame),
        turns);
}

bool FlightPlannerViewModel::AddJump(
        int targetMissionItem, int repeatCount)
{
    if (!requireEditableMission(tr("Add jump"))) return false;
    const int resultingRowCount = m_waypoints.storeRowCount(
        FlightPlannerMissionModel::MissionStore::Mission) + 1;
    if (targetMissionItem < 1 || targetMissionItem > resultingRowCount) {
        setStatus(tr("Add jump failed: target must be within mission items 1..%1.")
                      .arg(resultingRowCount));
        return false;
    }
    if (repeatCount < -1 || repeatCount > 32767) {
        setStatus(tr("Add jump failed: repeat count must be -1 or 0..32767."));
        return false;
    }
    return appendMissionCommand(
        MAV_CMD_DO_JUMP, DefaultFrameId(), 0.0, 0.0, 0.0,
        targetMissionItem, repeatCount);
}

bool FlightPlannerViewModel::ReverseWaypoints()
{
    if (!requireEditableMission(tr("Reverse waypoints"))) return false;
    QVector<WpRowData> rows = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    if (rows.size() < 2) {
        setStatus(tr("Reverse waypoints requires at least two mission items."));
        return false;
    }
    const int rowCount = rows.size();
    QString jumpError;
    if (!remapJumpTargets(
            &rows, rowCount, [rowCount](int oldIndex) {
                return rowCount - oldIndex - 1;
            }, &jumpError)) {
        setStatus(tr("Reverse waypoints refused: %1.").arg(jumpError));
        return false;
    }
    std::reverse(rows.begin(), rows.end());
    return replaceMissionRows(rows, tr("Reversed %1 mission items.")
                                    .arg(rowCount));
}

bool FlightPlannerViewModel::ModifyAllAlt(const QString &expression)
{
    if (!requireEditableMission(tr("Modify all altitudes"))) return false;
    QString value = expression.trimmed();
    const bool multiply = value.startsWith(QLatin1Char('*'));
    if (multiply) value = value.mid(1).trimmed();
    double operand = 0.0;
    if (value.isEmpty() || !parseDouble(value, &operand)) {
        setStatus(tr("Modify all altitudes failed: invalid expression '%1'.")
                  .arg(expression));
        return false;
    }

    QVector<WpRowData> rows = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    if (rows.isEmpty()) {
        setStatus(tr("Modify all altitudes failed: mission is empty."));
        return false;
    }
    for (WpRowData &row : rows) {
        const double altitude = multiply ? row.Alt * operand
            : row.Alt + operand / m_altitudeMultiplier;
        if (!std::isfinite(altitude)) {
            setStatus(tr("Modify all altitudes failed: result is not finite."));
            return false;
        }
        row.Alt = altitude;
    }
    return replaceMissionRows(
        rows, multiply
            ? tr("Multiplied all mission altitudes by %1.").arg(operand)
            : tr("Changed all mission altitudes by %1 %2.")
                  .arg(operand).arg(AltUnit()));
}

bool FlightPlannerViewModel::Undo()
{
    if (m_transferBusy) {
        setStatus(tr("Undo failed: wait for the active mission transfer."));
        return false;
    }
    if (m_undoHistory.isEmpty()) {
        setStatus(tr("Nothing to undo."));
        return false;
    }

    const UndoEntry snapshot = m_undoHistory.takeLast();
    m_waypoints.replaceStore(snapshot.changedStore, snapshot.rows);
    m_waypoints.setMissionStore(snapshot.previousActiveStore);
    emit canUndoChanged(CanUndo());
    setStatus(tr("Undo restored %1 %2 item(s); Home was unchanged.")
              .arg(snapshot.rows.size())
              .arg(FlightPlannerMissionModel::storeName(
                       snapshot.changedStore)));
    return true;
}

WpRow *FlightPlannerViewModel::AddWaypointAt(double latitude, double longitude)
{
    if (m_transferBusy || !std::isfinite(latitude)
        || latitude < -90.0 || latitude > 90.0
        || !std::isfinite(longitude) || longitude < -180.0
        || longitude > 180.0
        || m_waypoints.missionStore()
            != FlightPlannerMissionModel::MissionStore::Mission) {
        return AddWaypointAt(latitude, longitude, m_defaultAltitude);
    }
    return AddWaypointAt(latitude, longitude,
        verifyPlaceAltitude(latitude, longitude, m_defaultAltitude,
                            DefaultFrameId()));
}

WpRow *FlightPlannerViewModel::AddWaypointAt(double latitude, double longitude,
                                             double altitude)
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before editing waypoints."));
        return nullptr;
    }
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
        row.Command = m_splineDefault
            ? MAV_CMD_NAV_SPLINE_WAYPOINT : MAV_CMD_NAV_WAYPOINT;
        row.Frame = DefaultFrameId();
        row.Alt = altitude;
        captureMissionUndo();
        break;
    }
    WpRow *result = m_waypoints.appendRow(row);
    setStatus(tr("Added %1 item %2.")
                  .arg(MissionType()).arg(result->DisplayNumber()));
    return result;
}

bool FlightPlannerViewModel::DeleteWaypoint(int row)
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before editing waypoints."));
        return false;
    }
    if (m_waypoints.missionStore()
            == FlightPlannerMissionModel::MissionStore::Mission) {
        QVector<WpRowData> rows = m_waypoints.rows(
            FlightPlannerMissionModel::MissionStore::Mission);
        if (row < 0 || row >= rows.size()) {
            setStatus(tr("Delete waypoint failed: row %1 is outside the mission.")
                      .arg(row + 1));
            return false;
        }
        const int oldRowCount = rows.size();
        rows.removeAt(row);
        QString jumpError;
        if (!remapJumpTargets(
                &rows, oldRowCount, [row](int oldIndex) {
                    if (oldIndex == row) return -1;
                    return oldIndex > row ? oldIndex - 1 : oldIndex;
                }, &jumpError)) {
            setStatus(tr("Delete waypoint refused: %1.").arg(jumpError));
            return false;
        }
        captureMissionUndo();
        m_waypoints.replaceStore(
            FlightPlannerMissionModel::MissionStore::Mission, rows);
        setStatus(tr("Removed item %1.").arg(row + 1));
        return true;
    }
    const bool removed = m_waypoints.removeRow(row);
    if (removed) setStatus(tr("Removed item %1.").arg(row + 1));
    return removed;
}

bool FlightPlannerViewModel::MoveWaypointUp(int row)
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before editing waypoints."));
        return false;
    }
    if (m_waypoints.missionStore()
            == FlightPlannerMissionModel::MissionStore::Mission) {
        QVector<WpRowData> rows = m_waypoints.rows(
            FlightPlannerMissionModel::MissionStore::Mission);
        if (row <= 0 || row >= rows.size()) {
            setStatus(tr("Move waypoint up failed: row %1 cannot move up.")
                      .arg(row + 1));
            return false;
        }
        QString jumpError;
        if (!remapJumpTargets(
                &rows, rows.size(), [row](int oldIndex) {
                    if (oldIndex == row) return row - 1;
                    if (oldIndex == row - 1) return row;
                    return oldIndex;
                }, &jumpError)) {
            setStatus(tr("Move waypoint refused: %1.").arg(jumpError));
            return false;
        }
        std::swap(rows[row], rows[row - 1]);
        return replaceMissionRows(rows, tr("Moved mission item %1 up.")
                                        .arg(row + 1));
    }
    return m_waypoints.moveWaypointUp(row);
}

bool FlightPlannerViewModel::MoveWaypointDown(int row)
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before editing waypoints."));
        return false;
    }
    if (m_waypoints.missionStore()
            == FlightPlannerMissionModel::MissionStore::Mission) {
        QVector<WpRowData> rows = m_waypoints.rows(
            FlightPlannerMissionModel::MissionStore::Mission);
        if (row < 0 || row + 1 >= rows.size()) {
            setStatus(tr("Move waypoint down failed: row %1 cannot move down.")
                      .arg(row + 1));
            return false;
        }
        QString jumpError;
        if (!remapJumpTargets(
                &rows, rows.size(), [row](int oldIndex) {
                    if (oldIndex == row) return row + 1;
                    if (oldIndex == row + 1) return row;
                    return oldIndex;
                }, &jumpError)) {
            setStatus(tr("Move waypoint refused: %1.").arg(jumpError));
            return false;
        }
        std::swap(rows[row], rows[row + 1]);
        return replaceMissionRows(rows, tr("Moved mission item %1 down.")
                                        .arg(row + 1));
    }
    return m_waypoints.moveWaypointDown(row);
}

void FlightPlannerViewModel::ClearWaypoints()
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before clearing waypoints."));
        return;
    }
    if (m_waypoints.missionStore()
            == FlightPlannerMissionModel::MissionStore::Mission
        && m_waypoints.storeRowCount(
               FlightPlannerMissionModel::MissionStore::Mission) > 0) {
        captureMissionUndo();
    }
    m_waypoints.clearActiveStore();
    setStatus(tr("Cleared local %1 items.").arg(MissionType().toLower()));
}

bool FlightPlannerViewModel::AddPolygonPoint(double latitude,
                                             double longitude)
{
    return AddPolygonPoint(latitude, longitude, m_defaultAltitude);
}

bool FlightPlannerViewModel::AddPolygonPoint(double latitude,
                                             double longitude,
                                             double altitude)
{
    if (m_transferBusy) {
        setStatus(tr("Wait for the active mission transfer before editing the polygon."));
        return false;
    }
    return m_drawnPolygon.AddDrawnPolygonPoint(
            latitude, longitude, altitude);
}

void FlightPlannerViewModel::ClearPolygon()
{
    if (m_transferBusy) return;
    m_drawnPolygon.ClearDrawnPolygon();
}

bool FlightPlannerViewModel::BuildPolygonFromWaypoints()
{
    if (m_transferBusy) return false;
    return m_drawnPolygon.BuildPolygonFromWaypoints(m_waypoints.rows(
            FlightPlannerMissionModel::MissionStore::Mission));
}

bool FlightPlannerViewModel::OffsetDrawnPolygon(double meters)
{
    if (m_transferBusy) return false;
    return m_drawnPolygon.OffsetDrawnPolygon(meters);
}

bool FlightPlannerViewModel::LoadPolygon(const QString &path, bool append)
{
    if (m_transferBusy) return false;
    return m_drawnPolygon.LoadPolygon(path, append);
}

bool FlightPlannerViewModel::SavePolygon(const QString &path)
{
    return m_drawnPolygon.SavePolygon(path);
}

bool FlightPlannerViewModel::AddDrawnPolygonToFence(bool inclusion)
{
    if (m_transferBusy) return false;
    QString validationError;
    if (!m_drawnPolygon.IsValid(&validationError)) {
        setStatus(tr("Fence polygon failed: %1").arg(validationError));
        return false;
    }

    const QVector<SurveyGridCoordinate> &polygon =
            m_drawnPolygon.DrawnPolygon();
    QVector<WpRowData> fenceRows;
    fenceRows.reserve(polygon.size());
    const quint16 command = inclusion
            ? MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
            : MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
    for (const SurveyGridCoordinate &point : polygon) {
        WpRowData row;
        row.Command = command;
        row.Frame = MAV_FRAME_GLOBAL;
        row.P1 = polygon.size();
        row.Lat = point.latitude;
        row.Lng = point.longitude;
        row.Alt = 0.0;
        fenceRows.append(row);
    }
    m_waypoints.appendStore(
            FlightPlannerMissionModel::MissionStore::Fence, fenceRows);
    setMissionType(QStringLiteral("Fence"));
    setStatus(tr("Added %1 fence polygon (%2 vertices).")
            .arg(inclusion ? tr("inclusion") : tr("exclusion"))
            .arg(polygon.size()));
    return true;
}

bool FlightPlannerViewModel::LoadFile(const QString &path, bool append)
{
    if (m_transferBusy) {
        setStatus(tr("Load failed: wait for the active mission transfer."));
        return false;
    }
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
        setStatus(tr("Save failed: use the typed Fence or Rally export for "
                     "the active store."));
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

bool FlightPlannerViewModel::LoadPlanFile(const QString &path, bool append)
{
    if (m_transferBusy) {
        setStatus(tr("Plan load failed: wait for the active mission transfer."));
        return false;
    }
    const MissionPlanner::QgcPlanFileCodec::DecodeResult loaded =
        MissionPlanner::QgcPlanFileCodec::LoadPlan(path);
    if (!loaded.ok) {
        setStatus(tr("Plan load failed: %1").arg(loaded.error));
        return false;
    }

    QVector<WpRowData> fenceRows = loaded.plan.Fence;
    const int existingMissionCount = append
        ? m_waypoints.storeRowCount(
              FlightPlannerMissionModel::MissionStore::Mission)
        : 0;
    const QVector<WpRowData> missionRows = remapMissionJumpTargets(
        loaded.plan.Mission, existingMissionCount);
    if (append) {
        const QVector<WpRowData> existingFence = m_waypoints.rows(
            FlightPlannerMissionModel::MissionStore::Fence);
        const bool existingHasReturn = std::any_of(
            existingFence.cbegin(), existingFence.cend(),
            [](const WpRowData &row) {
                return row.Command == MAV_CMD_NAV_FENCE_RETURN_POINT;
            });
        if (existingHasReturn) {
            for (int index = fenceRows.size() - 1; index >= 0; --index) {
                if (fenceRows.at(index).Command
                        == MAV_CMD_NAV_FENCE_RETURN_POINT) {
                    fenceRows.removeAt(index);
                }
            }
        }
        m_waypoints.appendStore(
            FlightPlannerMissionModel::MissionStore::Mission,
            missionRows);
        m_waypoints.appendStore(
            FlightPlannerMissionModel::MissionStore::Fence, fenceRows);
        m_waypoints.appendStore(
            FlightPlannerMissionModel::MissionStore::Rally,
            loaded.plan.Rally);
    } else {
        m_waypoints.replaceStore(
            FlightPlannerMissionModel::MissionStore::Mission,
            missionRows);
        m_waypoints.replaceStore(
            FlightPlannerMissionModel::MissionStore::Fence, fenceRows);
        m_waypoints.replaceStore(
            FlightPlannerMissionModel::MissionStore::Rally,
            loaded.plan.Rally);
        setHomeLat(loaded.plan.Home.Latitude);
        setHomeLng(loaded.plan.Home.Longitude);
        setHomeAlt(loaded.plan.Home.Altitude);
    }

    setStatus(tr("%1 QGC Plan: %2 mission, %3 fence and %4 rally item(s) from %5.")
        .arg(append ? tr("Appended") : tr("Loaded"))
        .arg(loaded.plan.Mission.size())
        .arg(fenceRows.size())
        .arg(loaded.plan.Rally.size())
        .arg(QFileInfo(path).fileName()));
    return true;
}

bool FlightPlannerViewModel::SavePlanFile(const QString &path)
{
    if (!m_homeValid) {
        setStatus(tr("Plan save failed: set a valid Home location first."));
        return false;
    }

    MissionPlanner::QgcPlanFileCodec::PlanData plan;
    plan.Home = {m_homeLat, m_homeLng, m_homeAlt};
    plan.Mission = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    plan.Fence = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    plan.Rally = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Rally);
    const MissionPlanner::QgcPlanFileCodec::Result saved =
        MissionPlanner::QgcPlanFileCodec::SavePlan(path, plan);
    if (!saved.ok) {
        setStatus(tr("Plan save failed: %1").arg(saved.error));
        return false;
    }
    setStatus(tr("Saved QGC Plan with %1 mission, %2 fence and %3 rally item(s) to %4.")
        .arg(plan.Mission.size()).arg(plan.Fence.size())
        .arg(plan.Rally.size()).arg(QFileInfo(path).fileName()));
    return true;
}

QVector<SurveyGridCoordinate>
FlightPlannerViewModel::SurveyBoundary(double altitude) const
{
    QVector<SurveyGridCoordinate> polygon;
    if (m_drawnPolygon.IsValid()) {
        polygon = m_drawnPolygon.DrawnPolygon();
        for (SurveyGridCoordinate &point : polygon)
            point.altitude = altitude;
        return polygon;
    }
    if (m_drawnPolygon.Count() > 0)
        return polygon;

    const QVector<WpRowData> rows = m_waypoints.rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    polygon.reserve(rows.size());
    for (const WpRowData &row : rows) {
        if (!WpRow::CommandIsFlightPath(row.Command)
            || !WpRow::FrameHasGlobalLocation(row.Frame)
            || !std::isfinite(row.Lat) || row.Lat < -90.0 || row.Lat > 90.0
            || !std::isfinite(row.Lng) || row.Lng < -180.0
            || row.Lng > 180.0 || (row.Lat == 0.0 && row.Lng == 0.0)) {
            continue;
        }
        polygon.append({row.Lat, row.Lng, altitude});
    }
    return polygon;
}

bool FlightPlannerViewModel::AppendSurveyPlan(const SurveyMissionPlan &plan)
{
    if (m_transferBusy) {
        setStatus(tr("Survey append failed: wait for the active mission transfer."));
        return false;
    }
    if (!plan.success) {
        setStatus(tr("Survey failed: %1").arg(plan.error));
        return false;
    }
    if (plan.commands.isEmpty()) {
        setStatus(tr("Grid produced no mission commands."));
        return false;
    }

    QVector<WpRowData> commands = plan.commands;
    const int existingCommandCount = m_waypoints.storeRowCount(
        FlightPlannerMissionModel::MissionStore::Mission);
    if (plan.jumpTargetsAreRelative) {
        for (WpRowData &row : commands) {
            if (row.Command == MAV_CMD_DO_JUMP) {
                row.P1 += existingCommandCount;
            }
        }
    }
    m_waypoints.appendStore(
        FlightPlannerMissionModel::MissionStore::Mission, commands);
    setMissionType(QStringLiteral("Mission"));
    const QString segments = plan.segmentCount > 1
        ? tr(" in %1 flight segments").arg(plan.segmentCount) : QString();
    setStatus(tr("Survey added %1 navigation point(s) and %2 camera command(s)%3.")
        .arg(plan.navigationCount)
        .arg(plan.cameraCommandCount)
        .arg(segments));
    return true;
}

void FlightPlannerViewModel::recomputeRouteMetrics()
{
    if (m_recomputingRouteMetrics) return;
    m_recomputingRouteMetrics = true;

    const FlightPlannerMissionModel::MissionStore activeStore =
        m_waypoints.missionStore();
    const QVector<WpRowData> rows = m_waypoints.rows(activeStore);
    QVector<FlightPlannerRouteMetrics::RoutePoint> points;
    points.reserve(rows.size());
    for (const WpRowData &row : rows) {
        FlightPlannerRouteMetrics::RoutePoint point;
        point.coordinate = {row.Lat, row.Lng, row.Alt};
        point.included = (activeStore
                != FlightPlannerMissionModel::MissionStore::Mission
                || WpRow::CommandIsFlightPath(row.Command))
            && (row.Lat != 0.0 || row.Lng != 0.0);
        point.loiterTurnsCommand =
            row.Command == MAV_CMD_NAV_LOITER_TURNS;
        point.loiterTurns = row.P1;
        point.commandLoiterRadiusMeters = row.P3;
        points.append(point);
    }

    FlightPlannerRouteMetrics::Options options;
    options.configuredLoiterRadiusMeters = m_loiterRadius;
    options.useConfiguredLoiterRadiusWhenCommandRadiusIsZero =
        !isCopterVehicleType(m_vehicleType);
    const FlightPlannerRouteMetrics::Result metrics =
        FlightPlannerRouteMetrics::Calculate(
            {m_homeLat, m_homeLng, m_homeAlt}, m_homeValid,
            points, options);

    for (int row = 0; row < metrics.legs.size(); ++row) {
        const FlightPlannerRouteMetrics::Leg &leg = metrics.legs.at(row);
        if (!leg.valid) {
            m_waypoints.setRouteMetrics(row, QString(), QString(),
                                        QString(), QString());
            continue;
        }
        m_waypoints.setRouteMetrics(
            row,
            QLocale::c().toString(leg.gradientPercent, 'f', 1),
            QLocale::c().toString(leg.angleDegrees, 'f', 1),
            QLocale::c().toString(
                leg.distanceMeters * m_distanceMultiplier, 'f', 1),
            QLocale::c().toString(leg.bearingDegrees, 'f', 0));
    }

    const bool imperial = m_distUnits == QStringLiteral("Feet");
    const QString total = imperial
        ? tr("%1 miles").arg(QLocale::c().toString(
              metrics.missionDistanceMeters / 1000.0 * 0.621371,
              'f', 4))
        : tr("%1 km").arg(QLocale::c().toString(
              metrics.missionDistanceMeters / 1000.0, 'f', 4));
    const QString previous = QLocale::c().toString(
        metrics.lastLegDistanceMeters * m_distanceMultiplier, 'f', 2)
        + QLatin1Char(' ') + DistanceUnit();
    const QString home = QLocale::c().toString(
        metrics.lastToHomeDistanceMeters * m_distanceMultiplier, 'f', 2)
        + QLatin1Char(' ') + DistanceUnit();
    const bool changed = m_totalDist != total || m_prevDist != previous
        || m_homeDist != home;
    m_totalDist = total;
    m_prevDist = previous;
    m_homeDist = home;
    m_recomputingRouteMetrics = false;
    if (changed) emit routeMetricsChanged();
}

void FlightPlannerViewModel::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(status);
}

void FlightPlannerViewModel::setTransferBusy(bool busy)
{
    // MissionTransferController publishes busy=false immediately before the
    // terminal result. Keep the UI locked until that matching result has been
    // validated and committed (or rejected as stale).
    if (!busy && m_activeTransferId != 0)
        return;
    if (m_transferBusy == busy)
        return;
    m_transferBusy = busy;
    emit transferBusyChanged(busy);
    emit transferAvailabilityChanged();
}

void FlightPlannerViewModel::setTransferProgress(int progress)
{
    progress = qBound(0, progress, 100);
    if (m_transferProgress == progress)
        return;
    m_transferProgress = progress;
    emit transferProgressChanged(progress);
}

void FlightPlannerViewModel::clearActiveTransfer()
{
    const bool hadActiveTransfer = m_activeTransferId != 0
            || m_activeTransferController;
    m_activeTransferController = nullptr;
    m_activeTransferId = 0;
    m_activeMissionType = 0;
    m_activeTransferStore =
            FlightPlannerMissionModel::MissionStore::Mission;
    m_activeTransferRowCount = 0;
    m_activeTransferIsDownload = false;
    if (hadActiveTransfer)
        emit transferAvailabilityChanged();
}

void FlightPlannerViewModel::handleTransferFinished(
        MissionTransferController *controller,
        quint64 bindingGeneration,
        const MissionTransferResult &result)
{
    const MissionTransferService::Direction expectedDirection =
            m_activeTransferIsDownload
            ? MissionTransferService::Direction::Download
            : MissionTransferService::Direction::Upload;
    if (!controller
        || bindingGeneration != m_controllerBindingGeneration
        || controller != m_missionTransferController.data()
        || controller != m_activeTransferController.data()
        || result.transferId != m_activeTransferId) {
        return;
    }
    if (static_cast<quint8>(result.missionType) != m_activeMissionType
        || result.direction != expectedDirection) {
        clearActiveTransfer();
        setTransferBusy(controller->busy());
        setTransferProgress(controller->progress());
        setStatus(tr("Mission transfer failed: terminal result did not match "
                     "the requested mission type."));
        return;
    }

    const auto store = m_activeTransferStore;
    const int uploadedRowCount = m_activeTransferRowCount;
    const bool wasDownload = m_activeTransferIsDownload;
    const QString storeName =
            FlightPlannerMissionModel::storeName(store).toLower();
    clearActiveTransfer();
    setTransferBusy(controller->busy());
    setTransferProgress(controller->progress());

    if (!result.succeeded()) {
        if (result.state == MissionTransferService::State::Cancelled
            || result.result == MAV_MISSION_OPERATION_CANCELLED) {
            setStatus(tr("%1 transfer cancelled.")
                      .arg(FlightPlannerMissionModel::storeName(store)));
            return;
        }
        const QString detail = result.errorString.isEmpty()
                ? tr("vehicle returned MAV_MISSION_RESULT %1")
                          .arg(static_cast<int>(result.result))
                : result.errorString;
        setStatus(tr("%1 failed: %2")
                  .arg(wasDownload ? tr("Read") : tr("Write"), detail));
        return;
    }

    if (!wasDownload) {
        if (store == FlightPlannerMissionModel::MissionStore::Mission) {
            WriteRadiusParams();
        }
        setStatus(tr("Wrote %1 %2 point(s).")
                  .arg(uploadedRowCount).arg(storeName));
        return;
    }

    const FlightPlannerMissionCodec::DecodeResult decoded =
            FlightPlannerMissionCodec::decode(store, result.downloadedItems);
    if (!decoded.ok) {
        setStatus(tr("Read failed: %1").arg(decoded.error));
        return;
    }

    // Mission Planner treats the first mission item as Home when present.
    // An empty mission response does not erase an already known vehicle Home.
    if (store == FlightPlannerMissionModel::MissionStore::Mission
        && decoded.homeValid) {
        setHomeLat(decoded.homeLatitude);
        setHomeLng(decoded.homeLongitude);
        setHomeAlt(decoded.homeAltitude);
    }
    m_waypoints.replaceStore(store, decoded.rows);
    setStatus(tr("Read %1 %2 point(s).")
              .arg(decoded.rows.size()).arg(storeName));
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
    if (altitudeChanged) {
        emit homeAltChanged(0.0);
        emit homeAltDisplayChanged(0.0);
    }
}
