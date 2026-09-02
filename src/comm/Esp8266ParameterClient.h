#ifndef ESP8266PARAMETERCLIENT_H
#define ESP8266PARAMETERCLIENT_H

#include "VehicleEndpoint.h"
#include "ui/configuration/Esp8266Settings.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <mavlink.h>

class ExactLinkTransmitter;
class VehicleTargetManager;

using Esp8266RawParameters = Esp8266SettingsCodec::RawValues;
Q_DECLARE_METATYPE(Esp8266RawParameters)

/**
 * Exact-link MAVLink transport for Mission Planner 10's ESP8266 setup page.
 *
 * The page is selected through the current autopilot target, while the actual
 * protocol peer is MAV_COMP_ID_UDP_BRIDGE (240) on the same physical link and
 * system.  This client deliberately keeps that secondary component out of the
 * global target selector: an immutable lease of the selected autopilot guards
 * every request, reply, retry and command in the operation.
 */
class Esp8266ParameterClient final : public QObject
{
    Q_OBJECT

public:
    enum class Operation
    {
        None,
        Loading,
        Saving,
        Resetting
    };
    Q_ENUM(Operation)

    enum class SendResult
    {
        Sent,
        NotBound,
        StaleTarget,
        Busy,
        InvalidRequest,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    static constexpr quint8 UdpBridgeComponentId = MAV_COMP_ID_UDP_BRIDGE;
    static constexpr int DefaultLoadTimeoutMs = 3000;
    static constexpr int DefaultAcknowledgementTimeoutMs = 700;
    static constexpr int DefaultMaximumRetries = 3;

    Esp8266ParameterClient(VehicleTargetManager *targetManager,
                           ExactLinkTransmitter *transmitter,
                           QObject *parent = nullptr);
    ~Esp8266ParameterClient() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    bool bind(const VehicleTargetLease &primaryTarget);
    void unbind();
    bool isBound() const { return m_primaryTarget.isValid(); }
    VehicleTargetLease primaryTarget() const { return m_primaryTarget; }

    Operation operation() const { return m_operation; }
    bool isBusy() const { return m_operation != Operation::None; }
    QString lastError() const { return m_lastError; }

    SendResult requestParameters();
    SendResult save(const QList<Esp8266ParameterWrite> &writes);
    SendResult resetDefaults();
    void cancel();

    // Feed frames received on the selected UAS/link. Only replies from
    // (leased link, leased system, component 240) are accepted.
    void observeMessage(int linkId, const mavlink_message_t &message);

    void setTimingForTesting(int loadTimeoutMs,
                             int acknowledgementTimeoutMs,
                             int maximumRetries = DefaultMaximumRetries);

signals:
    void loadStarted(qulonglong targetGeneration);
    void parametersLoaded(qulonglong targetGeneration,
                          const Esp8266RawParameters &values);
    void loadFailed(qulonglong targetGeneration, const QString &reason,
                    const QStringList &missingParameters);

    void saveStarted(qulonglong targetGeneration, int totalWrites);
    void saveProgress(qulonglong targetGeneration, int completedWrites,
                      int totalWrites, const QString &parameterName);
    void saveCompleted(qulonglong targetGeneration);

    void resetStarted(qulonglong targetGeneration);
    void resetCompleted(qulonglong targetGeneration);

    void operationFailed(Esp8266ParameterClient::Operation operation,
                         qulonglong targetGeneration, const QString &reason,
                         bool cancelled);
    void leaseInvalidated(qulonglong targetGeneration);

private:
    enum class Stage
    {
        Idle,
        LoadReply,
        ParameterEcho,
        SaveStorageAck,
        SaveRebootAck,
        ResetEraseAck,
        ResetStorageAck
    };

    bool targetIsCurrent() const;
    bool sourceMatches(int linkId, const mavlink_message_t &message) const;
    SendResult validateStart() const;
    SendResult sendMessage(mavlink_message_t message);
    SendResult sendParameterListRequest();
    SendResult sendCurrentParameterWrite();
    SendResult sendCurrentCommand();
    SendResult sendCommand(MAV_CMD command, float param1, float param2,
                           quint8 confirmation);
    void startTimer();
    void handleTimeout();
    void handleParameterValue(const mavlink_message_t &message);
    void handleCommandAck(const mavlink_message_t &message);
    void beginCommandStage(Stage stage);
    void finishLoad();
    void finishSuccess();
    void finishFailure(const QString &reason, bool cancelled);
    void clearOperation();
    QStringList missingParameters() const;
    void handleTargetGenerationChanged(qulonglong generation);

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    VehicleTargetLease m_primaryTarget;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    Operation m_operation = Operation::None;
    quint64 m_operationGeneration = 0;
    Stage m_stage = Stage::Idle;
    Esp8266RawParameters m_values;
    QList<Esp8266ParameterWrite> m_writes;
    int m_writeIndex = 0;
    int m_attempts = 0;
    int m_loadTimeoutMs = DefaultLoadTimeoutMs;
    int m_acknowledgementTimeoutMs = DefaultAcknowledgementTimeoutMs;
    int m_maximumRetries = DefaultMaximumRetries;
    QTimer m_timer;
    QString m_lastError;
};

#endif // ESP8266PARAMETERCLIENT_H
