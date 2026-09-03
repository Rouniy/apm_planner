#include "SerialOutputCotViewModel.h"

#include "comm/CotOutputService.h"

#include <QAbstractItemModel>
#include <QJsonValue>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace {

const char EndpointKey[] = "CoT_AvaloniaEndpoint";
const char HostKey[] = "CoT_Host";
const char PortKey[] = "CoT_Port";
const char BaudKey[] = "CoT_Baud";
const char UpdateKey[] = "CoT_UpdateSeconds";
const char EventTypeKey[] = "CoT_EventType";
const char UidPrefixKey[] = "CoT_UidPrefix";
const char CallsignKey[] = "CoT_Callsign";
const char IndentKey[] = "CoT_IndentXml";
const char AdvancedKey[] = "CoT_CB_advancedMode";
const char IdentityKey[] = "CoTUID";

QStringList fixedEndpoints()
{
    return {
        SerialOutputCotViewModel::TakMulticastText(),
        SerialOutputCotViewModel::UdpClientText(),
        SerialOutputCotViewModel::UdpHostText(),
        SerialOutputCotViewModel::TcpClientText(),
        SerialOutputCotViewModel::TcpHostText()
    };
}

} // namespace

SerialOutputCotViewModel::SerialOutputCotViewModel(
    CotOutputService *service, Dependencies dependencies, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_dependencies(std::move(dependencies))
    , m_identityModel(new CotIdentityModel(this))
{
    loadSettings();
    refreshEndpoints();

    const auto identitiesChanged = [this]() {
        markIdentitiesDirty();
        applyLiveSettings();
        emit changed();
    };
    connect(m_identityModel, &QAbstractItemModel::dataChanged,
            this, [identitiesChanged](const QModelIndex &, const QModelIndex &,
                                      const QVector<int> &) {
        identitiesChanged();
    });
    connect(m_identityModel, &QAbstractItemModel::rowsInserted,
            this, [identitiesChanged](const QModelIndex &, int, int) {
        identitiesChanged();
    });
    connect(m_identityModel, &QAbstractItemModel::rowsRemoved,
            this, [identitiesChanged](const QModelIndex &, int, int) {
        identitiesChanged();
    });
    connect(m_identityModel, &QAbstractItemModel::modelReset,
            this, identitiesChanged);

    if (m_service) {
        connect(m_service, &CotOutputService::statusChanged,
                this, [this]() {
            m_localStatus.clear();
            refreshStatus();
        });
    }
    refreshStatus();
}

SerialOutputCotViewModel::~SerialOutputCotViewModel()
{
    // Destruction must stay quiet: saveSettings()/stop() update the visible
    // status and emit changed(), while the owning window is already tearing
    // down its widgets. Persist the final snapshot and stop the service
    // directly after disconnecting its status callback instead.
    if (m_dependencies.saveSettings) {
        QVariantMap values = settingsSnapshot();
        if (!m_identitiesDirty) {
            values.remove(QString::fromLatin1(IdentityKey));
        }
        QString ignoredError;
        m_dependencies.saveSettings(values, &ignoredError);
    }
    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
        m_service->stop();
    }
}

QString SerialOutputCotViewModel::TakMulticastText()
{
    return QStringLiteral("TAK Multicast");
}

QString SerialOutputCotViewModel::UdpClientText()
{
    return QStringLiteral("UDP Client");
}

QString SerialOutputCotViewModel::UdpHostText()
{
    return QStringLiteral("UDP Host");
}

QString SerialOutputCotViewModel::TcpClientText()
{
    return QStringLiteral("TCP Client");
}

QString SerialOutputCotViewModel::TcpHostText()
{
    return QStringLiteral("TCP Host");
}

bool SerialOutputCotViewModel::isNetworkEndpoint() const
{
    return isFixedEndpoint(m_selectedEndpoint);
}

bool SerialOutputCotViewModel::isSerialEndpoint() const
{
    return !m_selectedEndpoint.isEmpty() && !isNetworkEndpoint();
}

