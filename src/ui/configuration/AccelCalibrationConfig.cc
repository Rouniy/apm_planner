/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2013 APM_PLANNER PROJECT <http://www.diydrones.com>

This file is part of the APM_PLANNER project

    APM_PLANNER is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    APM_PLANNER is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with APM_PLANNER. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

#include "AccelCalibrationConfig.h"
#include "GAudioOutput.h"
#include "MainWindow.h"

#include <QMessageBox>
#include <QPointer>


const char* COUNTDOWN_STRING = "<h3>Calibrate MAV%03d<br>Time remaining until timeout: <b>%d</b><h3>";
const char* CALIBRATE_BUTTON_TEXT = "Full\nAccel Calibration";
const char* CONTINUE_BUTTON_TEXT = "Continue\nPress SpaceBar";
const char* LEVEL_CALIBRATE_BUTTON_TEXT = "Calibrate Level";

namespace {

using VehicleAction = DeveloperVehicleToolService::Action;

bool isOneShotAccelAction(VehicleAction action)
{
    return action == VehicleAction::CalibrateLevel
        || action == VehicleAction::SimpleAccelCalibration;
}

QString oneShotActionName(VehicleAction action)
{
    return action == VehicleAction::CalibrateLevel
        ? QObject::tr("Level calibration")
        : QObject::tr("Simple accelerometer calibration");
}

} // namespace


AccelCalibrationConfig::AccelCalibrationConfig(QWidget *parent) : AP2ConfigWidget(parent),
    m_muted(false),
    m_countdownCount(CALIBRATION_TIMEOUT_SEC)
{
    ui.setupUi(this);
    connect(ui.calibrateAccelButton,SIGNAL(clicked()),this,SLOT(calibrateButtonClicked()));
    connect(ui.calibrateAccelLevelButton, SIGNAL(clicked()),
            this, SLOT(calibrateLevelButtonClicked()));
    connect(ui.calibrateAccelSimpleButton,SIGNAL(clicked()),this,SLOT(calibrateSimpleButtonClicked()));

    initConnections();
    //coutdownLabel
    connect(&m_countdownTimer,SIGNAL(timeout()),this,SLOT(countdownTimerTick()));
    refreshCalibrationControls();
}

AccelCalibrationConfig::~AccelCalibrationConfig()
{
    ++m_oneShotFlowRevision;
    dismissOneShotConsent();
    if (m_vehicleToolService) {
        disconnect(m_vehicleToolService, nullptr, this, nullptr);
    }
}

void AccelCalibrationConfig::setVehicleToolService(
    DeveloperVehicleToolService *service)
{
    if (m_vehicleToolService == service) {
        refreshCalibrationControls();
        return;
    }

    const QPointer<AccelCalibrationConfig> guard(this);
    const QPointer<DeveloperVehicleToolService> incoming(service);
    const quint64 serviceRevision = ++m_vehicleToolServiceRevision;
    ++m_oneShotFlowRevision;
    dismissOneShotConsent();
    if (!guard || serviceRevision != m_vehicleToolServiceRevision) {
        return;
    }
    if (m_vehicleToolService) {
        disconnect(m_vehicleToolService, nullptr, this, nullptr);
    }

    m_vehicleToolService = incoming;
    m_oneShotPlan = {};
    m_oneShotOperationId = 0;
    m_oneShotPreparing = false;
    m_oneShotSubmitting = false;
    m_oneShotTerminalDuringSubmit = false;

    if (incoming) {
        connect(incoming, &DeveloperVehicleToolService::stateChanged,
                this,
                &AccelCalibrationConfig::handleVehicleToolServiceStateChanged);
        connect(incoming, &DeveloperVehicleToolService::operationFinished,
                this,
                &AccelCalibrationConfig::handleVehicleToolOperationFinished);
        connect(incoming, &QObject::destroyed, this,
                [this, serviceRevision]() {
            if (serviceRevision != m_vehicleToolServiceRevision) {
                return;
            }
            const QPointer<AccelCalibrationConfig> pageGuard(this);
            ++m_oneShotFlowRevision;
            dismissOneShotConsent();
            if (!pageGuard) {
                return;
            }
            m_vehicleToolService.clear();
            m_oneShotPlan = {};
            m_oneShotOperationId = 0;
            m_oneShotPreparing = false;
            m_oneShotSubmitting = false;
            ui.calibrateAccelLevelButton->setText(
                tr(LEVEL_CALIBRATE_BUTTON_TEXT));
            ui.levelOutputLabel->setText(
                tr("Level and Simple calibration service is unavailable."));
            refreshCalibrationControls();
        });
    }
    refreshCalibrationControls();
}

