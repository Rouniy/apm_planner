#include "ExactLinkTransmitter.h"

#include <mavlink_helpers.h>

#include <QPointer>

#include <cstring>
#include <utility>

namespace {
void cleanse(void *address, size_t size)
{
    auto *bytes = static_cast<volatile unsigned char *>(address);
    while (size--) *bytes++ = 0;
}
struct SecretMessageGuard {
    mavlink_message_t &message;
    bool sensitive;
    ~SecretMessageGuard() { if (sensitive) cleanse(&message, sizeof(message)); }
};
struct SecretFrameGuard {
    bool sensitive;
    quint8 *buffer;
    QByteArray &frame, &signedFrame;
    ~SecretFrameGuard()
    {
        if (!sensitive) return;
        cleanse(buffer, MAVLINK_MAX_PACKET_LEN);
        if (!frame.isEmpty()) cleanse(frame.data(), size_t(frame.size()));
        if (!signedFrame.isEmpty()) cleanse(signedFrame.data(), size_t(signedFrame.size()));
    }
};
}

ExactLinkTransmitter::ExactLinkTransmitter(
    FrameWriter frameWriter, QObject *parent)
    : QObject(parent)
    , m_frameWriter(std::move(frameWriter))
{
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendMessage(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    const mavlink_message_t &message, bool *frameWriterInvoked)
{
    if (frameWriterInvoked) *frameWriterInvoked = false;
    if (message.msgid == MAVLINK_MSG_ID_SETUP_SIGNING
        || message.msgid == MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS
        || message.msgid == MAVLINK_MSG_ID_SERIAL_CONTROL)
        return SendResult::RestrictedMessage;
    return sendMessageImpl(linkId, localSystemId, localComponentId,
                           message, frameWriterInvoked);
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendSerialControl(
    int linkId, quint64 expectedEpoch, quint8 localSystemId,
    quint8 localComponentId, quint8 device, quint8 flags, quint16 timeout,
    quint32 baudRate, const QByteArray &data, bool *frameWriterInvoked)
{
    if (frameWriterInvoked) *frameWriterInvoked = false;
    if (linkId < 0 || !expectedEpoch
        || m_linkSessionEpochs.value(linkId, 0) != expectedEpoch)
        return SendResult::InvalidLink;
    const bool deviceValid = device <= SERIAL_CONTROL_DEV_GPS2
        || device == SERIAL_CONTROL_DEV_SHELL
        || (device >= SERIAL_CONTROL_SERIAL0 && device <= SERIAL_CONTROL_SERIAL9);
    const quint8 allowed = SERIAL_CONTROL_FLAG_EXCLUSIVE
        | SERIAL_CONTROL_FLAG_RESPOND | SERIAL_CONTROL_FLAG_MULTI;
    if (!localSystemId || !localComponentId || !deviceValid
        || data.size() > MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN
        || timeout > 100 || (flags & ~allowed)
        || (flags && !(flags & SERIAL_CONTROL_FLAG_EXCLUSIVE))
        || ((flags & SERIAL_CONTROL_FLAG_MULTI) && !(flags & SERIAL_CONTROL_FLAG_RESPOND))
        || (!flags && (!data.isEmpty() || timeout || baudRate)))
        return SendResult::InvalidMessage;
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_SERIAL_CONTROL;
    message.len = MAVLINK_MSG_ID_SERIAL_CONTROL_LEN;
    char *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint32_t(payload, 0, baudRate);
    _mav_put_uint16_t(payload, 4, timeout);
    _mav_put_uint8_t(payload, 6, device);
    _mav_put_uint8_t(payload, 7, flags);
    _mav_put_uint8_t(payload, 8, static_cast<quint8>(data.size()));
    if (!data.isEmpty()) std::memcpy(payload + 9, data.constData(), data.size());
    return sendMessageImpl(linkId, localSystemId, localComponentId,
                           message, frameWriterInvoked);
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendRemoteLogBlockStatus(
    int linkId, quint64 expectedEpoch, quint8 localSystemId,
    quint8 localComponentId, quint8 targetSystem, quint8 targetComponent,
    quint32 sequence, quint8 status, bool *frameWriterInvoked)
{
    if (frameWriterInvoked) *frameWriterInvoked = false;
    if (linkId < 0 || !expectedEpoch
        || m_linkSessionEpochs.value(linkId, 0) != expectedEpoch)
        return SendResult::InvalidLink;
    if (!localSystemId || !localComponentId || !targetSystem || !targetComponent
        || status != MAV_REMOTE_LOG_DATA_BLOCK_ACK
        || (sequence >= MAV_REMOTE_LOG_DATA_BLOCK_STOP
            && sequence != MAV_REMOTE_LOG_DATA_BLOCK_START
            && sequence != MAV_REMOTE_LOG_DATA_BLOCK_STOP))
        return SendResult::InvalidMessage;
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS;
    message.len = MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS_LEN;
    char *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint32_t(payload, 0, sequence);
    _mav_put_uint8_t(payload, 4, targetSystem);
    _mav_put_uint8_t(payload, 5, targetComponent);
    _mav_put_uint8_t(payload, 6, status);
    return sendMessageImpl(linkId, localSystemId, localComponentId,
                           message, frameWriterInvoked);
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendSetupSigning(
    int linkId, quint64 expectedEpoch, quint8 localSystemId,
    quint8 localComponentId, quint8 targetSystem, quint8 targetComponent,
    const QByteArray &key, quint64 initialTimestamp, bool *frameWriterInvoked)
{
    if (frameWriterInvoked) *frameWriterInvoked = false;
    if (linkId < 0 || !expectedEpoch
        || m_linkSessionEpochs.value(linkId, 0) != expectedEpoch)
        return SendResult::InvalidLink;
    if (!targetSystem || !targetComponent || key.size() != 32
        || key == QByteArray(32, '\0') || !initialTimestamp
        || initialTimestamp >= ((quint64(1) << 48) - 6000000))
        return SendResult::InvalidMessage;
    mavlink_message_t message{};
    const SecretMessageGuard guard{message, true};
    message.msgid = MAVLINK_MSG_ID_SETUP_SIGNING;
    message.len = MAVLINK_MSG_ID_SETUP_SIGNING_LEN;
    char *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint64_t(payload, 0, initialTimestamp);
    _mav_put_uint8_t(payload, 8, targetSystem);
    _mav_put_uint8_t(payload, 9, targetComponent);
    std::memcpy(payload + 10, key.constData(), 32);
    return sendMessageImpl(linkId, localSystemId, localComponentId,
                           message, frameWriterInvoked);
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendGuardedCommandLong(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    const mavlink_message_t &message, std::function<bool()> finalGuard,
    bool *frameWriterInvoked)
{
    if (frameWriterInvoked) *frameWriterInvoked = false;
    if (message.msgid != MAVLINK_MSG_ID_COMMAND_LONG || !finalGuard)
        return SendResult::InvalidMessage;
    return sendMessageImpl(linkId, localSystemId, localComponentId, message,
                           frameWriterInvoked, std::move(finalGuard));
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendMessageImpl(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    mavlink_message_t message, bool *frameWriterInvoked,
    std::function<bool()> finalGuard)
{
    const bool sensitive = message.msgid == MAVLINK_MSG_ID_SETUP_SIGNING;
    const SecretMessageGuard messageGuard{message, sensitive};
    if (frameWriterInvoked) {
        *frameWriterInvoked = false;
    }
    if (linkId < 0) {
        return SendResult::InvalidLink;
    }
    if (!m_frameWriter) {
        return SendResult::TransportUnavailable;
    }

    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(message.msgid);
    if (!entry) {
        return SendResult::InvalidMessage;
    }
    const auto existingStatus = m_transmitStates.constFind(linkId);
    const bool mavlink1 = existingStatus != m_transmitStates.constEnd()
        && (existingStatus->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1);
    if (mavlink1 && message.msgid > 255U) {
        // mavlink_finalize_message_buffer() truncates a v2-only message ID in
        // a v1 header.  Reject it without consuming the link sequence.
        return SendResult::IncompatibleVersion;
    }

    // Generated *_encode() helpers finalize once through MAVLINK_COMM_0.
    // For a trimmed MAVLink 2 payload they place the checksum immediately at
    // payload[len].  A second finalization could then mistake that checksum
    // byte for a non-zero trailing field and put it on the wire as payload
    // (notably the NUL terminator of a 15-character parameter name).  Restore
    // the protocol-defined zero tail before assigning this link's sequence.
    if (!mavlink1 && message.magic == MAVLINK_STX_MAVLINK1
        && entry->max_msg_len > entry->min_msg_len) {
        // A generated extension-bearing message finalized as MAVLink 1 has
        // already overwritten the first extension bytes with its checksum.
        // Reconstructing it would silently invent field values. Require a
        // headerless/typed sender instead of emitting a corrupted MAVLink 2
        // frame (sendCommandAck() is the typed path used by CompassMot).
        return SendResult::IncompatibleVersion;
    }
    if (message.len < entry->max_msg_len) {
        std::memset(
            &_MAV_PAYLOAD_NON_CONST(&message)[message.len], 0,
            static_cast<size_t>(entry->max_msg_len - message.len));
    }
    mavlink_status_t &status = transmitStatus(linkId);
    mavlink_finalize_message_buffer(
        &message, localSystemId, localComponentId, &status,
        entry->min_msg_len, entry->max_msg_len, entry->crc_extra);

    quint8 buffer[MAVLINK_MAX_PACKET_LEN]{};
    const quint16 frameLength =
        mavlink_msg_to_send_buffer(buffer, &message);
    QByteArray frame(reinterpret_cast<const char *>(buffer), frameLength);
    QByteArray signedFrame;
    const SecretFrameGuard frameGuard{sensitive, buffer, frame, signedFrame};
    const quint64 submittedEpoch = m_linkSessionEpochs.value(linkId, 0);
    const quint64 signingRevision = m_signerRevision;
    const QPointer<ExactLinkTransmitter> guardedThis(this);
    if (m_frameSigner) {
        const FrameSigner signer = m_frameSigner;
        const quint64 revision = m_signerRevision;
        if (!signer(linkId, frame, &signedFrame) || !guardedThis
            || guardedThis->m_signerRevision != revision
            || guardedThis->m_linkSessionEpochs.value(linkId, 0) != submittedEpoch)
            return SendResult::SigningUnavailable;
        if (signedFrame != frame) {
            // A signer may only attach an authentication trailer. It cannot
            // retarget/resequence/re-trim or otherwise rewrite the command.
            if (message.magic != MAVLINK_STX || message.incompat_flags != 0
                || signedFrame.size() != frame.size() + MAVLINK_SIGNATURE_BLOCK_LEN
                || signedFrame.left(2) != frame.left(2)
                || quint8(signedFrame[2]) != MAVLINK_IFLAG_SIGNED
                || signedFrame.mid(3, frame.size() - 5) != frame.mid(3, frame.size() - 5))
                return SendResult::SigningUnavailable;
            const auto *bytes = reinterpret_cast<const uint8_t *>(signedFrame.constData());
            quint16 crc = crc_calculate(bytes + 1, frame.size() - 3);
            crc_accumulate(entry->crc_extra, &crc);
            if (bytes[frame.size() - 2] != (crc & 255)
                || bytes[frame.size() - 1] != (crc >> 8)) return SendResult::SigningUnavailable;
            message.incompat_flags = MAVLINK_IFLAG_SIGNED;
            message.checksum = crc;
            message.ck[0] = crc & 255;
            message.ck[1] = crc >> 8;
            std::memcpy(message.signature, bytes + frame.size(), MAVLINK_SIGNATURE_BLOCK_LEN);
        } else if (m_signingRequired.value(linkId, false)) {
            return SendResult::SigningUnavailable;
        }
        if (sensitive && frame != signedFrame && !frame.isEmpty())
            cleanse(frame.data(), size_t(frame.size()));
        frame = std::move(signedFrame);
    } else if (m_signingRequired.value(linkId, false)) {
        return SendResult::SigningUnavailable;
    }
    if (finalGuard) {
        // This callable is a local copy and may delete either owner. Its
        // callbacks can also retire the physical session or replace signing
        // policy, so preserve the signed frame's transport context as well.
        if (!finalGuard() || !guardedThis
            || guardedThis->m_linkSessionEpochs.value(linkId, 0) != submittedEpoch
            || guardedThis->m_signerRevision != signingRevision)
            return SendResult::TransportUnavailable;
        if (guardedThis->m_signingRequired.value(linkId, false)
            && !(message.incompat_flags & MAVLINK_IFLAG_SIGNED))
            return SendResult::SigningUnavailable;
    }
    if (frameWriterInvoked) {
        *frameWriterInvoked = true;
    }
    // The writer is allowed to synchronously tear down this transmitter as
    // part of link removal. Keep its callable alive independently of `this`
    // and guard every access after the callback returns.
    const FrameWriter frameWriter = m_frameWriter;
    if (!frameWriter(linkId, frame)) {
        return SendResult::TransportUnavailable;
    }
    if (message.msgid != MAVLINK_MSG_ID_SETUP_SIGNING
        && submittedEpoch != 0 && guardedThis
        && guardedThis->m_linkSessionEpochs.value(linkId, 0)
            == submittedEpoch) {
        emit guardedThis->messageSubmitted(
            linkId, submittedEpoch, message);
    }
    return SendResult::Sent;
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendCommandAck(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    quint16 command, quint8 result,
    quint8 targetSystem, quint8 targetComponent,
    quint8 progress, qint32 resultParam2)
{
    if ((targetSystem != 0 || targetComponent != 0)
        && !supportsTargetedCommandAck(linkId)) {
        return SendResult::IncompatibleVersion;
    }

    // Do not use generated *_pack/encode here: they finalize through global
    // MAVLINK_COMM_0, whose version belongs to whichever physical link most
    // recently mutated it. Build the schema payload headerless, then let this
    // transmitter finalize it against the destination link's own status.
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_COMMAND_ACK;
    message.len = MAVLINK_MSG_ID_COMMAND_ACK_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint16_t(payload, 0, command);
    _mav_put_uint8_t(payload, 2, result);
    _mav_put_uint8_t(payload, 3, progress);
    _mav_put_int32_t(payload, 4, resultParam2);
    _mav_put_uint8_t(payload, 8, targetSystem);
    _mav_put_uint8_t(payload, 9, targetComponent);
    return sendMessage(linkId, localSystemId, localComponentId, message);
}

void ExactLinkTransmitter::setOutboundVersion(
    int linkId, unsigned int version)
{
    if (linkId < 0 || (version != 1U && version != 2U)
        || (version == 1U && m_signingRequired.value(linkId, false))) {
        return;
    }
    mavlink_status_t &status = transmitStatus(linkId);
    if (version == 1U) {
        status.flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        status.flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
}

void ExactLinkTransmitter::setSigningRequired(int linkId, bool required)
{
    if (linkId < 0) return;
    if (required) {
        m_signingRequired.insert(linkId, true);
        setOutboundVersion(linkId, 2);
    } else {
        m_signingRequired.remove(linkId);
    }
}

void ExactLinkTransmitter::setFrameSigner(FrameSigner signer)
{
    m_frameSigner = std::move(signer);
    ++m_signerRevision;
}

unsigned int ExactLinkTransmitter::outboundVersion(int linkId) const
{
    const auto status = m_transmitStates.constFind(linkId);
    if (status == m_transmitStates.constEnd()) {
        // MAVLink 2 is the application default until protocol negotiation
        // explicitly constrains a physical link to MAVLink 1.
        return 2U;
    }
    return (status->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1) ? 1U : 2U;
}

bool ExactLinkTransmitter::supportsTargetedCommandAck(int linkId) const
{
    // COMMAND_ACK target_system/target_component are MAVLink 2 extension
    // fields. They improve observability but do not isolate CompassMot:
    // ArduCopter treats every received COMMAND_ACK as its stop signal.
    return linkId >= 0 && outboundVersion(linkId) == 2U;
}

void ExactLinkTransmitter::setMotorStopLinkEligible(
    int linkId, bool eligible)
{
    if (linkId < 0) {
        return;
    }
    m_motorStopLinkEligibility.insert(linkId, eligible);
}

bool ExactLinkTransmitter::motorStopLinkEligible(int linkId) const
{
    return linkId >= 0
        && m_motorStopLinkEligibility.value(linkId, false);
}

void ExactLinkTransmitter::setLinkSessionEpoch(int linkId, quint64 epoch)
{
    if (linkId < 0) {
        return;
    }
    if (epoch == 0) {
        m_linkSessionEpochs.remove(linkId);
    } else {
        m_linkSessionEpochs.insert(linkId, epoch);
    }
}

void ExactLinkTransmitter::forgetLink(int linkId)
{
    m_transmitStates.remove(linkId);
    m_motorStopLinkEligibility.remove(linkId);
    m_linkSessionEpochs.remove(linkId);
}

mavlink_status_t &ExactLinkTransmitter::transmitStatus(int linkId)
{
    auto status = m_transmitStates.find(linkId);
    if (status == m_transmitStates.end()) {
        mavlink_status_t initial{};
        status = m_transmitStates.insert(linkId, initial);
    }
    return status.value();
}
