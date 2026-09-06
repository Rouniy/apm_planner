#include "JoystickControlService.h"

#include "comm/ExactLinkTransmitter.h"
#include "comm/SwarmFlightMode.h"
#include "comm/VehicleTargetManager.h"

#include <QPointer>
#include <QSharedData>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int MaximumHeartbeatAgeMs = 5000;

void assignError(QString *error, const QString &message)
{
    if (error) *error = message;
}

QString exactTargetText(const VehicleEndpoint &endpoint)
{
    return QStringLiteral("%1 (link %2, system/component %3/%4)")
        .arg(endpoint.displayName())
        .arg(endpoint.linkId)
        .arg(endpoint.systemId)
        .arg(endpoint.componentId);
}

bool sameRange(const JoystickConfiguration::Range &left,
               const JoystickConfiguration::Range &right)
{
    return left.minimum == right.minimum && left.maximum == right.maximum;
}

bool sameProfile(const JoystickConfiguration::Profile &left,
                 const JoystickConfiguration::Profile &right)
{
    if (left.deviceId != right.deviceId
        || left.deviceName != right.deviceName
        || left.firmware != right.firmware
        || left.elevons != right.elevons
        || left.manualControl != right.manualControl
        || left.channels.size() != right.channels.size()
        || left.buttons.size() != right.buttons.size()
        || left.calibration.size() != right.calibration.size()) {
        return false;
    }
    for (int i = 0; i < left.channels.size(); ++i) {
        const auto &a = left.channels.at(i);
        const auto &b = right.channels.at(i);
        if (a.channel != b.channel || a.axis != b.axis
            || a.reverse != b.reverse || a.expo != b.expo) {
            return false;
        }
    }
    for (int i = 0; i < left.buttons.size(); ++i) {
        const auto &a = left.buttons.at(i);
        const auto &b = right.buttons.at(i);
        if (a.buttonno != b.buttonno || a.function != b.function
            || a.mode != b.mode || a.p1 != b.p1 || a.p2 != b.p2
            || a.p3 != b.p3 || a.p4 != b.p4 || a.state != b.state) {
            return false;
        }
    }
    for (auto device = left.calibration.constBegin();
         device != left.calibration.constEnd(); ++device) {
        const auto otherDevice = right.calibration.constFind(device.key());
        if (otherDevice == right.calibration.constEnd()
            || device->size() != otherDevice->size()) {
            return false;
        }
        for (auto axis = device->constBegin(); axis != device->constEnd(); ++axis) {
            const auto otherAxis = otherDevice->constFind(axis.key());
            if (otherAxis == otherDevice->constEnd()
                || !sameRange(axis.value(), otherAxis.value())) {
                return false;
            }
        }
    }
    return true;
}

bool sameRanges(const JoystickConfiguration::Ranges &left,
                const JoystickConfiguration::Ranges &right)
{
    if (left.size() != right.size()) return false;
    for (auto item = left.constBegin(); item != left.constEnd(); ++item) {
        const auto other = right.constFind(item.key());
        if (other == right.constEnd()
            || !sameRange(item.value(), other.value())) return false;
    }
    return true;
}

bool customModeFor(const SwarmTelemetrySnapshot &snapshot,
                   const QString &name, quint32 *customMode)
{
    if (!customMode) return false;
    if (snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA) return false;
    QString key = name.trimmed().toLower();
    key.remove(QLatin1Char(' '));
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    const auto match = [&key, customMode](const char *text, quint32 value) {
        if (key == QLatin1String(text)) {
            *customMode = value;
            return true;
        }
        return false;
    };
    if (SwarmFlightMode::isCopter(snapshot.vehicleType)) {
        return match("stabilize", 0) || match("acro", 1)
            || match("althold", 2) || match("auto", 3)
            || match("guided", 4) || match("loiter", 5)
            || match("rtl", 6) || match("circle", 7)
            || match("land", 9) || match("drift", 11)
            || match("sport", 13) || match("flip", 14)
            || match("autotune", 15) || match("poshold", 16)
            || match("brake", 17) || match("throw", 18)
            || match("avoidadsb", 19) || match("guidednogps", 20)
            || match("smartrtl", 21) || match("flowhold", 22)
            || match("follow", 23) || match("zigzag", 24)
            || match("systemid", 25) || match("heliautorotate", 26)
            || match("autorotate", 26) || match("autortl", 27)
            || match("turtle", 28) || match("rateacro", 29)
            || match("modelcal", 31);
    }
    if (SwarmFlightMode::isPlane(snapshot.vehicleType)) {
        return match("manual", 0) || match("circle", 1)
            || match("stabilize", 2) || match("training", 3)
            || match("acro", 4) || match("flybywirea", 5)
            || match("fbwa", 5) || match("flybywireb", 6)
            || match("fbwb", 6) || match("cruise", 7)
            || match("autotune", 8) || match("land", 9)
            || match("auto", 10)
            || match("rtl", 11) || match("loiter", 12)
            || match("takeoff", 13) || match("avoidadsb", 14)
            || match("guided", 15) || match("initializing", 16)
            || match("initialising", 16) || match("qstabilize", 17)
            || match("qhover", 18) || match("qloiter", 19)
            || match("qland", 20) || match("qrtl", 21)
            || match("qautotune", 22) || match("qacro", 23)
            || match("thermal", 24) || match("loiteraltqland", 25);
    }
    if (SwarmFlightMode::isRover(snapshot.vehicleType)) {
        return match("manual", 0) || match("acro", 1)
            || match("learning", 2) || match("steering", 3)
            || match("hold", 4)
            || match("loiter", 5) || match("follow", 6)
            || match("simple", 7) || match("dock", 8)
            || match("circle", 9) || match("auto", 10)
            || match("rtl", 11) || match("smartrtl", 12)
            || match("guided", 15) || match("initializing", 16)
            || match("initialising", 16);
    }
    if (snapshot.vehicleType == MAV_TYPE_ANTENNA_TRACKER)
        return match("manual", 0) || match("stop", 1)
            || match("scan", 2) || match("servotest", 3)
            || match("auto", 10) || match("initializing", 16)
            || match("initialising", 16);
    return false;
}

