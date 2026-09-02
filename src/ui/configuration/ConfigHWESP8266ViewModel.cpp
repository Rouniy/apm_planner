#include "ConfigHWESP8266ViewModel.h"

ConfigHWESP8266ViewModel::ConfigHWESP8266ViewModel(QObject *parent)
    : QObject(parent),
      m_baud(Esp8266SettingsCodec::DefaultBaud()),
      m_channel(Esp8266SettingsCodec::DefaultChannel()),
      m_ipSta(Esp8266SettingsCodec::DefaultIpSta()),
      m_gatewaySta(Esp8266SettingsCodec::DefaultGatewaySta()),
      m_subnetSta(Esp8266SettingsCodec::DefaultSubnetSta()),
      m_status(NotConnectedStatus()) // MP10 constructor when the stream is closed
{
}

ConfigHWESP8266ViewModel::~ConfigHWESP8266ViewModel()
{
    // The page is going away: never leave a write cadence running for it.
    if (m_client && m_owned != Owned::None) {
        m_client->disconnect(this);
        m_client->cancel();
    }
}

// --- texts ---------------------------------------------------------------------

QString ConfigHWESP8266ViewModel::Title()
{
    return QStringLiteral("ESP8266");
}

QString ConfigHWESP8266ViewModel::NotConnectedStatus()
{
    return tr("Not connected.");
}

QString ConfigHWESP8266ViewModel::RequestingStatus()
{
    return tr("Requesting ESP8266 parameters…");
}

QString ConfigHWESP8266ViewModel::TargetChangedStatus()
{
    return tr("The selected device changed before ESP8266 parameters were loaded.");
}

QString ConfigHWESP8266ViewModel::SavingStatus()
{
    return tr("Saving…");
}

QString ConfigHWESP8266ViewModel::ProgrammedOkStatus()
{
    return tr("Programmed OK.");
}

QString ConfigHWESP8266ViewModel::ErrorSettingStatus()
{
    return tr("Error setting parameter.");
}

QString ConfigHWESP8266ViewModel::ResettingStatus()
{
    return tr("Resetting to defaults…");
}

QString ConfigHWESP8266ViewModel::ProgrammedRefreshStatus()
{
    return tr("Programmed OK. Refreshing parameters…");
}

// --- client ----------------------------------------------------------------------

void ConfigHWESP8266ViewModel::setClient(Esp8266ParameterClient *client)
{
    if (m_client == client) {
        return;
    }
    if (m_client) {
        if (m_owned != Owned::None) {
            m_client->cancel(); // reported through onOperationFailed
        }
        m_client->disconnect(this);
    }
    finishOwned();
    m_client = client;
    if (!client) {
        return;
    }
    connect(client, &Esp8266ParameterClient::parametersLoaded, this,
            &ConfigHWESP8266ViewModel::onParametersLoaded);
    connect(client, &Esp8266ParameterClient::loadFailed, this,
            &ConfigHWESP8266ViewModel::onLoadFailed);
    connect(client, &Esp8266ParameterClient::saveCompleted, this,
            &ConfigHWESP8266ViewModel::onSaveCompleted);
    connect(client, &Esp8266ParameterClient::resetCompleted, this,
            &ConfigHWESP8266ViewModel::onResetCompleted);
    connect(client, &Esp8266ParameterClient::operationFailed, this,
            &ConfigHWESP8266ViewModel::onOperationFailed);
    connect(client, &Esp8266ParameterClient::leaseInvalidated, this,
            &ConfigHWESP8266ViewModel::onLeaseInvalidated);
}

bool ConfigHWESP8266ViewModel::isBusy() const
{
    return m_owned != Owned::None && m_client && m_client->isBusy();
}

bool ConfigHWESP8266ViewModel::isTransportAvailable() const
{
    return m_connected && m_client && m_client->isBound();
}

Esp8266SaveRequest ConfigHWESP8266ViewModel::saveRequest() const
{
    Esp8266SaveRequest request;
    request.ssid = m_ssid;
    request.password = m_password;
    request.channel = m_channel;
    request.baud = m_baud;
    request.ipSta = m_ipSta;
    request.gatewaySta = m_gatewaySta;
    request.subnetSta = m_subnetSta;
    request.staMode = m_staMode;
    return request;
}