bool SerialOutputCotViewModel::isRunning() const
{
    return m_service && m_service->isRunning();
}

QString SerialOutputCotViewModel::connectButtonText() const
{
    return isRunning() ? tr("Stop") : tr("Connect");
}

QVariantMap SerialOutputCotViewModel::settingsSnapshot() const
{
    QVariantMap values;
    values.insert(QString::fromLatin1(EndpointKey), m_selectedEndpoint);
    values.insert(QString::fromLatin1(HostKey), m_host);
    values.insert(QString::fromLatin1(PortKey), m_port);
    values.insert(QString::fromLatin1(BaudKey), m_baud);
    values.insert(QString::fromLatin1(UpdateKey), m_updateSeconds);
    values.insert(QString::fromLatin1(EventTypeKey), m_eventType);
    values.insert(QString::fromLatin1(UidPrefixKey), m_uidPrefix);
    values.insert(QString::fromLatin1(CallsignKey), m_callsign);
    values.insert(QString::fromLatin1(IndentKey), m_indentXml);
    values.insert(QString::fromLatin1(AdvancedKey), m_advancedMode);
    values.insert(QString::fromLatin1(IdentityKey),
                  QString::fromUtf8(m_identityModel->toJson()));
    return values;
}

void SerialOutputCotViewModel::updateAvailableEndpoints(
    const QList<VehicleEndpoint> &endpoints)
{
    m_availableEndpoints = endpoints;
    if (m_service && m_service->isRunning()) {
        m_service->updateEndpoints(endpoints);
    }
}

void SerialOutputCotViewModel::refreshEndpoints()
{
    const QString previous = m_selectedEndpoint;
    QStringList serialPorts = m_dependencies.enumeratePorts
        ? m_dependencies.enumeratePorts() : QStringList();
    serialPorts.removeDuplicates();
    std::sort(serialPorts.begin(), serialPorts.end(),
              [](const QString &left, const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });

    m_endpoints = fixedEndpoints();
    m_endpoints.append(serialPorts);
    if (!previous.isEmpty() && !m_endpoints.contains(previous)
        && !isFixedEndpoint(previous)) {
        // Keep an unavailable persisted serial selection visible so Connect
        // reports the real open error instead of silently retargeting it.
        m_endpoints.append(previous);
    }
    m_selectedEndpoint = m_endpoints.contains(previous)
        ? previous : TakMulticastText();
    emit changed();
}

void SerialOutputCotViewModel::setSelectedEndpoint(const QString &endpoint)
{
    if (endpoint.isEmpty() || m_selectedEndpoint == endpoint) {
        return;
    }
    m_selectedEndpoint = endpoint;
    if (!m_endpoints.contains(endpoint)) {
        m_endpoints.append(endpoint);
    }
    applyEndpointDefaults(endpoint);
    emit changed();
}

void SerialOutputCotViewModel::setHost(const QString &host)
{
    if (m_host == host) {
        return;
    }
    m_host = host;
    emit changed();
}

void SerialOutputCotViewModel::setPort(int port)
{
    if (port < 0 || port > 65535 || m_port == port) {
        return;
    }
    m_port = port;
    emit changed();
}

void SerialOutputCotViewModel::setBaud(int baud)
{
    if (!m_bauds.contains(baud) || m_baud == baud) {
        return;
    }
    m_baud = baud;
    emit changed();
}

void SerialOutputCotViewModel::setUpdateSeconds(double seconds)
{
    if (!qIsFinite(seconds) || qFuzzyCompare(m_updateSeconds, seconds)) {
        return;
    }
    m_updateSeconds = seconds;
    if (m_service && m_service->isRunning()) {
        m_service->setUpdateIntervalSeconds(seconds);
    }
    emit changed();
}

void SerialOutputCotViewModel::setEventType(const QString &eventType)
{
    if (m_eventType == eventType) {
        return;
    }
    m_eventType = eventType;
    applyLiveSettings();
    emit changed();
}

