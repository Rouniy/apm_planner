#include "ConfigADSBViewModel.h"

#include <QVariantMap>

#include <algorithm>

const QString ConfigADSBViewModel::Title = QStringLiteral("ADSB");
const QString ConfigADSBViewModel::Intro =
    QStringLiteral("ADS-B receiver / avoidance. Populated on connect.");
const int ConfigADSBViewModel::SearchMinimumLength = 2;

namespace {

const char *const kForcedBitmaskParameters[] = {
    "ADSB_OPTIONS",
    "ADSB_RF_CAPABLE",
    "ADSB_RF_SELECT",
};

bool valueLessThan(const ConfigFriendlyParameterValue &left,
                   const ConfigFriendlyParameterValue &right)
{
    if (left.componentId != right.componentId) {
        return left.componentId < right.componentId;
    }
    return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
}

} // namespace

ConfigADSBViewModel::ConfigADSBViewModel(QObject *parent)
    : QObject(parent)
{
}

bool ConfigADSBViewModel::IsAdsbParameter(const QString &name)
{
    return name.startsWith(QLatin1String("ADSB_"), Qt::CaseInsensitive) ||
           name.startsWith(QLatin1String("AVD_"), Qt::CaseInsensitive);
}

bool ConfigADSBViewModel::IsForcedBitmaskParameter(const QString &name)
{
    for (const char *forced : kForcedBitmaskParameters) {
        if (name.compare(QLatin1String(forced), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QList<ConfigFriendlyParameterValue> ConfigADSBViewModel::FilterSnapshot(
    const QList<ParameterRecord> &records)
{
    QList<ConfigFriendlyParameterValue> result;
    for (const ParameterRecord &record : records) {
        if (!IsAdsbParameter(record.key.name)) {
            continue;
        }
        ConfigFriendlyParameterValue value;
        value.componentId = record.key.componentId;
        value.name = record.key.name;
        value.value = record.value;
        result.append(value);
    }
    std::stable_sort(result.begin(), result.end(), valueLessThan);
    return result;
}

QString ConfigADSBViewModel::SearchTerm(const QString &text)
{
    const QString term = text.trimmed();
    return term.size() < SearchMinimumLength ? QString() : term;
}

QVariantList ConfigADSBViewModel::OrderedWriteChanges(const QList<ParamField> &fields)
{
    QList<ParamField> ordered = fields;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ParamField &left, const ParamField &right) {
                         const int leftRank =
                             left.name.contains(QLatin1String("ENABLE"), Qt::CaseInsensitive) ? 1 : 0;
                         const int rightRank =
                             right.name.contains(QLatin1String("ENABLE"), Qt::CaseInsensitive) ? 1 : 0;
                         if (leftRank != rightRank) {
                             return leftRank < rightRank;
                         }
                         return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
                     });
    QVariantList changes;
    for (const ParamField &field : ordered) {
        if (field.name.isEmpty() || !field.value.isValid()) {
            continue; // MP10 skips fields that do not exist on the vehicle
        }
        changes.append(QVariantMap{{QStringLiteral("name"), field.name},
                                   {QStringLiteral("value"), field.value}});
    }
    return changes;
}

bool ConfigADSBViewModel::isIdentificationBusy() const
{
    return m_client && m_client->isBusy();
}

bool ConfigADSBViewModel::isIdentificationAvailable() const
{
    return m_connected && m_client && m_client->isBound();
}

void ConfigADSBViewModel::setClient(AdsbIdentificationClient *client)
{
    if (m_client == client) {
        return;
    }
    if (m_client) {
        m_client->disconnect(this);
    }
    m_client = client;
    if (!client) {
        return;
    }
    connect(client, &AdsbIdentificationClient::flightIdReceived, this,
            &ConfigADSBViewModel::onFlightIdReceived);
    connect(client, &AdsbIdentificationClient::registrationReceived, this,
            &ConfigADSBViewModel::onRegistrationReceived);
    connect(client, &AdsbIdentificationClient::saveStarted, this,
            &ConfigADSBViewModel::onSaveStarted);
    connect(client, &AdsbIdentificationClient::saveCompleted, this,
            &ConfigADSBViewModel::onSaveCompleted);
    connect(client, &AdsbIdentificationClient::saveFailed, this,
            &ConfigADSBViewModel::onSaveFailed);
    connect(client, &AdsbIdentificationClient::leaseInvalidated, this,
            &ConfigADSBViewModel::onLeaseInvalidated);
}

void ConfigADSBViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected && m_client && m_client->isBusy()) {
        m_client->cancel(); // reports the cancellation through onSaveFailed
    }
    if (!connected && m_bulkWritePending) {
        m_bulkWritePending = false;
        m_bulkBatchId = 0;
        setStatus(tr("write failed"));
    }
    emit connectedChanged(connected);
}

