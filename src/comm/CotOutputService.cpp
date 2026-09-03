#include "CotOutputService.h"

#include <QPointer>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <utility>

namespace {

double normaliseDegrees(double degrees)
{
    if (!std::isfinite(degrees)) {
        return 0.0;
    }
    double result = std::fmod(degrees, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result;
}

} // namespace

constexpr double CotOutputService::MinimumIntervalSeconds;
constexpr double CotOutputService::MaximumIntervalSeconds;
constexpr int CotOutputService::MaximumStatusTextLength;
constexpr int CotOutputService::MaximumPreviewCharacters;

CotOutputService::TransportFactory
CotOutputService::ProductionTransportFactory()
{
    return [](const CotOutputTransport::Settings &settings, QObject *parent) {
        return std::unique_ptr<CotOutputTransport>(
            new CotOutputTransport(settings, parent));
    };
}

CotOutputService::Sender CotOutputService::ProductionSender()
{
    return [](CotOutputTransport *transport, const QByteArray &payload) {
        return transport
            ? transport->send(payload)
            : CotOutputTransport::SendResult::NotReady;
    };
}

QString CotOutputService::StoppedText()
{
    return QStringLiteral("Stopped.");
}

QString CotOutputService::ErrorStartingText(const QString &reason)
{
    return QStringLiteral("Unable to start CoT output: %1").arg(reason);
}

QString CotOutputService::SourceRemovedText(const QString &linkName)
{
    return QStringLiteral("Stopped: source link %1 was removed.")
        .arg(linkName);
}

bool CotOutputService::IsValidInterval(double seconds)
{
    return std::isfinite(seconds)
        && seconds >= MinimumIntervalSeconds
        && seconds <= MaximumIntervalSeconds;
}

CotOutputService::CotOutputService(TransportFactory transportFactory,
                                   Sender sender, Clock clock,
                                   QObject *parent)
    : QObject(parent)
    , m_transportFactory(std::move(transportFactory))
    , m_sender(std::move(sender))
    , m_clock(std::move(clock))
    , m_text(StoppedText())
{
    if (!m_transportFactory) {
        m_transportFactory = ProductionTransportFactory();
    }
    if (!m_sender) {
        m_sender = ProductionSender();
    }
    if (!m_clock) {
        m_clock = []() { return QDateTime::currentDateTimeUtc(); };
    }
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CotOutputService::emitNow);
}

CotOutputService::~CotOutputService()
{
    ++m_generation;
    m_timer.stop();
    releaseTransport();
}

CotOutputService::Status CotOutputService::status() const
{
    Status result;
    result.state = m_state;
    result.text = m_text;
    result.linkId = m_linkId;
    result.linkName = m_linkName;
    result.transportState = m_transport
        ? m_transport->state() : CotOutputTransport::State::Stopped;
    result.preview = m_preview;
    result.previewTruncated = m_previewTruncated;
    result.registeredEndpoints = m_activeEndpoints.size();
    for (auto it = m_endpointStates.constBegin();
         it != m_endpointStates.constEnd(); ++it) {
        if (it.value().hasPosition) {
            ++result.positionedEndpoints;
        }
    }
    result.ticks = m_ticks;
    result.eventsGenerated = m_eventsGenerated;
    result.eventsSent = m_eventsSent;
    result.eventsDropped = m_eventsDropped;
    result.eventsWithoutPeer = m_eventsWithoutPeer;
    result.errors = m_errors;
    result.lastSendResult = m_lastSendResult;
    result.lastError = m_lastError;
    return result;
}

int CotOutputService::timerIntervalMs() const
{
    if (!IsValidInterval(m_settings.updateIntervalSeconds)) {
        return 10000;
    }
    return qBound(100,
                  qRound(m_settings.updateIntervalSeconds * 1000.0),
                  3600000);
}

void CotOutputService::resetCounters()
{
    m_preview.clear();
    m_lastError.clear();
    m_previewTruncated = false;
    m_ticks = 0;
    m_eventsGenerated = 0;
    m_eventsSent = 0;
    m_eventsDropped = 0;
    m_eventsWithoutPeer = 0;
    m_errors = 0;
    m_lastSendResult = CotOutputTransport::SendResult::NotReady;
}

