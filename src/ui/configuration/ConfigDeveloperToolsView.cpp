#include "ConfigDeveloperToolsView.h"

#include "DeveloperToolParsers.h"

#include <QAction>
#include <QCloseEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <cmath>

ConfigDeveloperToolsView::ConfigDeveloperToolsView(QObject *actionSource,
                                                   QWidget *parent)
    : ActionPageView(
          tr("Developer Tools"),
          tr("Cross-platform diagnostics and recovery tools ported from "
             "Mission Planner's hidden developer window. Vehicle-changing "
             "actions require a connection, a disarmed vehicle, and explicit "
             "confirmation."),
          parent)
    , m_actionSource(actionSource)
{
    setObjectName(QStringLiteral("ConfigDeveloperToolsView"));

    const QString notPorted = tr("This Mission Planner developer workflow has "
                                 "not yet been ported to Qt.");
    AddAction(tr("Decode MAVLink Packet"),
              QStringLiteral("DecodeMavlinkPacketButton"),
              [this]() { DecodePacket(); });
    ++m_implementedActionCount;
    AddAction(tr("Decode Hardware ID"),
              QStringLiteral("DecodeHardwareIdButton"),
              [this]() { DecodeHardwareId(); });
    ++m_implementedActionCount;

    AddToolAction(tr("MAVLink Device Operations"),
                  QStringLiteral("MavlinkDeviceOperationsButton"),
                  QStringLiteral("actionMavlinkDeviceOperations"));
    AddToolAction(tr("3D Terrain View"),
                  QStringLiteral("Terrain3dViewButton"),
                  QStringLiteral("actionTerrain3D"));
    AddUnavailableAction(tr("MicroDrone Downlink"),
                         QStringLiteral("MicroDroneDownlinkButton"), notPorted);
    AddUnavailableAction(tr("MAVLink Serial TCP Bridge"),
                         QStringLiteral("MavlinkSerialTcpBridgeButton"), notPorted);
    AddUnavailableAction(tr("Download Firmware Archive"),
                         QStringLiteral("DownloadFirmwareArchiveButton"), notPorted);
    AddUnavailableAction(tr("Cancel Firmware Archive"),
                         QStringLiteral("CancelFirmwareArchiveButton"), notPorted);
    AddUnavailableAction(tr("Probe MAVLink Camera"),
                         QStringLiteral("ProbeMavlinkCameraButton"), notPorted);
    AddUnavailableAction(tr("Embed Defaults in APJ"),
                         QStringLiteral("EmbedDefaultsInApjButton"), notPorted);
    AddUnavailableAction(tr("Split DataFlash Log"),
                         QStringLiteral("SplitDataFlashLogButton"), notPorted);
    AddUnavailableAction(tr("Create DashWare CSV"),
                         QStringLiteral("CreateDashWareCsvButton"), notPorted);
    AddUnavailableAction(tr("Extract GPS Corrections"),
                         QStringLiteral("ExtractGpsCorrectionsButton"), notPorted);
    AddUnavailableAction(tr("Convert Shapefile to POLY"),
                         QStringLiteral("ConvertShapefileToPolyButton"), notPorted);
    AddUnavailableAction(tr("Translation / RESX Editor"),
                         QStringLiteral("TranslationResxEditorButton"), notPorted);
    AddToolAction(tr("OSD Video — Telemetry Overlay"),
                  QStringLiteral("OsdVideoTelemetryOverlayButton"),
                  QStringLiteral("actionOsdVideoOverlay"));
    AddUnavailableAction(tr("Offline Magnetometer Calibration (MagFit)"),
                         QStringLiteral("OfflineMagFitButton"), notPorted);
    AddUnavailableAction(tr("Flight Log Index"),
                         QStringLiteral("FlightLogIndexButton"), notPorted);
    AddUnavailableAction(tr("Organize Log Directory"),
                         QStringLiteral("OrganizeLogDirectoryButton"), notPorted);
    AddUnavailableAction(tr("Download DataFlash Logs over SFTP"),
                         QStringLiteral("DownloadDataFlashSftpButton"), notPorted);
    AddUnavailableAction(tr("Download MAVFTP File"),
                         QStringLiteral("DownloadMavftpFileButton"), notPorted);
    AddUnavailableAction(tr("Restore Parameters (Recovery)"),
                         QStringLiteral("RestoreParametersButton"), notPorted);
    AddUnavailableAction(tr("Cancel Parameter Restore"),
                         QStringLiteral("CancelParameterRestoreButton"), notPorted);
    AddVehicleAction(tr("Set QNH"), QStringLiteral("SetQnhButton"), VehicleAction::SetQnh);
    AddVehicleAction(tr("Adjust Barometer Altitude"),
                     QStringLiteral("AdjustBarometerAltitudeButton"), VehicleAction::AdjustBarometerAltitude);
    AddVehicleAction(tr("Force Accel Calibrated"),
                     QStringLiteral("ForceAccelCalibratedButton"), VehicleAction::ForceAccelCalibrated);
    AddVehicleAction(tr("Force Compass Calibrated"),
                     QStringLiteral("ForceCompassCalibratedButton"), VehicleAction::ForceCompassCalibrated);
    AddVehicleAction(tr("Reboot Vehicle"), QStringLiteral("RebootVehicleButton"), VehicleAction::RebootVehicle);
    AddUnavailableAction(tr("Upgrade Bootloader"),
                         QStringLiteral("UpgradeBootloaderButton"), notPorted);
    AddVehicleAction(tr("Reboot to DFU"), QStringLiteral("RebootToDfuButton"), VehicleAction::RebootToDfu);
    AddUnavailableAction(tr("Start Remote DataFlash Log"),
                         QStringLiteral("StartRemoteDataFlashLogButton"), notPorted);
    AddUnavailableAction(tr("Stop Remote DataFlash Log"),
                         QStringLiteral("StopRemoteDataFlashLogButton"), notPorted);

    AppendLog(tr("%1 of %2 Mission Planner Developer tools are available. "
                 "The remaining actions stay disabled until their guarded "
                 "end-to-end workflows are ported.")
                  .arg(m_implementedActionCount)
                  .arg(ActionCount()));
    auto *eligibilityTimer = new QTimer(this);
    eligibilityTimer->setInterval(300);
    connect(eligibilityTimer, &QTimer::timeout, this,
            &ConfigDeveloperToolsView::RefreshVehicleActions);
    eligibilityTimer->start();
}