quint16 rcValue(int channel, int value, bool mapped)
{
    if (!mapped) return channel <= 8 ? std::numeric_limits<quint16>::max() : 0;
    if (value == -1) return std::numeric_limits<quint16>::max();
    return static_cast<quint16>(qBound(0, value, 65535));
}
} // namespace

class JoystickControlService::EnablePlan::Data final : public QSharedData
{
public:
    const JoystickControlService *service = nullptr;
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    Profile profile;
    qint32 deviceInstanceId = -1;
    quint64 deviceGeneration = 0;
    QVector<ChannelLimits> limits;
    QVector<bool> ownedChannels;
    QString description;
};

JoystickControlService::EnablePlan::EnablePlan() = default;
JoystickControlService::EnablePlan::EnablePlan(const EnablePlan &) = default;
JoystickControlService::EnablePlan &JoystickControlService::EnablePlan::operator=(
    const EnablePlan &) = default;
JoystickControlService::EnablePlan::~EnablePlan() = default;

bool JoystickControlService::EnablePlan::isValid() const noexcept
{
    return d && d->service && d->target.isValid() && d->vehicle.isValid()
        && d->deviceInstanceId >= 0 && d->deviceGeneration != 0;
}

VehicleTargetLease JoystickControlService::EnablePlan::target() const
{
    return d ? d->target : VehicleTargetLease();
}

SwarmVehicleInstanceLease JoystickControlService::EnablePlan::vehicle() const
{
    return d ? d->vehicle : SwarmVehicleInstanceLease();
}

JoystickControlService::Profile
JoystickControlService::EnablePlan::profile() const
{
    return d ? d->profile : Profile();
}

qint32 JoystickControlService::EnablePlan::deviceInstanceId() const noexcept
{
    return d ? d->deviceInstanceId : -1;
}

quint64 JoystickControlService::EnablePlan::deviceGeneration() const noexcept
{
    return d ? d->deviceGeneration : 0;
}

QString JoystickControlService::EnablePlan::description() const
{
    return d ? d->description : QString();
}

JoystickControlService::JoystickControlService(
    JoystickDevice *device, VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ExactLinkTransmitter *transmitter,
    VehicleCommandService *commandService, quint8 localSystemId,
    quint8 localComponentId, ChannelLimitsProvider channelLimitsProvider,
    RouteValidator routeValidator, QObject *parent)
    : QObject(parent)
    , m_device(device)
    , m_targetManager(targetManager)
    , m_telemetryRegistry(telemetryRegistry)
    , m_transmitter(transmitter)
    , m_commandService(commandService)
    , m_localSystemId(localSystemId)
    , m_localComponentId(localComponentId)
    , m_routeValidator(std::move(routeValidator))
    , m_channelLimitsProvider(std::move(channelLimitsProvider))
    , m_profile(JoystickConfiguration::defaults())
    , m_status(QStringLiteral("Joystick control is disabled."))
{
    qRegisterMetaType<EnablePlan>();
    qRegisterMetaType<Preview>();
    m_sendTimer.setSingleShot(false);
    m_sendTimer.setTimerType(Qt::PreciseTimer);
    m_sendTimer.setInterval(m_sendIntervalMs);
    connect(&m_sendTimer, &QTimer::timeout,
            this, &JoystickControlService::sendControlFrame);
    if (m_device) {
        connect(m_device, &JoystickDevice::snapshotChanged,
                this, &JoystickControlService::handleSnapshot);
        connect(m_device, &JoystickDevice::disconnected,
                this, &JoystickControlService::handleDisconnected);
        connect(m_device, &JoystickDevice::calibrationChanged,
                this, [this]() {
            if (m_enabled) {
                finishDisable(QStringLiteral(
                    "Joystick calibration changed; output stopped."), true);
            } else {
                m_preview = makePreview(currentDeviceSnapshot(), m_profile);
                emit previewChanged(m_preview);
            }
        });
        m_preview = makePreview(currentDeviceSnapshot(), m_profile);
    }
    if (m_targetManager) {
        connect(m_targetManager, &VehicleTargetManager::targetGenerationChanged,
                this, [this]() {
            if (m_enabled) {
                finishDisable(QStringLiteral(
                    "The selected vehicle changed; joystick output stopped."),
                    true);
            } else {
                emit stateChanged();
            }
        });
    }
    if (m_telemetryRegistry) {
        connect(m_telemetryRegistry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](const SwarmVehicleInstanceLease &lease,
                             SwarmTelemetryRegistry::RetirementReason) {
            if (m_enabled && m_activePlan.isValid()
                && m_activePlan.vehicle().sameInstance(lease)) {
                finishDisable(QStringLiteral(
                    "The joystick target was retired; output stopped."), true);
            }
        });
    }
    if (m_commandService) {
        connect(m_commandService, &VehicleCommandService::exactCommandFinished,
                this, &JoystickControlService::handleCommandFinished,
                Qt::QueuedConnection);
    }
}

JoystickControlService::~JoystickControlService()
{
    m_shuttingDown = true;
    m_sendTimer.stop();
    m_enabled = false;
}

VehicleTargetLease JoystickControlService::activeTarget() const
{
    return m_activePlan.target();
}

QString JoystickControlService::targetDescription() const
{
    if (m_activePlan.isValid()) return exactTargetText(m_activePlan.target().endpoint);
    if (!m_targetManager) return QStringLiteral("No eligible connected vehicle");
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    return target.isValid() ? exactTargetText(target.endpoint)
                            : QStringLiteral("No eligible connected vehicle");
}

