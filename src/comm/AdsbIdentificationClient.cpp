#include "AdsbIdentificationClient.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"

#include <cstring>

namespace {

QString transmitterFailure(ExactLinkTransmitter::SendResult result)
{
    switch (result) {
    case ExactLinkTransmitter::SendResult::Sent:
        return QString();
    case ExactLinkTransmitter::SendResult::InvalidLink:
        return AdsbIdentificationClient::tr("the target link is invalid");
    case ExactLinkTransmitter::SendResult::InvalidMessage:
        return AdsbIdentificationClient::tr("the message is not part of the dialect");
    case ExactLinkTransmitter::SendResult::IncompatibleVersion:
        return AdsbIdentificationClient::tr(
            "the link speaks MAVLink 1, which cannot carry uAvionix messages");
    case ExactLinkTransmitter::SendResult::TransportUnavailable:
        break;
    }
    return AdsbIdentificationClient::tr("the link could not accept the frame");
}

} // namespace

AdsbIdentificationClient::AdsbIdentificationClient(
    VehicleTargetManager *targetManager, ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent),
      m_targetManager(targetManager),
      m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<AdsbIdentificationClient::Field>("AdsbIdentificationClient::Field");
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &AdsbIdentificationClient::advanceSave);
    if (m_targetManager) {
        connect(m_targetManager, &VehicleTargetManager::targetGenerationChanged, this,
                &AdsbIdentificationClient::handleTargetGenerationChanged);
    }
}

AdsbIdentificationClient::~AdsbIdentificationClient()
{
    // Destruction is silent: stop the cadence, drop the operation, no signals.
    m_timer.stop();
    m_save.reset();
}

void AdsbIdentificationClient::setLocalIdentity(quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

void AdsbIdentificationClient::setResendDelayMs(int milliseconds)
{
    m_resendDelayMs = qMax(0, milliseconds);
}

bool AdsbIdentificationClient::bind(const VehicleTargetLease &lease)
{
    // Drop the old lease BEFORE reporting the cancellation: a slot reacting to
    // saveFailed that tries to save or read re-entrantly gets NotBound instead
    // of starting an operation the new binding would then leave alive.
    m_lease = VehicleTargetLease{};
    if (m_save) {
        finishSave(false, tr("the client was bound to another target"), true);
    }
    // Only now validate and install the requested lease.
    if (!lease.isValid() || !m_targetManager ||
        !m_targetManager->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation)) {
        m_lease = VehicleTargetLease{};
        return false;
    }
    m_lease = lease;
    return true;
}

void AdsbIdentificationClient::unbind()
{
    // Same order as bind(): unbound first, cancellation signal second.
    m_lease = VehicleTargetLease{};
    if (m_save) {
        finishSave(false, tr("the client was unbound"), true);
    }
}

bool AdsbIdentificationClient::targetIsCurrent() const
{
    return m_targetManager && m_lease.isValid() &&
           m_targetManager->isCurrentTarget(
               m_lease.endpoint.linkId, m_lease.endpoint.systemId,
               m_lease.endpoint.componentId, m_lease.generation);
}

AdsbIdentificationClient::SendResult AdsbIdentificationClient::sendMessage(
    mavlink_message_t message)
{
    if (!m_lease.isValid()) {
        m_lastError = tr("no target is bound");
        return SendResult::NotBound;
    }
    if (!targetIsCurrent()) {
        m_lastError = tr("the selected vehicle changed");
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        m_lastError = tr("the transmitter is unavailable");
        return SendResult::TransportUnavailable;
    }
    const ExactLinkTransmitter::SendResult result = m_transmitter->sendMessage(
        m_lease.endpoint.linkId, m_localSystemId, m_localComponentId, message);
    m_lastError = transmitterFailure(result);
    return result == ExactLinkTransmitter::SendResult::Sent
               ? SendResult::Sent
               : SendResult::TransportUnavailable;
}

AdsbIdentificationClient::SendResult AdsbIdentificationClient::sendGet(
    quint32 requestedMessageId)
{
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_uavionix_adsb_get_pack(m_localSystemId, m_localComponentId,
                                       &message, requestedMessageId);
    return sendMessage(message);
}

AdsbIdentificationClient::SendResult AdsbIdentificationClient::sendConfiguration(
    const SaveOperation &operation)
{
    char payload[DevicePayloadLength];
    std::memset(payload, 0, sizeof(payload));
    std::memcpy(payload, operation.payload.constData(),
                static_cast<size_t>(qMin(operation.payload.size(), DevicePayloadLength)));
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    if (operation.field == Field::FlightId) {
        mavlink_msg_uavionix_adsb_out_cfg_flightid_pack(
            m_localSystemId, m_localComponentId, &message, payload);
    } else {
        mavlink_msg_uavionix_adsb_out_cfg_registration_pack(
            m_localSystemId, m_localComponentId, &message, payload);
    }
    return sendMessage(message);
}

AdsbIdentificationClient::SendResult AdsbIdentificationClient::requestIdentification()
{
    // MP10 RequestIdentification: GET registration (10004) then flight id (10005).
    const SendResult first = sendGet(RegistrationMessageId);
    if (first != SendResult::Sent) {
        return first;
    }
    const SendResult second = sendGet(FlightIdMessageId);
    if (second != SendResult::Sent) {
        return second;
    }
    emit identificationRequested(m_lease.generation);
    return SendResult::Sent;
}

