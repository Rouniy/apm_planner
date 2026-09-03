#ifndef DEVICEOPERATIONSERVICE_H
#define DEVICEOPERATIONSERVICE_H

#include "VehicleEndpoint.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include <mavlink.h>

class ExactLinkTransmitter;
class VehicleTargetManager;

/**
 * One independent, exact-link MAVLink DEVICE_OP session.
 *
 * A window owns one service and may have one request in flight. Wire request
 * identifiers are unique across every service instance in this process, so
 * independently opened windows cannot consume one another's replies.
 */
class DeviceOperationService final : public QObject
{
    Q_OBJECT

public:
    enum class BusType : quint8 {
        I2C = 0,
        SPI = 1
    };
    Q_ENUM(BusType)

    enum class ArmState {
        Unknown,
        Disarmed,
        Armed
    };
    Q_ENUM(ArmState)

    enum class StartResult {
        Started,
        NotBound,
        StaleTarget,
        Busy,
        InvalidRequest,
        UnsafeArmState,
        Mavlink1Unsupported,
        TransportUnavailable,
        ShuttingDown
    };
    Q_ENUM(StartResult)

    struct Request
    {
        int destinationSystemId = 1;
        int destinationComponentId = 1;
        BusType busType = BusType::SPI;
        QString busName = QStringLiteral("icm20948_ext");
        int bus = 0;
        int address = 0;
        int registerStart = 255;
        int count = 1;
    };

    struct OperationResult
    {
        Request request;
        bool icm20948Test = false;
        bool cancelled = false;
        QString error;

        bool writeReplyReceived = false;
        bool writeTimedOut = false;
        quint8 writeResult = 0;

        bool readReplyReceived = false;
        bool readTimedOut = false;
        quint8 readResult = 0;
        quint8 readRegister = 0;
        QByteArray readData;
    };

    static constexpr int DefaultTimeoutMs = 1000;
    static constexpr int MaximumDataLength = 128;
    static constexpr int MaximumBusNameBytes = 40;

    DeviceOperationService(VehicleTargetManager *targetManager,
                           ExactLinkTransmitter *transmitter,
                           QObject *parent = nullptr);
    ~DeviceOperationService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setTimeoutMs(int timeoutMs);
    int timeoutMs() const { return m_timeoutMs; }

    bool bind(const VehicleTargetLease &lease);
    void unbind();
    bool isBound() const;
    bool isBusy() const { return m_operation != nullptr; }
    bool requiresRebind() const { return m_requiresRebind; }
    VehicleTargetLease lease() const { return m_lease; }
    ArmState armState() const { return m_armState; }
    QString lastError() const { return m_lastError; }

    StartResult startRead(const Request &request);
    StartResult startIcm20948Test(const Request &request);

    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void cancel();
    void shutdown();

    static QString ValidateRequest(const Request &request);

signals:
    void stateChanged();
    void operationFinished(
        const DeviceOperationService::OperationResult &result);
    void bindingInvalidated(const QString &reason);

private:
    enum class Stage {
        Read,
        IcmWrite,
        IcmRead
    };

    struct Operation
    {
        Stage stage = Stage::Read;
        quint32 requestId = 0;
        quint64 targetGeneration = 0;
        OperationResult result;
    };

    bool targetIsCurrent() const;
    StartResult begin(const Request &request, bool icm20948Test);
    StartResult sendRead(Operation &operation);
    StartResult sendIcmWrite(Operation &operation);
    StartResult sendMessage(mavlink_message_t message);
    void beginIcmRead();
    void handleTimeout();
    void finish(OperationResult result);
    void invalidateBinding(const QString &reason);
    void updateArmState(int linkId, const mavlink_message_t &message);
    ArmState knownArmState(const VehicleEndpoint &endpoint) const;
    static quint32 nextRequestId();
    static QString StartFailureText(StartResult result);

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    VehicleTargetLease m_lease;
    QHash<VehicleEndpoint, ArmState> m_armStates;
    ArmState m_armState = ArmState::Unknown;
    std::unique_ptr<Operation> m_operation;
    QTimer m_timeout;
    QString m_lastError;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    int m_timeoutMs = DefaultTimeoutMs;
    bool m_requiresRebind = false;
    bool m_shuttingDown = false;
};

Q_DECLARE_METATYPE(DeviceOperationService::Request)
Q_DECLARE_METATYPE(DeviceOperationService::OperationResult)

#endif // DEVICEOPERATIONSERVICE_H