void SerialOutputCotViewModel::setUidPrefix(const QString &prefix)
{
    if (m_uidPrefix == prefix) {
        return;
    }
    m_uidPrefix = prefix;
    applyLiveSettings();
    emit changed();
}

void SerialOutputCotViewModel::setCallsign(const QString &callsign)
{
    if (m_callsign == callsign) {
        return;
    }
    m_callsign = callsign;
    applyLiveSettings();
    emit changed();
}

void SerialOutputCotViewModel::setIndentXml(bool enabled)
{
    if (m_indentXml == enabled) {
        return;
    }
    m_indentXml = enabled;
    applyLiveSettings();
    emit changed();
}

void SerialOutputCotViewModel::setAdvancedMode(bool enabled)
{
    if (m_advancedMode == enabled) {
        return;
    }
    m_advancedMode = enabled;
    applyLiveSettings();
    emit changed();
}

void SerialOutputCotViewModel::refreshIdentitySystems()
{
    const CotOutputSource source = m_dependencies.resolveSource
        ? m_dependencies.resolveSource() : CotOutputSource();
    if (!source.isValid()) {
        setLocalStatus(tr("Unable to refresh CoT systems: no current vehicle target."));
        return;
    }
    updateAvailableEndpoints(source.endpoints);

    QList<int> systems;
    for (const VehicleEndpoint &endpoint : source.endpoints) {
        if (CotIdentityModel::isValidSystemId(endpoint.systemId)
            && !systems.contains(endpoint.systemId)) {
            systems.append(endpoint.systemId);
        }
    }
    std::sort(systems.begin(), systems.end());
    int added = 0;
    for (int systemId : systems) {
        if (m_identityModel->rowForSystemId(systemId) >= 0) {
            continue;
        }
        CotIdentityRecord identity;
        identity.systemId = QString::number(systemId);
        identity.eventUid = m_uidPrefix + QLatin1Char('-')
            + QString::number(systemId);
        if (m_identityModel->appendIdentity(identity)) {
            ++added;
        }
    }
    setLocalStatus(added > 0
        ? tr("Added %1 MAVLink system identity row(s).").arg(added)
        : tr("No new MAVLink systems found. Identity rows are preserved for offline systems."));
}

void SerialOutputCotViewModel::addIdentity()
{
    for (int systemId = 1; systemId <= 255; ++systemId) {
        if (m_identityModel->rowForSystemId(systemId) >= 0) {
            continue;
        }
        CotIdentityRecord identity;
        identity.systemId = QString::number(systemId);
        identity.eventUid = m_uidPrefix + QLatin1Char('-')
            + QString::number(systemId);
        if (m_identityModel->appendIdentity(identity)) {
            setLocalStatus(tr("Added identity row for MAVLink system %1.")
                               .arg(systemId));
            return;
        }
    }
    setLocalStatus(tr("All MAVLink system IDs already have identity rows."));
}

void SerialOutputCotViewModel::removeIdentity(int row)
{
    if (row < 0 || !m_identityModel->removeRows(row, 1)) {
        setLocalStatus(tr("Select an identity row to remove."));
    }
}

