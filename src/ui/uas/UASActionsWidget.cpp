#include "logging.h"
#include "ArduPilotMegaMAV.h"
#include "ApmUiHelpers.h"
#include "UASActionsWidget.h"
#include "UASActionCatalog.h"
#include "UASActionsLayout.h"
#include "PreFlightCalibrationDialog.h"
#include "comm/LinkManager.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "qml/ApmQmlApi.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <UAS.h>

namespace {
constexpr int kCommandAckTimeoutMs = 10000;
constexpr int kQmlActionNameRole = Qt::UserRole + 1;
}

void UASActionsWidget::setupApmPlaneModes()
{
    QLOG_INFO() << "UASActionWidget: Set for Plane";
    ApmUiHelpers::addPlaneModes(ui.ModeSelector);

    ui.ArmDisarmButton->setVisible(true);

    setupApmActionList();
}

void UASActionsWidget::setupApmCopterModes()
{
    QLOG_INFO() << "UASActionWidget: set for Copter";
    ApmUiHelpers::addCopterModes(ui.ModeSelector);

    ui.ArmDisarmButton->setVisible(true);

    setupApmActionList();
}

void UASActionsWidget::setupApmRoverModes()
{
    QLOG_INFO() << "UASActionWidget: Set for Rover";
    ApmUiHelpers::addRoverModes(ui.ModeSelector);

    ui.ArmDisarmButton->setVisible(true);

    setupApmActionList();
}

UASActionsWidget::UASActionsWidget(QWidget *parent) : QWidget(parent)
{
    QLOG_INFO() << "UASActionsWidget creating " << this;
    ui.setupUi(this);

    UASActionsLayout::applyMissionPlannerProportions(ui);

    m_qmlApi = ApmQmlApi::instance();
    if (m_qmlApi) {
        connect(m_qmlApi, &ApmQmlApi::vehicleActionsChanged,
                this, &UASActionsWidget::syncQmlActions);
    }

    m_commandAckTimer = new QTimer(this);
    m_commandAckTimer->setSingleShot(true);
    connect(m_commandAckTimer, &QTimer::timeout,
            this, &UASActionsWidget::commandAckTimedOut);

    LinkManager *const linkManager = LinkManager::instance();
    m_commandService = linkManager->vehicleCommandService();
    m_targetManager = linkManager->vehicleTargetManager();
    connect(m_commandService, &VehicleCommandService::commandAckReceived,
            this, &UASActionsWidget::exactCommandAckReceived);
    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationChanged,
            this, &UASActionsWidget::targetGenerationChanged);

    connect(ui.ChangeAltitudeButton, &QPushButton::clicked,
            this, &UASActionsWidget::changeAltitudeClicked);
    connect(ui.ChangeSpeedButton, &QPushButton::clicked,
            this, &UASActionsWidget::changeSpeedClicked);
    connect(ui.SetWpButton, &QPushButton::clicked,
            this, &UASActionsWidget::goToWaypointClicked);
    connect(ui.ArmDisarmButton, &QPushButton::clicked,
            this, &UASActionsWidget::armButtonClicked);
    connect(ui.DoActionButton, &QPushButton::clicked,
            this, &UASActionsWidget::setAction);
    connect(ui.SetModeButton, &QPushButton::clicked,
            this, &UASActionsWidget::setMode);
    connect(ui.QuickAutoButton, &QPushButton::clicked,
            this, &UASActionsWidget::setShortcutMode);
    connect(ui.QuickRtlButton, &QPushButton::clicked,
            this, &UASActionsWidget::setRTLMode);
    connect(ui.QuickLoiterButton, &QPushButton::clicked,
            this, &UASActionsWidget::setShortcutMode);
    connect(ui.RestartMissionButton, &QPushButton::clicked,
            this, &UASActionsWidget::restartMissionClicked);
    connect(ui.AbortLandingButton, &QPushButton::clicked,
            this, &UASActionsWidget::abortLandingClicked);
    connect(ui.ClearTrackButton, &QPushButton::clicked,
            this, &UASActionsWidget::clearTrackRequested);
    connect(ui.RawSensorViewButton, &QPushButton::clicked,
            this, &UASActionsWidget::rawSensorViewRequested);
    connect(ui.JoystickSetupButton, &QPushButton::clicked,
            this, &UASActionsWidget::joystickSetupRequested);

    connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)),
            this, SLOT(activeUASSet(UASInterface*)));

    setControlsConnected(false);

    if (UASManager::instance()->getActiveUAS())
    {
        activeUASSet(UASManager::instance()->getActiveUAS());
    }
}