void AccelCalibrationConfig::countdownTimerTick()
{
    ui.coutdownLabel->setText(QString().asprintf(COUNTDOWN_STRING, m_uas->getUASID(), m_countdownCount--));
    if (m_countdownCount <= 0)
    {
        ui.coutdownLabel->setText("Command timed out, please try again");
        m_countdownTimer.stop();
        ui.calibrateAccelButton->setText(ui.calibrateAccelButton->text());
        cancelCalibration();
    }
}

void AccelCalibrationConfig::activeUASSet(UASInterface *uas)
{
    if (m_uas) {
        disconnect(m_uas,SIGNAL(mavlinkMessageCommandLong(UASInterface*,mavlink_command_long_t&)),
                   this, SLOT(mavlinkMessageCommandLong(UASInterface*,mavlink_command_long_t&)));

        disconnect(m_uas,SIGNAL(textMessageReceived(int,int,int,QString)),
                   this,SLOT(uasTextMessageReceived(int,int,int,QString)));

        disconnect(m_uas,SIGNAL(connected()), this,SLOT(uasConnected()));
        disconnect(m_uas,SIGNAL(disconnected()), this,SLOT(uasDisconnected()));
    }
    AP2ConfigWidget::activeUASSet(uas);

    if (!uas) {
        refreshCalibrationControls();
        return;
    }

    connect(m_uas,SIGNAL(mavlinkMessageCommandLong(UASInterface*,mavlink_command_long_t&)),
            this, SLOT(mavlinkMessageCommandLong(UASInterface*,mavlink_command_long_t&)));
    connect(m_uas,SIGNAL(textMessageReceived(int,int,int,QString)),
            this,SLOT(uasTextMessageReceived(int,int,int,QString)));

    connect(m_uas,SIGNAL(connected()),this,SLOT(uasConnected()));
    connect(m_uas,SIGNAL(disconnected()),this,SLOT(uasDisconnected()));
    uasConnected();

}
void AccelCalibrationConfig::uasConnected()
{
    cancelCalibration();
    refreshCalibrationControls();
}

void AccelCalibrationConfig::uasDisconnected()
{
    refreshCalibrationControls();
}


void AccelCalibrationConfig::calibrateSimpleButtonClicked()
{
    startOneShotCalibration(VehicleAction::SimpleAccelCalibration);
}

void AccelCalibrationConfig::calibrateLevelButtonClicked()
{
    startOneShotCalibration(VehicleAction::CalibrateLevel);
}

void AccelCalibrationConfig::calibrateButtonClicked() {
    const CalibrationType requested = ui.legacyCheckBox->checkState()
        ? CalibrationType::Legacy_Calibration
        : CalibrationType::Full_Calibration;
    if (isInCalibration && m_calibrationType != requested) {
        ui.outputLabel->setText(
            tr("Finish the active accelerometer calibration first."));
        return;
    }
    if (!isInCalibration && oneShotInteractionBusy()) {
        ui.outputLabel->setText(
            tr("Finish the active Level or Simple calibration first."));
        return;
    }
    m_calibrationType = requested;
    startCalibration();
}

bool AccelCalibrationConfig::oneShotInteractionBusy() const
{
    return m_oneShotPreparing || m_oneShotConsent || m_oneShotSubmitting
        || m_oneShotOperationId != 0
        || (m_vehicleToolService && m_vehicleToolService->busy());
}