void SerialOutputCotViewModel::toggleConnection()
{
    if (!m_service) {
        setLocalStatus(tr("Unable to start CoT output: service is unavailable."));
        return;
    }
    if (m_service->isRunning()) {
        stop();
        return;
    }
    if (m_selectedEndpoint.isEmpty()) {
        setLocalStatus(tr("Select an output endpoint."));
        return;
    }
    if (!CotOutputService::IsValidInterval(m_updateSeconds)) {
        setLocalStatus(tr("Update interval must be between 0.1 and 3600 seconds."));
        return;
    }

    const CotOutputTransport::Settings transport = transportSettings();
    if (isNetworkEndpoint() && transport.port == 0) {
        setLocalStatus(tr("Unable to start CoT output: network port must be between 1 and 65535."));
        return;
    }
    if (m_dependencies.validateSelection) {
        const QString reason = m_dependencies.validateSelection(transport);
        if (!reason.isEmpty()) {
            setLocalStatus(tr("Unable to start CoT output: %1").arg(reason));
            return;
        }
    }
    const CotOutputSource source = m_dependencies.resolveSource
        ? m_dependencies.resolveSource() : CotOutputSource();
    if (!source.isValid()) {
        setLocalStatus(tr("Unable to start CoT output: no current vehicle target; select a vehicle or connect a link."));
        return;
    }

    CotOutputServiceSettings settings;
    settings.transport = transport;
    settings.updateIntervalSeconds = m_updateSeconds;
    settings.event.eventType = m_eventType;
    settings.event.uidPrefix = m_uidPrefix;
    settings.event.callsign = m_callsign;
    settings.event.indentXml = m_indentXml;
    settings.event.advancedIdentityFields = m_advancedMode;
    settings.identities = identityOverrides();

    saveSettings();
    m_localStatus.clear();
    m_service->updateEndpoints(source.endpoints);
    QString error;
    if (!m_service->start(source.linkId, source.linkName, settings, &error)) {
        setLocalStatus(tr("Unable to start CoT output: %1")
                           .arg(error.isEmpty() ? tr("unknown error") : error));
        return;
    }
    refreshStatus();
}

void SerialOutputCotViewModel::stop()
{
    m_localStatus.clear();
    if (m_service) {
        m_service->stop();
    }
    refreshStatus();
}

void SerialOutputCotViewModel::saveSettings()
{
    if (!m_dependencies.saveSettings) {
        m_identitiesDirty = false;
        return;
    }
    QVariantMap values = settingsSnapshot();
    if (!m_identitiesDirty) {
        values.remove(QString::fromLatin1(IdentityKey));
    }
    QString error;
    if (!m_dependencies.saveSettings(values, &error)) {
        setLocalStatus(tr("Unable to save CoT settings: %1")
                           .arg(error.isEmpty() ? tr("unknown error") : error));
        return;
    }
    m_identitiesDirty = false;
}

void SerialOutputCotViewModel::refreshStatus()
{
    QString status = m_localStatus;
    QString preview;
    if (m_service) {
        const CotOutputService::Status serviceStatus = m_service->status();
        if (status.isEmpty()) {
            status = serviceStatus.text;
        }
        preview = serviceStatus.preview;
    }
    if (status == m_statusText && preview == m_lastEvent) {
        return;
    }
    m_statusText = status;
    m_lastEvent = preview;
    emit changed();
}

bool SerialOutputCotViewModel::isFixedEndpoint(const QString &endpoint)
{
    return fixedEndpoints().contains(endpoint);
}

CotOutputTransport::Mode SerialOutputCotViewModel::modeForEndpoint(
    const QString &endpoint)
{
    if (endpoint == TakMulticastText()) {
        return CotOutputTransport::Mode::TakMulticast;
    }
    if (endpoint == UdpClientText()) {
        return CotOutputTransport::Mode::UdpClient;
    }
    if (endpoint == UdpHostText()) {
        return CotOutputTransport::Mode::UdpHost;
    }
    if (endpoint == TcpClientText()) {
        return CotOutputTransport::Mode::TcpClient;
    }
    if (endpoint == TcpHostText()) {
        return CotOutputTransport::Mode::TcpHost;
    }
    return CotOutputTransport::Mode::Serial;
}

CotOutputTransport::Settings SerialOutputCotViewModel::transportSettings() const
{
    const CotOutputTransport::Mode mode = modeForEndpoint(m_selectedEndpoint);
    CotOutputTransport::Settings settings = CotOutputTransport::Defaults(mode);
    settings.host = m_host.trimmed();
    settings.port = static_cast<quint16>(qBound(0, m_port, 65535));
    settings.baud = m_baud;
    if (mode == CotOutputTransport::Mode::Serial) {
        settings.serialPort = m_selectedEndpoint;
    }
    return settings;
}

