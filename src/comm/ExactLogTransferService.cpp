#include "ExactLogTransferService.h"

#include "ExactLinkTransmitter.h"

#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

#include <mavlink_helpers.h>

#include <algorithm>
#include <utility>

namespace {

class ScopeExit final
{
public:
    explicit ScopeExit(std::function<void()> callback)
        : m_callback(std::move(callback))
    {
    }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

    ~ScopeExit() { m_callback(); }

private:
    std::function<void()> m_callback;
};

QString defaultRouteError()
{
    return QStringLiteral(
        "The exact vehicle route is no longer safe or available.");
}

bool sameEntry(const ExactLogEntry &left, const ExactLogEntry &right)
{
    return left.id == right.id && left.numLogs == right.numLogs
        && left.lastLogNumber == right.lastLogNumber
        && left.timeUtc == right.timeUtc && left.size == right.size;
}

} // namespace

ExactLogTransferService::ExactLogTransferService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    QObject *parent)
    : ExactLogTransferService(
          registry, transmitter, std::move(routeValidator), Clock(),
          DefaultListTimeoutMs, DefaultListRetries,
          DefaultStreamSilenceMs, DefaultRepairSilenceMs,
          DefaultSilenceBudgetMs, parent)
{
}

ExactLogTransferService::ExactLogTransferService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    Clock clock,
    int listTimeoutMs,
    int listRetries,
    int streamSilenceMs,
    int repairSilenceMs,
    int silenceBudgetMs,
    QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_transmitter(transmitter)
    , m_routeValidator(std::move(routeValidator))
    , m_clock(std::move(clock))
    , m_listTimeoutMs(std::max(1, listTimeoutMs))
    , m_listRetries(std::max(0, listRetries))
    , m_streamSilenceMs(std::max(1, streamSilenceMs))
    , m_repairSilenceMs(std::max(1, repairSilenceMs))
    , m_silenceBudgetMs(std::max(1, silenceBudgetMs))
{
    m_monotonicClock.start();
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout,
            this, &ExactLogTransferService::timeout);

    qRegisterMetaType<ExactLogTransferToken>();
    qRegisterMetaType<ExactLogEntry>();
    qRegisterMetaType<ExactLogTransferResult>();
    qRegisterMetaType<ExactLogTransferService::StartResult>();
    qRegisterMetaType<ExactLogTransferService::Operation>();
    qRegisterMetaType<ExactLogTransferService::Outcome>();

    if (registry) {
        connect(registry, &SwarmTelemetryRegistry::endpointRetired,
                this,
                [this](SwarmVehicleInstanceLease vehicle,
                       SwarmTelemetryRegistry::RetirementReason) {
            if (m_active.token.isValid()
                && m_active.vehicle.sameInstance(vehicle)) {
                finishActive(
                    m_active.token, Outcome::LeaseRetired,
                    QStringLiteral(
                        "The exact vehicle instance was retired."));
            }
        });
        connect(registry, &QObject::destroyed, this, [this]() {
            m_registry = nullptr;
            if (m_active.token.isValid()) {
                finishActive(
                    m_active.token, Outcome::LeaseRetired,
                    QStringLiteral("The telemetry registry disappeared."));
            }
        });
    }
    if (transmitter) {
        connect(transmitter, &QObject::destroyed, this, [this]() {
            m_transmitter = nullptr;
            if (m_active.token.isValid()) {
                finishActive(
                    m_active.token, Outcome::TransportFailed,
                    QStringLiteral("The exact MAVLink transmitter disappeared."));
            }
        });
    }
}

ExactLogTransferService::~ExactLogTransferService()
{
    m_shuttingDown = true;
    disarmTimeout();
    if (m_active.ownerDestroyed) {
        disconnect(m_active.ownerDestroyed);
    }
    if (m_active.file) {
        m_active.file->cancelWriting();
    }
}

void ExactLogTransferService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    assertServiceThread();
    if (!busy() && !m_shuttingDown && systemId != 0
        && componentId != 0) {
        m_localSystemId = systemId;
        m_localComponentId = componentId;
    }
}

void ExactLogTransferService::setRouteIdentityProvider(
    RouteIdentityProvider provider)
{
    assertServiceThread();
    if (!busy() && !m_shuttingDown) {
        m_routeIdentityProvider = std::move(provider);
    }
}

ExactLogTransferService::StartResult ExactLogTransferService::requestList(
    QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    ExactLogTransferToken *token,
    QString *error)
{
    return beginOperation(
        owner, vehicle, Operation::List, QString(), 0, 0, token, error);
}

ExactLogTransferService::StartResult ExactLogTransferService::startDownload(
    QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    quint16 logId,
    quint32 advertisedSize,
    const QString &destinationPath,
    ExactLogTransferToken *token,
    QString *error)
{
    return beginOperation(
        owner, vehicle, Operation::Download, destinationPath, logId,
        advertisedSize, token, error);
}