void AccelCalibrationConfig::startOneShotCalibration(VehicleAction action)
{
    if (!isOneShotAccelAction(action)) {
        return;
    }
    if (isInCalibration) {
        ui.levelOutputLabel->setText(
            tr("Finish the active Full or Legacy calibration first."));
        return;
    }
    const QPointer<AccelCalibrationConfig> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleToolService);
    if (!service) {
        ui.levelOutputLabel->setText(
            tr("The exact-target calibration service is unavailable."));
        refreshCalibrationControls();
        return;
    }
    if (oneShotInteractionBusy()) {
        ui.levelOutputLabel->setText(
            tr("Finish the active vehicle operation or confirmation first."));
        refreshCalibrationControls();
        return;
    }

    const quint64 flow = ++m_oneShotFlowRevision;
    const quint64 serviceRevision = m_vehicleToolServiceRevision;
    m_oneShotPreparing = true;
    m_oneShotPlan = {};
    m_oneShotOperationId = 0;
    m_oneShotTerminalDuringSubmit = false;
    if (action == VehicleAction::CalibrateLevel) {
        ui.calibrateAccelLevelButton->setText(
            tr(LEVEL_CALIBRATE_BUTTON_TEXT));
    }
    ui.levelOutputLabel->setText(
        tr("Checking the selected vehicle for %1...")
            .arg(oneShotActionName(action).toLower()));
    refreshCalibrationControls();
    if (!guard || serviceRevision != m_vehicleToolServiceRevision
        || m_vehicleToolService != service || flow != m_oneShotFlowRevision) {
        return;
    }

    DeveloperVehicleToolService::Plan plan;
    QString error;
    const bool prepared = service->prepare(action, &plan, &error);
    if (!guard || !service || serviceRevision != m_vehicleToolServiceRevision
        || m_vehicleToolService != service || flow != m_oneShotFlowRevision) {
        return;
    }
    m_oneShotPreparing = false;
    if (!prepared) {
        ui.levelOutputLabel->setText(
            tr("%1 was not prepared: %2")
                .arg(oneShotActionName(action), error));
        refreshCalibrationControls();
        return;
    }

    m_oneShotPlan = plan;
    const VehicleEndpoint endpoint = plan.target.endpoint;
    const bool level = action == VehicleAction::CalibrateLevel;
    const QString detail = level
        ? tr("Place the vehicle and autopilot on a stable, flat, level surface and keep them still. "
             "This performs the one-axis level calibration and updates the default accelerometer offsets/AHRS trim. "
             "It is not the Full six-position or Simple accelerometer calibration.")
        : tr("Place the vehicle and autopilot on a stable, flat, level surface and keep them still. "
             "This performs the firmware's Simple accelerometer calibration. "
             "It is not the Full six-position or one-axis Level calibration.");
    const QString warning =
        tr("%1\n"
           "Target: %2 — link %3, system %4, component %5.\n\n"
           "%6\n\n"
           "The vehicle must remain disarmed. The command changes calibration values on the selected vehicle; "
           "there is no automatic rollback. An acknowledgement means the firmware accepted the request, not "
           "independent verification of the resulting calibration. Continue?")
            .arg(oneShotActionName(action), endpoint.displayName(),
                 QString::number(endpoint.linkId),
                 QString::number(endpoint.systemId),
                 QString::number(endpoint.componentId), detail);
    auto *dialog = new QMessageBox(
        QMessageBox::Warning,
        level ? tr("Calibrate Level")
              : tr("Simple Accelerometer Calibration"),
        warning, QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(level
        ? QStringLiteral("AccelLevelConfirmation")
        : QStringLiteral("AccelSimpleConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_oneShotConsent = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, serviceRevision, plan, action, flow](int result) {
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service
            || flow != m_oneShotFlowRevision
            || m_oneShotPlan.planId != plan.planId) {
            return;
        }
        m_oneShotConsent.clear();
        if (result != QMessageBox::Yes) {
            m_oneShotPlan = {};
            ui.levelOutputLabel->setText(
                tr("%1 cancelled; no command was sent.")
                    .arg(oneShotActionName(action)));
            refreshCalibrationControls();
            return;
        }

        QString validationError;
        const bool valid = service->validate(plan, &validationError);
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service
            || flow != m_oneShotFlowRevision
            || m_oneShotPlan.planId != plan.planId) {
            return;
        }
        if (!valid) {
            m_oneShotPlan = {};
            ui.levelOutputLabel->setText(
                tr("%1 cancelled before transmission: %2")
                    .arg(oneShotActionName(action), validationError));
            refreshCalibrationControls();
            return;
        }

        m_oneShotSubmitting = true;
        m_oneShotTerminalDuringSubmit = false;
        ui.levelOutputLabel->setText(
            tr("Submitting %1 to the exact selected vehicle...")
                .arg(oneShotActionName(action).toLower()));
        refreshCalibrationControls();
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service
            || flow != m_oneShotFlowRevision
            || m_oneShotPlan.planId != plan.planId) {
            return;
        }

        QString submitError;
        const auto submitResult = service->execute(plan, 0.0, &submitError);
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service
            || flow != m_oneShotFlowRevision) {
            return;
        }
        const bool terminalDuringSubmit = m_oneShotTerminalDuringSubmit;
        m_oneShotSubmitting = false;
        if (terminalDuringSubmit) {
            refreshCalibrationControls();
            return;
        }
        if (submitResult != DeveloperVehicleToolService::SubmitResult::Started) {
            m_oneShotPlan = {};
            ui.levelOutputLabel->setText(
                tr("%1 was not started: %2")
                    .arg(oneShotActionName(action), submitError));
        } else if (service->busy() && service->currentOperationId() != 0) {
            m_oneShotOperationId = service->currentOperationId();
            ui.levelOutputLabel->setText(
                tr("%1 submitted; awaiting the vehicle acknowledgement...")
                    .arg(oneShotActionName(action)));
        } else {
            // A terminal callback should normally have reported this case.
            // Fail closed if an injected transport completed without one.
            m_oneShotPlan = {};
            ui.levelOutputLabel->setText(
                tr("%1 ended before an operation could be tracked; inspect the vehicle before retrying.")
                    .arg(oneShotActionName(action)));
        }
        refreshCalibrationControls();
    });
    dialog->open();
    if (!guard || flow != m_oneShotFlowRevision) {
        return;
    }
    refreshCalibrationControls();
}