bool JoystickControlService::setConfiguration(
    const Profile &submitted, QString *error)
{
    if (error) error->clear();
    if (isEnabled()) {
        assignError(error, QStringLiteral(
            "Disable joystick output before changing its configuration."));
        return false;
    }
    Profile profile = submitted;
    JoystickConfiguration::normalize(&profile);
    if (!JoystickConfiguration::validate(profile, error)) return false;
    QPointer<JoystickControlService> guard(this);
    if (!m_hasTestDeviceState && m_device && m_device->isOpen()
        && m_device->selectedDevice().id == profile.deviceId) {
        const auto wanted = profile.calibration.value(profile.deviceId);
        if (!m_device->setRanges(wanted, error) || !guard) return false;
    }
    m_profile = profile;
    m_customAxis0 = profile.manualControl ? 0 : 1500;
    m_customAxis1 = profile.manualControl ? 0 : 1500;
    m_preview = makePreview(currentDeviceSnapshot(), m_profile);
    emit previewChanged(m_preview);
    if (!guard) return true;
    emit stateChanged();
    return true;
}

bool JoystickControlService::prepareEnable(
    EnablePlan *planOut, QString *error) const
{
    if (planOut) *planOut = EnablePlan();
    if (error) error->clear();
    if (!planOut || m_shuttingDown || isEnabled() || !m_device
        || !m_targetManager || !m_telemetryRegistry || !m_transmitter
        || !m_routeValidator || !m_localSystemId || !m_localComponentId) {
        assignError(error, QStringLiteral(
            "Joystick output is active or its exact services are unavailable."));
        return false;
    }
    if (!JoystickConfiguration::validate(m_profile, error)) return false;
    const JoystickDevice::Snapshot input = currentDeviceSnapshot();
    const JoystickDevice::Info selected = currentDeviceInfo();
    if (!input.connected || (!m_hasTestDeviceState && m_device->isCalibrating())
        || input.generation == 0 || selected.instanceId < 0
        || selected.instanceId != input.instanceId
        || m_profile.deviceId.isEmpty()
        || m_profile.deviceId != selected.id) {
        assignError(error, QStringLiteral(
            "Select the configured connected joystick before enabling output."));
        return false;
    }
    if (!m_hasTestDeviceState
        && !sameRanges(m_device->ranges(),
                       m_profile.calibration.value(m_profile.deviceId))) {
        assignError(error, QStringLiteral(
            "The selected joystick calibration differs from this configuration."));
        return false;
    }
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || !m_targetManager->isTargetGenerationSettled()) {
        assignError(error, QStringLiteral(
            "Select one settled connected vehicle first."));
        return false;
    }
    const SwarmVehicleInstanceLease vehicle =
        m_telemetryRegistry->acquireVehicle(target.endpoint,
                                            MaximumHeartbeatAgeMs);
    if (!vehicle.isValid()) {
        assignError(error, QStringLiteral(
            "The selected vehicle has no fresh exact heartbeat."));
        return false;
    }
    EnablePlan plan;
    plan.d = new EnablePlan::Data;
    plan.d->service = this;
    plan.d->target = target;
    plan.d->vehicle = vehicle;
    plan.d->profile = m_profile;
    plan.d->deviceInstanceId = input.instanceId;
    plan.d->deviceGeneration = input.generation;
    plan.d->description = QStringLiteral("%1 to %2")
        .arg(selected.name, exactTargetText(target.endpoint));
    QPointer<const JoystickControlService> guard(this);
    if (!vehicleIsSafe(plan, error) || !guard) return false;
    QVector<ChannelLimits> limits;
    if (m_channelLimitsProvider
        && !m_channelLimitsProvider(vehicle, &limits, error)) {
        return false;
    }
    if (!guard) return false;
    while (limits.size() < JoystickConfiguration::ChannelCount)
        limits.append(ChannelLimits());
    if (limits.size() > JoystickConfiguration::ChannelCount)
        limits.resize(JoystickConfiguration::ChannelCount);
    for (ChannelLimits &limit : limits) {
        if (limit.minimum >= limit.maximum || limit.trim < limit.minimum
            || limit.trim > limit.maximum) {
            limit = ChannelLimits();
        }
    }
    plan.d->limits = limits;
    plan.d->ownedChannels = makePreview(input, m_profile, limits).mapped;
    for (int i = 0; i < m_profile.channels.size()
         && i < plan.d->ownedChannels.size(); ++i) {
        if (m_profile.channels.at(i).axis == QLatin1String("UINT16_MAX"))
            plan.d->ownedChannels[i] = false;
    }
    if (!vehicleIsSafe(plan, error) || !guard) return false;
    if (!m_device || currentDeviceSnapshot().generation != input.generation) {
        assignError(error, QStringLiteral(
            "The joystick changed while enablement was being prepared."));
        return false;
    }
    *planOut = plan;
    return true;
}

bool JoystickControlService::planIsCurrent(
    const EnablePlan &plan, QString *error) const
{
    if (!plan.isValid() || plan.d.constData()->service != this
        || !m_device || !m_targetManager || !m_telemetryRegistry
        || !m_transmitter || !m_routeValidator
        || !sameProfile(plan.d.constData()->profile, m_profile)) {
        assignError(error, QStringLiteral(
            "The joystick enable plan is invalid or its configuration changed."));
        return false;
    }
    const JoystickDevice::Snapshot input = currentDeviceSnapshot();
    const JoystickDevice::Info selected = currentDeviceInfo();
    if (!input.connected || (!m_hasTestDeviceState && m_device->isCalibrating())
        || input.instanceId != plan.d.constData()->deviceInstanceId
        || input.generation != plan.d.constData()->deviceGeneration
        || selected.instanceId != input.instanceId
        || selected.id != plan.d.constData()->profile.deviceId) {
        assignError(error, QStringLiteral(
            "The selected joystick changed or disconnected."));
        return false;
    }
    const auto &target = plan.d.constData()->target;
    if (!m_targetManager->isTargetGenerationSettled()
        || !m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation)) {
        assignError(error, QStringLiteral(
            "The selected vehicle changed after joystick consent."));
        return false;
    }
    SwarmTelemetrySnapshot snapshot;
    if (!m_telemetryRegistry->snapshotForLease(
            plan.d.constData()->vehicle, &snapshot)
        || !snapshot.heartbeatValid
        || !m_telemetryRegistry->observationIsFresh(
            snapshot.heartbeatObservedMs, MaximumHeartbeatAgeMs)
        || snapshot.autopilot == MAV_AUTOPILOT_INVALID) {
        assignError(error, QStringLiteral(
            "The exact joystick target heartbeat is stale or retired."));
        return false;
    }
    return true;
}