void UASActionsWidget::activeUASSet(UASInterface *uas)
{
    QLOG_INFO() << "UASActionWidget::activeUASSet";
    if (m_uas) {
        // disconnect previous connections
        disconnect(m_uas->getWaypointManager(),SIGNAL(waypointEditableListChanged()),
                this,SLOT(updateWaypointList()));
        disconnect(m_uas->getWaypointManager(),SIGNAL(currentWaypointChanged(quint16)),
                this,SLOT(currentWaypointChanged(quint16)));
        disconnect(m_uas,SIGNAL(armingChanged(bool)),this,SLOT(armingChanged(bool)));

        disconnect(m_uas, SIGNAL(connected()), this, SLOT(uasConnected()));
        disconnect(m_uas, SIGNAL(disconnected()), this, SLOT(uasDisconnected()));

        disconnect(m_uas,SIGNAL(parameterChanged(int,int,int,int,QString,QVariant)),
                this,SLOT(parameterChanged(int,int,int,int,QString,QVariant)));
        disconnect(m_uas, &UASInterface::commandAckReceived,
                   this, &UASActionsWidget::commandAckReceived);
    }

    clearPendingCommand();
    m_uas = qobject_cast<UAS *>(uas);
    if (!m_uas) {
        QLOG_WARN() << "UASActionsWidget: no compatible active UAS";
        setControlsConnected(false);
        return;
    }

    ui.ModeSelector->clear();
    ui.ActionSelector->clear();

    connect(m_uas->getWaypointManager(),SIGNAL(waypointEditableListChanged()),
            this,SLOT(updateWaypointList()));
    connect(m_uas->getWaypointManager(),SIGNAL(currentWaypointChanged(quint16)),
            this,SLOT(currentWaypointChanged(quint16)));
    connect(m_uas,SIGNAL(armingChanged(bool)),this,SLOT(armingChanged(bool)));

    connect(m_uas, SIGNAL(connected()), this, SLOT(uasConnected()));
    connect(m_uas, SIGNAL(disconnected()), this, SLOT(uasDisconnected()));

    connect(m_uas,SIGNAL(parameterChanged(int,int,int,int,QString,QVariant)),
            this,SLOT(parameterChanged(int,int,int,int,QString,QVariant)));
    connect(m_uas, &UASInterface::commandAckReceived,
            this, &UASActionsWidget::commandAckReceived);

    setControlsConnected(m_uas->isConnected());

    armingChanged(m_uas->isArmed());
    updateWaypointList();

    switch (uas->getAutopilotType()){
        case MAV_AUTOPILOT_ARDUPILOTMEGA: {
            if (uas->isFixedWing()){
                setupApmPlaneModes();

            } else if (uas->isMultirotor()){
                setupApmCopterModes();

            } else if (uas->isGroundRover()){
                setupApmRoverModes();

            } else {
                QLOG_WARN() << "UASActionWidget: Unsupported System Type" << uas->getSystemType();
            }
        } break;
        case MAV_AUTOPILOT_PX4:
        {
            // [TODO] PX4 flight controller go here
        }
        case MAV_AUTOPILOT_GENERIC:
        default:
        {
            // [TODO] Generic, and other flight controllers
        }
    }
}

void UASActionsWidget::uasConnected()
{
    QLOG_INFO() << "UASActionsWidget::connected()" << m_uas;
    setControlsConnected(m_uas && m_uas->isConnected());
}

void UASActionsWidget::uasDisconnected()
{
    QLOG_INFO() << "UASActionsWidget::disconnected()" << m_uas;
    const bool connected = m_uas && m_uas->isConnected();
    if (!connected) {
        clearPendingCommand();
    }
    setControlsConnected(connected);
}

void UASActionsWidget::armButtonClicked()
{
    QLOG_INFO() << "UASActionsWidget::armButtonClicked";

    if(!activeUas())
        return;

    if (m_uas->isArmed())
    {
        const auto answer = QMessageBox::warning(
            this, tr("Disarm"),
            tr("Disarm the vehicle now? In-flight disarming can cause a crash."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            setActionStatus(tr("Disarm cancelled"));
            return;
        }
        QLOG_INFO() << "UAS:: Attempt to Disarm System";
        setActionStatus(tr("Disarm requested; awaiting vehicle state"));
        m_uas->disarmSystem();

    }
    else
    {
        QLOG_INFO() << "UAS:: Attempt to Arm System";
        setActionStatus(tr("Arm requested; awaiting vehicle state"));
        m_uas->armSystem();
    }
}

void UASActionsWidget::armingChanged(bool state)
{
    QLOG_INFO() << "Arming State Changed: " << (state?"ARMED":"DISARM");

    //TODO:
    //Figure out why arm/disarm is in UAS.h and not part of the interface, and fix.
    // Mission Planner keeps the command caption stable and communicates the
    // result separately. This also prevents a stale optimistic label from
    // being mistaken for an acknowledgement.
    ui.ArmDisarmButton->setText(tr("Arm / Disarm"));
    ui.ArmDisarmButton->setToolTip(
        state ? tr("Vehicle reports ARMED") : tr("Vehicle reports DISARMED"));
    if (m_pendingCommand < 0) {
        setActionStatus(state ? tr("Vehicle confirmed: ARMED")
                              : tr("Vehicle confirmed: DISARMED"));
    }

}

void UASActionsWidget::currentWaypointChanged(quint16 wpid)
{
    if (m_last_wpid != wpid) {
        m_last_wpid = wpid;
        QLOG_INFO() << "Waypoint Changed to: " << wpid;
        ui.WaypointSelector->setCurrentIndex(
            ui.WaypointSelector->findText(QString::number(wpid)));
        if (m_pendingCommand < 0) {
            setActionStatus(
                tr("Vehicle confirmed current waypoint %1").arg(wpid));
        }
    }
}

void UASActionsWidget::updateWaypointList()
{
    if(!activeUas())
        return;
    //QLOG_INFO() << "updateWaypointList: ";

    ui.WaypointSelector->clear();
    for (int i=0;i<m_uas->getWaypointManager()->getWaypointEditableList().size();i++)
    {
        //QLOG_INFO() << "  WP:" << i;
        ui.WaypointSelector->addItem(QString::number(i));
    }
}

UASActionsWidget::~UASActionsWidget()
{
}

void UASActionsWidget::goToWaypointClicked()
{
    if(!activeUas())
        return;
    const int waypoint = ui.WaypointSelector->currentIndex();
    if (waypoint < 0) {
        setActionStatus(tr("Set WP: select a mission waypoint first"), true);
        return;
    }
    QLOG_INFO() << "Go to Waypoint" << waypoint;
    setActionStatus(tr("Set WP %1 requested; awaiting vehicle state").arg(waypoint));
    m_uas->getWaypointManager()->setCurrentWaypoint(waypoint);
}

void UASActionsWidget::changeAltitudeClicked()
{
    if (!activeUas())
        return;
    QLOG_WARN() << "changeAltitudeClicked";

    constexpr MAV_FRAME frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    QLOG_DEBUG() << "Start guided action requested. Lat:" << m_uas->getLatitude()
                 << "Lon:" << m_uas->getLongitude()
                 << "Alt:" << ui.ChangeAltitudeInput->value()
                 << "MAV_FRAME: AGL";
    Waypoint wp;
    wp.setFrame(frame);
    wp.setLatitude(m_uas->getLatitude());
    wp.setLongitude(m_uas->getLongitude());
    wp.setAltitude(ui.ChangeAltitudeInput->value());
    setActionStatus(tr("Change Alt requested; awaiting vehicle state"));
    m_uas->getWaypointManager()->goToWaypoint(&wp);
}

void UASActionsWidget::changeSpeedClicked()
{
    if(!activeUas())
        return;

    QLOG_INFO() << "Change Vehicle Speed ";

	int confirm = 1;    // Confirm.
	float param1 = 0.0; // Empty
	float param2 = ui.ChangeSpeedInput->value();
	float param3 = 0.0; //
	float param4 = 0.0; //
	float param5 = 0.0; // Latitude
	float param6 = 0.0; // Longitude
	float param7 = 0.0; // Altitude
	int component = MAV_COMP_ID_PRIMARY;
	if (!beginCommand(MAV_CMD_DO_CHANGE_SPEED, tr("Change Speed")))
		return;
	dispatchCommandLong(MAV_CMD_DO_CHANGE_SPEED,
						  confirm, param1, param2, param3,
						  param4, param5, param6, param7, component);

}

void UASActionsWidget::setMode()
{
    QLOG_INFO() << "    UASActionsWidget::setMode()";

    if(!activeUas())
        return;

    if (ui.ModeSelector->currentIndex() < 0) {
        setActionStatus(tr("Set Mode: no mode is selected"), true);
        return;
    }

    QLOG_INFO() << "Set Mode to "
                << ui.ModeSelector->itemData(ui.ModeSelector->currentIndex()).toInt();

    m_uas->setMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
                   ui.ModeSelector->itemData(ui.ModeSelector->currentIndex()).toInt());
    setActionStatus(tr("Mode %1 requested; verify the vehicle state")
                        .arg(ui.ModeSelector->currentText()));
}

