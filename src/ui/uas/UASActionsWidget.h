#ifndef UASACTIONSWIDGET_H
#define UASACTIONSWIDGET_H

#include <QPointer>
#include <QWidget>
#include "comm/VehicleEndpoint.h"
#include "ui_UASActionsWidget.h"
#include <UASManager.h>
#include <UASInterface.h>

class QTimer;
class ApmQmlApi;
class VehicleCommandService;
class VehicleTargetManager;

class UASActionsWidget : public QWidget
{
    Q_OBJECT
    
public:
    explicit UASActionsWidget(QWidget *parent = 0);
    ~UASActionsWidget();

    int preFlightWarningBox(QWidget* parent);
    bool commandSurfaceAvailable() const;
    bool vehicleConnected() const;
    QString actionStatus() const;

public slots:
    void requestQuickMode(const QString &mode);

signals:
    void commandSurfaceAvailableChanged(bool available);
    void actionStatusChanged(const QString &message, bool error);
    void clearTrackRequested();
    void rawSensorViewRequested();
    void joystickSetupRequested();

private slots:
    void activeUASSet(UASInterface *uas);
    void uasConnected();
    void uasDisconnected();

    void armButtonClicked();
    void armingChanged(bool state);
    void currentWaypointChanged(quint16 wpid);
    void updateWaypointList();
    void goToWaypointClicked();
    void changeAltitudeClicked();
    void changeSpeedClicked();
    void setMode();   //[TODO] create a new ardupilot mode types
    void setShortcutMode();
    void setAction();
    void setRTLMode();
    void restartMissionClicked();
    void abortLandingClicked();
    void commandAckReceived(int uasId, int componentId, int command,
                            int result, int progress, int resultParam2,
                            int targetSystem, int targetComponent);
    void exactCommandAckReceived(
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        int command, int result, int progress, int resultParam2,
        int targetSystem, int targetComponent);
    void targetGenerationChanged(qulonglong generation);
    void commandAckTimedOut();

    void parameterChanged(int uas, int component, int parameterCount,
                          int parameterId, QString parameterName, QVariant value);
private:
    void setupApmCopterModes();
    void setupApmPlaneModes();
    void setupApmRoverModes();
    void setupApmActionList();
    void syncQmlActions();
    void sendFormatSdCard();

    void sendApmPlaneCommand(MAV_CMD command);
    void sendApmCopterCommand(MAV_CMD command);
    void sendApmRoverCommand(MAV_CMD command);

    bool activeUas();

    int modeChangeWarningBox(const QString& modeString);

    void showPreflightCalibrationDialog();

    void setControlsConnected(bool connected);
    void setActionStatus(const QString &message, bool error = false);
    bool beginCommand(MAV_CMD command, const QString &action);
    bool dispatchCommandLong(
        MAV_CMD command, int confirmation,
        float param1, float param2, float param3, float param4,
        float param5, float param6, float param7,
        int legacyComponent);
    void processCommandAck(int command, int result, int progress);
    void clearPendingCommand();
    static QString commandAckResultText(int result);

private:
    Ui::UASActionsWidget ui;
    QPointer<UAS> m_uas;
    quint16 m_last_wpid = 0;
    QTimer *m_commandAckTimer = nullptr;
    int m_pendingCommand = -1;
    QString m_pendingAction;
    VehicleTargetLease m_pendingTarget;
    bool m_connected = false;
    QPointer<ApmQmlApi> m_qmlApi;
    QPointer<VehicleCommandService> m_commandService;
    QPointer<VehicleTargetManager> m_targetManager;
};

#endif // UASACTIONSWIDGET_H