void ConfigADSBViewModel::setSearch(const QString &search)
{
    if (m_search == search) {
        return;
    }
    const QString previousTerm = SearchTerm(m_search);
    m_search = search;
    const QString term = SearchTerm(search);
    if (term != previousTerm) {
        emit searchTermChanged(term);
    }
}

void ConfigADSBViewModel::setFlightId(const QString &flightId)
{
    if (m_flightId == flightId) {
        return;
    }
    m_flightId = flightId;
    emit flightIdChanged(flightId);
}

void ConfigADSBViewModel::setAircraftRegistration(const QString &registration)
{
    if (m_aircraftRegistration == registration) {
        return;
    }
    m_aircraftRegistration = registration;
    emit aircraftRegistrationChanged(registration);
}

bool ConfigADSBViewModel::requestIdentification()
{
    if (!isIdentificationAvailable()) {
        return false; // MP10 RequestIdentification returns silently when offline
    }
    setStatus(tr("Reading Flight ID and aircraft registration…"));
    const AdsbIdentificationClient::SendResult result = m_client->requestIdentification();
    if (result != AdsbIdentificationClient::SendResult::Sent) {
        setStatus(tr("Identification read failed: %1").arg(m_client->lastError()));
        return false;
    }
    return true;
}

bool ConfigADSBViewModel::canSave()
{
    if (!isIdentificationAvailable()) {
        setStatus(tr("offline"));
        return false;
    }
    return true;
}

void ConfigADSBViewModel::declineClear(Field field)
{
    setStatus(field == Field::FlightId ? tr("Flight ID was not changed.")
                                       : tr("Aircraft registration was not changed."));
}

bool ConfigADSBViewModel::save(Field field, const QString &text)
{
    if (!canSave()) {
        return false;
    }
    const AdsbIdentificationClient::SendResult result = m_client->save(field, text);
    switch (result) {
    case AdsbIdentificationClient::SendResult::Sent:
        return true; // onSaveStarted/onSaveCompleted report the progress
    case AdsbIdentificationClient::SendResult::NotBound:
    case AdsbIdentificationClient::SendResult::StaleTarget:
        setStatus(tr("offline"));
        return false;
    case AdsbIdentificationClient::SendResult::Busy:
        setStatus(tr("send failed: another identification write is still running"));
        return false;
    case AdsbIdentificationClient::SendResult::InvalidText: {
        QString error;
        AdsbIdentificationClient::EncodeDeviceText(text, &error);
        setStatus(tr("send failed: %1").arg(error));
        return false;
    }
    case AdsbIdentificationClient::SendResult::TransportUnavailable:
        break;
    }
    setStatus(tr("send failed: %1").arg(m_client->lastError()));
    return false;
}

void ConfigADSBViewModel::beginBulkWrite(int parameterCount)
{
    m_bulkWritePending = true;
    m_bulkBatchId = 0;
    setStatus(tr("Writing %n parameter(s)…", nullptr, parameterCount));
}

void ConfigADSBViewModel::bulkWriteSubmitted(qulonglong batchId)
{
    if (!m_bulkWritePending) {
        return; // not our batch
    }
    m_bulkBatchId = batchId;
}