void UASActionsWidget::setShortcutMode()
{
    QLOG_INFO() << "    UASActionsWidget::setShortcutMode()";

    if(!activeUas())
        return;

    const auto *button = qobject_cast<QPushButton *>(sender());
    if (!button) {
        setActionStatus(tr("Mode shortcut source is invalid"), true);
        return;
    }
    requestQuickMode(button->text());
}


void UASActionsWidget::setAction()
{
    QLOG_INFO() << "UASActionsWidget::setAction()";

    const int currentIndex = ui.ActionSelector->currentIndex();
    if (currentIndex < 0) {
        setActionStatus(tr("Do Action: select an action first"), true);
        return;
    }

    const QString qmlActionName =
        ui.ActionSelector->itemData(currentIndex, kQmlActionNameRole)
            .toString();
    if (!qmlActionName.isEmpty()) {
        QString error;
        if (!m_qmlApi
            || !m_qmlApi->invokeVehicleAction(qmlActionName, &error)) {
            setActionStatus(
                tr("%1 failed: %2").arg(
                    qmlActionName,
                    error.isEmpty() ? tr("callback is unavailable") : error),
                true);
        } else {
            setActionStatus(tr("%1 callback completed").arg(qmlActionName));
        }
        return;
    }

    if (!activeUas())
        return;

    QLOG_INFO() << "Set Action to " << currentIndex;
    MAV_CMD currentCommand = static_cast<MAV_CMD>(ui.ActionSelector->itemData(currentIndex).toInt());

    if (currentCommand == MAV_CMD_STORAGE_FORMAT) {
        sendFormatSdCard();
        return;
    }

    switch (m_uas->getAutopilotType()) {
        case MAV_AUTOPILOT_ARDUPILOTMEGA: {
            if (currentCommand != MAV_CMD_PREFLIGHT_CALIBRATION
                    && currentCommand != MAV_CMD_MISSION_START
                    && currentCommand != MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) {
                if (!beginCommand(currentCommand,
                                  ui.ActionSelector->currentText())) {
                    return;
                }
            }
            if (m_uas->isFixedWing()){
                sendApmPlaneCommand(currentCommand);

            } else if (m_uas->isMultirotor()){
                sendApmCopterCommand(currentCommand);

            } else if (m_uas->isGroundRover()){
                sendApmRoverCommand(currentCommand);

            } else {
                QLOG_WARN() << "UASActionWidget: Unsupported System Type" << m_uas->getSystemType();
                clearPendingCommand();
                setActionStatus(tr("This action is not supported for the vehicle type"),
                                true);
            }

            } break;

        case MAV_AUTOPILOT_PX4: {
            setActionStatus(tr("Do Action is not implemented for PX4"), true);
        } break;

        case MAV_AUTOPILOT_GENERIC:
        default: {
            setActionStatus(tr("Do Action is not implemented for this autopilot"),
                            true);
        } break;
    }
}

