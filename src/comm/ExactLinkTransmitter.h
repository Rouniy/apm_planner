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
        IncompatibleVersion,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    using FrameWriter = std::function<bool(int linkId,
                                            const QByteArray &frame)>;

    explicit ExactLinkTransmitter(FrameWriter frameWriter,
                                  QObject *parent = nullptr);

    SendResult sendMessage(
        int linkId, quint8 localSystemId, quint8 localComponentId,
        mavlink_message_t message);
    SendResult sendCommandAck(
        int linkId, quint8 localSystemId, quint8 localComponentId,
        quint16 command, quint8 result,
        quint8 targetSystem, quint8 targetComponent,
        quint8 progress = 255, qint32 resultParam2 = 0);

    void setOutboundVersion(int linkId, unsigned int version);
    unsigned int outboundVersion(int linkId) const;
    bool supportsTargetedCommandAck(int linkId) const;
    void setMotorStopLinkEligible(int linkId, bool eligible);
    bool motorStopLinkEligible(int linkId) const;
    void forgetLink(int linkId);

private:
    mavlink_status_t &transmitStatus(int linkId);

    const FrameWriter m_frameWriter;
    QHash<int, mavlink_status_t> m_transmitStates;
    QHash<int, bool> m_motorStopLinkEligibility;
};

#endif // EXACTLINKTRANSMITTER_H
