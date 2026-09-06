#ifndef MAVLINKSERIALTCPBRIDGESERVICE_H
#define MAVLINKSERIALTCPBRIDGESERVICE_H

#include "SwarmTelemetryRegistry.h"
#include "VehicleTargetManager.h"

#include <QObject>
#include <QSharedDataPointer>

#include <functional>

class ExactLinkTransmitter;
class SerialBridgeTcpServer;

/**
 * Application-owned, single-vehicle SERIAL_CONTROL to TCP bridge.
 *
 * SERIAL_CONTROL has no target-system fields. A Plan therefore pins both the
 * selected vehicle instance and its physical link session, and ordinary UART
 * traffic is admitted only while that link contains one known MAVLink system.
 * The TCP listener owns no UART while it has no client. Disconnect/Stop sends
 * one best-effort zero-flag release only to the still-valid original instance;
 * neither release nor restoration of a changed baud rate is acknowledged by
 * the protocol.
 */
class MavlinkSerialTcpBridgeService final : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        quint8 device = SERIAL_CONTROL_DEV_GPS1;
        quint32 baudRate = 0;       // zero preserves the current UART baud
        quint16 listenPort = 500;   // zero is accepted for isolated tests
        bool allowRemoteClients = false;

        bool operator==(const Options &other) const noexcept
        {
            return device == other.device
                && baudRate == other.baudRate
                && listenPort == other.listenPort
                && allowRemoteClients == other.allowRemoteClients;
        }
        bool operator!=(const Options &other) const noexcept
        {
            return !(*this == other);
        }
    };

    class Plan
    {
    public:
        Plan();
        Plan(const Plan &other);
        Plan &operator=(const Plan &other);
        ~Plan();

        bool isValid() const noexcept;
        Options options() const;
        VehicleTargetLease target() const;
        SwarmVehicleInstanceLease vehicle() const;
        QString description() const;

    private:
        class Data;
        QSharedDataPointer<Data> d;
        friend class MavlinkSerialTcpBridgeService;
    };

    using RouteValidator =
        std::function<bool(const SwarmVehicleInstanceLease &, QString *)>;

    explicit MavlinkSerialTcpBridgeService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ExactLinkTransmitter *transmitter,
        quint8 localSystemId,
        quint8 localComponentId,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ~MavlinkSerialTcpBridgeService() override;

    bool prepare(const Options &options, Plan *plan,
                 QString *error = nullptr);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    bool start(const Plan &plan, quint64 *operationId,
               QString *error = nullptr);
    bool stop(quint64 operationId, QString *error = nullptr);
    void shutdown();

    bool busy() const;
    quint64 operationId() const;
    Plan activePlan() const;
    QString status() const;
    QString targetDescription() const;
    quint16 boundPort() const;
    bool hasClient() const;
    quint64 bytesFromTcp() const;
    quint64 bytesToTcp() const;
    quint64 droppedBytes() const;

public slots:
    void observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);

signals:
    void stateChanged();
    void finished(quint64 operationId, const QString &description);

private:
    struct Runtime;

    static bool validDevice(quint8 device) noexcept;
    static QString deviceName(quint8 device);
    static void assignError(QString *error, const QString &message);

    bool capturePlan(const Options &options, Plan *plan,
                     QString *error) const;
    bool planBelongsHere(const Plan &plan) const noexcept;
    bool validateVehicle(const Plan &plan, bool requireSelected,
                         bool requireDisarmed, QString *error) const;
    bool stateIsSafe(const Plan &plan, bool requireSelected,
                     bool requireDisarmed, QString *error) const;
    bool hasOneKnownSystem(const Plan &plan, QString *error) const;
    bool operationIsCurrent(quint64 id, const Plan::Data *identity = nullptr) const;

    void handleClientConnected();
    void handleClientDisconnected();
    void handleTcpReadyRead();
    void handleTcpFailure(const QString &reason);
    void serviceTimers();
    void scheduleTransmit(int delayMs = 0);
    void transmitNext();
    bool sendSerialControl(quint64 id, const Plan &plan,
                           quint8 flags, quint16 timeout,
                           quint32 baudRate, const QByteArray &data,
                           bool cleanup, quint64 expectedClientGeneration,
                           bool *attempted = nullptr);
    void releaseClientUart(quint64 id, bool keepListening,
                           const QString &description);
    void terminate(quint64 id, const QString &description,
                   bool attemptRelease);
    void complete(quint64 id, const QString &description);
    void updateStatus(const QString &status);

    Runtime *m_runtime;
};

Q_DECLARE_METATYPE(MavlinkSerialTcpBridgeService::Options)
Q_DECLARE_METATYPE(MavlinkSerialTcpBridgeService::Plan)

#endif // MAVLINKSERIALTCPBRIDGESERVICE_H