ExactLogTransferService::StartResult ExactLogTransferService::erase(
    QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    ExactLogTransferToken *token,
    QString *error)
{
    return beginOperation(
        owner, vehicle, Operation::Erase, QString(), 0, 0, token, error);
}

ExactLogTransferService::StartResult ExactLogTransferService::beginOperation(
    QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    Operation operation,
    const QString &destinationPath,
    quint16 logId,
    quint32 advertisedSize,
    ExactLogTransferToken *token,
    QString *error)
{
    assertServiceThread();
    if (token) {
        *token = ExactLogTransferToken();
    }
    if (error) {
        error->clear();
    }
    if (m_shuttingDown) {
        return StartResult::ShuttingDown;
    }
    if (busy()) {
        return StartResult::Busy;
    }
    if (!owner) {
        return StartResult::InvalidOwner;
    }
    if (!vehicle.isValid()) {
        return StartResult::InvalidLease;
    }
    if (!token || operation == Operation::None
        || (operation == Operation::Download
            && destinationPath.isEmpty())) {
        return StartResult::InvalidArgument;
    }
    if (m_nextTokenId == std::numeric_limits<quint64>::max()) {
        return StartResult::IdentifierExhausted;
    }
    if (!m_registry || !m_transmitter) {
        return StartResult::TransportUnavailable;
    }
    if (!exactLeaseIsCurrent(vehicle)) {
        return StartResult::InvalidLease;
    }

    m_startInFlight = true;
    const QPointer<ExactLogTransferService> startGuard(this);
    ScopeExit reservation([startGuard]() {
        if (startGuard) {
            startGuard->m_startInFlight = false;
        }
    });

    const QPointer<QObject> guardedOwner(owner);
    QString routeIdentity;
    QString routeError;
    if (!validateStartRoute(vehicle, &routeIdentity, &routeError)) {
        if (error) {
            *error = routeError;
        }
        return startGuard ? StartResult::UnsafeRoute
                          : StartResult::TransportUnavailable;
    }
    if (!startGuard) {
        return StartResult::TransportUnavailable;
    }
    if (m_shuttingDown) {
        return StartResult::ShuttingDown;
    }
    if (!guardedOwner) {
        return StartResult::InvalidOwner;
    }

    std::unique_ptr<QSaveFile> file;
    if (operation == Operation::Download) {
        file = std::make_unique<QSaveFile>(destinationPath);
        file->setDirectWriteFallback(false);
        if (!file->open(QIODevice::WriteOnly)) {
            if (error) {
                *error = file->errorString();
            }
            return StartResult::IoError;
        }
    }

    // File creation and injected policy callbacks are preflight barriers.
    QString revalidatedIdentity;
    if (!guardedOwner
        || !validateStartRoute(
            vehicle, &revalidatedIdentity, &routeError)
        || revalidatedIdentity != routeIdentity) {
        if (file) {
            file->cancelWriting();
        }
        if (error) {
            *error = guardedOwner
                ? (routeError.isEmpty() ? defaultRouteError() : routeError)
                : QStringLiteral("The log operation owner disappeared.");
        }
        return startGuard
            ? (guardedOwner ? StartResult::UnsafeRoute
                            : StartResult::InvalidOwner)
            : StartResult::TransportUnavailable;
    }
    if (!startGuard) {
        return StartResult::TransportUnavailable;
    }
    if (m_shuttingDown) {
        if (file) {
            file->cancelWriting();
        }
        return StartResult::ShuttingDown;
    }

    ++m_nextTokenId;
    m_active = ActiveOperation();
    m_active.token.id = m_nextTokenId;
    m_active.vehicle = vehicle;
    m_active.operation = operation;
    m_active.owner = guardedOwner;
    m_active.routeIdentity = routeIdentity;
    m_active.destinationPath = destinationPath;
    m_active.logId = logId;
    m_active.advertisedSize = advertisedSize;
    m_active.listRetriesRemaining = m_listRetries;
    m_active.localSystemId = m_localSystemId;
    m_active.localComponentId = m_localComponentId;
    m_active.file = std::move(file);
    m_active.tracker.reset();
    m_active.silenceBudgetRemainingMs = m_silenceBudgetMs;
    const ExactLogTransferToken activeToken = m_active.token;
    m_active.ownerDestroyed = connect(
        guardedOwner.data(), &QObject::destroyed, this,
        [this, activeToken]() {
            if (tokenIsCurrent(activeToken) && !m_finishing) {
                finishActive(
                    activeToken, Outcome::Cancelled,
                    QStringLiteral("The log operation owner was destroyed."));
            }
        });

    // The token is visible before every signal and before the first writer;
    // synchronous transports may deliver a terminal reply inside sendMessage.
    *token = activeToken;
    emit busyChanged(true);
    if (!startGuard || !tokenIsCurrent(activeToken)) {
        return StartResult::Started;
    }

    QString sendError;
    switch (operation) {
    case Operation::List:
        if (!sendListRequest(activeToken, &sendError)
            && startGuard && tokenIsCurrent(activeToken)) {
            finishActive(activeToken, Outcome::TransportFailed, sendError);
        }
        break;
    case Operation::Download:
        if (!sendDataRequest(
                activeToken,
                {0, std::numeric_limits<quint32>::max()}, &sendError)
            && startGuard && tokenIsCurrent(activeToken)) {
            finishActive(activeToken, Outcome::TransportFailed, sendError);
        }
        break;
    case Operation::Erase: {
        const bool first = sendErase(activeToken, &sendError);
        if (!startGuard || !tokenIsCurrent(activeToken)) {
            break;
        }
        const bool second = first && sendErase(activeToken, &sendError);
        if (!startGuard || !tokenIsCurrent(activeToken)) {
            break;
        }
        finishActive(
            activeToken,
            first && second ? Outcome::SubmittedUnconfirmed
                            : Outcome::TransportFailed,
            first && second ? QString() : sendError);
        break;
    }
    case Operation::None:
        break;
    }
    return StartResult::Started;
}

