#include "GuidedNavigationController.h"

#include "GuidedAltitudeDialog.h"
#include "map/AbstractMapWidget.h"

#include <QEvent>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QSettings>
#include <QWidget>
#include <cmath>

namespace {
QString frameName(MAV_FRAME frame)
{
    switch (frame) {
    case MAV_FRAME_GLOBAL: return QObject::tr("Absolute (AMSL)");
    case MAV_FRAME_GLOBAL_TERRAIN_ALT: return QObject::tr("Terrain (above terrain)");
    default: return QObject::tr("Relative (above home)");
    }
}

bool parseNumber(const QString &text, double *value)
{
    bool ok = false;
    *value = QLocale::c().toDouble(text.trimmed(), &ok);
    if (!ok) *value = QLocale().toDouble(text.trimmed(), &ok);
    return ok && std::isfinite(*value);
}

bool isCopter(int vehicleType)
{
    switch (vehicleType) {
    case MAV_TYPE_QUADROTOR: case MAV_TYPE_COAXIAL: case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR: case MAV_TYPE_OCTOROTOR: case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_DODECAROTOR: return true;
    default: return false;
    }
}
}

GuidedNavigationController::GuidedNavigationController(
    Dependencies dependencies, QWidget *owner)
    : QObject(owner), m_owner(owner), m_altitudes(dependencies.altitudes),
      m_navigation(dependencies.navigation), m_telemetry(dependencies.telemetry),
      m_settings(dependencies.settings), m_isGuided(std::move(dependencies.isGuided))
{
    setObjectName(QStringLiteral("GuidedNavigationController"));
    if (!m_settings) m_settings = new QSettings(this);
    if (m_owner) m_owner->installEventFilter(this);
    if (m_navigation) {
        m_status = m_navigation->status();
        connect(m_navigation, &GuidedNavigationService::stateChanged, this, [this]() {
            if (m_navigation && m_operation
                && m_navigation->currentOperationId() == m_operation)
                setStatus(m_navigation->status());
        });
        connect(m_navigation, &GuidedNavigationService::operationFinished, this,
                [this](const GuidedNavigationService::Report &report) {
            if (m_operation != 0 && report.operationId == m_operation) {
                m_operation = 0;
                finish(report.description);
            }
        });
        connect(m_navigation, &QObject::destroyed, this, [this]() {
            m_operation = 0;
            cancel();
        });
    }
}

GuidedNavigationController::~GuidedNavigationController()
{
    m_destroying = true;
    if (m_owner) m_owner->removeEventFilter(this);
    if (m_navigation) disconnect(m_navigation, nullptr, this, nullptr);
    cancel();
}

bool GuidedNavigationController::busy() const
{
    return m_active || (m_navigation && m_navigation->busy());
}

void GuidedNavigationController::attachMap(AbstractMapWidget *map)
{
    if (!map) return;
    connect(map, &AbstractMapWidget::GuidedAltitudeEditRequested,
            this, &GuidedNavigationController::editAltitude, Qt::UniqueConnection);
    connect(map, &AbstractMapWidget::GuidedTargetRequested,
            this, &GuidedNavigationController::flyToHere, Qt::UniqueConnection);
    connect(map, &AbstractMapWidget::GuidedCoordinatesRequested,
            this, &GuidedNavigationController::flyToCoordinates, Qt::UniqueConnection);
    map->SetGuidedNavigationEnabled(true);
}

double GuidedNavigationController::multiplier() const
{
    return m_settings && m_settings->value(QStringLiteral("altunits"),
        QStringLiteral("Meters")).toString().compare(QStringLiteral("Feet"),
        Qt::CaseInsensitive) == 0 ? 3.280839895013123 : 1.0;
}

MAV_FRAME GuidedNavigationController::savedFrame() const
{
    bool ok = false;
    const int raw = m_settings ? m_settings->value(
        QStringLiteral("guided_alt_frame"), int(MAV_FRAME_GLOBAL_RELATIVE_ALT)).toInt(&ok) : 3;
    const auto frame = static_cast<MAV_FRAME>(raw);
    return ok && GuidedAltitudeDialog::isSupportedFrame(frame)
        ? frame : MAV_FRAME_GLOBAL_RELATIVE_ALT;
}

QString GuidedNavigationController::describe(const Context &context) const
{
    return tr("Link %1 · system %2 / component %3 · instance %4")
        .arg(context.target.endpoint.linkId).arg(context.target.endpoint.systemId)
        .arg(context.target.endpoint.componentId).arg(context.vehicle.instanceEpoch);
}

