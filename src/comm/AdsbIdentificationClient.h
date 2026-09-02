#ifndef ADSBIDENTIFICATIONCLIENT_H
#define ADSBIDENTIFICATIONCLIENT_H

#include "VehicleEndpoint.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include <mavlink.h>

class ExactLinkTransmitter;
class VehicleTargetManager;

/**
 * uAvionix ADS-B identification transport used by SETUP > ADSB
 * (Mission Planner 10 ConfigADSBViewModel identification commands).
 *
 * Reads and writes the transponder's Flight Identification
 * (UAVIONIX_ADSB_OUT_CFG_FLIGHTID, 10005) and Aircraft Registration
 * (UAVIONIX_ADSB_OUT_CFG_REGISTRATION, 10004) through UAVIONIX_ADSB_GET
 * (10006) and the configuration messages themselves, exactly as MP10 does:
 * a save sends the configuration twice with a 200 ms pause and then requests
 * the value back. All sends go through the shared ExactLinkTransmitter to the
 * physical link of an immutable VehicleTargetLease, every step re-checks the
 * target generation, and replies are accepted only from the leased
 * (link, system, component). The cadence is a QTimer state machine; nothing
 * blocks the GUI thread.
 */
class AdsbIdentificationClient final : public QObject
{
    Q_OBJECT

public:
    enum class Field
    {
        FlightId,
        Registration
    };
    Q_ENUM(Field)

    enum class SendResult
    {
        Sent,
        NotBound,
        StaleTarget,
        Busy,
        InvalidText,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    static constexpr quint32 RegistrationMessageId = MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_REGISTRATION; // 10004
    static constexpr quint32 FlightIdMessageId = MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_FLIGHTID;         // 10005
    static constexpr quint32 GetMessageId = MAVLINK_MSG_ID_UAVIONIX_ADSB_GET;                           // 10006
    static constexpr int DeviceTextLength = 8;   // characters accepted by the device
    static constexpr int DevicePayloadLength = 9; // wire field: 8 characters + NUL
    static constexpr int DefaultResendDelayMs = 200; // MP10 Thread.Sleep(200)

    AdsbIdentificationClient(VehicleTargetManager *targetManager,
                             ExactLinkTransmitter *transmitter,
                             QObject *parent = nullptr);
    ~AdsbIdentificationClient() override;

    // Local MAVLink identity used as the sender of every frame.
    void setLocalIdentity(quint8 systemId, quint8 componentId);
    quint8 localSystemId() const { return m_localSystemId; }
    quint8 localComponentId() const { return m_localComponentId; }

    int resendDelayMs() const { return m_resendDelayMs; }
    void setResendDelayMs(int milliseconds);

    // Binds the client to one exact target. Fails (and leaves the client
    // unbound) unless the lease is the current target. Rebinding cancels a
    // running save.
    bool bind(const VehicleTargetLease &lease);
    void unbind();
    bool isBound() const { return m_lease.isValid(); }
    VehicleTargetLease lease() const { return m_lease; }
    bool isBusy() const { return m_save != nullptr; }
    // Reason of the last transmitter failure (empty after a successful send).
    QString lastError() const { return m_lastError; }

    // MP10 RequestIdentification: UAVIONIX_ADSB_GET{10004} then {10005}.
    SendResult requestIdentification();
    // MP10 SaveFlightId / SaveAircraftRegistration: cfg, +delay cfg, +delay
    // GET{field message id}. Returns Busy while a save is running.
    SendResult save(Field field, const QString &text);
    // Aborts a running save; reports saveFailed(..., cancelled = true) once.
    void cancel();

    // Feed every received frame of the bound link here; only 10004/10005
    // from the leased (link, system, component) are decoded.
    void observeMessage(int linkId, const mavlink_message_t &message);

    // --- MP10 static helpers ---
    // Encoding.ASCII.GetString(bytes).TrimEnd('\0', ' '): bytes above 0x7F
    // become '?', trailing NUL and spaces are removed.
    static QString DecodeDeviceText(const char *bytes, int length);
    static QString DecodeDeviceText(const QByteArray &bytes);
    // Strict counterpart of MakeBytesSize(9): at most 8 printable ASCII
    // characters, NUL-padded to 9 bytes; empty on error.
    static QByteArray EncodeDeviceText(const QString &text, QString *error = nullptr);
    // string.IsNullOrWhiteSpace(value): an empty value clears the device
    // setting and needs an explicit confirmation.
    static bool NeedsClearConfirmation(const QString &text);
    static quint32 messageIdFor(Field field);

signals:
    void flightIdReceived(qulonglong targetGeneration, const QString &flightId);
    void registrationReceived(qulonglong targetGeneration, const QString &registration);
    void identificationRequested(qulonglong targetGeneration);
    void saveStarted(AdsbIdentificationClient::Field field, qulonglong targetGeneration);
    void saveCompleted(AdsbIdentificationClient::Field field, qulonglong targetGeneration);
    void saveFailed(AdsbIdentificationClient::Field field, const QString &reason,
                    bool cancelled);
    // The bound target stopped being current; the client is unbound.
    void leaseInvalidated(qulonglong targetGeneration);

private:
    struct SaveOperation
    {
        Field field = Field::FlightId;
        QByteArray payload; // 9 bytes
        quint64 generation = 0;
        int step = 0;       // 0: first cfg sent, 1: second cfg sent, 2: GET sent
    };

    bool targetIsCurrent() const;
    SendResult sendMessage(mavlink_message_t message);
    SendResult sendConfiguration(const SaveOperation &operation);
    SendResult sendGet(quint32 requestedMessageId);
    void advanceSave();
    void finishSave(bool completed, const QString &reason, bool cancelled);
    void handleTargetGenerationChanged(qulonglong generation);

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    VehicleTargetLease m_lease;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    int m_resendDelayMs = DefaultResendDelayMs;
    std::unique_ptr<SaveOperation> m_save;
    QTimer m_timer;
    QString m_lastError;
};

#endif // ADSBIDENTIFICATIONCLIENT_H