bool ExactLogTransferService::cancel(
    const ExactLogTransferToken &token, const QString &reason)
{
    assertServiceThread();
    if (!tokenIsCurrent(token) || m_finishing) {
        return false;
    }
    finishActive(
        token, Outcome::Cancelled,
        reason.isEmpty() ? QStringLiteral("The log operation was cancelled.")
                         : reason);
    return true;
}

void ExactLogTransferService::shutdown()
{
    assertServiceThread();
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    if (m_active.token.isValid() && !m_finishing) {
        finishActive(
            m_active.token, Outcome::Cancelled,
            QStringLiteral("The log service is shutting down."));
    }
}

bool ExactLogTransferService::exactLeaseIsCurrent(
    const SwarmVehicleInstanceLease &vehicle) const
{
    return m_registry && vehicle.isValid()
        && m_registry->currentLinkSessionEpoch(vehicle.endpoint.linkId)
            == vehicle.linkSessionEpoch
        && m_registry->validateLease(vehicle);
}

bool ExactLogTransferService::validateStartRoute(
    const SwarmVehicleInstanceLease &vehicle,
    QString *identity,
    QString *error)
{
    if (identity) {
        identity->clear();
    }
    if (error) {
        error->clear();
    }
    if (!m_registry || !m_transmitter || !exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease is stale.");
        }
        return false;
    }
    const RouteValidator validator = m_routeValidator;
    if (!validator) {
        if (error) {
            *error = QStringLiteral("No exact log route policy is installed.");
        }
        return false;
    }

    QPointer<ExactLogTransferService> guard(this);
    QString callbackError;
    const bool accepted = validator(vehicle, &callbackError);
    if (!guard) {
        if (error) {
            *error = QStringLiteral(
                "The log service was destroyed during route validation.");
        }
        return false;
    }
    if (!accepted || !exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = callbackError.isEmpty()
                ? defaultRouteError() : callbackError;
        }
        return false;
    }

    const RouteIdentityProvider provider = m_routeIdentityProvider;
    if (provider) {
        const QString sampled = provider(vehicle);
        if (!guard) {
            if (error) {
                *error = QStringLiteral(
                    "The log service was destroyed while resolving the route identity.");
            }
            return false;
        }
        if (sampled.isEmpty() || !exactLeaseIsCurrent(vehicle)) {
            if (error) {
                *error = QStringLiteral(
                    "The selected physical route identity is unavailable.");
            }
            return false;
        }
        if (identity) {
            *identity = sampled;
        }
    }
    return true;
}

bool ExactLogTransferService::validateActiveRoute(
    const ExactLogTransferToken &token, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!tokenIsCurrent(token) || !m_registry || !m_transmitter) {
        if (error) {
            *error = defaultRouteError();
        }
        return false;
    }
    const SwarmVehicleInstanceLease vehicle = m_active.vehicle;
    const QString capturedIdentity = m_active.routeIdentity;
    if (!exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease was retired.");
        }
        return false;
    }

    const RouteValidator validator = m_routeValidator;
    if (!validator) {
        if (error) {
            *error = QStringLiteral("The exact log route policy disappeared.");
        }
        return false;
    }
    QPointer<ExactLogTransferService> guard(this);
    QString callbackError;
    if (!validator(vehicle, &callbackError)) {
        if (guard && error) {
            *error = callbackError.isEmpty()
                ? defaultRouteError() : callbackError;
        }
        return false;
    }
    if (!guard || !tokenIsCurrent(token)
        || !m_active.vehicle.sameInstance(vehicle)
        || !exactLeaseIsCurrent(vehicle)) {
        if (guard && error) {
            *error = QStringLiteral(
                "The exact vehicle changed during route validation.");
        }
        return false;
    }

    const RouteIdentityProvider provider = m_routeIdentityProvider;
    if (provider) {
        const QString sampled = provider(vehicle);
        if (!guard || !tokenIsCurrent(token)
            || sampled.isEmpty() || sampled != capturedIdentity
            || !exactLeaseIsCurrent(vehicle)) {
            if (guard && error) {
                *error = QStringLiteral(
                    "The physical route identity changed during the log operation.");
            }
            return false;
        }
    } else if (!capturedIdentity.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "The physical route identity provider disappeared.");
        }
        return false;
    }
    return true;
}