bool JoystickControlService::vehicleIsSafe(
    const EnablePlan &submitted, QString *error) const
{
    const EnablePlan plan = submitted;
    const QPointer<const JoystickControlService> guard(this);
    if (!planIsCurrent(plan, error)) return false;
    const RouteValidator route = m_routeValidator;
    if (!route || !route(plan.d.constData()->vehicle, error)) return false;
    if (!guard) return false;
    return planIsCurrent(plan, error);
}

bool JoystickControlService::validate(
    const EnablePlan &plan, QString *error) const
{
    if (error) error->clear();
    return !m_shuttingDown && !isEnabled() && vehicleIsSafe(plan, error);
}

bool JoystickControlService::enable(
    const EnablePlan &submitted, QString *error)
{
    const EnablePlan plan = submitted;
    if (error) error->clear();
    if (m_shuttingDown || isEnabled()) {
        assignError(error, QStringLiteral("Joystick output is already active."));
        return false;
    }
    QPointer<JoystickControlService> guard(this);
    if (!vehicleIsSafe(plan, error) || !guard) return false;
    ++m_generation;
    if (!m_generation) ++m_generation;
    m_activePlan = plan;
    m_enabled = true;
    m_previousButtons = currentDeviceSnapshot().buttons;
    m_customAxis0 = plan.profile().manualControl ? 0 : 1500;
    m_customAxis1 = plan.profile().manualControl ? 0 : 1500;
    m_hatVertical = 32768;
    m_hatHorizontal = 32768;
    m_framesAttempted = 0;
    m_framesSubmitted = 0;
    m_buttonActionsSubmitted = 0;
    m_status = QStringLiteral("Joystick output enabled at 20 Hz for %1.")
        .arg(exactTargetText(plan.target().endpoint));
    m_sendTimer.setInterval(m_sendIntervalMs);
    m_sendTimer.start();
    emit stateChanged();
    return true;
}

bool JoystickControlService::disable(const QString &reason)
{
    if (!m_enabled || m_finishing) return false;
    finishDisable(reason.isEmpty()
        ? QStringLiteral("Joystick output disabled.") : reason, true);
    return true;
}

void JoystickControlService::shutdown()
{
    if (m_shuttingDown) return;
    QPointer<JoystickControlService> guard(this);
    m_shuttingDown = true;
    if (m_enabled) finishDisable(
        QStringLiteral("Joystick service stopped."), true);
    if (!guard) return;
    m_sendTimer.stop();
    if (m_commandService && m_buttonReservation.isValid()) {
        m_commandService->releaseExactReservation(m_buttonReservation);
    }
    if (!guard) return;
    m_buttonReservation = {};
    m_buttonCommand = {};
}

void JoystickControlService::setSendIntervalForTesting(int milliseconds)
{
    m_sendIntervalMs = qBound(1, milliseconds, 1000);
    if (m_sendTimer.isActive()) m_sendTimer.setInterval(m_sendIntervalMs);
}

void JoystickControlService::setDeviceStateForTesting(
    const JoystickDevice::Info &device,
    const JoystickDevice::Snapshot &snapshot)
{
    m_hasTestDeviceState = true;
    m_testDeviceInfo = device;
    m_testDeviceSnapshot = snapshot;
    handleSnapshot(snapshot);
}

JoystickDevice::Snapshot
JoystickControlService::currentDeviceSnapshot() const
{
    if (m_hasTestDeviceState) return m_testDeviceSnapshot;
    return m_device ? m_device->snapshot() : JoystickDevice::Snapshot();
}

JoystickDevice::Info JoystickControlService::currentDeviceInfo() const
{
    if (m_hasTestDeviceState) return m_testDeviceInfo;
    return m_device ? m_device->selectedDevice() : JoystickDevice::Info();
}

bool JoystickControlService::operationIsCurrent(
    quint64 generation, const EnablePlan &plan) const noexcept
{
    return m_enabled && !m_finishing && generation != 0
        && generation == m_generation && plan.isValid()
        && m_activePlan.isValid()
        && plan.d.constData() == m_activePlan.d.constData();
}