QHash<int, CotIdentityOverride> SerialOutputCotViewModel::identityOverrides() const
{
    QHash<int, CotIdentityOverride> result;
    for (int systemId = 0; systemId <= 255; ++systemId) {
        CotIdentityRecord record;
        if (!m_identityModel->identityForSystemId(systemId, &record)) {
            continue;
        }
        CotIdentityOverride identity;
        identity.uid = record.eventUid;
        identity.includeTakv = record.includeTakv;
        identity.contactCallsign = record.contactCallsign;
        identity.contactEndpoint = record.contactEndpoint;
        identity.vmf = record.vmf;
        result.insert(systemId, identity);
    }
    return result;
}

void SerialOutputCotViewModel::applyEndpointDefaults(const QString &endpoint)
{
    const CotOutputTransport::Mode mode = modeForEndpoint(endpoint);
    if (mode == CotOutputTransport::Mode::Serial) {
        return;
    }
    const CotOutputTransport::Settings defaults = CotOutputTransport::Defaults(mode);
    if (mode == CotOutputTransport::Mode::TakMulticast
        || mode == CotOutputTransport::Mode::UdpHost
        || mode == CotOutputTransport::Mode::TcpHost) {
        m_host = defaults.host;
    } else if (m_host.trimmed().isEmpty()
               || m_host == QStringLiteral("0.0.0.0")
               || m_host == QStringLiteral("239.2.3.1")) {
        m_host = defaults.host;
    }
    m_port = defaults.port;
}

void SerialOutputCotViewModel::applyLiveSettings()
{
    if (!m_service || !m_service->isRunning()) {
        return;
    }
    CotEventSettings settings;
    settings.eventType = m_eventType;
    settings.uidPrefix = m_uidPrefix;
    settings.callsign = m_callsign;
    settings.indentXml = m_indentXml;
    settings.advancedIdentityFields = m_advancedMode;
    m_service->setEventSettings(settings);
    m_service->setIdentityOverrides(identityOverrides());
}

void SerialOutputCotViewModel::loadSettings()
{
    const QVariantMap values = m_dependencies.loadSettings
        ? m_dependencies.loadSettings() : QVariantMap();
    m_selectedEndpoint = values.value(QString::fromLatin1(EndpointKey),
                                      TakMulticastText()).toString();
    m_host = values.value(QString::fromLatin1(HostKey),
                          QStringLiteral("239.2.3.1")).toString();

    bool ok = false;
    const int port = values.value(QString::fromLatin1(PortKey), 6969).toInt(&ok);
    if (ok && port >= 0 && port <= 65535) {
        m_port = port;
    }
    const int baud = values.value(QString::fromLatin1(BaudKey), 57600).toInt(&ok);
    if (ok && m_bauds.contains(baud)) {
        m_baud = baud;
    }
    const double seconds = values.value(QString::fromLatin1(UpdateKey), 10.0)
                               .toDouble(&ok);
    if (ok && CotOutputService::IsValidInterval(seconds)) {
        m_updateSeconds = seconds;
    }
    m_eventType = values.value(QString::fromLatin1(EventTypeKey),
                               QStringLiteral("a-f-A-M-F-Q")).toString();
    m_uidPrefix = values.value(QString::fromLatin1(UidPrefixKey),
                               QStringLiteral("MissionPlanner")).toString();
    m_callsign = values.value(QString::fromLatin1(CallsignKey)).toString();
    m_indentXml = values.value(QString::fromLatin1(IndentKey), false).toBool();
    m_advancedMode = values.value(QString::fromLatin1(AdvancedKey), true).toBool();

    QString identityError;
    if (!m_identityModel->loadSetting(
            values.value(QString::fromLatin1(IdentityKey)), &identityError)) {
        m_localStatus = tr("Unable to load CoT identities: %1")
                            .arg(identityError);
    }
    m_identitiesDirty = false;
}

void SerialOutputCotViewModel::markIdentitiesDirty()
{
    m_identitiesDirty = true;
}

void SerialOutputCotViewModel::setLocalStatus(const QString &text)
{
    m_localStatus = text;
    m_statusText = text;
    emit changed();
}