bool ExactLogTransferService::tokenIsCurrent(
    const ExactLogTransferToken &token) const noexcept
{
    return token.isValid() && m_active.token == token;
}

bool ExactLogTransferService::sendListRequest(
    const ExactLogTransferToken &token, QString *error)
{
    const QPointer<ExactLogTransferService> guard(this);
    if (!validateActiveRoute(token, error)) {
        if (guard && tokenIsCurrent(token)) {
            failForRouteChange(token, error ? *error : QString());
        }
        return false;
    }
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_LOG_REQUEST_LIST;
    message.len = MAVLINK_MSG_ID_LOG_REQUEST_LIST_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint16_t(payload, 0, 0);
    _mav_put_uint16_t(payload, 2, std::numeric_limits<quint16>::max());
    _mav_put_uint8_t(
        payload, 4, static_cast<quint8>(m_active.vehicle.endpoint.systemId));
    _mav_put_uint8_t(
        payload, 5, static_cast<quint8>(m_active.vehicle.endpoint.componentId));

    const quint64 revision = ++m_active.activityRevision;
    if (!sendMessage(token, message, error, false)) {
        return false;
    }
    if (!guard) {
        return false;
    }
    if (tokenIsCurrent(token) && m_active.activityRevision == revision) {
        armTimeout(m_listTimeoutMs);
    }
    return true;
}

bool ExactLogTransferService::sendDataRequest(
    const ExactLogTransferToken &token,
    const LogDownloadRequest &request,
    QString *error)
{
    const QPointer<ExactLogTransferService> guard(this);
    if (!validateActiveRoute(token, error)) {
        if (guard && tokenIsCurrent(token)) {
            failForRouteChange(token, error ? *error : QString());
        }
        return false;
    }
    if (request.count == 0) {
        if (error) {
            *error = QStringLiteral("The tracker produced an empty repair request.");
        }
        return false;
    }
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_LOG_REQUEST_DATA;
    message.len = MAVLINK_MSG_ID_LOG_REQUEST_DATA_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint32_t(payload, 0, request.offset);
    _mav_put_uint32_t(payload, 4, request.count);
    _mav_put_uint16_t(payload, 8, m_active.logId);
    _mav_put_uint8_t(
        payload, 10, static_cast<quint8>(m_active.vehicle.endpoint.systemId));
    _mav_put_uint8_t(
        payload, 11, static_cast<quint8>(m_active.vehicle.endpoint.componentId));

    m_active.lastRequest = request;
    const quint64 revision = ++m_active.activityRevision;
    if (!sendMessage(token, message, error, false)) {
        return false;
    }
    if (!guard) {
        return false;
    }
    if (tokenIsCurrent(token) && m_active.activityRevision == revision) {
        armTimeout(currentDownloadSilenceWindow());
    }
    return true;
}

bool ExactLogTransferService::sendErase(
    const ExactLogTransferToken &token, QString *error)
{
    const QPointer<ExactLogTransferService> guard(this);
    if (!validateActiveRoute(token, error)) {
        if (guard && tokenIsCurrent(token)) {
            failForRouteChange(token, error ? *error : QString());
        }
        return false;
    }
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_LOG_ERASE;
    message.len = MAVLINK_MSG_ID_LOG_ERASE_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint8_t(
        payload, 0, static_cast<quint8>(m_active.vehicle.endpoint.systemId));
    _mav_put_uint8_t(
        payload, 1, static_cast<quint8>(m_active.vehicle.endpoint.componentId));
    return sendMessage(token, message, error, false);
}

bool ExactLogTransferService::sendEndBestEffort(
    const ExactLogTransferToken &token)
{
    if (!tokenIsCurrent(token)
        || (m_active.operation != Operation::List
            && m_active.operation != Operation::Download)) {
        return false;
    }
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_LOG_REQUEST_END;
    message.len = MAVLINK_MSG_ID_LOG_REQUEST_END_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint8_t(
        payload, 0, static_cast<quint8>(m_active.vehicle.endpoint.systemId));
    _mav_put_uint8_t(
        payload, 1, static_cast<quint8>(m_active.vehicle.endpoint.componentId));
    return sendMessage(token, message, nullptr, true);
}