void AccelCalibrationConfig::dismissOneShotConsent()
{
    QPointer<QMessageBox> dialog(m_oneShotConsent);
    m_oneShotConsent.clear();
    if (!dialog) {
        return;
    }
    disconnect(dialog, nullptr, this, nullptr);
    dialog->blockSignals(true);
    dialog->reject();
    if (dialog) {
        dialog->deleteLater();
    }
}

void AccelCalibrationConfig::handleVehicleToolServiceStateChanged()
{
    const QPointer<AccelCalibrationConfig> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleToolService);
    const quint64 serviceRevision = m_vehicleToolServiceRevision;
    if (service && m_oneShotConsent && m_oneShotPlan.isValid()) {
        QString error;
        const bool valid = service->validate(m_oneShotPlan, &error);
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service) {
            return;
        }
        if (!valid) {
            const VehicleAction action = m_oneShotPlan.action;
            ++m_oneShotFlowRevision;
            const quint64 cancelledFlow = m_oneShotFlowRevision;
            m_oneShotPlan = {};
            dismissOneShotConsent();
            if (!guard || cancelledFlow != m_oneShotFlowRevision) {
                return;
            }
            ui.levelOutputLabel->setText(
                tr("%1 cancelled before transmission: %2")
                    .arg(oneShotActionName(action), error));
        }
    }
    if (guard) {
        refreshCalibrationControls();
    }
}