JoystickControlService::Preview JoystickControlService::makePreview(
    const JoystickDevice::Snapshot &snapshot, const Profile &profile,
    const QVector<ChannelLimits> &limits) const
{
    Preview preview;
    preview.connected = snapshot.connected;
    preview.deviceGeneration = snapshot.generation;
    preview.buttons = snapshot.buttons;
    preview.channels.fill(profile.manualControl ? 0 : 1500,
                          JoystickConfiguration::ChannelCount);
    preview.mapped.fill(false, JoystickConfiguration::ChannelCount);
    for (int i = 0; i < profile.channels.size()
         && i < JoystickConfiguration::ChannelCount; ++i) {
        const auto &channel = profile.channels.at(i);
        const QString axis = channel.axis;
        int value = profile.manualControl ? 0 : 1500;
        bool mapped = axis != QLatin1String("None");
        if (axis == QLatin1String("Pass")) {
            value = profile.manualControl ? 0 : 1500;
        } else if (axis == QLatin1String("Custom1")) {
            value = static_cast<int>(m_customAxis0);
        } else if (axis == QLatin1String("Custom2")) {
            value = static_cast<int>(m_customAxis1);
        } else if (axis == QLatin1String("UINT16_MAX")) {
            value = -1;
        } else if (mapped) {
            quint16 normalized = 0;
            const bool found = axis == QLatin1String("Hatud1")
                ? (normalized = m_hatVertical, true)
                : axis == QLatin1String("Hatlr2")
                    ? (normalized = m_hatHorizontal, true)
                    : JoystickConfiguration::axisValue(
                        axis, snapshot.axes, snapshot.hats, &normalized);
            if (!found) {
                mapped = false;
            } else {
                const ChannelLimits limit = limits.value(i);
                const int minimum = profile.manualControl ? -1000 : limit.minimum;
                const int maximum = profile.manualControl ? 1000 : limit.maximum;
                int trim = profile.manualControl ? 0 : limit.trim;
                if (i == 2) trim = (minimum + maximum) / 2;
                value = JoystickConfiguration::channelValue(
                    normalized, channel, profile.manualControl,
                    minimum, maximum, trim);
            }
        }
        preview.channels[i] = value;
        preview.mapped[i] = mapped;
    }
    if (profile.elevons && preview.channels.size() >= 2
        && preview.mapped.at(0) && preview.mapped.at(1)) {
        Profile raw = profile;
        raw.channels[0].reverse = false;
        raw.channels[1].reverse = false;
        quint16 firstAxis = 0, secondAxis = 0;
        if (JoystickConfiguration::axisValue(raw.channels.at(0).axis,
                snapshot.axes, snapshot.hats, &firstAxis)
            && JoystickConfiguration::axisValue(raw.channels.at(1).axis,
                snapshot.axes, snapshot.hats, &secondAxis)) {
            const int roll = JoystickConfiguration::channelValue(
                firstAxis, raw.channels.at(0), profile.manualControl,
                profile.manualControl ? -1000 : limits.value(0).minimum,
                profile.manualControl ? 1000 : limits.value(0).maximum,
                profile.manualControl ? 0 : limits.value(0).trim);
            const int pitch = JoystickConfiguration::channelValue(
                secondAxis, raw.channels.at(1), profile.manualControl,
                profile.manualControl ? -1000 : limits.value(1).minimum,
                profile.manualControl ? 1000 : limits.value(1).maximum,
                profile.manualControl ? 0 : limits.value(1).trim);
            const int midpoint = profile.manualControl ? 0 : 1500;
            const int first = ((pitch - midpoint) - (roll - midpoint)) / 2;
            const int second = ((pitch - midpoint) + (roll - midpoint)) / 2;
            const int sign1 = profile.channels.at(0).reverse ? -1 : 1;
            const int sign2 = profile.channels.at(1).reverse ? -1 : 1;
            const int minimum = profile.manualControl ? -1000 : 1000;
            const int maximum = profile.manualControl ? 1000 : 2000;
            preview.channels[0] = qBound(minimum,
                midpoint + sign1 * first, maximum);
            preview.channels[1] = qBound(minimum,
                midpoint + sign2 * second, maximum);
        }
    }
    return preview;
}

void JoystickControlService::handleSnapshot(
    const JoystickDevice::Snapshot &snapshot)
{
    const Profile profile = m_enabled ? m_activePlan.profile() : m_profile;
    m_preview = makePreview(snapshot, profile,
        m_enabled ? m_activePlan.d.constData()->limits
                  : QVector<ChannelLimits>());
    QPointer<JoystickControlService> guard(this);
    emit previewChanged(m_preview);
    if (!guard) return;
    if (!m_enabled) return;
    const quint64 generation = m_generation;
    const EnablePlan plan = m_activePlan;
    if (!snapshot.connected
        || snapshot.instanceId != plan.deviceInstanceId()
        || snapshot.generation != plan.deviceGeneration()) {
        finishDisable(QStringLiteral(
            "The selected joystick disconnected; output stopped."), true);
        return;
    }
    handleButtonEdges(snapshot);
    if (!guard || !operationIsCurrent(generation, plan)) return;
}

void JoystickControlService::handleDisconnected(const QString &reason)
{
    if (m_enabled) {
        finishDisable(reason.isEmpty()
            ? QStringLiteral("The selected joystick disconnected; output stopped.")
            : reason, true);
    }
}