bool ExactLogTransferService::sendMessage(
    const ExactLogTransferToken &token,
    mavlink_message_t message,
    QString *error,
    bool validateRoute)
{
    if (error) {
        error->clear();
    }
    if (validateRoute && !validateActiveRoute(token, error)) {
        return false;
    }
    if (!tokenIsCurrent(token) || !m_transmitter
        || !exactLeaseIsCurrent(m_active.vehicle)) {
        if (error) {
            *error = QStringLiteral(
                "The exact log route changed before transmission.");
        }
        return false;
    }

    const SwarmVehicleInstanceLease vehicle = m_active.vehicle;
    const quint8 localSystemId = m_active.localSystemId;
    const quint8 localComponentId = m_active.localComponentId;
    QPointer<ExactLinkTransmitter> transmitter = m_transmitter;
    QPointer<ExactLogTransferService> guard(this);
    const ExactLinkTransmitter::SendResult result =
        transmitter->sendMessage(
            vehicle.endpoint.linkId, localSystemId, localComponentId,
            message);
    if (!guard) {
        return false;
    }
    if (!tokenIsCurrent(token)) {
        if (error) {
            *error = QStringLiteral(
                "The log operation ended during transmission.");
        }
        return false;
    }
    if (result != ExactLinkTransmitter::SendResult::Sent) {
        if (error) {
            *error = QStringLiteral(
                "The exact MAVLink transport rejected the log request.");
        }
        return false;
    }
    return true;
}

void ExactLogTransferService::observeMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    assertServiceThread();
    if (!m_active.token.isValid() || m_finishing) {
        return;
    }
    const ExactLogTransferToken token = m_active.token;
    const SwarmVehicleInstanceLease vehicle = m_active.vehicle;
    if (linkId != vehicle.endpoint.linkId
        || linkSessionEpoch != vehicle.linkSessionEpoch
        || message.sysid != vehicle.endpoint.systemId
        || message.compid != vehicle.endpoint.componentId) {
        return;
    }

    QString routeError;
    QPointer<ExactLogTransferService> guard(this);
    if (!validateActiveRoute(token, &routeError)) {
        if (guard && tokenIsCurrent(token)) {
            failForRouteChange(token, routeError);
        }
        return;
    }
    if (!guard || !tokenIsCurrent(token)) {
        return;
    }

    if (m_active.operation == Operation::List) {
        if (message.msgid == MAVLINK_MSG_ID_LOG_ENTRY) {
            observeListEntry(token, message);
        } else if (message.msgid == MAVLINK_MSG_ID_LOG_DATA) {
            finishActive(
                token, Outcome::ProtocolError,
                QStringLiteral(
                    "LOG_DATA arrived while a log-list request owned the protocol."));
        }
    } else if (m_active.operation == Operation::Download
               && message.msgid == MAVLINK_MSG_ID_LOG_DATA) {
        observeLogData(token, message);
    }
}

void ExactLogTransferService::observeListEntry(
    const ExactLogTransferToken &token,
    const mavlink_message_t &message)
{
    mavlink_log_entry_t payload{};
    mavlink_msg_log_entry_decode(&message, &payload);
    if (payload.num_logs == 0 && payload.last_log_num == 0) {
        m_active.entries.clear();
        m_active.expectedListCount = 0;
        m_active.lastLogNumber = 0;
        finishActive(token, Outcome::Completed);
        return;
    }
    if (payload.num_logs == 0) {
        return;
    }

    const ExactLogEntry entry{
        payload.id, payload.num_logs, payload.last_log_num,
        payload.time_utc, payload.size};
    const auto existing = m_active.entries.constFind(entry.id);
    const bool newEntry = existing == m_active.entries.constEnd();
    const bool changedEntry = !newEntry && !sameEntry(existing.value(), entry);
    if (newEntry || changedEntry) {
        m_active.entries.insert(entry.id, entry);
    }
    m_active.expectedListCount = payload.num_logs;
    m_active.lastLogNumber = payload.last_log_num;

    if (newEntry) {
        ++m_active.activityRevision;
        armTimeout(m_listTimeoutMs);
        QPointer<ExactLogTransferService> guard(this);
        emit progress(
            token, static_cast<qulonglong>(m_active.entries.size()),
            payload.num_logs, true);
        if (!guard || !tokenIsCurrent(token)) {
            return;
        }
    }
    if (m_active.entries.size() >= payload.num_logs) {
        finishActive(token, Outcome::Completed);
    }
}

