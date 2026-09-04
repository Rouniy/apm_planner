#include "ConfigDeveloperToolsView.h"

#include "DeveloperToolParsers.h"

#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>

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
    AddUnavailableAction(tr("OSD Video — Telemetry Overlay"),
                         QStringLiteral("OsdVideoTelemetryOverlayButton"), notPorted);
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
    AddUnavailableAction(tr("Set QNH"),
                         QStringLiteral("SetQnhButton"), notPorted);
    AddUnavailableAction(tr("Adjust Barometer Altitude"),
                         QStringLiteral("AdjustBarometerAltitudeButton"), notPorted);
    AddUnavailableAction(tr("Force Accel Calibrated"),
                         QStringLiteral("ForceAccelCalibratedButton"), notPorted);
    AddUnavailableAction(tr("Force Compass Calibrated"),
                         QStringLiteral("ForceCompassCalibratedButton"), notPorted);
    AddUnavailableAction(tr("Reboot Vehicle"),
                         QStringLiteral("RebootVehicleButton"), notPorted);
    AddUnavailableAction(tr("Upgrade Bootloader"),
                         QStringLiteral("UpgradeBootloaderButton"), notPorted);
    AddUnavailableAction(tr("Reboot to DFU"),
                         QStringLiteral("RebootToDfuButton"), notPorted);
    AddUnavailableAction(tr("Start Remote DataFlash Log"),
                         QStringLiteral("StartRemoteDataFlashLogButton"), notPorted);
    AddUnavailableAction(tr("Stop Remote DataFlash Log"),
                         QStringLiteral("StopRemoteDataFlashLogButton"), notPorted);

    AppendLog(tr("%1 of %2 Mission Planner Developer tools are available. "
                 "The remaining actions stay disabled until their guarded "
                 "end-to-end workflows are ported.")
                  .arg(m_implementedActionCount)
                  .arg(ActionCount()));
}

int ConfigDeveloperToolsView::ImplementedActionCount() const
{
    return m_implementedActionCount;
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
