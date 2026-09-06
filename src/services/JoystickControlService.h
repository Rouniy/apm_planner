#ifndef JOYSTICKCONTROLSERVICE_H
#define JOYSTICKCONTROLSERVICE_H

#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "input/JoystickConfiguration.h"
#include "input/JoystickDevice.h"

#include <QObject>
#include <QPointer>
#include <QSharedDataPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

class ExactLinkTransmitter;
class VehicleTargetManager;

/**
 * Application-owned joystick output. The selected SDL device may be viewed
 * and configured while disconnected, but MAVLink output is enabled only from
 * an immutable, consented device/vehicle plan. Closing a setup page does not
 * retarget or stop an active controller.
 */
class JoystickControlService final : public QObject
{
    Q_OBJECT

public:
    using Profile = JoystickConfiguration::Profile;
    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &, QString *)>;
    struct ChannelLimits {
        int minimum = 1000;
        int maximum = 2000;
        int trim = 1500;
    };
    // Returns up to 16 RC calibration rows. Missing rows use MP10's
    // 1000/2000/1500 fallback; MANUAL_CONTROL always uses -1000/1000/0.
    using ChannelLimitsProvider = std::function<bool(
        const SwarmVehicleInstanceLease &, QVector<ChannelLimits> *,
        QString *)>;

    class EnablePlan
    {
    public:
        EnablePlan();
        EnablePlan(const EnablePlan &);
        EnablePlan &operator=(const EnablePlan &);
        ~EnablePlan();

        bool isValid() const noexcept;
        VehicleTargetLease target() const;
        SwarmVehicleInstanceLease vehicle() const;
        Profile profile() const;
        qint32 deviceInstanceId() const noexcept;
        quint64 deviceGeneration() const noexcept;
        QString description() const;

    private:
        struct Data;
        QSharedDataPointer<Data> d;
        friend class JoystickControlService;
    };

    struct Preview
    {
        bool connected = false;
        quint64 deviceGeneration = 0;
        QVector<int> channels;
        QVector<bool> mapped;
        QVector<bool> buttons;
    };

    explicit JoystickControlService(
        JoystickDevice *device,
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ExactLinkTransmitter *transmitter,
        VehicleCommandService *commandService,
        quint8 localSystemId,
        quint8 localComponentId,
        ChannelLimitsProvider channelLimitsProvider,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ~JoystickControlService() override;

    JoystickDevice *device() const noexcept { return m_device; }
    Profile configuration() const { return m_profile; }
    bool setConfiguration(const Profile &profile, QString *error = nullptr);

    bool isEnabled() const noexcept { return m_enabled || m_finishing; }
    QString statusText() const { return m_status; }
    EnablePlan activePlan() const { return m_activePlan; }
    VehicleTargetLease activeTarget() const;
    QString targetDescription() const;
    Preview preview() const { return m_preview; }
    quint64 framesAttempted() const noexcept { return m_framesAttempted; }
    quint64 framesSubmitted() const noexcept { return m_framesSubmitted; }
    quint64 buttonActionsSubmitted() const noexcept
    {
        return m_buttonActionsSubmitted;
    }

    bool prepareEnable(EnablePlan *planOut, QString *error = nullptr) const;
    bool validate(const EnablePlan &plan, QString *error = nullptr) const;
    bool enable(const EnablePlan &plan, QString *error = nullptr);
    bool disable(const QString &reason = QString());
    void shutdown();
    void setSendIntervalForTesting(int milliseconds);
    void setDeviceStateForTesting(const JoystickDevice::Info &device,
                                  const JoystickDevice::Snapshot &snapshot);

signals:
    void stateChanged();
    void previewChanged(JoystickControlService::Preview preview);
    void stopped(QString reason);
    void buttonActionFinished(QString function, bool accepted,
                              QString description);

private:
    struct PendingButtonAction;
    bool planIsCurrent(const EnablePlan &plan, QString *error) const;
    bool vehicleIsSafe(const EnablePlan &plan, QString *error) const;
    bool operationIsCurrent(quint64 generation,
                            const EnablePlan &plan) const noexcept;
    Preview makePreview(const JoystickDevice::Snapshot &snapshot,
                        const Profile &profile,
                        const QVector<ChannelLimits> &limits = {}) const;
    void handleSnapshot(const JoystickDevice::Snapshot &snapshot);
    void handleDisconnected(const QString &reason);
    void sendControlFrame();
    bool sendRelease(const EnablePlan &plan);
    void handleButtonEdges(const JoystickDevice::Snapshot &snapshot);
    void dispatchButtonAction(int configurationIndex, bool down);
    bool buildButtonCommand(const JoystickConfiguration::Button &button,
                            const SwarmTelemetrySnapshot &snapshot,
                            VehicleCommandService::ExactCommandRequest *request,
                            QString *error) const;
    void handleCommandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void finishDisable(const QString &reason, bool attemptRelease);
    void setStatus(const QString &status);
    JoystickDevice::Snapshot currentDeviceSnapshot() const;
    JoystickDevice::Info currentDeviceInfo() const;

    QPointer<JoystickDevice> m_device;
    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<SwarmTelemetryRegistry> m_telemetryRegistry;
    QPointer<ExactLinkTransmitter> m_transmitter;
    QPointer<VehicleCommandService> m_commandService;
    const quint8 m_localSystemId;
    const quint8 m_localComponentId;
    RouteValidator m_routeValidator;
    ChannelLimitsProvider m_channelLimitsProvider;
    Profile m_profile;
    EnablePlan m_activePlan;
    Preview m_preview;
    QTimer m_sendTimer;
    VehicleCommandService::ExactReservationToken m_buttonReservation;
    VehicleCommandService::ExactCommandToken m_buttonCommand;
    QString m_pendingButtonFunction;
    QVector<bool> m_previousButtons;
    qint64 m_customAxis0 = 1500;
    qint64 m_customAxis1 = 1500;
    quint16 m_hatVertical = 32768;
    quint16 m_hatHorizontal = 32768;
    float m_pendingTakeoffAltitude = 0.0F;
    QString m_status;
    quint64 m_generation = 0;
    quint64 m_framesAttempted = 0;
    quint64 m_framesSubmitted = 0;
    quint64 m_buttonActionsSubmitted = 0;
    int m_sendIntervalMs = 50;
    bool m_enabled = false;
    bool m_finishing = false;
    bool m_sending = false;
    bool m_shuttingDown = false;
    bool m_pendingTakeoff = false;
    bool m_hasTestDeviceState = false;
    JoystickDevice::Info m_testDeviceInfo;
    JoystickDevice::Snapshot m_testDeviceSnapshot;
};

Q_DECLARE_METATYPE(JoystickControlService::EnablePlan)
Q_DECLARE_METATYPE(JoystickControlService::Preview)

#endif // JOYSTICKCONTROLSERVICE_H