void AccelCalibrationConfig::handleVehicleToolOperationFinished(
    const DeveloperVehicleToolService::Report &report)
{
    if (!isOneShotAccelAction(report.action)) {
        return;
    }
    const bool matchesTrackedOperation = m_oneShotOperationId != 0
        && report.operationId == m_oneShotOperationId;
    const bool matchesSynchronousSubmission = m_oneShotSubmitting
        && m_oneShotPlan.isValid()
        && report.endpoint.sameIdentity(m_oneShotPlan.target.endpoint);
    if (!matchesTrackedOperation && !matchesSynchronousSubmission) {
        return;
    }

    if (m_oneShotSubmitting) {
        m_oneShotTerminalDuringSubmit = true;
    }
    m_oneShotOperationId = 0;
    m_oneShotPlan = {};
    const QString actionName = oneShotActionName(report.action);
    switch (report.outcome) {
    case DeveloperVehicleToolService::Outcome::Succeeded:
        ui.levelOutputLabel->setText(
            tr("%1 accepted. %2").arg(actionName, report.description));
        if (report.action == VehicleAction::CalibrateLevel) {
            ui.calibrateAccelLevelButton->setText(tr("Completed"));
        }
        break;
    case DeveloperVehicleToolService::Outcome::Rejected:
        ui.levelOutputLabel->setText(
            tr("%1 rejected: %2").arg(actionName, report.description));
        if (report.action == VehicleAction::CalibrateLevel) {
            ui.calibrateAccelLevelButton->setText(
                tr(LEVEL_CALIBRATE_BUTTON_TEXT));
        }
        break;
    case DeveloperVehicleToolService::Outcome::OutcomeUncertain:
        ui.levelOutputLabel->setText(
            tr("%1 outcome is uncertain: %2 Do not assume calibration completed; inspect the vehicle before retrying.")
                .arg(actionName, report.description));
        if (report.action == VehicleAction::CalibrateLevel) {
            ui.calibrateAccelLevelButton->setText(
                tr(LEVEL_CALIBRATE_BUTTON_TEXT));
        }
        break;
    }
    refreshCalibrationControls();
}

void AccelCalibrationConfig::refreshCalibrationControls()
{
    if (m_refreshingCalibrationControls) {
        return;
    }
    m_refreshingCalibrationControls = true;
    const QPointer<AccelCalibrationConfig> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleToolService);
    const quint64 serviceRevision = m_vehicleToolServiceRevision;

    const bool oneShotBusy = oneShotInteractionBusy();
    const bool fullOrLegacyActive = isInCalibration;
    const bool fullCanContinue = fullOrLegacyActive
        && m_calibrationType != CalibrationType::Simple_Calibration;
    const bool canStartDirect = !fullOrLegacyActive && !oneShotBusy;

    ui.calibrateAccelButton->setEnabled(fullCanContinue || canStartDirect);
    if (!guard) {
        return;
    }
    ui.calibrateAccelSimpleButton->setEnabled(
        !fullOrLegacyActive && !oneShotBusy && service);
    if (!guard) {
        return;
    }
    ui.legacyCheckBox->setEnabled(!fullOrLegacyActive && !oneShotBusy);
    if (!guard) {
        return;
    }

    QString levelReason;
    bool canLevel = !fullOrLegacyActive && !oneShotBusy && service;
    if (!service) {
        levelReason = tr("The exact-target calibration service is unavailable.");
    } else if (fullOrLegacyActive) {
        levelReason = tr("Finish the active Full or Legacy calibration first.");
    } else if (oneShotBusy) {
        levelReason = tr("A vehicle operation or calibration confirmation is active.");
    } else {
        canLevel = service->canPrepare(VehicleAction::CalibrateLevel,
                                       &levelReason);
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service) {
            if (guard) {
                m_refreshingCalibrationControls = false;
                refreshCalibrationControls();
            }
            return;
        }
    }
    ui.calibrateAccelLevelButton->setEnabled(canLevel);
    if (!guard) {
        return;
    }
    ui.calibrateAccelLevelButton->setToolTip(canLevel
        ? tr("Set one-axis level offsets/AHRS trim for the exact selected disarmed vehicle.")
        : levelReason);
    if (!guard) {
        return;
    }

    QString simpleReason;
    bool canSimple = !fullOrLegacyActive && !oneShotBusy && service;
    if (!service) {
        simpleReason = tr("The exact-target calibration service is unavailable.");
    } else if (fullOrLegacyActive) {
        simpleReason = tr("Finish the active Full or Legacy calibration first.");
    } else if (oneShotBusy) {
        simpleReason = tr("A vehicle operation or calibration confirmation is active.");
    } else {
        canSimple = service->canPrepare(
            VehicleAction::SimpleAccelCalibration, &simpleReason);
        if (!guard || !service
            || serviceRevision != m_vehicleToolServiceRevision
            || m_vehicleToolService != service) {
            if (guard) {
                m_refreshingCalibrationControls = false;
                refreshCalibrationControls();
            }
            return;
        }
    }
    ui.calibrateAccelSimpleButton->setEnabled(canSimple);
    if (!guard) {
        return;
    }
    ui.calibrateAccelSimpleButton->setToolTip(canSimple
        ? tr("Run Simple accelerometer calibration on the exact selected disarmed vehicle.")
        : simpleReason);
    m_refreshingCalibrationControls = false;
}