void ConfigADSBViewModel::bulkWriteCompleted(qulonglong batchId, int succeeded, int failed)
{
    if (!m_bulkWritePending || (m_bulkBatchId != 0 && batchId != m_bulkBatchId)) {
        return; // foreign batch
    }
    m_bulkWritePending = false;
    m_bulkBatchId = 0;
    Q_UNUSED(succeeded)
    setStatus(failed > 0 ? tr("write failed") : tr("Parameters successfully saved."));
}

void ConfigADSBViewModel::bulkWriteCancelled(qulonglong batchId)
{
    if (!m_bulkWritePending || (m_bulkBatchId != 0 && batchId != m_bulkBatchId)) {
        return;
    }
    m_bulkWritePending = false;
    m_bulkBatchId = 0;
    setStatus(tr("write failed"));
}

void ConfigADSBViewModel::bulkWriteSubmissionFailed(const QString &reason)
{
    if (!m_bulkWritePending) {
        return;
    }
    m_bulkWritePending = false;
    m_bulkBatchId = 0;
    setStatus(reason.isEmpty() ? tr("write failed") : tr("write failed: %1").arg(reason));
}

void ConfigADSBViewModel::cancelIdentification()
{
    if (m_client) {
        m_client->cancel();
    }
}

void ConfigADSBViewModel::activate()
{
    m_active = true;
    if (isIdentificationAvailable() && !isIdentificationBusy()) {
        requestIdentification();
    }
}

void ConfigADSBViewModel::deactivate()
{
    m_active = false;
    cancelIdentification();
}

void ConfigADSBViewModel::parameterTargetChanged()
{
    // The identification shown belongs to the previous vehicle.
    cancelIdentification();
    m_bulkWritePending = false;
    m_bulkBatchId = 0;
    setFlightId(QString());
    setAircraftRegistration(QString());
    setStatus(QString());
}

void ConfigADSBViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
}

bool ConfigADSBViewModel::replyIsCurrent(qulonglong generation) const
{
    return m_connected && m_client && m_client->isBound() &&
           m_client->lease().generation == generation;
}

QString ConfigADSBViewModel::fieldLabel(Field field)
{
    return field == Field::FlightId ? tr("Flight ID") : tr("Aircraft registration");
}

void ConfigADSBViewModel::onFlightIdReceived(qulonglong generation, const QString &flightId)
{
    if (!replyIsCurrent(generation)) {
        return;
    }
    setFlightId(flightId);
    setStatus(tr("Flight ID read from the device."));
}

void ConfigADSBViewModel::onRegistrationReceived(qulonglong generation,
                                                 const QString &registration)
{
    if (!replyIsCurrent(generation)) {
        return;
    }
    setAircraftRegistration(registration);
    setStatus(tr("Aircraft registration read from the device."));
}

void ConfigADSBViewModel::onSaveStarted(Field field, qulonglong generation)
{
    Q_UNUSED(generation)
    setStatus(tr("Sending %1…").arg(fieldLabel(field)));
    emit identificationBusyChanged(true);
}

void ConfigADSBViewModel::onSaveCompleted(Field field, qulonglong generation)
{
    Q_UNUSED(generation)
    setStatus(field == Field::FlightId ? tr("Flight ID sent.")
                                       : tr("Aircraft registration sent."));
    emit identificationBusyChanged(false);
}

void ConfigADSBViewModel::onSaveFailed(Field field, const QString &reason, bool cancelled)
{
    if (cancelled) {
        setStatus(tr("%1 write cancelled.").arg(fieldLabel(field)));
    } else if (reason.startsWith(QLatin1String("send failed"))) {
        setStatus(reason);
    } else {
        setStatus(tr("send failed: %1").arg(reason));
    }
    emit identificationBusyChanged(false);
}

void ConfigADSBViewModel::onLeaseInvalidated(qulonglong generation)
{
    Q_UNUSED(generation)
    // The client already cancelled a running write; nothing else to do, the
    // owner recreates the binding for the new target.
}