void JoystickControlService::sendControlFrame()
{
    if (!m_enabled || m_finishing || m_sending || !m_transmitter) return;
    const quint64 generation = m_generation;
    const EnablePlan plan = m_activePlan;
    QPointer<JoystickControlService> guard(this);
    if (!vehicleIsSafe(plan, nullptr)) {
        if (guard && operationIsCurrent(generation, plan)) {
            finishDisable(QStringLiteral(
                "The exact joystick target or route changed; output stopped."),
                true);
        }
        return;
    }
    if (!guard || !operationIsCurrent(generation, plan)) return;
    const JoystickDevice::Snapshot input = currentDeviceSnapshot();
    if (!input.hats.isEmpty()) {
        const int bits = input.hats.constFirst();
        const int vertical = ((bits & 1) ? 1 : 0) - ((bits & 4) ? 1 : 0);
        const int horizontal = ((bits & 2) ? 1 : 0) - ((bits & 8) ? 1 : 0);
        m_hatVertical = quint16(qBound(0,
            int(m_hatVertical) + vertical * 500, 65535));
        m_hatHorizontal = quint16(qBound(0,
            int(m_hatHorizontal) + horizontal * 500, 65535));
    }
    const Preview output = makePreview(input, plan.profile(),
                                       plan.d.constData()->limits);
    mavlink_message_t message{};
    if (plan.profile().manualControl) {
        message.msgid = MAVLINK_MSG_ID_MANUAL_CONTROL;
        message.len = MAVLINK_MSG_ID_MANUAL_CONTROL_LEN;
        char *payload = _MAV_PAYLOAD_NON_CONST(&message);
        for (int i = 0; i < 4; ++i) {
            const int value = i < output.channels.size()
                && output.mapped.value(i) ? output.channels.at(i) : 0;
            _mav_put_int16_t(payload, static_cast<uint8_t>(i * 2),
                static_cast<qint16>(qBound(-1000, value, 1000)));
        }
        _mav_put_uint16_t(payload, 8, 0);
        _mav_put_uint8_t(payload, 10,
            static_cast<quint8>(plan.target().endpoint.systemId));
    } else {
        message.msgid = MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE;
        message.len = MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE_LEN;
        char *payload = _MAV_PAYLOAD_NON_CONST(&message);
        for (int channel = 1; channel <= 8; ++channel) {
            _mav_put_uint16_t(payload, static_cast<uint8_t>((channel - 1) * 2),
                rcValue(channel, output.channels.value(channel - 1),
                        output.mapped.value(channel - 1)));
        }
        _mav_put_uint8_t(payload, 16,
            static_cast<quint8>(plan.target().endpoint.systemId));
        _mav_put_uint8_t(payload, 17,
            static_cast<quint8>(plan.target().endpoint.componentId));
        for (int channel = 9; channel <= 18; ++channel) {
            _mav_put_uint16_t(payload, static_cast<uint8_t>(18 + (channel - 9) * 2),
                rcValue(channel, output.channels.value(channel - 1),
                        output.mapped.value(channel - 1)));
        }
    }

    m_sending = true;
    bool attempted = false;
    const auto result = m_transmitter->sendJoystickControl(
        plan.vehicle().endpoint.linkId, plan.vehicle().linkSessionEpoch,
        m_localSystemId, m_localComponentId, message,
        [guard, generation, plan]() {
            if (!guard || !guard->operationIsCurrent(generation, plan)) {
                return false;
            }
            QString ignored;
            return guard->vehicleIsSafe(plan, &ignored)
                && guard && guard->operationIsCurrent(generation, plan);
        }, &attempted);
    if (!guard) return;
    if (attempted) ++m_framesAttempted;
    if (!operationIsCurrent(generation, plan)) return;
    m_sending = false;
    if (result == ExactLinkTransmitter::SendResult::Sent) {
        ++m_framesSubmitted;
    } else {
        finishDisable(attempted
            ? QStringLiteral(
                "Joystick transport outcome is uncertain; output stopped.")
            : QStringLiteral(
                "Joystick control could not reach the exact route; output stopped."),
            true);
    }
}

bool JoystickControlService::sendRelease(const EnablePlan &plan)
{
    if (!plan.isValid() || !m_transmitter || !m_telemetryRegistry
        || !m_routeValidator) return false;
    if (plan.profile().manualControl) return true;
    const SwarmVehicleInstanceLease vehicle = plan.vehicle();
    const RouteValidator route = m_routeValidator;
    const QPointer<JoystickControlService> guard(this);
    const auto releaseSafe = [guard, registry = m_telemetryRegistry,
                              route, vehicle]() {
        if (!guard || !registry || !route) return false;
        SwarmTelemetrySnapshot snapshot;
        if (!registry->snapshotForLease(vehicle, &snapshot)
            || !snapshot.heartbeatValid
            || snapshot.autopilot == MAV_AUTOPILOT_INVALID) return false;
        QString ignored;
        if (!route(vehicle, &ignored) || !guard) return false;
        SwarmTelemetrySnapshot after;
        return registry && registry->snapshotForLease(vehicle, &after)
            && after.heartbeatValid
            && after.autopilot != MAV_AUTOPILOT_INVALID;
    };
    if (!releaseSafe()) return false;
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE;
    message.len = MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE_LEN;
    char *payload = _MAV_PAYLOAD_NON_CONST(&message);
    for (int channel = 1; channel <= 8; ++channel) {
        const quint16 value = plan.d.constData()->ownedChannels.value(channel - 1)
            ? 0 : std::numeric_limits<quint16>::max();
        _mav_put_uint16_t(payload, static_cast<uint8_t>((channel - 1) * 2),
                          value);
    }
    _mav_put_uint8_t(payload, 16,
        static_cast<quint8>(vehicle.endpoint.systemId));
    _mav_put_uint8_t(payload, 17,
        static_cast<quint8>(vehicle.endpoint.componentId));
    for (int channel = 9; channel <= 18; ++channel) {
        const quint16 value = plan.d.constData()->ownedChannels.value(channel - 1)
            ? std::numeric_limits<quint16>::max() - 1 : 0;
        _mav_put_uint16_t(payload, static_cast<uint8_t>(18 + (channel - 9) * 2),
                          value);
    }
    bool attempted = false;
    const auto result = m_transmitter->sendJoystickControl(
        vehicle.endpoint.linkId, vehicle.linkSessionEpoch,
        m_localSystemId, m_localComponentId, message, releaseSafe, &attempted);
    if (!guard) return false;
    if (attempted) ++m_framesAttempted;
    if (result == ExactLinkTransmitter::SendResult::Sent) {
        ++m_framesSubmitted;
        return true;
    }
    return false;
}

void JoystickControlService::handleButtonEdges(
    const JoystickDevice::Snapshot &snapshot)
{
    const Profile profile = m_activePlan.profile();
    if (m_previousButtons.size() < snapshot.buttons.size())
        m_previousButtons.resize(snapshot.buttons.size());
    for (int i = 0; i < profile.buttons.size(); ++i) {
        const int physical = profile.buttons.at(i).buttonno;
        if (physical < 0 || physical >= snapshot.buttons.size()) continue;
        const bool down = snapshot.buttons.at(physical);
        const bool before = m_previousButtons.value(physical);
        if (down != before) {
            QPointer<JoystickControlService> guard(this);
            dispatchButtonAction(i, down);
            if (!guard) return;
        }
        if (!m_enabled) return;
    }
    m_previousButtons = snapshot.buttons;
}