// --- field setters -----------------------------------------------------------------

void ConfigHWESP8266ViewModel::setSsid(const QString &value)
{
    if (m_ssid != value) {
        m_ssid = value;
        emit ssidChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setPassword(const QString &value)
{
    if (m_password != value) {
        m_password = value;
        emit passwordChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setBaud(const QString &value)
{
    if (m_baud != value) {
        m_baud = value;
        emit baudChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setChannel(const QString &value)
{
    if (m_channel != value) {
        m_channel = value;
        emit channelChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setStaMode(bool value)
{
    if (m_staMode != value) {
        m_staMode = value;
        emit staModeChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setIpSta(const QString &value)
{
    if (m_ipSta != value) {
        m_ipSta = value;
        emit ipStaChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setGatewaySta(const QString &value)
{
    if (m_gatewaySta != value) {
        m_gatewaySta = value;
        emit gatewayStaChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setSubnetSta(const QString &value)
{
    if (m_subnetSta != value) {
        m_subnetSta = value;
        emit subnetStaChanged(value);
    }
}

void ConfigHWESP8266ViewModel::setDetails(const QString &details)
{
    if (m_details != details) {
        m_details = details;
        emit detailsChanged(details);
    }
}

void ConfigHWESP8266ViewModel::setStatus(const QString &status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged(status);
    }
}

void ConfigHWESP8266ViewModel::setLoaded(bool loaded)
{
    if (m_loaded != loaded) {
        m_loaded = loaded;
        emit loadedChanged(loaded);
    }
}

// --- lifecycle -----------------------------------------------------------------------

void ConfigHWESP8266ViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        cancelOperation(); // statuses come from onOperationFailed
        setLoaded(false);
        setStatus(NotConnectedStatus());
    }
    emit connectedChanged(connected);
}

void ConfigHWESP8266ViewModel::activate()
{
    m_active = true;
    if (!isTransportAvailable()) {
        setStatus(NotConnectedStatus());
        return;
    }
    // LatestOperationController: a newer activation replaces a running load,
    // but a save/reset that is still writing is never interrupted.
    if (m_owned == Owned::Save || m_owned == Owned::Reset) {
        return;
    }
    if (m_owned == Owned::Load && m_client && m_client->isBusy()) {
        m_client->cancel(); // silent for our own superseded load
    }
    startLoad();
}

void ConfigHWESP8266ViewModel::deactivate()
{
    m_active = false;
    if (m_owned == Owned::Load) {
        cancelOperation();
    }
}

void ConfigHWESP8266ViewModel::parameterTargetChanged()
{
    cancelOperation();
    setLoaded(false);
}

bool ConfigHWESP8266ViewModel::startLoad()
{
    setLoaded(false);
    setStatus(RequestingStatus());
    m_owned = Owned::Load;
    m_ownedGeneration = m_client->primaryTarget().generation;
    const Esp8266ParameterClient::SendResult result = m_client->requestParameters();
    switch (result) {
    case Esp8266ParameterClient::SendResult::Sent:
        emit busyChanged(true);
        return true;
    case Esp8266ParameterClient::SendResult::NotBound:
    case Esp8266ParameterClient::SendResult::StaleTarget:
        finishOwned();
        setStatus(NotConnectedStatus());
        return false;
    case Esp8266ParameterClient::SendResult::Busy:
        finishOwned(); // somebody else's operation; keep the client alone
        setStatus(tr("ESP8266 parameter load failed: the link is busy."));
        return false;
    case Esp8266ParameterClient::SendResult::InvalidRequest:
    case Esp8266ParameterClient::SendResult::TransportUnavailable:
        break;
    }
    finishOwned();
    setStatus(tr("ESP8266 parameter load failed: %1").arg(m_client->lastError()));
    return false;
}

bool ConfigHWESP8266ViewModel::save()
{
    if (!isTransportAvailable()) {
        setStatus(NotConnectedStatus());
        return false;
    }
    if (isBusy()) {
        return false; // controls are locked; ignore a stray click
    }
    QString error;
    const Esp8266SaveRequest request = saveRequest();
    if (!Esp8266SettingsCodec::ValidateSave(request, &error)) {
        setStatus(error);
        return false;
    }
    setStatus(SavingStatus());
    m_owned = Owned::Save;
    m_ownedGeneration = m_client->primaryTarget().generation;
    const Esp8266ParameterClient::SendResult result =
        m_client->save(Esp8266SettingsCodec::BuildWriteList(request));
    switch (result) {
    case Esp8266ParameterClient::SendResult::Sent:
        emit busyChanged(true);
        return true;
    case Esp8266ParameterClient::SendResult::NotBound:
    case Esp8266ParameterClient::SendResult::StaleTarget:
        finishOwned();
        setStatus(NotConnectedStatus());
        return false;
    case Esp8266ParameterClient::SendResult::Busy:
    case Esp8266ParameterClient::SendResult::InvalidRequest:
    case Esp8266ParameterClient::SendResult::TransportUnavailable:
        break;
    }
    finishOwned();
    setStatus(ErrorSettingStatus());
    return false;
}

bool ConfigHWESP8266ViewModel::resetDefaults()
{
    if (!isTransportAvailable()) {
        setStatus(NotConnectedStatus());
        return false;
    }
    if (isBusy()) {
        return false;
    }
    setStatus(ResettingStatus());
    m_owned = Owned::Reset;
    m_ownedGeneration = m_client->primaryTarget().generation;
    const Esp8266ParameterClient::SendResult result = m_client->resetDefaults();
    switch (result) {
    case Esp8266ParameterClient::SendResult::Sent:
        emit busyChanged(true);
        return true;
    case Esp8266ParameterClient::SendResult::NotBound:
    case Esp8266ParameterClient::SendResult::StaleTarget:
        finishOwned();
        setStatus(NotConnectedStatus());
        return false;
    case Esp8266ParameterClient::SendResult::Busy:
    case Esp8266ParameterClient::SendResult::InvalidRequest:
    case Esp8266ParameterClient::SendResult::TransportUnavailable:
        break;
    }
    finishOwned();
    setStatus(ErrorSettingStatus());
    return false;
}

void ConfigHWESP8266ViewModel::cancelOperation()
{
    if (m_owned != Owned::None && m_client) {
        m_cancelling = true;
        m_client->cancel(); // onOperationFailed(cancelled) settles the state
        m_cancelling = false;
    }
    if (m_owned != Owned::None) {
        // The client is gone or did not report: settle here.
        finishOwned();
    }
}

// --- client results ------------------------------------------------------------------

bool ConfigHWESP8266ViewModel::ownsGeneration(Owned owned, qulonglong generation) const
{
    return m_owned == owned && m_ownedGeneration == generation && m_client &&
           m_client->isBound() && m_client->primaryTarget().generation == generation;
}

void ConfigHWESP8266ViewModel::finishOwned()
{
    const bool wasBusy = m_owned != Owned::None;
    m_owned = Owned::None;
    m_ownedGeneration = 0;
    if (wasBusy) {
        emit busyChanged(false);
    }
}

void ConfigHWESP8266ViewModel::applySettings(const Esp8266Settings &settings)
{
    setSsid(settings.ssid);
    setPassword(settings.password);
    setBaud(settings.baud);
    setChannel(settings.channel);
    setIpSta(settings.ipSta);
    setGatewaySta(settings.gatewaySta);
    setSubnetSta(settings.subnetSta);
    setStaMode(settings.staMode());
    setDetails(settings.details);
    setLoaded(true);
    setStatus(QString());
}

void ConfigHWESP8266ViewModel::onParametersLoaded(qulonglong generation,
                                                  const Esp8266RawParameters &values)
{
    if (!ownsGeneration(Owned::Load, generation)) {
        return; // a foreign or superseded load
    }
    finishOwned();
    if (!m_connected) {
        setLoaded(false);
        setStatus(TargetChangedStatus());
        return;
    }
    Esp8266Settings settings;
    QStringList missing;
    if (!Esp8266SettingsCodec::TryReadSettings(values, &settings, &missing)) {
        setLoaded(false);
        setStatus(missing.isEmpty()
                      ? tr("No ESP8266 / UDP-bridge component responded.")
                      : tr("Incomplete ESP8266 response; missing: %1.")
                            .arg(missing.join(QStringLiteral(", "))));
        return;
    }
    applySettings(settings);
}

void ConfigHWESP8266ViewModel::onLoadFailed(qulonglong generation, const QString &reason,
                                            const QStringList &missing)
{
    Q_UNUSED(missing)
    if (m_owned != Owned::Load || m_ownedGeneration != generation) {
        return;
    }
    finishOwned();
    setLoaded(false);
    setStatus(reason); // "No ESP8266 / UDP-bridge component responded." / "Incomplete …"
}

void ConfigHWESP8266ViewModel::onSaveCompleted(qulonglong generation)
{
    if (m_owned != Owned::Save || m_ownedGeneration != generation) {
        return;
    }
    finishOwned();
    setStatus(ProgrammedOkStatus());
}

void ConfigHWESP8266ViewModel::onResetCompleted(qulonglong generation)
{
    if (m_owned != Owned::Reset || m_ownedGeneration != generation) {
        return;
    }
    finishOwned();
    setStatus(ProgrammedRefreshStatus());
    // MP10 ResetDefaults(): Activate() again to re-read the parameters.
    if (isTransportAvailable()) {
        startLoad();
        // startLoad() replaces the status with "Requesting…"; keep the MP10
        // combined text until the load reports.
        if (m_owned == Owned::Load) {
            setStatus(ProgrammedRefreshStatus());
        }
    }
}

QString ConfigHWESP8266ViewModel::failureStatus(Esp8266ParameterClient::Operation operation,
                                                const QString &reason, bool cancelled) const
{
    switch (operation) {
    case Esp8266ParameterClient::Operation::Loading:
        if (cancelled) {
            return isTransportAvailable() ? QString() : TargetChangedStatus();
        }
        return tr("ESP8266 parameter load failed: %1").arg(reason);
    case Esp8266ParameterClient::Operation::Saving:
        if (!cancelled) {
            return ErrorSettingStatus();
        }
        return m_cancelling ? tr("Saving cancelled.") : tr("Saving cancelled: %1.").arg(reason);
    case Esp8266ParameterClient::Operation::Resetting:
        if (!cancelled) {
            return ErrorSettingStatus();
        }
        return m_cancelling ? tr("Reset cancelled.") : tr("Reset cancelled: %1.").arg(reason);
    case Esp8266ParameterClient::Operation::None:
        break;
    }
    return reason;
}

void ConfigHWESP8266ViewModel::onOperationFailed(Esp8266ParameterClient::Operation operation,
                                                 qulonglong generation, const QString &reason,
                                                 bool cancelled)
{
    if (m_owned == Owned::None || m_ownedGeneration != generation) {
        return; // not ours
    }
    const Owned expected = operation == Esp8266ParameterClient::Operation::Loading   ? Owned::Load
                           : operation == Esp8266ParameterClient::Operation::Saving  ? Owned::Save
                           : operation == Esp8266ParameterClient::Operation::Resetting ? Owned::Reset
                                                                                       : Owned::None;
    if (expected != m_owned) {
        return;
    }
    finishOwned();
    if (operation == Esp8266ParameterClient::Operation::Loading) {
        setLoaded(false);
    }
    const QString status = failureStatus(operation, reason, cancelled);
    if (!status.isEmpty()) {
        setStatus(status);
    }
}

void ConfigHWESP8266ViewModel::onLeaseInvalidated(qulonglong generation)
{
    Q_UNUSED(generation)
    // The client already cancelled our operation (reported through
    // onOperationFailed with cancelled = true). Nothing else to settle.
}