int ConfigDeveloperToolsView::ImplementedActionCount() const
{
    return m_implementedActionCount + (m_vehicleTools ? m_vehicleButtons.size() : 0);
}

void ConfigDeveloperToolsView::AddVehicleAction(
    const QString &label, const QString &objectName, VehicleAction action)
{
    m_vehicleButtons.insert(action, AddAction(label, objectName,
        [this, action]() { StartVehicleAction(action); }, false,
        tr("The guarded vehicle tool service is unavailable.")));
}

void ConfigDeveloperToolsView::setVehicleToolService(DeveloperVehicleToolService *service)
{
    if (m_vehicleTools == service)
        return;
    CancelVehiclePrompt();
    if (m_vehicleTools)
        disconnect(m_vehicleTools, nullptr, this, nullptr);
    m_vehicleTools = service;
    m_seenHistory.clear();
    m_seenStatus.clear();
    if (service) {
        connect(service, &DeveloperVehicleToolService::stateChanged,
                this, &ConfigDeveloperToolsView::RefreshVehicleActions);
        connect(service, &QObject::destroyed, this, [this]() {
            CancelVehiclePrompt();
            RefreshVehicleActions();
        });
    }
    AppendLog(tr("%1 of %2 Mission Planner Developer tools are available.")
                  .arg(ImplementedActionCount()).arg(ActionCount()));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::RefreshVehicleActions()
{
    if (m_refreshingVehicleActions)
        return;
    m_refreshingVehicleActions = true;
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    for (auto it = m_vehicleButtons.cbegin(); it != m_vehicleButtons.cend(); ++it) {
        QString reason;
        bool enabled = false;
        if (!service)
            reason = tr("The guarded vehicle tool service is unavailable.");
        else if (m_vehiclePrompt)
            reason = tr("Finish or cancel the current confirmation first.");
        else if (service->busy())
            reason = tr("A vehicle operation is awaiting its terminal outcome.");
        else
            enabled = service->canPrepare(it.key(), &reason);
        if (!guard)
            return;
        if (m_vehicleTools != service) {
            m_refreshingVehicleActions = false;
            return;
        }
        if (!service) {
            enabled = false;
            reason = tr("The guarded vehicle tool service is unavailable.");
        }
        it.value()->setEnabled(enabled);
        it.value()->setToolTip(enabled ? tr("Requires explicit confirmation for the selected disarmed vehicle.") : reason);
    }
    if (service) {
        const QStringList history = service->history();
        // The service retains a bounded history across page closure. Match its
        // old suffix to the new prefix when its oldest entries roll off.
        int overlap = qMin(m_seenHistory.size(), history.size());
        while (overlap > 0 && m_seenHistory.mid(m_seenHistory.size() - overlap) != history.mid(0, overlap))
            --overlap;
        for (int i = overlap; i < history.size(); ++i)
            AppendLog(history.at(i));
        m_seenHistory = history;
        const QString status = service->status();
        if (!status.isEmpty() && status != m_seenStatus && !history.contains(status))
            AppendLog(status);
        m_seenStatus = status;
    }
    m_refreshingVehicleActions = false;
}

void ConfigDeveloperToolsView::CancelVehiclePrompt()
{
    ++m_promptRevision;
    const QPointer<QDialog> prompt = m_vehiclePrompt;
    m_vehiclePrompt.clear();
    if (prompt)
        prompt->reject();
}

void ConfigDeveloperToolsView::closeEvent(QCloseEvent *event)
{
    CancelVehiclePrompt();
    ActionPageView::closeEvent(event);
}

void ConfigDeveloperToolsView::StartVehicleAction(VehicleAction action)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    if (!service || m_vehiclePrompt)
        return;
    VehiclePlan plan;
    QString error;
    const bool prepared = service->prepare(action, &plan, &error);
    if (!guard || !service || m_vehicleTools != service)
        return;
    if (!prepared) {
        AppendLog(tr("Cannot prepare vehicle operation: %1").arg(error));
        RefreshVehicleActions();
        return;
    }
    const quint64 revision = ++m_promptRevision;
    if (action != VehicleAction::SetQnh && action != VehicleAction::AdjustBarometerAltitude) {
        ConfirmVehicleAction(plan, 0.0, revision);
        return;
    }
    auto *input = new QInputDialog(this);
    input->setObjectName(QStringLiteral("DeveloperVehicleValueDialog"));
    input->setAttribute(Qt::WA_DeleteOnClose);
    input->setWindowTitle(m_vehicleButtons.value(action)->text());
    input->setInputMode(QInputDialog::DoubleInput);
    input->setDoubleDecimals(3);
    if (action == VehicleAction::SetQnh) {
        input->setLabelText(tr("%1 — pressure in Pa (current: %2 Pa)")
                                .arg(plan.parameterName).arg(plan.originalValue.toDouble(), 0, 'f', 3));
        input->setDoubleRange(DeveloperVehicleToolService::MinimumPressurePa,
                              DeveloperVehicleToolService::MaximumPressurePa);
        input->setDoubleValue(plan.originalValue.toDouble());
    } else {
        input->setLabelText(tr("Altitude correction in metres (−100 to +100 m).\n"
                               "%1 changes by 11.1 Pa per metre; current: %2 Pa.")
                                .arg(plan.parameterName).arg(plan.originalValue.toDouble(), 0, 'f', 3));
        input->setDoubleRange(-DeveloperVehicleToolService::MaximumAltitudeAdjustmentMetres,
                              DeveloperVehicleToolService::MaximumAltitudeAdjustmentMetres);
        input->setDoubleValue(0.0);
    }
    m_vehiclePrompt = input;
    connect(input, &QDialog::finished, this, [this, service, plan, revision, input](int result) {
        if (revision != m_promptRevision || m_vehicleTools != service)
            return;
        m_vehiclePrompt.clear();
        if (result == QDialog::Accepted)
            ConfirmVehicleAction(plan, input->doubleValue(), revision);
        else
            RefreshVehicleActions();
    });
    input->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ConfirmVehicleAction(
    const VehiclePlan &plan, double value, quint64 revision)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    if (!service || revision != m_promptRevision)
        return;
    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_vehicleTools != service || revision != m_promptRevision)
        return;
    if (!valid) {
        AppendLog(tr("Vehicle operation cancelled: %1").arg(error));
        RefreshVehicleActions();
        return;
    }
    QString warning = tr("%1\nTarget: link %2, system %3, component %4 (%5).\n\n")
        .arg(m_vehicleButtons.value(plan.action)->text())
        .arg(plan.target.endpoint.linkId).arg(plan.target.endpoint.systemId)
        .arg(plan.target.endpoint.componentId).arg(plan.target.endpoint.linkName);
    switch (plan.action) {
    case VehicleAction::SetQnh:
    case VehicleAction::AdjustBarometerAltitude: {
        const double pressure = plan.action == VehicleAction::SetQnh ? value
            : plan.originalValue.toDouble() + value * DeveloperVehicleToolService::PressurePerMetrePa;
        if (!std::isfinite(pressure) || pressure < DeveloperVehicleToolService::MinimumPressurePa ||
            pressure > DeveloperVehicleToolService::MaximumPressurePa) {
            AppendLog(tr("Requested pressure must be between 80000 and 120000 Pa; nothing was sent."));
            RefreshVehicleActions();
            return;
        }
        if (plan.action == VehicleAction::AdjustBarometerAltitude && value == 0.0) {
            AppendLog(tr("Zero altitude correction: no change was requested."));
            RefreshVehicleActions();
            return;
        }
        warning += tr("Write %1: %2 Pa → %3 Pa?")
            .arg(plan.parameterName).arg(plan.originalValue.toDouble(), 0, 'f', 3)
            .arg(pressure, 0, 'f', 3);
        warning += tr("\nThis developer tool changes the ground-pressure reference; "
                      "it is not certified sea-level QNH setting. Existing firmware write "
                      "permissions are required; this tool does not enable internal writes.");
        if (plan.action == VehicleAction::AdjustBarometerAltitude)
            warning += tr("\nAltitude correction: %1 m.").arg(value, 0, 'f', 3);
        break;
    }
    case VehicleAction::ForceAccelCalibrated:
    case VehicleAction::ForceCompassCalibrated:
        warning += tr("Recovery only: mark the sensor as calibrated WITHOUT performing calibration. "
                      "This bypass is intended for recovery after a parameter wipe; it does not calibrate "
                      "the sensor and can leave the vehicle unsafe to fly. Continue?");
        break;
    case VehicleAction::RebootVehicle:
        warning += tr("Reboot this vehicle now? Telemetry will be interrupted.");
        break;
    case VehicleAction::RebootToDfu:
        warning += tr("Reboot this vehicle into DFU firmware recovery mode? "
                      "Telemetry may stop before an acknowledgement. Connection loss does not "
                      "confirm DFU entry; an unacknowledged outcome remains unknown. "
                      "Unsupported boards or secure firmware may reject the request. "
                      "Normal flight operation will stop if DFU is entered.");
        break;
    }
    auto *dialog = new QMessageBox(QMessageBox::Warning,
        tr("Confirm vehicle operation"), warning,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("DeveloperVehicleConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_vehiclePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, plan, value, revision](int result) {
        if (revision != m_promptRevision || m_vehicleTools != service)
            return;
        m_vehiclePrompt.clear();
        if (result != QMessageBox::Yes || !service) {
            RefreshVehicleActions();
            return;
        }
        QString error;
        const bool valid = service->validate(plan, &error);
        if (!guard || !service || m_vehicleTools != service || revision != m_promptRevision)
            return;
        if (!valid) {
            AppendLog(tr("Vehicle operation cancelled: %1").arg(error));
            RefreshVehicleActions();
            return;
        }
        const auto submitResult = service->execute(plan, value, &error);
        if (!guard || !service || m_vehicleTools != service || revision != m_promptRevision)
            return;
        if (submitResult != DeveloperVehicleToolService::SubmitResult::Started)
            AppendLog(tr("Vehicle operation was not started: %1").arg(error));
        RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

QPushButton *ConfigDeveloperToolsView::AddToolAction(
    const QString &label, const QString &buttonObjectName,
    const QString &actionObjectName)
{
    QAction *action = m_actionSource
        ? m_actionSource->findChild<QAction *>(actionObjectName)
        : nullptr;
    if (!action) {
        return AddUnavailableAction(
            label, buttonObjectName,
            tr("The shared application action '%1' is unavailable.")
                .arg(actionObjectName));
    }

    QPointer<QAction> guardedAction(action);
    QPushButton *button = AddAction(
        label, buttonObjectName,
        [this, guardedAction, label]() {
            if (!guardedAction || !guardedAction->isEnabled()) {
                AppendLog(tr("%1 is currently unavailable.").arg(label));
                return;
            }
            guardedAction->trigger();
            AppendLog(tr("Opened %1.").arg(label));
        },
        action->isEnabled(), action->toolTip());
    ++m_implementedActionCount;

    connect(action, &QAction::changed, button,
            [button, guardedAction]() {
        button->setEnabled(guardedAction && guardedAction->isEnabled());
        button->setToolTip(guardedAction ? guardedAction->toolTip()
                                         : QString());
    });
    connect(action, &QObject::destroyed, button,
            [button]() { button->setEnabled(false); });
    return button;
}

void ConfigDeveloperToolsView::DecodeMavlinkInput(const QString &input)
{
    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(input);
    AppendLog(result.ok()
                  ? result.text()
                  : tr("MAVLink decode failed: %1").arg(result.error));
}

void ConfigDeveloperToolsView::DecodeHardwareIdInput(
    const QString &input, const QString &parameterName)
{
    const DecodedHardwareId result =
        DeveloperToolParsers::DecodeHardwareId(input, parameterName);
    AppendLog(result.ok()
                  ? result.text
                  : tr("Hardware ID decode failed: %1").arg(result.error));
}

void ConfigDeveloperToolsView::DecodePacket()
{
    bool accepted = false;
    const QString input = QInputDialog::getMultiLineText(
        this, tr("Decode MAVLink Packet"),
        tr("Enter compact hex (fd0500...) or separated decimal/hex bytes"),
        QString(), &accepted);
    if (accepted) {
        DecodeMavlinkInput(input);
    }
}

void ConfigDeveloperToolsView::DecodeHardwareId()
{
    bool accepted = false;
    const QString id = QInputDialog::getText(
        this, tr("Decode Hardware ID"),
        tr("Device ID (decimal or 0x-prefixed hex)"),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted) {
        return;
    }

    const QString parameterName = QInputDialog::getText(
        this, tr("Decode Hardware ID"),
        tr("Optional parameter name (for example COMPASS_DEV_ID or INS_ACC_ID)"),
        QLineEdit::Normal, QString(), &accepted);
    if (accepted) {
        DecodeHardwareIdInput(id, parameterName);
    }
}