bool CotOutputService::start(int linkId, const QString &linkName,
                             const CotOutputServiceSettings &settings,
                             QString *error)
{
    ++m_generation;
    m_timer.stop();
    releaseTransport();
    m_activeEndpoints.clear();
    m_endpointStates.clear();
    ++m_endpointGeneration;
    resetCounters();
    m_settings = settings;
    m_settings.event = boundedEventSettings(settings.event);
    m_settings.identities = boundedIdentities(settings.identities);
    m_linkId = linkId;
    m_linkName = boundedStatusText(
        linkName.trimmed().isEmpty()
            ? QStringLiteral("Link %1").arg(linkId)
            : linkName.trimmed());
    rebuildActiveEndpoints();

    const auto refuse = [this, error](const QString &reason) {
        const QString boundedReason = boundedStatusText(reason);
        const QString text = boundedStatusText(
            ErrorStartingText(boundedReason));
        if (error) {
            *error = text;
        }
        fail(text, boundedReason);
        return false;
    };

    if (error) {
        error->clear();
    }
    if (linkId < 0) {
        return refuse(QStringLiteral(
            "No current physical link: select a vehicle or connect a link."));
    }
    if (!IsValidInterval(m_settings.updateIntervalSeconds)) {
        return refuse(QStringLiteral(
            "Update interval must be between 0.1 and 3600 seconds."));
    }

    std::unique_ptr<CotOutputTransport> transport =
        m_transportFactory(m_settings.transport, this);
    if (!transport) {
        return refuse(QStringLiteral("transport factory returned no transport."));
    }
    if (transport->parent() != this) {
        transport->setParent(this);
    }

    QString reason;
    if (!transport->start(&reason)) {
        transport->stop();
        return refuse(reason.isEmpty()
            ? QStringLiteral("transport rejected the start request.") : reason);
    }

    m_transport = std::move(transport);
    connect(m_transport.get(), &CotOutputTransport::stateChanged,
            this, &CotOutputService::handleTransportStateChanged);
    connect(m_transport.get(), &CotOutputTransport::statusChanged,
            this, &CotOutputService::handleTransportStatusChanged);
    connect(m_transport.get(), &CotOutputTransport::errorOccurred,
            this, &CotOutputService::handleTransportError);

    m_state = State::Starting;
    m_text = boundedStatusText(m_transport->statusText());
    QPointer<CotOutputService> guard(this);
    activateIfPrepared();
    return !guard.isNull() && guard->m_transport != nullptr;
}

void CotOutputService::stop()
{
    if (!m_transport && m_state == State::Stopped
        && m_text == StoppedText()) {
        return;
    }
    setStopped(StoppedText());
}

void CotOutputService::clear()
{
    stop();
}

void CotOutputService::updateEndpoints(
    const QList<VehicleEndpoint> &endpoints)
{
    QList<VehicleEndpoint> filtered;
    QSet<VehicleEndpoint> seen;
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (!endpoint.isValid() || seen.contains(endpoint)) {
            continue;
        }
        seen.insert(endpoint);
        filtered.append(endpoint);
    }
    std::sort(filtered.begin(), filtered.end());
    m_knownEndpoints = filtered;
    rebuildActiveEndpoints();
    emit statusChanged();
}

void CotOutputService::rebuildActiveEndpoints()
{
    QList<VehicleEndpoint> active;
    if (m_linkId >= 0) {
        for (const VehicleEndpoint &endpoint : m_knownEndpoints) {
            if (endpoint.linkId == m_linkId) {
                active.append(endpoint);
            }
        }
    }

    QHash<VehicleEndpoint, EndpointState> states;
    states.reserve(active.size());
    for (const VehicleEndpoint &endpoint : active) {
        const auto previous = m_endpointStates.constFind(endpoint);
        states.insert(endpoint, previous == m_endpointStates.constEnd()
            ? EndpointState() : previous.value());
    }
    m_activeEndpoints = active;
    m_endpointStates = states;
    ++m_endpointGeneration;
}

void CotOutputService::setEventSettings(const CotEventSettings &settings)
{
    m_settings.event = boundedEventSettings(settings);
    emit statusChanged();
}

void CotOutputService::setIdentityOverrides(
    const QHash<int, CotIdentityOverride> &identities)
{
    m_settings.identities = boundedIdentities(identities);
    emit statusChanged();
}

bool CotOutputService::setUpdateIntervalSeconds(double seconds)
{
    if (!IsValidInterval(seconds)) {
        return false;
    }
    if (qFuzzyCompare(m_settings.updateIntervalSeconds, seconds)) {
        return true;
    }
    m_settings.updateIntervalSeconds = seconds;
    updateTimerInterval();
    emit statusChanged();
    return true;
}