void ExactLogTransferService::observeLogData(
    const ExactLogTransferToken &token,
    const mavlink_message_t &message)
{
    mavlink_log_data_t payload{};
    mavlink_msg_log_data_decode(&message, &payload);
    if (payload.id != m_active.logId
        || payload.count > LogDownloadTracker::PacketSize) {
        return;
    }
    const quint64 end = quint64(payload.ofs) + payload.count;
    if (end > std::numeric_limits<quint32>::max()) {
        return;
    }
    const std::optional<quint32> totalBefore =
        m_active.tracker.totalLength();
    if (totalBefore && end > *totalBefore) {
        return;
    }

    const QByteArray data(
        reinterpret_cast<const char *>(payload.data), payload.count);
    const quint64 writeLimitBefore = immediateWriteLimit();
    const bool initiallyDeferred = end > writeLimitBefore;
    if (initiallyDeferred && payload.count > 0) {
        int additionalBytes = 0;
        for (int index = 0; index < data.size(); ++index) {
            if (!m_active.deferredBytes.contains(
                    payload.ofs + static_cast<quint32>(index))) {
                ++additionalBytes;
            }
        }
        if (m_active.deferredBytes.size() + additionalBytes
            > MaximumDeferredBytes) {
            finishActive(
                token, Outcome::ProtocolError,
                QStringLiteral(
                    "Too many deferred out-of-order log bytes were received."));
            return;
        }
    }

    const quint64 coveredBefore = m_active.tracker.coveredBytes();
    LogDownloadTracker candidateTracker = m_active.tracker;
    const LogDownloadTracker::AddResult added =
        candidateTracker.add(payload.ofs, payload.count, !totalBefore);
    if (added == LogDownloadTracker::AddResult::InvalidRange) {
        return;
    }
    if (added == LogDownloadTracker::AddResult::RangeCapacityExceeded) {
        finishActive(
            token, Outcome::ProtocolError,
            QStringLiteral(
                "The log stream exceeded the bounded range capacity."));
        return;
    }

    const std::optional<quint32> totalAfter =
        candidateTracker.totalLength();
    const quint64 coveredAfter = candidateTracker.coveredBytes();
    const bool madeProgress = coveredAfter > coveredBefore
        || (!totalBefore && totalAfter);
    if (!madeProgress) {
        // Tracker end inference changes when a stream continues. A stale
        // duplicate is not continuation: discard its speculative tracker
        // mutation and do not extend the silence budget.
        return;
    }
    m_active.tracker = std::move(candidateTracker);
    QString ioError;

    if (totalAfter) {
        auto deferred = m_active.deferredBytes.lowerBound(*totalAfter);
        while (deferred != m_active.deferredBytes.end()) {
            deferred = m_active.deferredBytes.erase(deferred);
        }
    }

    if (madeProgress && !data.isEmpty()) {
        if (end <= immediateWriteLimit()) {
            QByteArray stableData = data;
            for (int index = 0; index < stableData.size(); ++index) {
                const quint32 position =
                    payload.ofs + static_cast<quint32>(index);
                const auto deferred =
                    m_active.deferredBytes.constFind(position);
                if (deferred != m_active.deferredBytes.constEnd()) {
                    stableData[index] = deferred.value();
                }
            }
            if (!writeData(payload.ofs, stableData, &ioError)) {
                finishActive(token, Outcome::IoError, ioError);
                return;
            }
            for (int index = 0; index < stableData.size(); ++index) {
                m_active.deferredBytes.remove(
                    payload.ofs + static_cast<quint32>(index));
            }
        } else if (!storeDeferredData(payload.ofs, data, &ioError)) {
            finishActive(token, Outcome::ProtocolError, ioError);
            return;
        }
    }
    if (!flushDeferred(&ioError)) {
        finishActive(token, Outcome::IoError, ioError);
        return;
    }

    if (madeProgress) {
        ++m_active.activityRevision;
        m_active.silenceBudgetRemainingMs = m_silenceBudgetMs;
        armTimeout(currentDownloadSilenceWindow());
        const quint64 reportedTotal = totalAfter
            ? *totalAfter : m_active.advertisedSize;
        QPointer<ExactLogTransferService> guard(this);
        const quint64 revision = m_active.activityRevision;
        emit progress(token, coveredAfter, reportedTotal,
                      totalAfter.has_value());
        if (!guard || !tokenIsCurrent(token)
            || m_active.activityRevision != revision) {
            return;
        }
    }

    if (m_active.tracker.complete()) {
        // A progress receiver may synchronously change a UDP peer identity.
        // Revalidate at the last reentrant boundary before atomic commit.
        QString routeError;
        QPointer<ExactLogTransferService> commitGuard(this);
        if (!validateActiveRoute(token, &routeError)) {
            if (commitGuard && tokenIsCurrent(token)) {
                failForRouteChange(token, routeError);
            }
            return;
        }
        if (!commitGuard || !tokenIsCurrent(token)) {
            return;
        }
        if (!commitDownload(token, &ioError)) {
            finishActive(token, Outcome::IoError, ioError);
        } else {
            finishActive(token, Outcome::Completed);
        }
        return;
    }

    if (madeProgress && totalAfter) {
        const quint64 packetEnd = end;
        const quint64 requestedEnd =
            quint64(m_active.lastRequest.offset) + m_active.lastRequest.count;
        if (packetEnd >= requestedEnd) {
            const LogDownloadRequest next =
                m_active.tracker.nextRequest(MaximumRepairRequest);
            const QPointer<ExactLogTransferService> sendGuard(this);
            if (next.count != 0
                && !sendDataRequest(token, next, &ioError)
                && sendGuard && tokenIsCurrent(token)) {
                finishActive(token, Outcome::TransportFailed, ioError);
            }
        }
    }
}