void AccelCalibrationConfig::startCalibration()
{
    if (!m_uas) {
        showNullMAVErrorMessageBox();
        return;
    }

    ui.outputLabel->clear();
    ui.calibrateAccelButton->setFocus();

    // Mute Audio until calibrated to avoid HeartBeat Warning message
    if (GAudioOutput::instance()->isMuted() == false) {
        GAudioOutput::instance()->mute(true);
        m_muted = true;
    }
    m_uas->getLinks()->at(0)->disableTimeouts();

    MainWindow::instance()->toolBar().stopAnimation();

    switch (m_calibrationType) {
    case CalibrationType::Full_Calibration:
        if (isInCalibration) {
            // Send Ack
            m_uas->executeCommand(
                MAV_CMD_ACCELCAL_VEHICLE_POS, // command
                1.0, // confirm
                m_position, // param1
                0.0, // param2
                0.0, // param3
                0.0, // param4
                0.0, // param5
                0.0, // param6
                0.0, // param7
                1.0 // component = 1
                );
            m_countdownCount = CALIBRATION_TIMEOUT_SEC;
            ui.coutdownLabel->setText(QString().asprintf(
                COUNTDOWN_STRING, m_uas->getUASID(), m_countdownCount--));
            m_countdownTimer.start(1000);

        } else {
            // start calibration
            isInCalibration = true;
            m_uas->executeCommand(
                MAV_CMD_PREFLIGHT_CALIBRATION, // command
                1.0, // confirm
                0.0, // param1
                0.0, // param2
                0.0, // param3
                0.0, // param4
                1.0, // param5 - 1 = Full 3D Accel Cal
                0.0, // param6
                0.0, // param7
                1.0 // component = 1
                );
            m_countdownCount = CALIBRATION_TIMEOUT_SEC;
            ui.coutdownLabel->setText(QString().asprintf(
                COUNTDOWN_STRING, m_uas->getUASID(), m_countdownCount--));
            m_countdownTimer.start(1000);

            ui.outputLabel->clear();
        } break;

    case CalibrationType::Legacy_Calibration: {
        if (m_accelAckCount == -1) {
            isInCalibration = true;
            // Start full 3D Accel Calibration.
            MAV_CMD command = MAV_CMD_PREFLIGHT_CALIBRATION;
            int confirm = 0;
            float param1 = 0.0;
            float param2 = 0.0;
            float param3 = 0.0;
            float param4 = 0.0;
            float param5 = 1.0; // 1 = Full 3D Accel Cal
            float param6 = 0.0;
            float param7 = 0.0;
            int component = 1;
            m_uas->executeCommand(command, confirm, param1, param2, param3, param4,
                                  param5, param6, param7, component);
            m_countdownCount = CALIBRATION_TIMEOUT_SEC;
            ui.coutdownLabel->setText(QString().asprintf(
                COUNTDOWN_STRING, m_uas->getUASID(), m_countdownCount--));
            m_countdownTimer.start(1000);

            m_accelAckCount = 0;

            ui.outputLabel->clear();

        } else if (m_accelAckCount <= 6) {
            QLOG_DEBUG() << "m_accelAckCount = " << m_accelAckCount;
            m_countdownCount = CALIBRATION_TIMEOUT_SEC;
            m_uas->executeCommandAck(m_accelAckCount, true);

        } else {
            // Auto Reset if bad state
            cancelCalibration();
        }
    } break;
    case CalibrationType::Simple_Calibration: {
        isInCalibration = true;
        // Simple Accel Calibration
        MAV_CMD command = MAV_CMD_PREFLIGHT_CALIBRATION;
        int confirm = 0;
        float param1 = 0.0;
        float param2 = 0.0;
        float param3 = 0.0;
        float param4 = 0.0;
        float param5 = 4.0; // 4 = Simple Accel Calibration
        float param6 = 0.0;
        float param7 = 0.0;
        int component = 1;
        m_uas->executeCommand(command, confirm, param1, param2, param3, param4,
                              param5, param6, param7, component);
        ui.outputLabel->setText("Simple Accel Calibration...");
    }break;
    }
    refreshCalibrationControls();
}