void CotOutputService::updateTimerInterval()
{
    m_timer.setInterval(timerIntervalMs());
}

void CotOutputService::activateIfPrepared()
{
    if (!m_transport) {
        return;
    }
    const CotOutputTransport::State transportState = m_transport->state();
    if (transportState == CotOutputTransport::State::Error) {
        const QString reason = m_transport->lastError().isEmpty()
            ? m_transport->statusText() : m_transport->lastError();
        recordTransportError(reason);
        fail(boundedStatusText(reason), reason);
        return;
    }
    if (transportState == CotOutputTransport::State::Stopped) {
        const QString reason =
            QStringLiteral("CoT transport stopped unexpectedly.");
        recordTransportError(reason);
        fail(reason, reason);
        return;
    }
    if (transportState != CotOutputTransport::State::Ready
        && transportState != CotOutputTransport::State::Listening) {
        m_timer.stop();
        m_state = State::Starting;
        m_text = boundedStatusText(m_transport->statusText());
        emit statusChanged();
        return;
    }

    const bool firstPreparedTick = m_state != State::Emitting;
    m_state = State::Emitting;
    m_text = boundedStatusText(m_transport->statusText());
    updateTimerInterval();
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    if (firstPreparedTick) {
        emitNow();
    } else {
        emit statusChanged();
    }
}

void CotOutputService::observeMessage(int linkId,
                                      const mavlink_message_t &message)
{
    if (!m_transport || linkId != m_linkId) {
        return;
    }
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    auto state = m_endpointStates.find(endpoint);
    if (state == m_endpointStates.end()) {
        return;
    }
    applyMessage(state.value(), message);
}

bool CotOutputService::applyMessage(EndpointState &state,
                                    const mavlink_message_t &message)
{
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        // CurrentState treats zero GPI coordinates as dead-reckoning
        // placeholders. GPS_RAW_INT below intentionally accepts real 0,0.
        if (position.lat != 0 && position.lon != 0
            && position.lat != INT_MAX && position.lon != INT_MAX) {
            state.navigation.latitudeDegrees = position.lat * 1.0e-7;
            state.navigation.longitudeDegrees = position.lon * 1.0e-7;
            state.navigation.altitudeAmslMetres = position.alt * 1.0e-3;
            state.navigation.speedMetresPerSecond =
                std::hypot(static_cast<double>(position.vx),
                           static_cast<double>(position.vy)) * 1.0e-2;
            if (position.hdg != USHRT_MAX) {
                state.navigation.courseDegrees =
                    normaliseDegrees(position.hdg * 1.0e-2);
            }
            state.hasPosition = true;
        }
        return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        if (gps.lat != INT_MAX && gps.lon != INT_MAX) {
            state.navigation.latitudeDegrees = gps.lat * 1.0e-7;
            state.navigation.longitudeDegrees = gps.lon * 1.0e-7;
            state.navigation.altitudeAmslMetres = gps.alt * 1.0e-3;
            state.hasPosition = true; // Equator/prime-meridian 0,0 is valid.
        }
        if (gps.vel != USHRT_MAX) {
            state.navigation.speedMetresPerSecond = gps.vel * 1.0e-2;
        }
        if (gps.cog != USHRT_MAX) {
            state.navigation.courseDegrees =
                normaliseDegrees(gps.cog * 1.0e-2);
        }
        return true;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t hud{};
        mavlink_msg_vfr_hud_decode(&message, &hud);
        if (std::isfinite(hud.alt)) {
            state.navigation.altitudeAmslMetres = hud.alt;
        }
        if (std::isfinite(hud.groundspeed)) {
            state.navigation.speedMetresPerSecond =
                qMax(0.0, static_cast<double>(hud.groundspeed));
        }
        if (hud.heading >= 0 && hud.heading <= 360) {
            state.navigation.courseDegrees = normaliseDegrees(hud.heading);
        }
        return true;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY: {
        mavlink_high_latency_t highLatency{};
        mavlink_msg_high_latency_decode(&message, &highLatency);
        if (highLatency.latitude != INT_MAX
            && highLatency.longitude != INT_MAX) {
            state.navigation.latitudeDegrees =
                highLatency.latitude * 1.0e-7;
            state.navigation.longitudeDegrees =
                highLatency.longitude * 1.0e-7;
            state.navigation.altitudeAmslMetres =
                highLatency.altitude_amsl;
            state.hasPosition = true;
        }
        state.navigation.speedMetresPerSecond = highLatency.groundspeed;
        // Intentional useful SI fix over MP10 CurrentState: HL updates yaw but
        // leaves groundcourse stale; CoT track uses heading/100 degrees here.
        state.navigation.courseDegrees =
            normaliseDegrees(highLatency.heading * 1.0e-2);
        return true;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY2: {
        mavlink_high_latency2_t highLatency{};
        mavlink_msg_high_latency2_decode(&message, &highLatency);
        if (highLatency.latitude != INT_MAX
            && highLatency.longitude != INT_MAX) {
            state.navigation.latitudeDegrees =
                highLatency.latitude * 1.0e-7;
            state.navigation.longitudeDegrees =
                highLatency.longitude * 1.0e-7;
            state.navigation.altitudeAmslMetres = highLatency.altitude;
            state.hasPosition = true;
        }
        state.navigation.speedMetresPerSecond =
            highLatency.groundspeed / 5.0;
        // HIGH_LATENCY2 heading is half-degrees; use it as CoT course instead
        // of preserving MP10's stale groundcourse value.
        state.navigation.courseDegrees =
            normaliseDegrees(highLatency.heading * 2.0);
        return true;
    }
    default:
        return false;
    }
}