bool ExactLogTransferService::storeDeferredData(
    quint32 offset, const QByteArray &data, QString *error)
{
    int additionalBytes = 0;
    for (int index = 0; index < data.size(); ++index) {
        if (!m_active.deferredBytes.contains(
                offset + static_cast<quint32>(index))) {
            ++additionalBytes;
        }
    }
    if (m_active.deferredBytes.size() + additionalBytes
        > MaximumDeferredBytes) {
        if (error) {
            *error = QStringLiteral(
                "The deferred log-data byte limit was exceeded.");
        }
        return false;
    }
    for (int index = 0; index < data.size(); ++index) {
        const quint32 position = offset + static_cast<quint32>(index);
        if (!m_active.deferredBytes.contains(position)) {
            m_active.deferredBytes.insert(position, data.at(index));
        }
    }
    return true;
}

bool ExactLogTransferService::writeData(
    quint32 offset, const QByteArray &data, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!m_active.file || !m_active.file->isOpen()
        || !m_active.file->seek(offset)) {
        if (error) {
            *error = m_active.file
                ? m_active.file->errorString()
                : QStringLiteral("The atomic log output is unavailable.");
        }
        return false;
    }
    if (!data.isEmpty() && m_active.file->write(data) != data.size()) {
        if (error) {
            *error = m_active.file->errorString();
        }
        return false;
    }
    return true;
}

bool ExactLogTransferService::flushDeferred(QString *error)
{
    const quint32 frontier = m_active.tracker.frontierEnd();
    while (!m_active.deferredBytes.isEmpty()
           && m_active.deferredBytes.constBegin().key() < frontier) {
        auto current = m_active.deferredBytes.constBegin();
        const quint32 start = current.key();
        quint32 expected = start;
        QByteArray bytes;
        QList<quint32> positions;
        while (current != m_active.deferredBytes.constEnd()
               && current.key() == expected && current.key() < frontier) {
            positions.append(current.key());
            bytes.append(current.value());
            ++expected;
            ++current;
        }
        if (!writeData(start, bytes, error)) {
            return false;
        }
        for (quint32 position : positions) {
            m_active.deferredBytes.remove(position);
        }
    }
    return true;
}

quint64 ExactLogTransferService::immediateWriteLimit() const
{
    const quint64 frontierLimit =
        quint64(m_active.tracker.frontierEnd())
        + LogDownloadTracker::InferenceSlack;
    const quint64 advertisedLimit =
        quint64(m_active.advertisedSize)
        + LogDownloadTracker::InferenceSlack;
    return std::min(
        std::max(frontierLimit, advertisedLimit),
        quint64(std::numeric_limits<quint32>::max()));
}

bool ExactLogTransferService::commitDownload(
    const ExactLogTransferToken &token, QString *error)
{
    if (error) {
        error->clear();
    }
    const std::optional<quint32> total =
        m_active.tracker.totalLength();
    if (!tokenIsCurrent(token) || !m_active.file
        || !m_active.tracker.complete() || !total
        || !m_active.deferredBytes.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "The log cannot be committed before every byte is durable.");
        }
        return false;
    }
    if (!m_active.file->resize(*total)) {
        if (error) {
            *error = m_active.file->errorString();
        }
        return false;
    }
    if (!m_active.file->commit()) {
        if (error) {
            *error = m_active.file->errorString();
        }
        return false;
    }
    return true;
}

void ExactLogTransferService::timeout()
{
    assertServiceThread();
    if (!m_active.token.isValid() || m_finishing) {
        return;
    }
    const ExactLogTransferToken token = m_active.token;
    const qint64 now = nowMs();
    if (m_active.deadlineMs > now) {
        const qint64 remaining = m_active.deadlineMs - now;
        m_timeoutTimer.start(static_cast<int>(std::min<qint64>(
            remaining, std::numeric_limits<int>::max())));
        return;
    }

    QString routeError;
    QPointer<ExactLogTransferService> guard(this);
    if (!validateActiveRoute(token, &routeError)) {
        if (guard && tokenIsCurrent(token)) {
            failForRouteChange(token, routeError);
        }
        return;
    }
    if (!guard || !tokenIsCurrent(token)) {
        return;
    }

    QString sendError;
    if (m_active.operation == Operation::List) {
        if (m_active.listRetriesRemaining <= 0) {
            finishActive(
                token, Outcome::TimedOut,
                QStringLiteral("Timed out while requesting the log list."));
            return;
        }
        --m_active.listRetriesRemaining;
        if (!sendListRequest(token, &sendError)
            && guard && tokenIsCurrent(token)) {
            finishActive(token, Outcome::TransportFailed, sendError);
        }
        return;
    }
    if (m_active.operation != Operation::Download) {
        return;
    }

    // Silence is a time budget, not a retry count. Every expired window
    // consumes its actual duration; only newly covered bytes reset it.
    const int elapsedWindow = currentDownloadSilenceWindow();
    if (m_active.silenceBudgetRemainingMs <= elapsedWindow) {
        finishActive(
            token, Outcome::TimedOut,
            QStringLiteral(
                "The log stream stopped before every byte was received."));
        return;
    }
    m_active.silenceBudgetRemainingMs -= elapsedWindow;

    if (m_active.tracker.acceptPendingTotalLength()) {
        ++m_active.activityRevision;
        const std::optional<quint32> total =
            m_active.tracker.totalLength();
        if (total) {
            auto deferred = m_active.deferredBytes.lowerBound(*total);
            while (deferred != m_active.deferredBytes.end()) {
                deferred = m_active.deferredBytes.erase(deferred);
            }
        }
        if (!flushDeferred(&sendError)) {
            finishActive(token, Outcome::IoError, sendError);
            return;
        }
        if (m_active.tracker.complete()) {
            QString commitRouteError;
            QPointer<ExactLogTransferService> commitGuard(this);
            if (!validateActiveRoute(token, &commitRouteError)) {
                if (commitGuard && tokenIsCurrent(token)) {
                    failForRouteChange(token, commitRouteError);
                }
                return;
            }
            if (!commitGuard || !tokenIsCurrent(token)) {
                return;
            }
            if (!commitDownload(token, &sendError)) {
                finishActive(token, Outcome::IoError, sendError);
            } else {
                finishActive(token, Outcome::Completed);
            }
            return;
        }
        const LogDownloadRequest request =
            m_active.tracker.nextRequest(MaximumRepairRequest);
        if (!sendDataRequest(token, request, &sendError)
            && guard && tokenIsCurrent(token)) {
            finishActive(token, Outcome::TransportFailed, sendError);
        }
        return;
    }

    const LogDownloadRequest request =
        m_active.tracker.nextRequest(MaximumRepairRequest);
    if (!sendDataRequest(token, request, &sendError)
        && guard && tokenIsCurrent(token)) {
        finishActive(token, Outcome::TransportFailed, sendError);
    }
}