void AccelCalibrationConfig::cancelCalibration()
{
    QLOG_INFO() << "Cancel Accelerometer Calibration.";
    const bool wasInCalibration = isInCalibration;
    ui.coutdownLabel->setText("");
    m_countdownTimer.stop();
    ui.calibrateAccelButton->setText(CALIBRATE_BUTTON_TEXT);
    isInCalibration = false;

    if (wasInCalibration && m_uas
        && m_calibrationType == CalibrationType::Legacy_Calibration
        && m_accelAckCount >= 0) {

        for (int i = 0; i < m_accelAckCount; i++) {
            QLOG_WARN() << "Canceling " << i << " of " << m_accelAckCount;
            m_uas->executeCommandAck(i,true);
        }
        m_accelAckCount = -1;
    } else if (wasInCalibration && m_uas) {
        m_uas->executeCommandAck(1,true);
    }
    refreshCalibrationControls();
}

void AccelCalibrationConfig::hideEvent(QHideEvent *evt)
{
    Q_UNUSED(evt);

    if (m_oneShotConsent) {
        const VehicleAction action = m_oneShotPlan.action;
        ++m_oneShotFlowRevision;
        m_oneShotPlan = {};
        dismissOneShotConsent();
        ui.levelOutputLabel->setText(
            tr("%1 cancelled because the page was closed; no command was sent.")
                .arg(oneShotActionName(action)));
    }

    if (m_muted) { // turns audio backon, when you leave the page
        GAudioOutput::instance()->mute(false);
        m_muted = false;
    }

    MainWindow::instance()->toolBar().startAnimation();

    if (!m_uas || !m_accelAckCount)
    {
        return;
    }
    cancelCalibration();
    m_uas->getLinks()->at(0)->enableTimeouts();
}