void CotOutputService::emitNow()
{
    if (!m_transport || m_state != State::Emitting) {
        return;
    }
    const CotOutputTransport::State transportState = m_transport->state();
    if (transportState != CotOutputTransport::State::Ready
        && transportState != CotOutputTransport::State::Listening) {
        return;
    }

    const quint64 tickGeneration = m_generation;
    if (m_tickGeneration == tickGeneration) {
        return;
    }
    m_tickGeneration = tickGeneration;
    const quint64 endpointGeneration = m_endpointGeneration;
    CotOutputTransport *const transport = m_transport.get();
    const QList<VehicleEndpoint> endpoints = m_activeEndpoints;
    const CotEventSettings eventSettings = m_settings.event;
    const QHash<int, CotIdentityOverride> identities = m_settings.identities;
    const Clock clock = m_clock;
    const Sender sender = m_sender;
    QPointer<CotOutputService> guard(this);
    const QDateTime timestampUtc = clock().toUTC();
    if (!guard || m_generation != tickGeneration
        || !m_transport || m_transport.get() != transport) {
        return;
    }

    incrementSaturated(m_ticks);
    m_preview.clear();
    m_previewTruncated = false;
    const bool transportHasNoPeer =
        transportState == CotOutputTransport::State::Listening;

    for (const VehicleEndpoint &endpoint : endpoints) {
        const auto currentState = m_endpointStates.constFind(endpoint);
        if (currentState == m_endpointStates.constEnd()
            || !currentState.value().hasPosition) {
            continue;
        }

        const auto identity = identities.constFind(endpoint.systemId);
        const CotIdentityOverride *identityOverride =
            identity == identities.constEnd() ? nullptr : &identity.value();
        QString xml;
        QString serializationError;
        if (!CotEventSerializer::TrySerialize(
                eventSettings, endpoint.systemId, endpoint.componentId,
                currentState.value().navigation, timestampUtc, &xml,
                &serializationError, identityOverride)) {
            incrementSaturated(m_eventsDropped);
            recordTransportError(serializationError);
            m_text = boundedStatusText(transport->statusText()
                + QStringLiteral(" Warning: ") + serializationError);
            continue;
        }
        const int separator = m_preview.isEmpty() ? 0 : 1;
        if (m_preview.size() + separator + xml.size()
            <= MaximumPreviewCharacters) {
            if (separator != 0) {
                m_preview += QLatin1Char('\n');
            }
            m_preview += xml;
        } else {
            m_previewTruncated = true;
        }
        incrementSaturated(m_eventsGenerated);

        if (transportHasNoPeer) {
            m_lastSendResult = CotOutputTransport::SendResult::NoPeer;
            incrementSaturated(m_eventsWithoutPeer);
            continue;
        }

        QByteArray payload = xml.toUtf8();
        payload.append('\n');
        const quint64 errorsBeforeSend = m_errors;
        const CotOutputTransport::SendResult result =
            sender(transport, payload);
        if (!guard) {
            return;
        }
        if (m_generation != tickGeneration
            || m_endpointGeneration != endpointGeneration
            || !m_transport || m_transport.get() != transport) {
            if (m_tickGeneration == tickGeneration) {
                m_tickGeneration = 0;
            }
            return;
        }
        m_lastSendResult = result;
        switch (result) {
        case CotOutputTransport::SendResult::Sent:
            incrementSaturated(m_eventsSent);
            break;
        case CotOutputTransport::SendResult::NoPeer:
            incrementSaturated(m_eventsWithoutPeer);
            break;
        case CotOutputTransport::SendResult::InvalidPayload:
        case CotOutputTransport::SendResult::IoError:
            incrementSaturated(m_eventsDropped);
            if (m_errors == errorsBeforeSend) {
                recordTransportError(
                    result == CotOutputTransport::SendResult::InvalidPayload
                        ? QStringLiteral("CoT event payload was rejected.")
                        : QStringLiteral("CoT transport could not send an event."));
            }
            break;
        case CotOutputTransport::SendResult::NotReady:
        case CotOutputTransport::SendResult::Backpressure:
            incrementSaturated(m_eventsDropped);
            break;
        }
    }

    if (m_tickGeneration == tickGeneration) {
        m_tickGeneration = 0;
    }
    emit statusChanged();
}

