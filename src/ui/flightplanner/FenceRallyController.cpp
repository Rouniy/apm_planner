#include "FenceRallyController.h"

#include "FenceRallyFileCodec.h"
#include "FlightPlannerViewModel.h"
#include "QGCMAVLink.h"

#include <QFileInfo>
#include <QThread>

namespace MissionPlanner
{
namespace
{
bool isZero(double value)
{
    return value == 0.0;
}

QString legacyFenceStoreLossError(const QVector<WpRowData> &rows)
{
    for (int index = 0; index < rows.size(); ++index) {
        const WpRowData &row = rows.at(index);
        const bool returnPoint =
                row.Command == MAV_CMD_NAV_FENCE_RETURN_POINT;
        const bool polygon =
                row.Command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
                || row.Command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
        if (!returnPoint && !polygon) {
            continue;
        }
        if (row.Frame != MAV_FRAME_GLOBAL) {
            return QObject::tr(
                    "Legacy .fen cannot preserve the MAVLink frame of fence item %1.")
                    .arg(index + 1);
        }
        if (!isZero(row.Alt)) {
            return QObject::tr(
                    "Legacy .fen cannot preserve the altitude of fence item %1.")
                    .arg(index + 1);
        }
        if (!isZero(row.P2) || !isZero(row.P3) || !isZero(row.P4)
            || (returnPoint && !isZero(row.P1))) {
            return QObject::tr(
                    "Legacy .fen cannot preserve all parameters of fence item %1.")
                    .arg(index + 1);
        }
    }
    return {};
}

QString legacyFenceLossError(const Fence &fence)
{
    if (fence.HasReturn) {
        if (fence.ReturnPoint.Frame != MAV_FRAME_GLOBAL) {
            return QObject::tr(
                    "Legacy .fen cannot preserve the fence return MAVLink frame.");
        }
        if (fence.ReturnPoint.Return.Altitude != 0.0) {
            return QObject::tr(
                    "Legacy .fen cannot preserve fence return altitude.");
        }
    }
    for (const FencePolygon &polygon : fence.Polygons) {
        for (const GeoCoordinate &point : polygon.Points) {
            if (point.Altitude != 0.0) {
                return QObject::tr(
                        "Legacy .fen cannot preserve polygon vertex altitude.");
            }
        }
    }
    return {};
}

QString legacyRallyLossError(const RallyPoints &rally)
{
    for (int index = 0; index < rally.Points.size(); ++index) {
        if (rally.Points.at(index).Frame
                != MAV_FRAME_GLOBAL_RELATIVE_ALT) {
            return QObject::tr(
                    "Legacy .ral cannot preserve the MAVLink frame of rally point %1.")
                    .arg(index + 1);
        }
    }
    return {};
}

QString legacyRallyStoreLossError(const QVector<WpRowData> &rows)
{
    for (int index = 0; index < rows.size(); ++index) {
        const WpRowData &row = rows.at(index);
        if (row.Command == MAV_CMD_NAV_RALLY_POINT && !isZero(row.P4)) {
            return QObject::tr(
                    "Legacy .ral cannot preserve P4 of rally point %1.")
                    .arg(index + 1);
        }
    }
    return {};
}
}

FenceRallyController::FenceRallyController(QObject *parent)
    : QObject(parent)
{
}

FenceRallyController::FenceRallyController(FlightPlannerViewModel *planner,
                                           QObject *parent)
    : QObject(parent)
{
    setPlanner(planner);
}

FenceRallyController::~FenceRallyController()
{
    if (m_plannerDestroyedConnection) {
        disconnect(m_plannerDestroyedConnection);
    }
}

FlightPlannerViewModel *FenceRallyController::Planner() const
{
    return m_planner.data();
}

QString FenceRallyController::LastError() const
{
    return m_lastError;
}

void FenceRallyController::setPlanner(FlightPlannerViewModel *planner)
{
    if (m_planner.data() == planner) {
        return;
    }
    if (m_plannerDestroyedConnection) {
        disconnect(m_plannerDestroyedConnection);
        m_plannerDestroyedConnection = {};
    }
    m_planner = planner;
    if (planner) {
        m_plannerDestroyedConnection = connect(
                planner, &QObject::destroyed, this, [this]() {
            m_planner = nullptr;
            m_plannerDestroyedConnection = {};
            emit plannerChanged(nullptr);
        });
    }
    clearError();
    emit plannerChanged(planner);
}

Fence::DecodeResult FenceRallyController::CurrentFence() const
{
    if (!m_planner) {
        Fence::DecodeResult result;
        result.error = tr("Fence is unavailable: no flight planner is bound.");
        return result;
    }
    if (m_planner->thread() != QThread::currentThread()) {
        Fence::DecodeResult result;
        result.error = tr("Fence is unavailable: the flight planner belongs to another thread.");
        return result;
    }
    return Fence::LocationToFence(m_planner->Waypoints()->rows(
            FlightPlannerMissionModel::MissionStore::Fence));
}

RallyPoints::DecodeResult FenceRallyController::CurrentRally() const
{
    if (!m_planner) {
        RallyPoints::DecodeResult result;
        result.error = tr("Rally points are unavailable: no flight planner is bound.");
        return result;
    }
    if (m_planner->thread() != QThread::currentThread()) {
        RallyPoints::DecodeResult result;
        result.error = tr(
                "Rally points are unavailable: the flight planner belongs to another thread.");
        return result;
    }
    return RallyPoints::LocationToRally(m_planner->Waypoints()->rows(
            FlightPlannerMissionModel::MissionStore::Rally));
}

bool FenceRallyController::SetFence(const Fence &fence, bool append)
{
    if (!requireIdlePlanner(tr("Update fence"))) {
        return false;
    }
    QString error;
    if (!applyFence(fence, append, &error)) {
        return fail(tr("Fence update failed: %1").arg(error));
    }
    setSuccessStatus(append ? tr("Appended local fence geometry.")
                            : tr("Updated local fence."));
    return true;
}

bool FenceRallyController::SetRally(const RallyPoints &rally, bool append)
{
    if (!requireIdlePlanner(tr("Update rally points"))) {
        return false;
    }
    QString error;
    if (!applyRally(rally, append, &error)) {
        return fail(tr("Rally update failed: %1").arg(error));
    }
    setSuccessStatus(append ? tr("Appended local rally points.")
                            : tr("Updated local rally points."));
    return true;
}

bool FenceRallyController::LoadLegacyFence(const QString &path, bool append)
{
    if (!requireIdlePlanner(tr("Load fence"))) {
        return false;
    }
    const FenceRallyFileCodec::FenceLoadResult loaded =
            FenceRallyFileCodec::LoadLegacyFence(path);
    if (!loaded.ok) {
        return fail(tr("Fence load failed: %1").arg(loaded.error));
    }
    QString error;
    if (!applyFence(loaded.fence, append, &error)) {
        return fail(tr("Fence load failed: %1").arg(error));
    }
    setSuccessStatus(tr("%1 legacy fence from %2.")
            .arg(append ? tr("Appended") : tr("Loaded"),
                 QFileInfo(path).fileName()));
    return true;
}

bool FenceRallyController::SaveLegacyFence(const QString &path)
{
    if (!m_planner) {
        return fail(tr("Fence save failed: no flight planner is bound."));
    }
    if (m_planner->thread() != QThread::currentThread()) {
        return fail(tr("Fence save failed: the flight planner belongs to another thread."));
    }
    const QString storeLossError = legacyFenceStoreLossError(
            m_planner->Waypoints()->rows(
                    FlightPlannerMissionModel::MissionStore::Fence));
    if (!storeLossError.isEmpty()) {
        return fail(tr("Fence save failed: %1").arg(storeLossError));
    }
    const Fence::DecodeResult snapshot = CurrentFence();
    if (!snapshot.ok) {
        return fail(tr("Fence save failed: %1").arg(snapshot.error));
    }
    const QString lossError = legacyFenceLossError(snapshot.fence);
    if (!lossError.isEmpty()) {
        return fail(tr("Fence save failed: %1").arg(lossError));
    }
    const FenceRallyFileCodec::SaveResult saved =
            FenceRallyFileCodec::SaveLegacyFence(path, snapshot.fence);
    if (!saved.ok) {
        return fail(tr("Fence save failed: %1").arg(saved.error));
    }
    setSuccessStatus(tr("Saved legacy fence to %1.")
                     .arg(QFileInfo(path).fileName()));
    return true;
}

bool FenceRallyController::LoadLegacyRally(const QString &path, bool append)
{
    if (!requireIdlePlanner(tr("Load rally points"))) {
        return false;
    }
    const FenceRallyFileCodec::RallyLoadResult loaded =
            FenceRallyFileCodec::LoadLegacyRally(path);
    if (!loaded.ok) {
        return fail(tr("Rally load failed: %1").arg(loaded.error));
    }
    QString error;
    if (!applyRally(loaded.rally, append, &error)) {
        return fail(tr("Rally load failed: %1").arg(error));
    }
    setSuccessStatus(tr("%1 %2 rally point(s) from %3.")
            .arg(append ? tr("Appended") : tr("Loaded"))
            .arg(loaded.rally.Points.size())
            .arg(QFileInfo(path).fileName()));
    return true;
}

bool FenceRallyController::SaveLegacyRally(const QString &path)
{
    if (!m_planner) {
        return fail(tr("Rally save failed: no flight planner is bound."));
    }
    if (m_planner->thread() != QThread::currentThread()) {
        return fail(tr("Rally save failed: the flight planner belongs to another thread."));
    }
    const QString storeLossError = legacyRallyStoreLossError(
            m_planner->Waypoints()->rows(
                    FlightPlannerMissionModel::MissionStore::Rally));
    if (!storeLossError.isEmpty()) {
        return fail(tr("Rally save failed: %1").arg(storeLossError));
    }
    const RallyPoints::DecodeResult snapshot = CurrentRally();
    if (!snapshot.ok) {
        return fail(tr("Rally save failed: %1").arg(snapshot.error));
    }
    const QString lossError = legacyRallyLossError(snapshot.rally);
    if (!lossError.isEmpty()) {
        return fail(tr("Rally save failed: %1").arg(lossError));
    }
    const FenceRallyFileCodec::SaveResult saved =
            FenceRallyFileCodec::SaveLegacyRally(path, snapshot.rally);
    if (!saved.ok) {
        return fail(tr("Rally save failed: %1").arg(saved.error));
    }
    setSuccessStatus(tr("Saved %1 rally point(s) to %2.")
            .arg(snapshot.rally.Points.size())
            .arg(QFileInfo(path).fileName()));
    return true;
}

bool FenceRallyController::DownloadFence()
{
    return startTransfer(FlightPlannerMissionModel::MissionStore::Fence,
                         false);
}

bool FenceRallyController::UploadFence()
{
    return startTransfer(FlightPlannerMissionModel::MissionStore::Fence,
                         true);
}

bool FenceRallyController::DownloadRally()
{
    return startTransfer(FlightPlannerMissionModel::MissionStore::Rally,
                         false);
}

bool FenceRallyController::UploadRally()
{
    return startTransfer(FlightPlannerMissionModel::MissionStore::Rally,
                         true);
}

bool FenceRallyController::applyFence(const Fence &fence, bool append,
                                      QString *error)
{
    if (error) {
        error->clear();
    }
    const ModelValidationResult incomingValidation = fence.validate();
    if (!incomingValidation.ok) {
        if (error) {
            *error = incomingValidation.error;
        }
        return false;
    }

    Fence merged = fence;
    if (append) {
        const Fence::DecodeResult current = CurrentFence();
        if (!current.ok) {
            if (error) {
                *error = tr("Existing fence is invalid: %1")
                        .arg(current.error);
            }
            return false;
        }
        merged = current.fence;
        if (!merged.HasReturn && fence.HasReturn) {
            merged.HasReturn = true;
            merged.ReturnPoint = fence.ReturnPoint;
        }
        merged.Polygons += fence.Polygons;
        merged.Circles += fence.Circles;
    }

    QString conversionError;
    const QVector<WpRowData> rows = merged.FenceToLocation(&conversionError);
    if (!conversionError.isEmpty()) {
        if (error) {
            *error = conversionError;
        }
        return false;
    }
    m_planner->setMissionType(QStringLiteral("Fence"));
    m_planner->Waypoints()->replaceStore(
            FlightPlannerMissionModel::MissionStore::Fence, rows);
    return true;
}

bool FenceRallyController::applyRally(const RallyPoints &rally, bool append,
                                      QString *error)
{
    if (error) {
        error->clear();
    }
    const ModelValidationResult incomingValidation = rally.validate();
    if (!incomingValidation.ok) {
        if (error) {
            *error = incomingValidation.error;
        }
        return false;
    }

    RallyPoints merged = rally;
    if (append) {
        const RallyPoints::DecodeResult current = CurrentRally();
        if (!current.ok) {
            if (error) {
                *error = tr("Existing rally points are invalid: %1")
                        .arg(current.error);
            }
            return false;
        }
        merged = current.rally;
        merged.Points += rally.Points;
    }

    QString conversionError;
    const QVector<WpRowData> rows = merged.RallyToLocation(&conversionError);
    if (!conversionError.isEmpty()) {
        if (error) {
            *error = conversionError;
        }
        return false;
    }
    m_planner->setMissionType(QStringLiteral("Rally"));
    m_planner->Waypoints()->replaceStore(
            FlightPlannerMissionModel::MissionStore::Rally, rows);
    return true;
}

bool FenceRallyController::startTransfer(
        FlightPlannerMissionModel::MissionStore store, bool upload)
{
    if (!m_planner) {
        return fail(tr("%1 failed: no flight planner is bound.")
                    .arg(upload ? tr("Upload") : tr("Download")));
    }
    if (m_planner->thread() != QThread::currentThread()) {
        return fail(tr("%1 failed: the flight planner belongs to another thread.")
                    .arg(upload ? tr("Upload") : tr("Download")));
    }
    const bool available = upload ? m_planner->CanWriteWaypoints()
                                  : m_planner->CanReadWaypoints();
    if (!available) {
        return fail(tr("%1 failed: no vehicle is available or another mission transfer is active.")
                    .arg(upload ? tr("Upload") : tr("Download")));
    }

    m_planner->setMissionType(FlightPlannerMissionModel::storeName(store));
    clearError();
    if (upload) {
        m_planner->WriteWaypoints();
    } else {
        m_planner->ReadWaypoints();
    }
    // The transfer may complete synchronously with a loopback/test transport;
    // true means that the request passed the facade's availability checks and
    // was handed to FlightPlannerViewModel, not that the vehicle accepted it.
    return true;
}

bool FenceRallyController::requireIdlePlanner(const QString &operation)
{
    if (!m_planner) {
        return fail(tr("%1 failed: no flight planner is bound.")
                    .arg(operation));
    }
    if (m_planner->thread() != QThread::currentThread()) {
        return fail(tr("%1 failed: the flight planner belongs to another thread.")
                    .arg(operation));
    }
    if (m_planner->TransferBusy()) {
        return fail(tr("%1 failed: a mission transfer is active.")
                    .arg(operation));
    }
    return true;
}

bool FenceRallyController::fail(const QString &error)
{
    if (m_lastError != error) {
        m_lastError = error;
        emit lastErrorChanged(m_lastError);
    }
    if (m_planner) {
        m_planner->setStatus(error);
    }
    emit errorOccurred(error);
    return false;
}

void FenceRallyController::clearError()
{
    if (m_lastError.isEmpty()) {
        return;
    }
    m_lastError.clear();
    emit lastErrorChanged(m_lastError);
}

void FenceRallyController::setSuccessStatus(const QString &status)
{
    clearError();
    if (m_planner) {
        m_planner->setStatus(status);
    }
}

} // namespace MissionPlanner