void ExactLogTransferService::armTimeout(int intervalMs)
{
    if (!m_active.token.isValid()) {
        return;
    }
    const int bounded = std::max(1, intervalMs);
    m_active.deadlineMs = nowMs() + bounded;
    m_timeoutTimer.start(bounded);
}

void ExactLogTransferService::disarmTimeout()
{
    m_timeoutTimer.stop();
    m_active.deadlineMs = -1;
}

int ExactLogTransferService::currentDownloadSilenceWindow() const
{
    return m_active.tracker.totalLength()
        ? m_repairSilenceMs : m_streamSilenceMs;
}

void ExactLogTransferService::finishActive(
    const ExactLogTransferToken &token,
    Outcome outcome,
    const QString &error)
{
    if (!tokenIsCurrent(token) || m_finishing) {
        return;
    }
    m_finishing = true;
    disarmTimeout();

    ExactLogTransferResult result;
    result.token = token;
    result.vehicle = m_active.vehicle;
    result.operation = m_active.operation;
    result.outcome = outcome;
    result.errorString = error;
    result.destinationPath = m_active.destinationPath;
    if (m_active.operation == Operation::List
        && outcome == Outcome::Completed) {
        result.entries = completedEntries();
        result.covered = static_cast<quint64>(result.entries.size());
        result.total = m_active.expectedListCount;
        result.totalKnown = true;
    } else if (m_active.operation == Operation::Download) {
        result.covered = m_active.tracker.coveredBytes();
        const std::optional<quint32> total =
            m_active.tracker.totalLength();
        result.total = total ? *total : m_active.advertisedSize;
        result.totalKnown = total.has_value();
    }

    // LOG_REQUEST_END is best effort, but always precedes making the global
    // protocol slot reusable. A changed fingerprint fails its route barrier
    // rather than relabelling END onto a successor UDP peer.
    QPointer<ExactLogTransferService> guard(this);
    sendEndBestEffort(token);
    if (!guard) {
        return;
    }

    if (m_active.file && outcome != Outcome::Completed) {
        m_active.file->cancelWriting();
    }
    if (m_active.ownerDestroyed) {
        disconnect(m_active.ownerDestroyed);
    }
    m_active = ActiveOperation();

    emit transferFinished(result);
    if (!guard) {
        return;
    }
    m_finishing = false;
    if (m_startInFlight) {
        QTimer::singleShot(0, guard.data(), [guard]() {
            if (guard && !guard->busy()) {
                emit guard->busyChanged(false);
            }
        });
    } else {
        emit busyChanged(false);
    }
}

void ExactLogTransferService::failForRouteChange(
    const ExactLogTransferToken &token, const QString &error)
{
    if (tokenIsCurrent(token)) {
        finishActive(
            token, Outcome::LeaseRetired,
            error.isEmpty() ? defaultRouteError() : error);
    }
}

QList<ExactLogEntry> ExactLogTransferService::completedEntries() const
{
    return m_active.entries.values();
}

qint64 ExactLogTransferService::nowMs() const
{
    const qint64 sampled = m_clock ? m_clock() : m_monotonicClock.elapsed();
    const qint64 nonNegative = std::max<qint64>(0, sampled);
    m_lastNowMs = std::max(m_lastNowMs, nonNegative);
    return m_lastNowMs;
}

void ExactLogTransferService::assertServiceThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
}