QString positionText(int position)
{
    switch (position) {
    case ACCELCAL_VEHICLE_POS_LEVEL:
        return "LEVEL";
    case ACCELCAL_VEHICLE_POS_LEFT:
        return "on it's LEFT";
    case ACCELCAL_VEHICLE_POS_RIGHT:
        return "on it's RIGHT";
    case ACCELCAL_VEHICLE_POS_NOSEDOWN:
        return "NOSE DOWN";
    case ACCELCAL_VEHICLE_POS_NOSEUP:
        return "NOSE UP";
    case ACCELCAL_VEHICLE_POS_BACK:
        return "on it's BACK";
    case ACCELCAL_VEHICLE_POS_SUCCESS:
        return "success";
    case ACCELCAL_VEHICLE_POS_FAILED:
        return "failed";
    case ACCELCAL_VEHICLE_POS_ENUM_END:
    default:
        return "n/a";
    }
}
void AccelCalibrationConfig::mavlinkMessageCommandLong(UASInterface* uas, mavlink_command_long_t& command_long)
{
    Q_UNUSED(uas);

    if (m_calibrationType == CalibrationType::Legacy_Calibration) {
        return;
    }

    switch (command_long.command) {
    case MAV_CMD_ACCELCAL_VEHICLE_POS:
        m_position = command_long.param1;
        switch (m_position) {
        case ACCELCAL_VEHICLE_POS_SUCCESS: {
            ui.outputLabel->setText("SUCCESS: Calibration Complete.");
            ui.coutdownLabel->setText("");
            m_countdownTimer.stop();
            ui.calibrateAccelButton->setText(CALIBRATE_BUTTON_TEXT);
            isInCalibration = false;
            refreshCalibrationControls();
        } break;

        case ACCELCAL_VEHICLE_POS_FAILED: {
            ui.outputLabel->setText("FAILED: Calibration failed.");
            ui.coutdownLabel->setText("");
            m_countdownTimer.stop();
            ui.calibrateAccelButton->setText(CALIBRATE_BUTTON_TEXT);
            isInCalibration = false;
            refreshCalibrationControls();
        } break;

        default:
            ui.calibrateAccelButton->setText(CONTINUE_BUTTON_TEXT);
            ui.outputLabel->setText(
                QString("Place vehicle %1 and press spacebar key")
                    .arg(positionText(m_position))
            );
        }

    }
}

void AccelCalibrationConfig::uasTextMessageReceived(int uasid, int componentid, int severity, QString text)
{
    Q_UNUSED(uasid);
    Q_UNUSED(componentid);

    switch (m_calibrationType) {
    case CalibrationType::Full_Calibration:
    case CalibrationType::Simple_Calibration:
    break;

    case CalibrationType::Legacy_Calibration: {
        QLOG_DEBUG() << "Severity:" << severity << " text:" <<text;

        if (severity <= MAV_SEVERITY_CRITICAL) {
            if (text.startsWith("PreArm:")
                || text.startsWith("EKF")
                || text.startsWith("Arm")
                || text.startsWith("Initialising")
            ) {
                // Filter these warning messages
                return;
            }

            if (text.startsWith("Place ") && m_accelAckCount != -1) {
                //Instruction
                if (m_accelAckCount == 0) {
                    ui.calibrateAccelButton->setText(CONTINUE_BUTTON_TEXT);
                }
                ui.outputLabel->setText(text);
                m_accelAckCount++;

            } else if (text.contains("Calibration successful")) {
                // Calibration complete success
                if (m_muted) { // turns audio back on, when you complete fail or success
                    GAudioOutput::instance()->mute(false);
                    m_muted = false;
                }
                ui.coutdownLabel->setText("");
                m_countdownTimer.stop();
                ui.calibrateAccelButton->setText(CALIBRATE_BUTTON_TEXT);
                ui.calibrateAccelButton->clearFocus();
                ui.outputLabel->setText(ui.outputLabel->text() + "\n" + text);
                MainWindow::instance()->toolBar().startAnimation();
                m_accelAckCount = -1;
                isInCalibration = false;
                refreshCalibrationControls();

            } else if (text.contains("FAILED")
                || text.contains("Failed CMD: 241") || text.startsWith("FAILURE:")
            ) {
                //Calibration complete success or failure
                if (m_muted) { // turns audio back on, when you complete fail or success
                    GAudioOutput::instance()->mute(false);
                    m_muted = false;
                }
                cancelCalibration();
                ui.outputLabel->setText(ui.outputLabel->text() + "\n" + text);
                MainWindow::instance()->toolBar().startAnimation();
            } else {
                ui.outputLabel->setText(ui.outputLabel->text() + "\n" + text);
            }
        }
    } break;
    }

}