bool GuidedNavigationController::begin(Context *context)
{
    if (m_destroying || busy()) {
        if (!m_destroying) setStatus(tr("Guided navigation is busy; finish or cancel the current operation."));
        return false;
    }
    const QPointer<GuidedNavigationController> guard(this);
    const quint64 flow = ++m_flow;
    m_active = true;
    QString error;
    if (!m_owner || !m_navigation || !m_altitudes
        || !m_altitudes->prepareCurrent(context, &error)) {
        if (guard && flow == m_flow)
            finish(error.isEmpty() ? tr("No current vehicle connection.") : error);
        return false;
    }
    if (!guard || flow != m_flow || !m_active) return false;
    return true;
}

void GuidedNavigationController::editAltitude()
{
    Context context;
    if (begin(&context)) openAltitude(context);
}

void GuidedNavigationController::openAltitude(
    const Context &context, AltitudeContinuation continuation)
{
    if (!m_active || !m_owner || !m_altitudes) return;
    const quint64 flow = m_flow;
    const QPointer<GuidedNavigationController> guard(this);
    QString error;
    if (!m_altitudes->validate(context, &error)) {
        if (guard && flow == m_flow) finish(tr("Guided altitude editor cancelled: %1").arg(error));
        return;
    }
    if (!guard || !m_active || flow != m_flow) return;
    const double scale = multiplier();
    SwarmTelemetrySnapshot snapshot;
    double initialM = m_telemetry
        && m_telemetry->snapshotForLease(context.vehicle, &snapshot)
        && isCopter(snapshot.vehicleType) ? 10.0 : 100.0;
    double saved = 0;
    if (m_settings && parseNumber(m_settings->value(QStringLiteral("guided_alt")).toString(), &saved))
        initialM = saved / scale;
    auto *dialog = new GuidedAltitudeDialog(initialM, savedFrame(), scale,
        scale == 1.0 ? tr("m") : tr("ft"), describe(context), m_owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    const QPointer<GuidedAltitudeDialog> prompt(dialog);
    connect(dialog, &QDialog::finished, this,
        [this, prompt, context, continuation, flow, scale](int result) {
        if (flow != m_flow || !m_active || m_destroying) return;
        m_prompt.clear();
        if (result != QDialog::Accepted || !prompt || !prompt->hasAcceptedValue()) {
            finish(tr("Guided altitude unchanged; no command sent."));
            return;
        }
        const double metres = prompt->altitudeMetres();
        const MAV_FRAME frame = prompt->frame();
        Context updated;
        QString error;
        const QPointer<GuidedNavigationController> guard(this);
        if (!m_altitudes || !m_altitudes->commitAltitude(context, metres, frame, &updated, &error)) {
            if (guard) finish(tr("Guided altitude was not changed: %1").arg(error));
            return;
        }
        if (!guard || flow != m_flow || !m_active) return;
        if (m_settings) {
            m_settings->setValue(QStringLiteral("guided_alt"),
                QString::number(metres * scale, 'g', 17));
            m_settings->setValue(QStringLiteral("guided_alt_frame"), int(frame));
            m_settings->sync();
            if (m_settings->status() != QSettings::NoError) {
                finish(tr("Guided altitude changed for this session, but saving the default failed. No command sent."));
                return;
            }
        }
        if (continuation) {
            continuation(updated);
            return;
        }
        const auto isGuided = m_isGuided;
        const bool guided = isGuided && isGuided(updated);
        if (!guard || flow != m_flow || !m_active) return;
        if (guided && updated.pointSet) {
            confirm(updated, updated.latitude, updated.longitude, updated.altitudeM,
                updated.frame, true, GuidedNavigationService::Purpose::AltitudeUpdate);
        } else {
            finish(tr("Guided altitude set to %1 m — %2. No command sent.")
                .arg(updated.altitudeM, 0, 'g', 8).arg(frameName(updated.frame)));
        }
    });
    dialog->open();
}

void GuidedNavigationController::flyToHere(double latitude, double longitude)
{
    Context context;
    if (!begin(&context)) return;
    if (!Terrain3DCore::GeoPoint{latitude, longitude, 0}.isValid()) {
        finish(tr("Invalid coordinates; no command sent."));
        return;
    }
    const auto proceed = [this, latitude, longitude](const Context &current) {
        confirm(current, latitude, longitude, current.altitudeM, current.frame,
                true, GuidedNavigationService::Purpose::FlyToHere);
    };
    if (!context.altitudeSet || context.altitudeM == 0) openAltitude(context, proceed);
    else proceed(context);
}

void GuidedNavigationController::flyToCoordinates()
{
    Context context;
    if (!begin(&context)) return;
    const quint64 flow = m_flow;
    const double scale = multiplier();
    const MAV_FRAME fallbackFrame = savedFrame();
    auto *dialog = new QInputDialog(m_owner);
    dialog->setObjectName(QStringLiteral("GuidedCoordinatesDialog"));
    dialog->setWindowTitle(tr("Fly To Coords"));
    dialog->setLabelText(tr("Enter lat;lng;alt or lat;lng\nAltitude in %1\n%2")
        .arg(scale == 1.0 ? tr("m") : tr("ft"), describe(context)));
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    const QPointer<QInputDialog> prompt(dialog);
    connect(dialog, &QDialog::finished, this,
        [this, prompt, context, flow, scale, fallbackFrame](int result) {
        if (flow != m_flow || !m_active || m_destroying) return;
        m_prompt.clear();
        if (result != QDialog::Accepted || !prompt) {
            finish(tr("Fly To Coords cancelled; no command sent."));
            return;
        }
        const QStringList parts = prompt->textValue().split(QLatin1Char(';'));
        double lat = 0, lon = 0, alt = 0;
        if ((parts.size() != 2 && parts.size() != 3)
            || !parseNumber(parts.at(0), &lat) || !parseNumber(parts.at(1), &lon)
            || (parts.size() == 3 && !parseNumber(parts.at(2), &alt))
            || !Terrain3DCore::GeoPoint{lat, lon, 0}.isValid()) {
            finish(tr("Invalid coordinates; use lat;lng;alt or lat;lng. No command sent."));
            return;
        }
        if (parts.size() == 3) {
            confirm(context, lat, lon, alt / scale,
                context.altitudeSet || context.pointSet ? context.frame : fallbackFrame,
                true, GuidedNavigationService::Purpose::Coordinates);
            return;
        }
        const auto proceed = [this, lat, lon](const Context &current) {
            confirm(current, lat, lon, current.altitudeM, current.frame,
                    true, GuidedNavigationService::Purpose::Coordinates);
        };
        if (!context.altitudeSet || context.altitudeM == 0) openAltitude(context, proceed);
        else proceed(context);
    });
    dialog->open();
}

void GuidedNavigationController::terrainClick(
    const Terrain3DCore::GeoPoint &point, const Terrain3DCore::Snapshot &rendered)
{
    Context context;
    if (!begin(&context)) return;
    const auto &endpoint = context.target.endpoint;
    if (!point.isValid() || rendered.linkId != endpoint.linkId
        || rendered.systemId != endpoint.systemId || rendered.componentId != endpoint.componentId
        || rendered.targetGeneration != context.target.generation
        || rendered.linkSessionEpoch != context.vehicle.linkSessionEpoch
        || rendered.vehicleInstanceEpoch != context.vehicle.instanceEpoch) {
        finish(tr("Guided target not sent: the rendered terrain belongs to an old vehicle instance. Reload terrain."));
        return;
    }
    const QPointer<GuidedNavigationController> guard(this);
    const quint64 flow = m_flow;
    const bool freshFrame = m_telemetry
        && m_telemetry->observationIsFresh(rendered.capturedMonotonicMs, 3000);
    if (!guard || flow != m_flow || !m_active) return;
    if (!freshFrame) {
        finish(tr("Guided target not sent: the rendered vehicle position is stale. Wait for a fresh terrain frame."));
        return;
    }
    if (!context.altitudeSet || std::abs(context.altitudeM) < 0.01F) {
        finish(tr("Guided target not sent: set a non-zero guided altitude in Flight Data or Guided altitude first."));
        return;
    }
    // MP10 Locationwp.Set forces frame 3; the confirmation makes this
    // reinterpretation explicit when the saved DATA frame is different.
    confirm(context, point.latitude, point.longitude, context.altitudeM,
            MAV_FRAME_GLOBAL_RELATIVE_ALT, false,
            GuidedNavigationService::Purpose::TerrainClick);
}

void GuidedNavigationController::confirm(
    const Context &context, double latitude, double longitude, double altitudeM,
    MAV_FRAME frame, bool changeMode, GuidedNavigationService::Purpose purpose)
{
    const QPointer<GuidedNavigationController> guard(this);
    const quint64 flow = m_flow;
    const QPointer<GuidedNavigationService> navigation(m_navigation);
    GuidedNavigationService::Plan plan;
    QString error;
    if (!m_navigation || !m_navigation->prepare(context, latitude, longitude,
            altitudeM, frame, changeMode, purpose, &plan, &error)) {
        if (guard) finish(tr("Guided target not sent: %1").arg(error));
        return;
    }
    if (!guard || !m_active || flow != m_flow || !m_owner) {
        if (navigation) navigation->discardPlan(plan);
        return;
    }
    m_plan = plan;
    QString text = tr("%1\nTarget: %2, %3\nAltitude: %4 m — %5\n%6\n\nThis command can move the vehicle. Send?")
        .arg(describe(context)).arg(latitude, 0, 'f', 7).arg(longitude, 0, 'f', 7)
        .arg(altitudeM, 0, 'g', 9).arg(frameName(frame))
        .arg(changeMode ? tr("Requests Guided mode.") : tr("Does NOT change flight mode. Vehicle must already support this target in its current mode."));
    if (purpose == GuidedNavigationService::Purpose::TerrainClick && context.frame != frame)
        text += tr("\n\nMission Planner 10 Terrain uses the saved numeric altitude as RELATIVE above home, even though DATA is set to %1. Terrain elevation is inspection only.")
            .arg(frameName(context.frame));
    auto *dialog = new QMessageBox(QMessageBox::Warning, tr("Guided target"), text,
        QMessageBox::Yes | QMessageBox::Cancel, m_owner);
    dialog->setObjectName(QStringLiteral("GuidedNavigationConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    if (auto *yes = qobject_cast<QPushButton *>(dialog->button(QMessageBox::Yes))) yes->setAutoDefault(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this, [this, plan, flow](int result) {
        if (flow != m_flow || !m_active || m_destroying) return;
        m_prompt.clear();
        if (result != QMessageBox::Yes) {
            finish(tr("Guided target cancelled; no command sent."));
            return;
        }
        const QPointer<GuidedNavigationController> guard(this);
        QString error;
        auto *progress = new QProgressDialog(tr("Submitting guided target…"),
            tr("Cancel retries"), 0, 0, m_owner);
        progress->setObjectName(QStringLiteral("GuidedNavigationProgressDialog"));
        progress->setWindowTitle(tr("Guided target"));
        progress->setWindowModality(Qt::NonModal);
        progress->setMinimumDuration(0);
        progress->setAutoClose(false);
        progress->setAutoReset(false);
        disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));
        m_progress = progress;
        connect(progress, &QProgressDialog::canceled, this, &GuidedNavigationController::cancel);
        connect(progress, &QDialog::rejected, this, &GuidedNavigationController::cancel);
        progress->show();
        if (!guard || flow != m_flow || !m_active) return;
        m_operation = 0;
        if (!m_navigation || m_navigation->execute(this, plan, &m_operation, &error)
            != GuidedNavigationService::SubmitResult::Started) {
            if (guard && flow == m_flow) finish(tr("Guided target not started: %1").arg(error));
            return;
        }
        // A synchronous writer ACK may already have finished the entire flow.
        if (guard && flow == m_flow && m_active && m_operation != 0)
            setStatus(tr("Guided target submitted; waiting for vehicle acknowledgement. Closing cancels retries, not an already applied movement."));
    });
    dialog->open();
}