void CotOutputService::handleTransportStateChanged(
    CotOutputTransport::State state)
{
    if (sender() != m_transport.get()) {
        return;
    }
    if (state == CotOutputTransport::State::Error) {
        const QString reason = m_transport->lastError().isEmpty()
            ? m_transport->statusText() : m_transport->lastError();
        recordTransportError(reason);
        fail(boundedStatusText(reason), reason);
        return;
    }
    if (state == CotOutputTransport::State::Stopped) {
        const QString reason =
            QStringLiteral("CoT transport stopped unexpectedly.");
        recordTransportError(reason);
        fail(reason, reason);
        return;
    }
    activateIfPrepared();
}

void CotOutputService::handleTransportStatusChanged(const QString &text)
{
    if (sender() != m_transport.get()) {
        return;
    }
    m_text = boundedStatusText(text);
    emit statusChanged();
}

void CotOutputService::handleTransportError(const QString &error)
{
    if (sender() != m_transport.get()) {
        return;
    }
    recordTransportError(error);
    if (m_transport->state() == CotOutputTransport::State::Error) {
        fail(boundedStatusText(error), error);
        return;
    }
    m_text = boundedStatusText(QStringLiteral("%1 Warning: %2")
        .arg(m_transport->statusText(), error));
    emit statusChanged();
}

void CotOutputService::recordTransportError(const QString &error)
{
    incrementSaturated(m_errors);
    m_lastError = boundedStatusText(error);
}

void CotOutputService::releaseTransport()
{
    if (!m_transport) {
        return;
    }
    CotOutputTransport *transport = m_transport.release();
    disconnect(transport, nullptr, this, nullptr);
    transport->stop();
    transport->deleteLater();
}

void CotOutputService::setStopped(const QString &text)
{
    ++m_generation;
    m_timer.stop();
    releaseTransport();
    m_linkId = -1;
    m_activeEndpoints.clear();
    m_endpointStates.clear();
    ++m_endpointGeneration;
    m_state = State::Stopped;
    m_text = boundedStatusText(text);
    emit statusChanged();
}

void CotOutputService::fail(const QString &text, const QString &error)
{
    ++m_generation;
    m_timer.stop();
    releaseTransport();
    m_linkId = -1;
    m_activeEndpoints.clear();
    m_endpointStates.clear();
    ++m_endpointGeneration;
    m_state = State::Failed;
    m_text = boundedStatusText(text);
    if (!error.isEmpty()) {
        m_lastError = boundedStatusText(error);
    }
    emit statusChanged();
}

void CotOutputService::forgetLink(int linkId)
{
    if (!m_transport || linkId != m_linkId) {
        return;
    }
    setStopped(SourceRemovedText(m_linkName));
}

QString CotOutputService::boundedStatusText(const QString &text)
{
    return text.left(MaximumStatusTextLength);
}

CotEventSettings CotOutputService::boundedEventSettings(
    const CotEventSettings &settings)
{
    return settings;
}

QHash<int, CotIdentityOverride> CotOutputService::boundedIdentities(
    const QHash<int, CotIdentityOverride> &identities)
{
    QHash<int, CotIdentityOverride> bounded;
    bounded.reserve(qMin(identities.size(), 256));
    for (auto it = identities.constBegin(); it != identities.constEnd(); ++it) {
        if (it.key() < 0 || it.key() > 255) {
            continue;
        }
        bounded.insert(it.key(), it.value());
    }
    return bounded;
}

void CotOutputService::incrementSaturated(quint64 &counter)
{
    if (counter != std::numeric_limits<quint64>::max()) {
        ++counter;
    }
}