void UASActionsWidget::setupApmActionList()
{
    ui.ActionSelector->clear();
    for (const UASActionCatalog::Entry &entry
         : UASActionCatalog::StandardActions) {
        ui.ActionSelector->addItem(
            QCoreApplication::translate("UASActionsWidget", entry.name),
            static_cast<int>(entry.command));
    }
    syncQmlActions();
}

void UASActionsWidget::syncQmlActions()
{
    const QString selected = ui.ActionSelector->currentText();
    for (int index = ui.ActionSelector->count() - 1; index >= 0; --index) {
        if (ui.ActionSelector->itemData(index, kQmlActionNameRole).isValid()) {
            ui.ActionSelector->removeItem(index);
        }
    }
    if (!m_qmlApi) {
        return;
    }

    for (const QString &name : m_qmlApi->vehicleActions()) {
        // Built-in action captions are reserved so the selector is never
        // ambiguous even though plugins are otherwise fully trusted.
        if (ui.ActionSelector->findText(name) >= 0) {
            continue;
        }

        int insertionIndex = ui.ActionSelector->count();
        const QString after = m_qmlApi->vehicleActionAfter(name);
        const int afterIndex = ui.ActionSelector->findText(after);
        if (!after.isEmpty() && afterIndex >= 0) {
            insertionIndex = afterIndex + 1;
        }
        const QString before = m_qmlApi->vehicleActionBefore(name);
        const int beforeIndex = ui.ActionSelector->findText(before);
        if (!before.isEmpty() && beforeIndex >= 0) {
            insertionIndex = beforeIndex;
        }

        ui.ActionSelector->insertItem(insertionIndex, name);
        ui.ActionSelector->setItemData(insertionIndex, name,
                                       kQmlActionNameRole);
    }

    const int selectedIndex = ui.ActionSelector->findText(selected);
    if (selectedIndex >= 0) {
        ui.ActionSelector->setCurrentIndex(selectedIndex);
    }
}

