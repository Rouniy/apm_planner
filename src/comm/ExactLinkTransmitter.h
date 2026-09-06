#ifndef EXACTLINKTRANSMITTER_H
#define EXACTLINKTRANSMITTER_H

#include <QByteArray>
#include <QHash>
#include <QObject>

#include <functional>

#include <mavlink.h>

/**
 * Single MAVLink frame sequencer for all exact-endpoint services.
 *
 * MAVLink sequence numbers and outbound protocol version belong to a physical
 * link, not to a command/parameter/mission feature.  Every exact service must
 * therefore share this application-owned transmitter.
 */
class ExactLinkTransmitter final : public QObject
{
    Q_OBJECT

public:
    enum class SendResult {
        Sent,
        InvalidLink,
        InvalidMessage,
        RestrictedMessage,
        IncompatibleVersion,
        SigningUnavailable,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    using FrameWriter = std::function<bool(int linkId,
                                            const QByteArray &frame)>;
    using FrameSigner = std::function<bool(int linkId, const QByteArray &frame,
                                            QByteArray *signedFrame)>;

    explicit ExactLinkTransmitter(FrameWriter frameWriter,
                                  QObject *parent = nullptr);

    SendResult sendMessage(
        int linkId, quint8 localSystemId, quint8 localComponentId,
        const mavlink_message_t &message,
        bool *frameWriterInvoked = nullptr);
    SendResult sendCommandAck(
        int linkId, quint8 localSystemId, quint8 localComponentId,
        quint16 command, quint8 result,
        quint8 targetSystem, quint8 targetComponent,
        quint8 progress = 255, qint32 resultParam2 = 0);

    void setOutboundVersion(int linkId, unsigned int version);
    void setSigningRequired(int linkId, bool required);
    void setFrameSigner(FrameSigner signer);
    unsigned int outboundVersion(int linkId) const;
    bool supportsTargetedCommandAck(int linkId) const;
    void setMotorStopLinkEligible(int linkId, bool eligible);
    bool motorStopLinkEligible(int linkId) const;
    void setLinkSessionEpoch(int linkId, quint64 epoch);
    void forgetLink(int linkId);

signals:
    void messageSubmitted(int linkId, quint64 epoch,
                          mavlink_message_t message);

private:
    friend class LinkManager;
    friend class VehicleCommandService;
    friend class RemoteDataFlashLogService;
    friend class MavlinkSerialTcpBridgeService;
    // The exact command owner must survive signing and may revalidate its
    // immutable reservation immediately before the physical writer.
    SendResult sendGuardedCommandLong(int linkId, quint8 localSystemId,
        quint8 localComponentId, const mavlink_message_t &message,
        std::function<bool()> finalGuard, bool *frameWriterInvoked);
    // SERIAL_CONTROL has no destination fields. Only the dedicated-route
    // session owner may submit UART requests, data or its bounded release.
    SendResult sendSerialControl(int linkId, quint64 expectedEpoch,
        quint8 localSystemId, quint8 localComponentId, quint8 device,
        quint8 flags, quint16 timeout, quint32 baudRate, const QByteArray &data,
        bool *frameWriterInvoked = nullptr);
    // REMOTE_LOG has no transaction nonce and its backend does not verify
    // session ownership. Only the reviewed exact-session service sends it.
    SendResult sendRemoteLogBlockStatus(int linkId, quint64 expectedEpoch,
        quint8 localSystemId, quint8 localComponentId,
        quint8 targetSystem, quint8 targetComponent, quint32 sequence,
        quint8 status, bool *frameWriterInvoked = nullptr);
    // Secret-bearing provisioning is not a generic tool/plugin send. Only the
    // reviewed LinkManager transition may reach this typed, no-observer path.
    SendResult sendSetupSigning(int linkId, quint64 expectedEpoch,
        quint8 localSystemId, quint8 localComponentId,
        quint8 targetSystem, quint8 targetComponent, const QByteArray &key,
        quint64 initialTimestamp, bool *frameWriterInvoked = nullptr);
    SendResult sendMessageImpl(int linkId, quint8 localSystemId,
        quint8 localComponentId, mavlink_message_t message,
        bool *frameWriterInvoked, std::function<bool()> finalGuard = {});
    mavlink_status_t &transmitStatus(int linkId);

    const FrameWriter m_frameWriter;
    FrameSigner m_frameSigner;
    quint64 m_signerRevision = 0;
    QHash<int, mavlink_status_t> m_transmitStates;
    QHash<int, bool> m_motorStopLinkEligibility;
    QHash<int, quint64> m_linkSessionEpochs;
    QHash<int, bool> m_signingRequired;
};

#endif // EXACTLINKTRANSMITTER_H