void GuidedNavigationController::setStatus(const QString &text)
{
    m_status = text;
    const QPointer<GuidedNavigationController> guard(this);
    if (m_progress) m_progress->setLabelText(text);
    if (!guard) return;
    if (!m_destroying) emit statusChanged(text);
}

void GuidedNavigationController::finish(const QString &text)
{
    m_active = false;
    ++m_flow;
    const QPointer<GuidedNavigationController> guard(this);
    const auto plan = m_plan;
    m_plan = {};
    if (m_navigation && plan.isValid()) m_navigation->discardPlan(plan);
    if (!guard) return;
    dismissProgress();
    if (guard) setStatus(text);
}

void GuidedNavigationController::dismissProgress()
{
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        disconnect(progress, nullptr, this, nullptr);
        progress->hide();
        if (progress) progress->deleteLater();
    }
}

void GuidedNavigationController::dismissPrompt()
{
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        disconnect(prompt, nullptr, this, nullptr);
        prompt->reject();
        if (prompt) prompt->deleteLater();
    }
}

void GuidedNavigationController::cancel()
{
    ++m_flow;
    m_active = false;
    const quint64 operation = m_operation;
    m_operation = 0;
    const QPointer<GuidedNavigationController> guard(this);
    const auto plan = m_plan;
    m_plan = {};
    if (m_navigation && plan.isValid()) m_navigation->discardPlan(plan);
    if (!guard) return;
    dismissPrompt();
    if (!guard) return;
    dismissProgress();
    if (!guard) return;
    if (m_navigation && operation) m_navigation->cancel(operation);
    if (guard && !m_destroying)
        setStatus(operation ? tr("Cancellation requested. An already submitted target may still be applied; the shared command channel drains before reuse.")
                            : tr("Guided navigation cancelled; no new command sent."));
}

bool GuidedNavigationController::eventFilter(QObject *watched, QEvent *event)
{
    const QPointer<QObject> watchedGuard(watched);
    const QPointer<GuidedNavigationController> guard(this);
    if (watched == m_owner && event->type() == QEvent::Close) {
        cancel();
        if (!guard || !watchedGuard) return true;
    }
    return false;
}