void UASActionsWidget::sendFormatSdCard()
{
    const auto answer = QMessageBox::warning(
        this, tr("Format SD Card"),
        tr("Format the vehicle SD card now? All logs and other data on "
           "the first storage device will be permanently erased."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        setActionStatus(tr("Format_SD_Card cancelled"));
        return;
    }

    const UASActionCatalog::CommandRequest request =
        UASActionCatalog::formatSdCardRequest();
    if (!beginCommand(request.command, tr("Format_SD_Card"))) {
        return;
    }
    dispatchCommandLong(
        request.command, request.confirmation,
        request.parameters[0], request.parameters[1], request.parameters[2],
        request.parameters[3], request.parameters[4], request.parameters[5],
        request.parameters[6], request.component);
}


int UASActionsWidget::preFlightWarningBox(QWidget* parent)
{
    QLOG_INFO() << "Display Pre-Flight Warning Box";
    return QMessageBox::critical(parent,tr("Warning"),tr("This action must be done when on the ground. If vehicle is in the air the this action will result in a crash!"),
                         QMessageBox::Ok | QMessageBox::Abort,
                         QMessageBox::Abort);
}

int UASActionsWidget::modeChangeWarningBox(const QString& modeString)
{
    QLOG_INFO() << "UASActionsWidget:Display Mode Change Warning Box?";

    QStringList warnList;
    warnList << "Auto";

    if (!warnList.contains(modeString)){
        return QMessageBox::Ok; // Only warn for modes in the list
    }

    return QMessageBox::critical(this,tr("Warning"),tr("Please confirm you want to enter\n %1 mode").arg(modeString),
                         QMessageBox::Ok | QMessageBox::Abort,
                         QMessageBox::Abort);
}

void UASActionsWidget::sendApmPlaneCommand(MAV_CMD command)
{
    switch(command) {

    case MAV_CMD_NAV_LOITER_UNLIM: {
        // Loiter around this MISSION an unlimited amount of time
        Q_ASSERT(command == MAV_CMD_NAV_LOITER_UNLIM);
        QLOG_INFO() << "MAV_CMD_NAV_LOITER_UNLIM";

        int confirm = 1;    // [TODO] Verify This is what ArduPlane Does.
        float param1 = 0.0; // Empty
        float param2 = 0.0; // Empty
        float param3 = 0.0; // [NOT USED] Radius around MISSION, in meters. If positive loiter clockwise, else counter-clockwise
        float param4 = 0.0; // Desired yaw angle.|
        float param5 = 0.0; // Latitude
        float param6 = 0.0; // Longitude
        float param7 = 0.0; // Altitude
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);
    } break;

    case MAV_CMD_NAV_RETURN_TO_LAUNCH: {
        /* Return to launch location |Empty| Empty| Empty| Empty| Empty| Empty| Empty|  */
        Q_ASSERT(command == MAV_CMD_NAV_RETURN_TO_LAUNCH);
        QLOG_INFO() << "MAV_CMD_NAV_RETURN_TO_LAUNCH";

        int confirm = 1;    // [TODO] Verify This is what ArduPlane Does.
        float param1 = 0.0; // Empty
        float param2 = 0.0; // Empty
        float param3 = 0.0; // [NOT USED] Radius around MISSION, in meters. If positive loiter clockwise, else counter-clockwise
        float param4 = 0.0; // Desired yaw angle.|
        float param5 = 0.0; // Latitude
        float param6 = 0.0; // Longitude
        float param7 = 0.0; // Altitude
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_CALIBRATION: {
        // Trigger calibration. This command will be only accepted if in pre-flight mode.
        showPreflightCalibrationDialog();
    } break;

    case MAV_CMD_MISSION_START: {
        // start running a mission last_item:
        Q_ASSERT(command == MAV_CMD_MISSION_START);
        QLOG_INFO() << "MAV_CMD_MISSION_START";

        if (modeChangeWarningBox("Mission Start") == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // first_item: the first mission item to run
        float param2 = 0.0; // the last mission item to run (after this item is run, the mission ends)|
        float param3 = 0.0; // | Empty|
        float param4 = 0.0; // | Empty|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Mission Start")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN: {
        // Request the reboot or shutdown of system components.
        Q_ASSERT(command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
        QLOG_INFO() << "MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN";

        if (preFlightWarningBox(this) == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // | 0: Do nothing for autopilot 1: Reboot autopilot, 2: Shutdown autopilot.
        float param2 = 1.0; // | 0: Do nothing for onboard computer, 1: Reboot onboard computer, 2: Shutdown onboard computer.
        float param3 = 0.0; // | Reserved|
        float param4 = 0.0; // | Reserved|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Preflight Reboot")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_DO_DIGICAM_CONTROL: {
        // Take a photo
        Q_ASSERT(command == MAV_CMD_DO_DIGICAM_CONTROL);
        QLOG_INFO() << "MAV_CMD_DO_DIGICAM_CONTROL";

        int confirm = 1;
        float param1 = 0.0; // | Session control e.g. show/hide lens
        float param2 = 0.0; // | Zoom's absolute position
        float param3 = 0.0; // | Zooming step value to offset zoom from the current position
        float param4 = 0.0; // | Focus Locking, Unlocking or Re-locking
        float param5 = 1.0; // | Shooting Command
        float param6 = 0.0; // | Command Identity
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    default:
        QLOG_INFO() << "sendApmPlaneCommand: Unknown Command " << command;
        clearPendingCommand();
        setActionStatus(tr("The selected action is not implemented for Plane"),
                        true);
    }
}

void UASActionsWidget::sendApmCopterCommand(MAV_CMD command)
{
    switch(command) {

    case MAV_CMD_NAV_LOITER_UNLIM: {
        // Loiter around this MISSION an unlimited amount of time
        Q_ASSERT(command == MAV_CMD_NAV_LOITER_UNLIM);
        QLOG_INFO() << "MAV_CMD_NAV_LOITER_UNLIM";

        int confirm = 1;    // [TODO] Verify This is what ArduCopter Does.
        float param1 = 0.0; // Empty
        float param2 = 0.0; // Empty
        float param3 = 0.0; // [NOT USED] Radius around MISSION, in meters. If positive loiter clockwise, else counter-clockwise
        float param4 = 0.0; // Desired yaw angle.|
        float param5 = 0.0; // Latitude
        float param6 = 0.0; // Longitude
        float param7 = 0.0; // Altitude
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);
    } break;

    case MAV_CMD_NAV_RETURN_TO_LAUNCH: {
        /* Return to launch location |Empty| Empty| Empty| Empty| Empty| Empty| Empty|  */
        Q_ASSERT(command == MAV_CMD_NAV_RETURN_TO_LAUNCH);
        QLOG_INFO() << "MAV_CMD_NAV_RETURN_TO_LAUNCH";

        int confirm = 1;    // [TODO] Verify This is what ArduCopter Does.
        float param1 = 0.0; // Empty
        float param2 = 0.0; // Empty
        float param3 = 0.0; // [NOT USED] Radius around MISSION, in meters. If positive loiter clockwise, else counter-clockwise
        float param4 = 0.0; // Desired yaw angle.|
        float param5 = 0.0; // Latitude
        float param6 = 0.0; // Longitude
        float param7 = 0.0; // Altitude
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_CALIBRATION: {
        // Trigger calibration. This command will be only accepted if in pre-flight mode.
        showPreflightCalibrationDialog();
    } break;

    case MAV_CMD_MISSION_START: {
        // start running a mission last_item:
        Q_ASSERT(command == MAV_CMD_MISSION_START);
        QLOG_INFO() << "MAV_CMD_MISSION_START";

        if (modeChangeWarningBox("Mission Start") == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // first_item: the first mission item to run
        float param2 = 0.0; // the last mission item to run (after this item is run, the mission ends)|
        float param3 = 0.0; // | Empty|
        float param4 = 0.0; // | Empty|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Mission Start")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN: {
        // Request the reboot or shutdown of system components.
        Q_ASSERT(command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
        QLOG_INFO() << "MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN";

        if (preFlightWarningBox(this) == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // | 0: Do nothing for autopilot 1: Reboot autopilot, 2: Shutdown autopilot.
        float param2 = 1.0; // | 0: Do nothing for onboard computer, 1: Reboot onboard computer, 2: Shutdown onboard computer.
        float param3 = 0.0; // | Reserved|
        float param4 = 0.0; // | Reserved|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Preflight Reboot")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_DO_DIGICAM_CONTROL: {
        // Take a photo
        Q_ASSERT(command == MAV_CMD_DO_DIGICAM_CONTROL);
        QLOG_INFO() << "MAV_CMD_DO_DIGICAM_CONTROL";

        int confirm = 1;
        float param1 = 0.0; // | Session control e.g. show/hide lens
        float param2 = 0.0; // | Zoom's absolute position
        float param3 = 0.0; // | Zooming step value to offset zoom from the current position
        float param4 = 0.0; // | Focus Locking, Unlocking or Re-locking
        float param5 = 1.0; // | Shooting Command
        float param6 = 0.0; // | Command Identity
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    default:
        QLOG_INFO() << "sendApmCopterCommand: Unknown Command " << command;
        clearPendingCommand();
        setActionStatus(tr("The selected action is not implemented for Copter"),
                        true);
    }
}

void UASActionsWidget::sendApmRoverCommand(MAV_CMD command)
{
    QLOG_INFO() << "UASActionWidget::sendApmRoverCommand";
    switch(command) {

    case MAV_CMD_NAV_RETURN_TO_LAUNCH: {
        /* Return to launch location |Empty| Empty| Empty| Empty| Empty| Empty| Empty|  */
        Q_ASSERT(command == MAV_CMD_NAV_RETURN_TO_LAUNCH);
        QLOG_INFO() << "MAV_CMD_NAV_RETURN_TO_LAUNCH";

        int confirm = 1;    // [TODO] Verify This is what ArduRover Does.
        float param1 = 0.0; // Empty
        float param2 = 0.0; // Empty
        float param3 = 0.0; // [NOT USED] Radius around MISSION, in meters. If positive loiter clockwise, else counter-clockwise
        float param4 = 0.0; // Desired yaw angle.|
        float param5 = 0.0; // Latitude
        float param6 = 0.0; // Longitude
        float param7 = 0.0; // Altitude
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_CALIBRATION: {
        // Trigger calibration. This command will be only accepted if in pre-flight mode.
        showPreflightCalibrationDialog();
    } break;

    case MAV_CMD_MISSION_START: {
        // start running a mission last_item:
        Q_ASSERT(command == MAV_CMD_MISSION_START);
        QLOG_INFO() << "MAV_CMD_MISSION_START";

        if (modeChangeWarningBox("Mission Start") == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // first_item: the first mission item to run
        float param2 = 0.0; // the last mission item to run (after this item is run, the mission ends)|
        float param3 = 0.0; // | Empty|
        float param4 = 0.0; // | Empty|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Mission Start")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN: {
        // Request the reboot or shutdown of system components.
        Q_ASSERT(command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
        QLOG_INFO() << "MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN";

        if (preFlightWarningBox(this) == QMessageBox::Abort)
            return;

        int confirm = 1;
        float param1 = 1.0; // | 0: Do nothing for autopilot 1: Reboot autopilot, 2: Shutdown autopilot.
        float param2 = 1.0; // | 0: Do nothing for onboard computer, 1: Reboot onboard computer, 2: Shutdown onboard computer.
        float param3 = 0.0; // | Reserved|
        float param4 = 0.0; // | Reserved|
        float param5 = 0.0; // | Empty|
        float param6 = 0.0; // | Empty|
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        if (!beginCommand(command, tr("Preflight Reboot")))
            return;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    case MAV_CMD_DO_DIGICAM_CONTROL: {
        // Take a photo
        Q_ASSERT(command == MAV_CMD_DO_DIGICAM_CONTROL);
        QLOG_INFO() << "MAV_CMD_DO_DIGICAM_CONTROL";

        int confirm = 1;
        float param1 = 0.0; // | Session control e.g. show/hide lens
        float param2 = 0.0; // | Zoom's absolute position
        float param3 = 0.0; // | Zooming step value to offset zoom from the current position
        float param4 = 0.0; // | Focus Locking, Unlocking or Re-locking
        float param5 = 1.0; // | Shooting Command
        float param6 = 0.0; // | Command Identity
        float param7 = 0.0; // | Empty|
        int component = MAV_COMP_ID_PRIMARY;
        dispatchCommandLong(command,
                              confirm, param1, param2, param3,
                              param4, param5, param6, param7, component);

    } break;

    default:
        QLOG_INFO() << "sendApmRoverCommand: Unknown Command " << command;
        clearPendingCommand();
        setActionStatus(tr("The selected action is not implemented for Rover"),
                        true);
    }
}

void UASActionsWidget::restartMissionClicked()
{
    if (!activeUas()) {
        return;
    }
    if (modeChangeWarningBox(QStringLiteral("Mission Start"))
            == QMessageBox::Abort) {
        return;
    }

    if (!beginCommand(MAV_CMD_MISSION_START, tr("Restart Mission"))) {
        return;
    }
    // Match Mission Planner's restart sequence: reset the current mission
    // item before issuing MISSION_START. Begin the serialized command first so
    // another pending action can never reset the waypoint as a side effect.
    m_uas->getWaypointManager()->setCurrentWaypoint(0);
    dispatchCommandLong(MAV_CMD_MISSION_START, 1,
                          0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, 0.0f, MAV_COMP_ID_PRIMARY);
}

void UASActionsWidget::abortLandingClicked()
{
    if (!activeUas()) {
        return;
    }
    if (!beginCommand(MAV_CMD_DO_GO_AROUND, tr("Abort Landing"))) {
        return;
    }
    dispatchCommandLong(MAV_CMD_DO_GO_AROUND, 1,
                          0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, 0.0f, MAV_COMP_ID_PRIMARY);
}

void UASActionsWidget::setRTLMode()
{
    requestQuickMode(QStringLiteral("RTL"));
}

void UASActionsWidget::requestQuickMode(const QString &mode)
{
    if (!activeUas()) {
        setActionStatus(tr("Mode %1 was not requested: vehicle is disconnected")
                            .arg(mode), true);
        return;
    }

    const QString requested = mode.trimmed();
    if (requested.isEmpty()) {
        setActionStatus(tr("Mode shortcut is empty"), true);
        return;
    }
    const int index = ui.ModeSelector->findText(requested,
                                                Qt::MatchFixedString);
    if (index < 0) {
        setActionStatus(tr("Mode %1 is not available for this vehicle")
                            .arg(requested), true);
        return;
    }
    if (modeChangeWarningBox(requested) == QMessageBox::Abort) {
        setActionStatus(tr("Mode %1 cancelled").arg(requested));
        return;
    }

    ui.ModeSelector->setCurrentIndex(index);
    m_uas->setMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
                   ui.ModeSelector->itemData(index).toInt());
    setActionStatus(tr("Mode %1 requested; verify the vehicle state")
                        .arg(requested));
}

bool UASActionsWidget::commandSurfaceAvailable() const
{
    return m_connected && !m_uas.isNull() && m_pendingCommand < 0;
}

bool UASActionsWidget::vehicleConnected() const
{
    return m_connected && !m_uas.isNull();
}

QString UASActionsWidget::actionStatus() const
{
    return ui.ActionStatusLabel->text();
}

void UASActionsWidget::setControlsConnected(bool connected)
{
    m_connected = connected && m_uas;
    ui.actionsGroupBox->setEnabled(commandSurfaceAvailable());
    emit commandSurfaceAvailableChanged(commandSurfaceAvailable());
    if (!m_connected) {
        setActionStatus(m_uas ? tr("Vehicle disconnected")
                              : tr("No active vehicle"), true);
    } else if (m_pendingCommand < 0) {
        setActionStatus(tr("Vehicle connected; no action pending"));
    }
}

void UASActionsWidget::setActionStatus(const QString &message, bool error)
{
    ui.ActionStatusLabel->setText(message);
    ui.ActionStatusLabel->setProperty("error", error);
    ui.ActionStatusLabel->style()->unpolish(ui.ActionStatusLabel);
    ui.ActionStatusLabel->style()->polish(ui.ActionStatusLabel);
    emit actionStatusChanged(message, error);
}

bool UASActionsWidget::beginCommand(MAV_CMD command, const QString &action)
{
    if (!m_connected || !m_uas) {
        setActionStatus(tr("%1 was not sent: vehicle is disconnected").arg(action),
                        true);
        return false;
    }
    if (m_pendingCommand >= 0) {
        setActionStatus(tr("%1 is still waiting for a vehicle response")
                            .arg(m_pendingAction), true);
        return false;
    }

    m_pendingTarget = {};
    if (m_commandService && m_targetManager) {
        const VehicleTargetLease candidate =
            m_targetManager->acquireTarget();
        if (candidate.isValid()
            && candidate.endpoint.systemId == m_uas->getUASID()) {
            m_pendingTarget = candidate;
        }
    }
    m_pendingCommand = static_cast<int>(command);
    m_pendingAction = action;
    ui.actionsGroupBox->setEnabled(false);
    emit commandSurfaceAvailableChanged(false);
    setActionStatus(tr("%1 sent; waiting for COMMAND_ACK").arg(action));
    m_commandAckTimer->start(kCommandAckTimeoutMs);
    return true;
}

bool UASActionsWidget::dispatchCommandLong(
    MAV_CMD command, int confirmation,
    float param1, float param2, float param3, float param4,
    float param5, float param6, float param7,
    int legacyComponent)
{
    if (!m_uas) {
        return false;
    }

    VehicleTargetLease target = m_pendingTarget;
    if (!target.isValid() && m_commandService && m_targetManager) {
        const VehicleTargetLease candidate =
            m_targetManager->acquireTarget();
        if (candidate.isValid()
            && candidate.endpoint.systemId == m_uas->getUASID()) {
            target = candidate;
        }
    }

    if (target.isValid() && m_commandService) {
        const VehicleCommandService::SendResult result =
            m_commandService->sendCommandLong(
                target,
                m_uas->gcsSystemId(),
                m_uas->gcsComponentId(),
                command, static_cast<quint8>(confirmation),
                param1, param2, param3, param4,
                param5, param6, param7);
        if (result == VehicleCommandService::SendResult::Sent) {
            return true;
        }

        const QString action = m_pendingAction.isEmpty()
            ? tr("Command %1").arg(static_cast<int>(command))
            : m_pendingAction;
        clearPendingCommand();
        setActionStatus(
            result == VehicleCommandService::SendResult::TransportUnavailable
                ? tr("%1 was not sent: selected link is unavailable")
                      .arg(action)
                : tr("%1 was not sent: selected target changed")
                      .arg(action),
            true);
        return false;
    }

    // Until every surface owns an exact endpoint, a UAS with no applicable
    // selection retains its established behavior.  An applicable but failed
    // exact target returned above and can never fall through to fan-out.
    m_uas->executeCommand(
        command, confirmation,
        param1, param2, param3, param4,
        param5, param6, param7, legacyComponent);
    return true;
}

void UASActionsWidget::clearPendingCommand()
{
    if (m_commandAckTimer) {
        m_commandAckTimer->stop();
    }
    m_pendingCommand = -1;
    m_pendingAction.clear();
    m_pendingTarget = {};
    ui.actionsGroupBox->setEnabled(commandSurfaceAvailable());
    emit commandSurfaceAvailableChanged(commandSurfaceAvailable());
}

QString UASActionsWidget::commandAckResultText(int result)
{
    switch (result) {
    case MAV_RESULT_ACCEPTED:
        return tr("accepted");
    case MAV_RESULT_TEMPORARILY_REJECTED:
        return tr("temporarily rejected");
    case MAV_RESULT_DENIED:
        return tr("denied");
    case MAV_RESULT_UNSUPPORTED:
        return tr("unsupported");
    case MAV_RESULT_FAILED:
        return tr("failed");
    case MAV_RESULT_IN_PROGRESS:
        return tr("in progress");
    default:
        return tr("unknown result %1").arg(result);
    }
}

void UASActionsWidget::commandAckReceived(
    int uasId, int componentId, int command, int result, int progress,
    int resultParam2, int targetSystem, int targetComponent)
{
    Q_UNUSED(resultParam2)

    if (m_pendingTarget.isValid()) {
        return;
    }

    if (!m_uas || uasId != m_uas->getUASID()) {
        return;
    }
    // The command was addressed to the primary autopilot component. MAVLink 1
    // ACKs have zero target fields; MAVLink 2 ACKs must either omit them or
    // address this GCS explicitly.
    UASInterface *const activeVehicle = m_uas.data();
    if (componentId != MAV_COMP_ID_PRIMARY
            || (targetSystem != 0
                && targetSystem != activeVehicle->getSystemId())
            || (targetComponent != 0
                && targetComponent != m_uas->gcsComponentId())) {
        return;
    }
    if (m_pendingCommand < 0) {
        // Calibration is dispatched by its modal child dialog, so it has no
        // opportunity to arm the pending state before sending.
        if (command == MAV_CMD_PREFLIGHT_CALIBRATION) {
            setActionStatus(tr("Preflight Calibration: %1")
                                .arg(commandAckResultText(result)),
                            result != MAV_RESULT_ACCEPTED
                                && result != MAV_RESULT_IN_PROGRESS);
        }
        return;
    }
    if (command != m_pendingCommand) {
        return;
    }

    processCommandAck(command, result, progress);
}

void UASActionsWidget::exactCommandAckReceived(
    qulonglong targetGeneration,
    int linkId, int systemId, int componentId,
    int command, int result, int progress, int resultParam2,
    int targetSystem, int targetComponent)
{
    Q_UNUSED(resultParam2)
    Q_UNUSED(targetSystem)
    Q_UNUSED(targetComponent)

    if (!m_targetManager || !m_pendingTarget.isValid()
        || targetGeneration != m_pendingTarget.generation
        || linkId != m_pendingTarget.endpoint.linkId
        || systemId != m_pendingTarget.endpoint.systemId
        || componentId != m_pendingTarget.endpoint.componentId
        || command != m_pendingCommand
        || !m_targetManager->isCurrentTarget(
            linkId, systemId, componentId, targetGeneration)) {
        return;
    }

    processCommandAck(command, result, progress);
}

void UASActionsWidget::targetGenerationChanged(qulonglong generation)
{
    if (!m_targetManager
        || generation != m_targetManager->targetGeneration()
        || !m_pendingTarget.isValid()
        || m_targetManager->isCurrentTarget(
            m_pendingTarget.endpoint.linkId,
            m_pendingTarget.endpoint.systemId,
            m_pendingTarget.endpoint.componentId,
            m_pendingTarget.generation)) {
        return;
    }

    const QString action = m_pendingAction;
    clearPendingCommand();
    setActionStatus(
        tr("%1 cancelled: selected target changed or disconnected")
            .arg(action),
        true);
}

void UASActionsWidget::processCommandAck(
    int command, int result, int progress)
{
    if (m_pendingCommand < 0 || command != m_pendingCommand) {
        return;
    }

    const QString action = m_pendingAction;
    if (result == MAV_RESULT_IN_PROGRESS) {
        const QString progressText = progress <= 100
            ? tr(" (%1%)").arg(progress) : QString();
        setActionStatus(tr("%1: in progress%2")
                            .arg(action, progressText));
        m_commandAckTimer->start(kCommandAckTimeoutMs);
        return;
    }

    clearPendingCommand();
    setActionStatus(tr("%1: %2").arg(action, commandAckResultText(result)),
                    result != MAV_RESULT_ACCEPTED);
}

void UASActionsWidget::commandAckTimedOut()
{
    if (m_pendingCommand < 0) {
        return;
    }
    const QString action = m_pendingAction;
    clearPendingCommand();
    setActionStatus(
        tr("%1: no COMMAND_ACK received; vehicle outcome is unknown")
            .arg(action),
        true);
}

bool UASActionsWidget::activeUas()
{
    if (!m_uas || !m_connected) {
        QLOG_ERROR() << "UASActionsWidget: no connected active UAS";
        setActionStatus(tr("No connected active vehicle"), true);
        return false;
    }

    return true;
}

void UASActionsWidget::parameterChanged(int uas, int component, int parameterCount,
                                        int parameterId, QString parameterName, QVariant value)
{
    Q_UNUSED(uas);
    Q_UNUSED(component);
    Q_UNUSED(parameterCount);
    Q_UNUSED(parameterId);

    if((parameterName == "WPNAV_SPEED")|| (parameterName == "TRIM_ARSPD_CN")
        || (parameterName == "CRUISE_SPEED")){
        QLOG_DEBUG() << "UASAction setting speed spin box from " << parameterName;
        ui.ChangeSpeedInput->setValue(value.toDouble()/100.0f);

    }

}

void UASActionsWidget::showPreflightCalibrationDialog()
{
    PreFlightCalibrationDialog *dialog = new PreFlightCalibrationDialog(this);
    if(dialog->exec() == 1){
        QLOG_DEBUG() << "Preflight calibration accepted";
    } else {
        QLOG_DEBUG() << "Preflight calibration rejected";
    }

}