void JoystickControlService::dispatchButtonAction(
    int configurationIndex, bool down)
{
    if (!m_enabled || !m_commandService || configurationIndex < 0
        || configurationIndex >= m_activePlan.profile().buttons.size()) return;
    const auto button = m_activePlan.profile().buttons.at(configurationIndex);
    if (button.function == QLatin1String("Button_axis0")
        || button.function == QLatin1String("Button_axis1")) {
        const qint64 value = std::llround(down ? button.p2 : button.p1);
        if (button.function == QLatin1String("Button_axis0")) m_customAxis0 = value;
        else m_customAxis1 = value;
        m_preview = makePreview(currentDeviceSnapshot(), m_activePlan.profile(),
                                m_activePlan.d.constData()->limits);
        emit previewChanged(m_preview);
        return;
    }
    if (!down && button.function != QLatin1String("Do_Set_Relay")) return;
    if (m_buttonCommand.isValid() || m_buttonReservation.isValid()) {
        emit buttonActionFinished(button.function, false,
            QStringLiteral("Another joystick button command is still active."));
        return;
    }
    const quint64 generation = m_generation;
    const EnablePlan plan = m_activePlan;
    QPointer<JoystickControlService> guard(this);
    SwarmTelemetrySnapshot snapshot;
    QString detail;
    if (!vehicleIsSafe(plan, &detail)
        || !m_telemetryRegistry->snapshotForLease(plan.vehicle(), &snapshot)) {
        if (guard && operationIsCurrent(generation, plan)) {
            finishDisable(detail.isEmpty()
                ? QStringLiteral("The joystick target became unavailable.")
                : detail, true);
        }
        return;
    }
    VehicleCommandService::ExactCommandRequest request;
    auto effectiveButton = button;
    if (effectiveButton.function == QLatin1String("Do_Set_Relay"))
        effectiveButton.state = down;
    if (!buildButtonCommand(effectiveButton, snapshot, &request, &detail)) {
        emit buttonActionFinished(button.function, false, detail);
        return;
    }
    if (button.function == QLatin1String("TakeOff")) {
        m_pendingTakeoff = true;
        m_pendingTakeoffAltitude = SwarmFlightMode::isCopter(snapshot.vehicleType)
            ? 2.0F : 20.0F;
    }
    request.maximumRetries = 0;
    request.acknowledgementTimeoutMs = 2000;
    request.maximumLifetimeMs = 30000;
    request.validateBeforeWrite = [guard, generation, plan](QString *error) {
        return guard && guard->operationIsCurrent(generation, plan)
            && guard->vehicleIsSafe(plan, error)
            && guard && guard->operationIsCurrent(generation, plan);
    };
    VehicleCommandService::ExactReservationToken reservation;
    const auto reserveResult = m_commandService->reserveSingleVehicleEndpoint(
        this, plan.target(), plan.vehicle(), &reservation, &detail);
    if (!guard) return;
    if (!operationIsCurrent(generation, plan)
        || reserveResult != VehicleCommandService::ExactReservationResult::Reserved
        || !reservation.isValid()) {
        if (reservation.isValid()) m_commandService->releaseExactReservation(reservation);
        if (!guard) return;
        emit buttonActionFinished(button.function, false,
            detail.isEmpty() ? QStringLiteral("The button command lane is busy.") : detail);
        return;
    }
    m_buttonReservation = reservation;
    m_pendingButtonFunction = button.function;
    VehicleCommandService::ExactCommandToken token;
    const auto submit = m_commandService->submitExactCommandLong(
        reservation, plan.vehicle(), request, &token, &detail);
    if (!guard) return;
    if (!operationIsCurrent(generation, plan)
        || submit != VehicleCommandService::ExactSubmitResult::Started
        || !token.isValid()) {
        if (m_buttonReservation.isValid())
            m_commandService->releaseExactReservation(m_buttonReservation);
        if (!guard) return;
        m_buttonReservation = {};
        m_buttonCommand = {};
        m_pendingTakeoff = false;
        const QString function = m_pendingButtonFunction;
        m_pendingButtonFunction.clear();
        emit buttonActionFinished(function, false,
            detail.isEmpty() ? QStringLiteral("The button command was not submitted.") : detail);
        return;
    }
    m_buttonCommand = token;
    ++m_buttonActionsSubmitted;
}