AdsbIdentificationClient::SendResult AdsbIdentificationClient::save(
    Field field, const QString &text)
{
    if (!m_lease.isValid()) {
        return SendResult::NotBound;
    }
    if (!targetIsCurrent()) {
        return SendResult::StaleTarget;
    }
    if (m_save) {
        return SendResult::Busy;
    }
    const QByteArray payload = EncodeDeviceText(text);
    if (payload.isEmpty()) {
        return SendResult::InvalidText;
    }

    auto operation = std::make_unique<SaveOperation>();
    operation->field = field;
    operation->payload = payload;
    operation->generation = m_lease.generation;
    const SendResult first = sendConfiguration(*operation);
    if (first != SendResult::Sent) {
        return first; // nothing started, nothing to cancel
    }
    m_save = std::move(operation);
    emit saveStarted(field, m_lease.generation);
    m_timer.start(m_resendDelayMs);
    return SendResult::Sent;
}

void AdsbIdentificationClient::advanceSave()
{
    if (!m_save) {
        return;
    }
    if (!targetIsCurrent() || m_save->generation != m_lease.generation) {
        finishSave(false, tr("the selected vehicle changed"), true);
        return;
    }
    if (m_save->step == 0) {
        // Second copy of the configuration, like MP10's repeated sendPacket.
        const SendResult result = sendConfiguration(*m_save);
        if (result != SendResult::Sent) {
            finishSave(false, tr("send failed: %1").arg(m_lastError),
                       result == SendResult::StaleTarget);
            return;
        }
        m_save->step = 1;
        m_timer.start(m_resendDelayMs);
        return;
    }
    // Final read-back request of the field that was written.
    const SendResult result = sendGet(messageIdFor(m_save->field));
    if (result != SendResult::Sent) {
        finishSave(false, tr("send failed: %1").arg(m_lastError),
                   result == SendResult::StaleTarget);
        return;
    }
    m_save->step = 2;
    finishSave(true, QString(), false);
}

void AdsbIdentificationClient::finishSave(bool completed, const QString &reason,
                                          bool cancelled)
{
    // Detach before emitting so a slot may start the next save re-entrantly.
    std::unique_ptr<SaveOperation> operation = std::move(m_save);
    m_timer.stop();
    if (!operation) {
        return;
    }
    if (completed) {
        emit saveCompleted(operation->field, operation->generation);
    } else {
        emit saveFailed(operation->field, reason, cancelled);
    }
}

void AdsbIdentificationClient::cancel()
{
    finishSave(false, tr("cancelled"), true);
}

void AdsbIdentificationClient::handleTargetGenerationChanged(qulonglong generation)
{
    if (!m_lease.isValid() || generation == m_lease.generation || targetIsCurrent()) {
        return;
    }
    const qulonglong invalidated = m_lease.generation;
    m_lease = VehicleTargetLease{};
    finishSave(false, tr("the selected vehicle changed"), true);
    emit leaseInvalidated(invalidated);
}

void AdsbIdentificationClient::observeMessage(int linkId, const mavlink_message_t &message)
{
    if (!m_lease.isValid() || linkId != m_lease.endpoint.linkId ||
        message.sysid != m_lease.endpoint.systemId ||
        message.compid != m_lease.endpoint.componentId) {
        return;
    }
    if (message.msgid != FlightIdMessageId && message.msgid != RegistrationMessageId) {
        return;
    }
    if (!targetIsCurrent()) {
        return;
    }
    const qulonglong generation = m_lease.generation;
    if (message.msgid == FlightIdMessageId) {
        mavlink_uavionix_adsb_out_cfg_flightid_t decoded;
        std::memset(&decoded, 0, sizeof(decoded));
        mavlink_msg_uavionix_adsb_out_cfg_flightid_decode(&message, &decoded);
        emit flightIdReceived(generation,
                              DecodeDeviceText(decoded.flight_id, DevicePayloadLength));
    } else {
        mavlink_uavionix_adsb_out_cfg_registration_t decoded;
        std::memset(&decoded, 0, sizeof(decoded));
        mavlink_msg_uavionix_adsb_out_cfg_registration_decode(&message, &decoded);
        emit registrationReceived(generation,
                                  DecodeDeviceText(decoded.registration, DevicePayloadLength));
    }
}

QString AdsbIdentificationClient::DecodeDeviceText(const char *bytes, int length)
{
    if (!bytes || length <= 0) {
        return QString();
    }
    QString text;
    text.reserve(length);
    for (int index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(bytes[index]);
        text.append(byte > 0x7F ? QLatin1Char('?') : QLatin1Char(static_cast<char>(byte)));
    }
    int end = text.size();
    while (end > 0 &&
           (text.at(end - 1) == QLatin1Char('\0') || text.at(end - 1) == QLatin1Char(' '))) {
        --end;
    }
    text.truncate(end);
    return text;
}

QString AdsbIdentificationClient::DecodeDeviceText(const QByteArray &bytes)
{
    return DecodeDeviceText(bytes.constData(), bytes.size());
}

QByteArray AdsbIdentificationClient::EncodeDeviceText(const QString &text, QString *error)
{
    if (text.size() > DeviceTextLength) {
        if (error) {
            *error = tr("Use at most %1 characters.").arg(DeviceTextLength);
        }
        return QByteArray();
    }
    QByteArray payload(DevicePayloadLength, '\0');
    for (int index = 0; index < text.size(); ++index) {
        const ushort code = text.at(index).unicode();
        if (code < 0x20 || code > 0x7E) {
            if (error) {
                *error = tr("Only printable ASCII characters are accepted.");
            }
            return QByteArray();
        }
        payload[index] = static_cast<char>(code);
    }
    return payload;
}

bool AdsbIdentificationClient::NeedsClearConfirmation(const QString &text)
{
    return text.trimmed().isEmpty();
}

quint32 AdsbIdentificationClient::messageIdFor(Field field)
{
    return field == Field::FlightId ? FlightIdMessageId : RegistrationMessageId;
}