bool JoystickControlService::buildButtonCommand(
    const JoystickConfiguration::Button &button,
    const SwarmTelemetrySnapshot &snapshot,
    VehicleCommandService::ExactCommandRequest *request,
    QString *error) const
{
    if (!request) return false;
    const QString function = button.function;
    if (function == QLatin1String("ChangeMode")) {
        quint32 mode = 0;
        if (!customModeFor(snapshot, button.mode, &mode)) {
            assignError(error, QStringLiteral(
                "The configured flight mode is not known for this vehicle."));
            return false;
        }
        request->command = MAV_CMD_DO_SET_MODE;
        request->params[0] = static_cast<float>(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED);
        request->params[1] = static_cast<float>(mode);
    } else if (function == QLatin1String("Arm")
               || function == QLatin1String("Disarm")) {
        request->command = MAV_CMD_COMPONENT_ARM_DISARM;
        request->params[0] = function == QLatin1String("Arm") ? 1.0F : 0.0F;
    } else if (function == QLatin1String("TakeOff")) {
        quint32 guided = 0;
        if (SwarmFlightMode::isCopter(snapshot.vehicleType)) guided = 4;
        else if (SwarmFlightMode::isPlane(snapshot.vehicleType)
                 || SwarmFlightMode::isRover(snapshot.vehicleType)) guided = 15;
        else {
            assignError(error, QStringLiteral(
                "Takeoff is not mapped for this vehicle family."));
            return false;
        }
        request->command = MAV_CMD_DO_SET_MODE;
        request->params[0] = static_cast<float>(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED);
        request->params[1] = static_cast<float>(guided);
    } else if (function == QLatin1String("Do_Set_Relay")) {
        request->command = MAV_CMD_DO_SET_RELAY;
        request->params[0] = static_cast<float>(button.p1);
        request->params[1] = button.state ? 1.0F : 0.0F;
    } else if (function == QLatin1String("Do_Repeat_Relay")) {
        request->command = MAV_CMD_DO_REPEAT_RELAY;
        request->params[0] = static_cast<float>(button.p1);
        request->params[1] = static_cast<float>(button.p2);
        request->params[2] = static_cast<float>(button.p3);
    } else if (function == QLatin1String("Do_Set_Servo")) {
        request->command = MAV_CMD_DO_SET_SERVO;
        request->params[0] = static_cast<float>(button.p1);
        request->params[1] = static_cast<float>(button.p2);
    } else if (function == QLatin1String("Do_Repeat_Servo")) {
        request->command = MAV_CMD_DO_REPEAT_SERVO;
        request->params[0] = static_cast<float>(button.p1);
        request->params[1] = static_cast<float>(button.p2);
        request->params[2] = static_cast<float>(button.p3);
        request->params[3] = static_cast<float>(button.p4);
    } else if (function == QLatin1String("Digicam_Control")) {
        request->command = MAV_CMD_DO_DIGICAM_CONTROL;
        request->params[4] = 1.0F;
    } else if (function == QLatin1String("Mount_Mode")) {
        request->command = MAV_CMD_DO_MOUNT_CONFIGURE;
        request->params[0] = static_cast<float>(button.p1);
    } else if (function == QLatin1String("Mount_Control_0")) {
        request->command = MAV_CMD_DO_MOUNT_CONTROL;
        request->params[6] = static_cast<float>(MAV_MOUNT_MODE_MAVLINK_TARGETING);
    } else {
        assignError(error, QStringLiteral(
            "This joystick button function needs a parameter or live pointing workflow and is not available here."));
        return false;
    }
    return true;
}

void JoystickControlService::handleCommandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (!m_buttonCommand.isValid()
        || report.token.transactionId != m_buttonCommand.transactionId
        || report.token.reservationId != m_buttonCommand.reservationId) return;
    const QString function = m_pendingButtonFunction;
    const bool accepted = report.terminalResult
        == VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted;
    m_buttonCommand = {};
    QPointer<JoystickControlService> guard(this);
    if (accepted && m_pendingTakeoff && m_enabled
        && function == QLatin1String("TakeOff")
        && m_commandService && m_buttonReservation.isValid()) {
        m_pendingTakeoff = false;
        const quint64 generation = m_generation;
        const EnablePlan plan = m_activePlan;
        VehicleCommandService::ExactCommandRequest request;
        request.command = MAV_CMD_NAV_TAKEOFF;
        request.params[6] = m_pendingTakeoffAltitude;
        request.acknowledgementTimeoutMs = 2000;
        request.maximumLifetimeMs = 30000;
        request.maximumRetries = 0;
        request.validateBeforeWrite = [guard, generation, plan](QString *error) {
            return guard && guard->operationIsCurrent(generation, plan)
                && guard->vehicleIsSafe(plan, error)
                && guard && guard->operationIsCurrent(generation, plan);
        };
        VehicleCommandService::ExactCommandToken token;
        QString detail;
        const auto result = m_commandService->submitExactCommandLong(
            m_buttonReservation, plan.vehicle(), request, &token, &detail);
        if (!guard) return;
        if (result == VehicleCommandService::ExactSubmitResult::Started
            && token.isValid() && operationIsCurrent(generation, plan)) {
            m_buttonCommand = token;
            return;
        }
        if (m_commandService && m_buttonReservation.isValid())
            m_commandService->releaseExactReservation(m_buttonReservation);
        if (!guard) return;
        m_buttonReservation = {};
        m_pendingButtonFunction.clear();
        emit buttonActionFinished(function, false,
            detail.isEmpty()
                ? QStringLiteral("Guided mode was accepted, but takeoff was not submitted.")
                : detail);
        return;
    }
    m_pendingTakeoff = false;
    if (m_commandService && m_buttonReservation.isValid())
        m_commandService->releaseExactReservation(m_buttonReservation);
    if (!guard) return;
    m_buttonReservation = {};
    m_pendingButtonFunction.clear();
    emit buttonActionFinished(function, accepted, report.description);
}

void JoystickControlService::finishDisable(
    const QString &reason, bool attemptRelease)
{
    if ((!m_enabled && !m_finishing) || m_finishing) return;
    const EnablePlan plan = m_activePlan;
    ++m_generation;
    if (!m_generation) ++m_generation;
    m_sendTimer.stop();
    m_enabled = false;
    m_finishing = true;
    m_sending = false;
    QPointer<JoystickControlService> guard(this);
    const auto buttonReservation = m_buttonReservation;
    m_buttonReservation = {};
    m_buttonCommand = {};
    m_pendingButtonFunction.clear();
    m_pendingTakeoff = false;
    m_pendingTakeoffAltitude = 0.0F;
    if (m_commandService && buttonReservation.isValid())
        m_commandService->releaseExactReservation(buttonReservation);
    if (!guard) return;
    const bool released = attemptRelease && sendRelease(plan);
    if (!guard) return;
    if (!m_shuttingDown && attemptRelease && !released) {
        m_status = reason + QStringLiteral(
            " RC override release could not be submitted on the original exact route.");
    } else {
        m_status = reason;
    }
    m_activePlan = {};
    m_previousButtons.clear();
    emit stateChanged();
    if (!guard) return;
    emit stopped(m_status);
    if (!guard) return;
    m_finishing = false;
    emit stateChanged();
}

void JoystickControlService::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit stateChanged();
}
