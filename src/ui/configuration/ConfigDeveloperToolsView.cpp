#include "ConfigDeveloperToolsView.h"

#include "DeveloperToolParsers.h"
#include "MavFtpFileDownload.h"
#include "comm/MavFtpServiceInterface.h"
#include "comm/VehicleTargetManager.h"
#include "comm/GpsCorrectionExtractor.h"
#include "ui/Loghandling/DataFlashDashWareCsvExporter.h"
#include "ui/Loghandling/DataFlashLogSplitter.h"
#include "ui/Loghandling/FlightLogOrganizer.h"
#include "ui/tools/ApjDefaultsEmbedder.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QProgressDialog>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <cmath>
#include <exception>

struct ConfigDeveloperToolsView::GpsExtractionState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> processed{0};
    std::atomic<qint64> total{0};
};

struct ConfigDeveloperToolsView::SplitState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> processed{0};
    std::atomic<qint64> total{0};
};

struct ConfigDeveloperToolsView::DashWareState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> processed{0};
    std::atomic<qint64> total{0};
};

struct ConfigDeveloperToolsView::ApjEmbeddingState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> processed{0};
    std::atomic<qint64> total{0};
};

struct ConfigDeveloperToolsView::LogOrganizerState
{
    // Execution can leave truthful partial filesystem results. Its terminal
    // callback is therefore retained across a page close; analysis is safely
    // invalidated because it never mutates the directory.
    bool executing = false;
    std::atomic_bool cancelled{false};
    std::atomic<qint64> processed{0};
    std::atomic<qint64> total{0};
};

namespace {
QStringList normalizedDashWareTypes(const QStringList &values)
{
    QStringList result;
    for (QString value : values) {
        value = value.trimmed().toUpper();
        if (!value.isEmpty() && !result.contains(value))
            result.append(value);
    }
    return result;
}
}

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
    m_apjButton = AddAction(tr("Embed Defaults in APJ"),
        QStringLiteral("EmbedDefaultsInApjButton"),
        [this]() { PickApjFirmware(); });
    m_apjButton->setToolTip(tr(
        "Embed a parameter-defaults file in a local ArduPilot APJ image; "
        "the result is written beside the source firmware and is not flashed."));
    ++m_implementedActionCount;
    m_splitButton = AddAction(tr("Split DataFlash Log"),
        QStringLiteral("SplitDataFlashLogButton"),
        [this]() { PickSplitInput(); });
    m_splitButton->setToolTip(tr("Split a recorded DataFlash .bin or .log file into complete, independently readable parts; no vehicle connection is required."));
    ++m_implementedActionCount;
    m_dashWareButton = AddAction(tr("Create DashWare CSV"),
        QStringLiteral("CreateDashWareCsvButton"),
        [this]() { PickDashWareInput(); });
    m_dashWareButton->setToolTip(tr("Export selected DataFlash message types to a DashWare-compatible CSV file; no vehicle connection is required."));
    ++m_implementedActionCount;
    m_gpsExtractionButton = AddAction(tr("Extract GPS Corrections"),
        QStringLiteral("ExtractGpsCorrectionsButton"),
        [this]() { PickGpsCorrectionInput(); });
    m_gpsExtractionButton->setToolTip(tr("Extract recorded GPS correction bytes from a telemetry log; no vehicle connection is required."));
    ++m_implementedActionCount;
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
    m_logOrganizerButton = AddAction(
        tr("Organize Log Directory"),
        QStringLiteral("OrganizeLogDirectoryButton"),
        [this]() { PickLogOrganizerDirectory(); });
    m_logOrganizerButton->setToolTip(tr(
        "Analyze a local log directory, review every proposed move and empty-log deletion, then explicitly execute the immutable plan."));
    ++m_implementedActionCount;
    AddUnavailableAction(tr("Download DataFlash Logs over SFTP"),
                         QStringLiteral("DownloadDataFlashSftpButton"), notPorted);
    m_mavFtpButton = AddAction(tr("Download MAVFTP File"),
        QStringLiteral("DownloadMavftpFileButton"),
        [this]() { StartMavFtpDownload(); }, false,
        tr("The MAVFTP download service is unavailable."));
    m_restoreParametersButton = AddAction(
        tr("Restore Parameters (Recovery)"),
        QStringLiteral("RestoreParametersButton"),
        [this]() { PickParameterRecoveryFile(); }, false,
        tr("The guarded parameter recovery service is unavailable."));
    m_cancelParameterRestoreButton = AddAction(
        tr("Cancel Parameter Restore"),
        QStringLiteral("CancelParameterRestoreButton"),
        [this]() { CancelOwnedParameterRecovery(); }, false,
        tr("No parameter recovery started by this page is active."));
    AddVehicleAction(tr("Set QNH"), QStringLiteral("SetQnhButton"), VehicleAction::SetQnh);
    AddVehicleAction(tr("Adjust Barometer Altitude"),
                     QStringLiteral("AdjustBarometerAltitudeButton"), VehicleAction::AdjustBarometerAltitude);
    AddVehicleAction(tr("Force Accel Calibrated"),
                     QStringLiteral("ForceAccelCalibratedButton"), VehicleAction::ForceAccelCalibrated);
    AddVehicleAction(tr("Force Compass Calibrated"),
                     QStringLiteral("ForceCompassCalibratedButton"), VehicleAction::ForceCompassCalibrated);
    AddVehicleAction(tr("Reboot Vehicle"), QStringLiteral("RebootVehicleButton"), VehicleAction::RebootVehicle);
    AddVehicleAction(tr("Upgrade Bootloader"),
                     QStringLiteral("UpgradeBootloaderButton"),
                     VehicleAction::UpgradeBootloader);
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
    return m_implementedActionCount + (m_vehicleTools ? m_vehicleButtons.size() : 0)
        + (m_mavFtpService && m_mavFtpTargets ? 1 : 0)
        + (m_parameterRecoveryService ? 2 : 0);
}

bool ConfigDeveloperToolsView::MavFtpDownloadBusy() const
{
    return (m_mavFtpDownload && m_mavFtpDownload->busy())
        || (m_mavFtpService && m_mavFtpService->isBusy());
}

bool ConfigDeveloperToolsView::ApjEmbeddingBusy() const
{
    return m_apjState || m_apjPrompt;
}

bool ConfigDeveloperToolsView::LogOrganizerBusy() const
{
    return m_logOrganizerState || m_logOrganizerPrompt;
}

bool ConfigDeveloperToolsView::ParameterRecoveryBusy() const
{
    return m_parameterRecoveryPrompt
        || (m_parameterRecoveryService && m_parameterRecoveryService->busy());
}

void ConfigDeveloperToolsView::setMavFtpDownloadServices(
    MavFtpServiceInterface *service, VehicleTargetManager *targets)
{
    if (m_mavFtpService == service && m_mavFtpTargets == targets)
        return;
    if (m_mavFtpDownload) {
        disconnect(m_mavFtpDownload, nullptr, this, nullptr);
        delete m_mavFtpDownload;
    }
    if (m_mavFtpService)
        disconnect(m_mavFtpService, nullptr, this, nullptr);
    if (m_mavFtpTargets)
        disconnect(m_mavFtpTargets, nullptr, this, nullptr);
    m_mavFtpService = service;
    m_mavFtpTargets = targets;
    if (service && targets) {
        m_mavFtpDownload = new MavFtpFileDownload(service, targets, this);
        connect(m_mavFtpDownload, &MavFtpFileDownload::busyChanged,
                this, &ConfigDeveloperToolsView::RefreshVehicleActions);
        connect(m_mavFtpDownload, &MavFtpFileDownload::logMessage,
                this, [this](const QString &message) {
            if (!m_fileToolsClosing)
                AppendLog(message);
        });
        connect(service, &MavFtpServiceInterface::stateChanged,
                this, &ConfigDeveloperToolsView::RefreshOfflineFileActions);
        connect(service, &QObject::destroyed,
                this, &ConfigDeveloperToolsView::RefreshVehicleActions);
        connect(targets, &QObject::destroyed,
                this, &ConfigDeveloperToolsView::RefreshVehicleActions);
    }
    AppendLog(tr("%1 of %2 Mission Planner Developer tools are available.")
                  .arg(ImplementedActionCount()).arg(ActionCount()));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::setParameterRecoveryService(
    ParameterRecoveryService *service)
{
    if (m_parameterRecoveryService == service) {
        RefreshVehicleActions();
        return;
    }

    const quint64 bindingRevision = ++m_parameterRecoveryBindingRevision;
    const QPointer<ParameterRecoveryService> incomingService(service);
    CancelParameterRecoveryPrompt();
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> oldService(
        m_parameterRecoveryService);
    const quint64 oldOperationId = m_ownedParameterRecoveryOperationId;
    if (oldService && oldOperationId != 0
        && oldService->currentOperationId() == oldOperationId) {
        oldService->cancel(oldOperationId);
    }
    if (!guard || bindingRevision != m_parameterRecoveryBindingRevision)
        return;
    if (oldService)
        disconnect(oldService, nullptr, this, nullptr);

    m_parameterRecoveryService = incomingService;
    m_ownedParameterRecoveryOperationId = 0;
    m_seenParameterRecoveryHistory.clear();
    m_seenParameterRecoveryStatus.clear();
    if (m_parameterRecoveryProgress) {
        const QPointer<QProgressDialog> progress(m_parameterRecoveryProgress);
        m_parameterRecoveryProgress.clear();
        if (progress) {
            const QSignalBlocker blocker(progress);
            progress->cancel();
            progress->deleteLater();
        }
    }

    if (incomingService) {
        connect(incomingService, &ParameterRecoveryService::stateChanged, this,
                &ConfigDeveloperToolsView::RefreshVehicleActions);
        connect(incomingService, &ParameterRecoveryService::operationFinished, this,
                &ConfigDeveloperToolsView::HandleParameterRecoveryFinished);
        connect(incomingService, &QObject::destroyed, this, [this]() {
            const QPointer<ConfigDeveloperToolsView> guard(this);
            ++m_parameterRecoveryBindingRevision;
            ++m_parameterRecoveryPromptRevision;
            const QPointer<QDialog> prompt(m_parameterRecoveryPrompt);
            m_parameterRecoveryPrompt.clear();
            const QPointer<QProgressDialog> progress(
                m_parameterRecoveryProgress);
            m_parameterRecoveryProgress.clear();
            m_ownedParameterRecoveryOperationId = 0;
            // Static application services can outlive QApplication.  Neither
            // text layout nor dialog hide/cancel may touch GUI infrastructure
            // after that teardown has started.
            if (m_fileToolsClosing || QCoreApplication::closingDown())
                return;
            if (prompt) {
                const QSignalBlocker blocker(prompt);
                prompt->reject();
            }
            if (!guard)
                return;
            if (progress) {
                const QSignalBlocker blocker(progress);
                progress->cancel();
                progress->deleteLater();
            }
            if (guard && !m_fileToolsClosing && !QCoreApplication::closingDown()) {
                AppendLog(tr("Parameter recovery service became unavailable."));
                if (guard)
                    RefreshVehicleActions();
            }
        });
    }

    AppendLog(tr("%1 of %2 Mission Planner Developer tools are available.")
                  .arg(ImplementedActionCount()).arg(ActionCount()));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::CancelParameterRecoveryPrompt()
{
    ++m_parameterRecoveryPromptRevision;
    const QPointer<QDialog> prompt(m_parameterRecoveryPrompt);
    m_parameterRecoveryPrompt.clear();
    if (prompt) {
        const QSignalBlocker blocker(prompt);
        prompt->reject();
    }
}

void ConfigDeveloperToolsView::CancelOwnedParameterRecovery()
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    const quint64 operationId = m_ownedParameterRecoveryOperationId;
    if (!service || operationId == 0
        || service->currentOperationId() != operationId) {
        if (!m_fileToolsClosing)
            AppendLog(tr("No parameter recovery started by this page is active."));
        RefreshVehicleActions();
        return;
    }

    const bool accepted = service->cancel(operationId);
    if (!guard)
        return;
    if (!accepted && !m_fileToolsClosing) {
        AppendLog(tr("Parameter recovery cancellation was not accepted; "
                     "the terminal service report remains authoritative."));
    }
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickParameterRecoveryFile()
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    if (m_fileToolsClosing || !service || ParameterRecoveryBusy()
        || m_gpsExtractionState || m_gpsExtractionPrompt || m_splitState
        || m_splitPrompt || m_dashWareState || m_dashWarePrompt
        || ApjEmbeddingBusy() || LogOrganizerBusy() || MavFtpDownloadBusy()
        || m_vehiclePrompt || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }

    const quint64 revision = ++m_parameterRecoveryPromptRevision;
    QString error;
    const bool available = service->canPrepare(&error);
    if (!guard || !service || m_fileToolsClosing
        || m_parameterRecoveryService != service
        || revision != m_parameterRecoveryPromptRevision
        || ParameterRecoveryBusy() || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy() || MavFtpDownloadBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    if (!available) {
        AppendLog(tr("Parameter recovery is unavailable: %1").arg(error));
        RefreshVehicleActions();
        return;
    }

    auto *dialog = new QFileDialog(this, tr("Select parameter recovery file"));
    dialog->setObjectName(
        QStringLiteral("DeveloperParameterRecoveryFileDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({
        tr("Parameter files (*.param *.parm *.PARAM *.PARM)"),
        tr("All files (*)")});
    m_parameterRecoveryPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, service, dialog, revision](int result) {
        if (!service || m_fileToolsClosing
            || revision != m_parameterRecoveryPromptRevision
            || m_parameterRecoveryService != service) {
            return;
        }
        m_parameterRecoveryPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result != QDialog::Accepted || files.size() != 1) {
            RefreshVehicleActions();
            return;
        }

        const QPointer<ConfigDeveloperToolsView> guard(this);
        ParameterRecoveryService::Plan plan;
        QString error;
        const bool prepared = service->prepare(files.first(), &plan, &error);
        if (!guard || !service || m_fileToolsClosing
            || revision != m_parameterRecoveryPromptRevision
            || m_parameterRecoveryService != service) {
            return;
        }
        if (!prepared || !plan.isValid()) {
            AppendLog(tr("Cannot prepare parameter recovery: %1")
                          .arg(error.isEmpty()
                                   ? tr("the service returned an invalid plan")
                                   : error));
            RefreshVehicleActions();
            return;
        }
        ConfirmParameterRecovery(plan, revision);
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ConfirmParameterRecovery(
    const ParameterRecoveryService::Plan &plan, quint64 revision)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    if (m_fileToolsClosing || !service || !plan.isValid()
        || revision != m_parameterRecoveryPromptRevision
        || m_parameterRecoveryPrompt || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy() || MavFtpDownloadBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }

    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_fileToolsClosing
        || m_parameterRecoveryService != service
        || revision != m_parameterRecoveryPromptRevision
        || m_parameterRecoveryPrompt || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy() || MavFtpDownloadBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    if (!valid) {
        AppendLog(tr("Parameter recovery cancelled before confirmation: %1")
                      .arg(error));
        RefreshVehicleActions();
        return;
    }

    const VehicleEndpoint endpoint = plan.target().endpoint;
    const QString warning = tr(
        "Restore %1 parameter entries from:\n%2\n\n"
        "Exact target: %3\n"
        "link %4, system %5, component %6.\n\n"
        "Writes are ordered deliberately: ENABLE parameters are applied in a "
        "first pass, then all parameters are processed in source-file order. "
        "Each changed *_ID parameter is reset to zero immediately before its "
        "own target value. Identity, serial-port, "
        "or link parameters can interrupt the recovery connection.\n\n"
        "The vehicle must remain disarmed. A failure, cancellation, target "
        "change, or lost link can leave a partial recovery; completed writes are "
        "not rolled back. Continue with this exact file and vehicle?")
        .arg(QString::number(plan.entries().size()),
             plan.sourcePath(), endpoint.displayName(),
             QString::number(endpoint.linkId),
             QString::number(endpoint.systemId),
             QString::number(endpoint.componentId));
    auto *dialog = new QMessageBox(
        QMessageBox::Critical, tr("Confirm Parameter Recovery"), warning,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(
        QStringLiteral("DeveloperParameterRecoveryConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_parameterRecoveryPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, plan, revision](int result) {
        if (!guard || m_fileToolsClosing
            || revision != m_parameterRecoveryPromptRevision
            || m_parameterRecoveryService != service) {
            return;
        }
        m_parameterRecoveryPrompt.clear();
        if (result != QMessageBox::Yes || !service) {
            RefreshVehicleActions();
            return;
        }
        StartParameterRecovery(plan, revision);
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::StartParameterRecovery(
    const ParameterRecoveryService::Plan &plan, quint64 revision)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    if (m_fileToolsClosing || !service || !plan.isValid()
        || revision != m_parameterRecoveryPromptRevision
        || m_parameterRecoveryPrompt || m_ownedParameterRecoveryOperationId
        || m_gpsExtractionState || m_gpsExtractionPrompt || m_splitState
        || m_splitPrompt || m_dashWareState || m_dashWarePrompt
        || ApjEmbeddingBusy() || LogOrganizerBusy() || MavFtpDownloadBusy()
        || m_vehiclePrompt || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }

    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_fileToolsClosing
        || m_parameterRecoveryService != service
        || revision != m_parameterRecoveryPromptRevision
        || m_parameterRecoveryPrompt || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy() || MavFtpDownloadBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    if (!valid) {
        AppendLog(tr("Parameter recovery cancelled: %1").arg(error));
        RefreshVehicleActions();
        return;
    }

    m_ownedParameterRecoveryOperationId = 0;
    const auto result = service->execute(
        plan, &m_ownedParameterRecoveryOperationId, &error);
    if (!guard || !service || m_parameterRecoveryService != service
        || revision != m_parameterRecoveryPromptRevision) {
        return;
    }
    if (result != ParameterRecoveryService::SubmitResult::Started) {
        m_ownedParameterRecoveryOperationId = 0;
        AppendLog(tr("Parameter recovery was not started: %1").arg(error));
        RefreshVehicleActions();
        return;
    }

    const quint64 operationId = m_ownedParameterRecoveryOperationId;
    if (operationId != 0 && service->busy()
        && service->currentOperationId() == operationId) {
        AppendLog(tr("Parameter recovery started for %1 from %2. Completed "
                     "writes are not rolled back on cancellation or failure.")
                      .arg(plan.target().endpoint.displayName(),
                           plan.sourcePath()));
        ShowParameterRecoveryProgress(operationId);
    }
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ShowParameterRecoveryProgress(
    quint64 operationId)
{
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    if (!service || operationId == 0
        || m_ownedParameterRecoveryOperationId != operationId
        || service->currentOperationId() != operationId || !service->busy()) {
        return;
    }
    if (m_parameterRecoveryProgress)
        return;

    const int total = qMax(1, service->progressTotal());
    auto *progress = new QProgressDialog(
        tr("Restoring parameters to the exact selected vehicle…"),
        tr("Cancel Recovery"), 0, total, this);
    progress->setObjectName(
        QStringLiteral("DeveloperParameterRecoveryProgressDialog"));
    progress->setWindowTitle(tr("Parameter Recovery"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(qBound(0, service->progressCompleted(), total));
    m_parameterRecoveryProgress = progress;
    connect(progress, &QProgressDialog::canceled, this,
            [this, operationId]() {
        if (m_ownedParameterRecoveryOperationId == operationId)
            CancelOwnedParameterRecovery();
    });
    progress->show();
}

void ConfigDeveloperToolsView::HandleParameterRecoveryFinished(
    const ParameterRecoveryService::Report &report)
{
    if (report.operationId == 0
        || report.operationId != m_ownedParameterRecoveryOperationId) {
        RefreshVehicleActions();
        return;
    }

    m_ownedParameterRecoveryOperationId = 0;
    const QPointer<QProgressDialog> progress(m_parameterRecoveryProgress);
    m_parameterRecoveryProgress.clear();
    if (progress) {
        const QSignalBlocker blocker(progress);
        progress->cancel();
        progress->deleteLater();
    }

    int enableWrites = 0;
    int identifierResets = 0;
    int parameterWrites = 0;
    for (const ParameterRecoveryService::Receipt &receipt : report.receipts) {
        switch (receipt.kind) {
        case ParameterRecoveryService::Receipt::Kind::EnableWrite:
            ++enableWrites;
            break;
        case ParameterRecoveryService::Receipt::Kind::IdentifierReset:
            ++identifierResets;
            break;
        case ParameterRecoveryService::Receipt::Kind::ParameterWrite:
            ++parameterWrites;
            break;
        }
    }

    QString outcome;
    switch (report.outcome) {
    case ParameterRecoveryService::Outcome::Completed:
        outcome = tr("completed");
        break;
    case ParameterRecoveryService::Outcome::Cancelled:
        outcome = tr("cancelled with a possibly partial result");
        break;
    case ParameterRecoveryService::Outcome::Rejected:
        outcome = tr("rejected");
        break;
    case ParameterRecoveryService::Outcome::OutcomeUncertain:
        outcome = tr("stopped with an uncertain partial outcome");
        break;
    }
    AppendLog(tr("Parameter recovery %1: %2 total entries; %3 set, %4 "
                 "unchanged, %5 failed; %6 source entries completed and %7 "
                 "remain. Confirmed writes: %8 ENABLE, %9 identifier resets "
                 "to zero, %10 parameter values. %11")
                  .arg(outcome)
                  .arg(report.totalEntries)
                  .arg(report.setCount)
                  .arg(report.unchangedCount)
                  .arg(report.failedCount)
                  .arg(report.completedEntries)
                  .arg(report.remainingEntries)
                  .arg(enableWrites)
                  .arg(identifierResets)
                  .arg(parameterWrites)
                  .arg(report.description));
    if (!report.failedParameters.isEmpty()) {
        AppendLog(tr("Parameter recovery failed parameters: %1")
                      .arg(report.failedParameters.join(
                          QStringLiteral(", "))));
    }
    if (report.outcome != ParameterRecoveryService::Outcome::Completed) {
        AppendLog(tr("Parameter recovery did not roll back completed writes. "
                     "Review the exact terminal report before retrying."));
    }
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::RefreshParameterRecoveryActions()
{
    if (m_refreshingParameterRecovery)
        return;
    m_refreshingParameterRecovery = true;

    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<ParameterRecoveryService> service(
        m_parameterRecoveryService);
    if (service) {
        const QStringList history = service->history();
        int overlap = qMin(m_seenParameterRecoveryHistory.size(),
                           history.size());
        while (overlap > 0
               && m_seenParameterRecoveryHistory.mid(
                      m_seenParameterRecoveryHistory.size() - overlap)
                   != history.mid(0, overlap)) {
            --overlap;
        }
        for (int index = overlap; index < history.size(); ++index)
            AppendLog(history.at(index));
        if (!guard)
            return;
        m_seenParameterRecoveryHistory = history;
        const QString status = service->status();
        if (!status.isEmpty() && status != m_seenParameterRecoveryStatus
            && !history.contains(status)) {
            AppendLog(status);
        }
        if (!guard)
            return;
        m_seenParameterRecoveryStatus = status;
    }

    if (!guard)
        return;
    if (m_parameterRecoveryProgress && service
        && m_ownedParameterRecoveryOperationId != 0
        && service->currentOperationId()
            == m_ownedParameterRecoveryOperationId
        && service->busy()) {
        const int total = qMax(1, service->progressTotal());
        m_parameterRecoveryProgress->setRange(0, total);
        m_parameterRecoveryProgress->setValue(
            qBound(0, service->progressCompleted(), total));
    }
    m_refreshingParameterRecovery = false;
}

void ConfigDeveloperToolsView::StartMavFtpDownload()
{
    if (m_fileToolsClosing || !m_mavFtpDownload)
        return;
    if (m_gpsExtractionState || m_gpsExtractionPrompt || m_splitState
        || m_splitPrompt || m_dashWareState || m_dashWarePrompt
        || ApjEmbeddingBusy() || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("MAVFTP download: finish or cancel the current Developer operation first."));
        return;
    }
    m_mavFtpDownload->start();
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
    if (m_refreshingVehicleActions || m_fileToolsClosing)
        return;
    m_refreshingVehicleActions = true;
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    for (auto it = m_vehicleButtons.cbegin(); it != m_vehicleButtons.cend(); ++it) {
        QString reason;
        bool enabled = false;
        if (!service)
            reason = tr("The guarded vehicle tool service is unavailable.");
        else if (m_gpsExtractionState || m_gpsExtractionPrompt
                 || m_splitState || m_splitPrompt
                 || m_dashWareState || m_dashWarePrompt
                 || ApjEmbeddingBusy() || LogOrganizerBusy()
                 || MavFtpDownloadBusy() || ParameterRecoveryBusy())
            reason = tr("Finish or cancel the current offline file operation first.");
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
    RefreshOfflineFileActions();
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
    const QPointer<ConfigDeveloperToolsView> guard(this);
    m_fileToolsClosing = true;
    CancelVehiclePrompt();
    if (!guard)
        return;
    CancelParameterRecoveryPrompt();
    CancelOwnedParameterRecovery();
    if (!guard)
        return;
    if (m_parameterRecoveryProgress) {
        const QPointer<QProgressDialog> progress(
            m_parameterRecoveryProgress);
        m_parameterRecoveryProgress.clear();
        if (progress) {
            const QSignalBlocker blocker(progress);
            progress->cancel();
            progress->deleteLater();
        }
    }
    CancelGpsExtraction();
    CancelSplit();
    CancelDashWareExport();
    CancelApjEmbedding();
    CancelLogOrganizer();
    if (m_mavFtpDownload)
        m_mavFtpDownload->cancel();
    ActionPageView::closeEvent(event);
}

ConfigDeveloperToolsView::~ConfigDeveloperToolsView()
{
    m_fileToolsClosing = true;
    CancelParameterRecoveryPrompt();
    const QPointer<ParameterRecoveryService> recovery(
        m_parameterRecoveryService);
    const quint64 recoveryOperationId =
        m_ownedParameterRecoveryOperationId;
    if (recovery)
        disconnect(recovery, nullptr, this, nullptr);
    if (recovery && recoveryOperationId != 0
        && recovery->currentOperationId() == recoveryOperationId) {
        recovery->cancel(recoveryOperationId);
    }
    m_ownedParameterRecoveryOperationId = 0;
    if (m_mavFtpDownload) {
        disconnect(m_mavFtpDownload, nullptr, this, nullptr);
        delete m_mavFtpDownload;
    }
    ++m_gpsPromptRevision;
    if (m_gpsExtractionState)
        m_gpsExtractionState->cancelled.store(true, std::memory_order_relaxed);
    ++m_splitPromptRevision;
    if (m_splitState)
        m_splitState->cancelled.store(true, std::memory_order_relaxed);
    ++m_dashWarePromptRevision;
    if (m_dashWareState)
        m_dashWareState->cancelled.store(true, std::memory_order_relaxed);
    ++m_apjPromptRevision;
    if (m_apjState)
        m_apjState->cancelled.store(true, std::memory_order_relaxed);
    ++m_logOrganizerRevision;
    if (m_logOrganizerState)
        m_logOrganizerState->cancelled.store(true, std::memory_order_relaxed);
    // The worker owns only copied paths and shared atomic state. Destruction
    // disconnects the watcher; it does not block the GUI waiting for file I/O.
}

void ConfigDeveloperToolsView::showEvent(QShowEvent *event)
{
    m_fileToolsClosing = false;
    if (m_parameterRecoveryService
        && m_ownedParameterRecoveryOperationId != 0
        && m_parameterRecoveryService->currentOperationId()
            == m_ownedParameterRecoveryOperationId
        && m_parameterRecoveryService->busy()) {
        ShowParameterRecoveryProgress(
            m_ownedParameterRecoveryOperationId);
    }
    RefreshOfflineFileActions();
    ActionPageView::showEvent(event);
}

void ConfigDeveloperToolsView::CancelGpsExtraction()
{
    ++m_gpsPromptRevision;
    if (m_gpsExtractionState)
        m_gpsExtractionState->cancelled.store(true, std::memory_order_relaxed);
    const QPointer<QDialog> prompt = m_gpsExtractionPrompt;
    m_gpsExtractionPrompt.clear();
    if (prompt)
        prompt->reject();
    if (m_gpsExtractionProgress)
        m_gpsExtractionProgress->cancel();
}

void ConfigDeveloperToolsView::PickGpsCorrectionInput()
{
    if (m_fileToolsClosing || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy()))
        return;
    const quint64 revision = ++m_gpsPromptRevision;
    auto *dialog = new QFileDialog(this, tr("Select telemetry log"));
    dialog->setObjectName(QStringLiteral("DeveloperGpsInputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilter(tr("Telemetry log (*.tlog)"));
    m_gpsExtractionPrompt = dialog;
    m_gpsExtractionButton->setEnabled(false);
    connect(dialog, &QDialog::finished, this, [this, dialog, revision](int result) {
        if (m_fileToolsClosing || revision != m_gpsPromptRevision)
            return;
        m_gpsExtractionPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            PickGpsCorrectionOutput(files.first(), revision);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickGpsCorrectionOutput(const QString &input, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_gpsPromptRevision)
        return;
    const QFileInfo source(input);
    auto *dialog = new QFileDialog(this, tr("Save GPS correction stream"), source.absolutePath());
    dialog->setObjectName(QStringLiteral("DeveloperGpsOutputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setOption(QFileDialog::DontConfirmOverwrite, false);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter(tr("GPS correction stream (*.dat)"));
    dialog->setDefaultSuffix(QStringLiteral("dat"));
    dialog->selectFile(source.completeBaseName() + QStringLiteral("-corrections.dat"));
    m_gpsExtractionPrompt = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog, input, revision](int result) {
        if (m_fileToolsClosing || revision != m_gpsPromptRevision)
            return;
        m_gpsExtractionPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            ExtractGpsCorrections(input, files.first());
        else
            RefreshVehicleActions();
    });
    dialog->open();
}

void ConfigDeveloperToolsView::RefreshOfflineFileActions()
{
    if (m_fileToolsClosing)
        return;
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const bool idle = !m_gpsExtractionState && !m_gpsExtractionPrompt
        && !m_splitState && !m_splitPrompt
        && !m_dashWareState && !m_dashWarePrompt && !ApjEmbeddingBusy()
        && !LogOrganizerBusy()
        && !MavFtpDownloadBusy() && !ParameterRecoveryBusy()
        && !m_vehiclePrompt
        && (!m_vehicleTools || !m_vehicleTools->busy());
    m_gpsExtractionButton->setEnabled(idle);
    m_splitButton->setEnabled(idle);
    m_dashWareButton->setEnabled(idle);
    m_apjButton->setEnabled(idle);
    m_logOrganizerButton->setEnabled(idle);
    const bool ftpAvailable = m_mavFtpService && m_mavFtpTargets;
    const bool ftpBusy = ftpAvailable && m_mavFtpService->isBusy();
    m_mavFtpButton->setEnabled(idle && ftpAvailable && !ftpBusy);
    m_mavFtpButton->setToolTip(!ftpAvailable
        ? tr("The MAVFTP download service is unavailable.")
        : (!idle || ftpBusy)
            ? tr("Finish or cancel the active Developer or MAVFTP operation first.")
            : tr("Download a remote file by path from the selected vehicle; requires a MAVLink connection."));

    const QPointer<ParameterRecoveryService> recovery(
        m_parameterRecoveryService);
    const quint64 recoveryBindingRevision =
        m_parameterRecoveryBindingRevision;
    QString recoveryReason;
    bool recoveryReady = false;
    if (!recovery) {
        recoveryReason = tr("The guarded parameter recovery service is unavailable.");
    } else if (!idle) {
        recoveryReason = tr("Finish or cancel the active Developer operation first.");
    } else {
        recoveryReady = recovery->canPrepare(&recoveryReason);
    }
    if (!guard || m_fileToolsClosing
        || recovery != m_parameterRecoveryService
        || recoveryBindingRevision != m_parameterRecoveryBindingRevision) {
        return;
    }
    if (recoveryReady
        && (m_gpsExtractionState || m_gpsExtractionPrompt || m_splitState
            || m_splitPrompt || m_dashWareState || m_dashWarePrompt
            || ApjEmbeddingBusy() || LogOrganizerBusy()
            || MavFtpDownloadBusy() || ParameterRecoveryBusy()
            || m_vehiclePrompt
            || (m_vehicleTools && m_vehicleTools->busy()))) {
        recoveryReady = false;
        recoveryReason = tr("Finish or cancel the active Developer operation first.");
    }
    if (!m_restoreParametersButton || !m_cancelParameterRestoreButton)
        return;
    m_restoreParametersButton->setEnabled(recoveryReady);
    m_restoreParametersButton->setToolTip(recoveryReady
        ? tr("Restore a reviewed .param or .parm file to the exact selected disarmed vehicle.")
        : recoveryReason);
    const bool ownsRecovery = recovery
        && m_ownedParameterRecoveryOperationId != 0
        && recovery->busy()
        && recovery->currentOperationId()
            == m_ownedParameterRecoveryOperationId;
    m_cancelParameterRestoreButton->setEnabled(ownsRecovery);
    m_cancelParameterRestoreButton->setToolTip(ownsRecovery
        ? tr("Request cancellation of this page's active parameter recovery. Completed writes are not rolled back.")
        : tr("No parameter recovery started by this page is active."));
    RefreshParameterRecoveryActions();
}

void ConfigDeveloperToolsView::ExtractGpsCorrections(const QString &input, const QString &output)
{
    if (m_fileToolsClosing)
        return;
    if (m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("GPS correction extraction: another extraction, file selection, or vehicle operation is already active."));
        RefreshOfflineFileActions();
        return;
    }
    if (input.isEmpty() || output.isEmpty()) {
        AppendLog(tr("GPS correction extraction: input and output paths are required."));
        RefreshOfflineFileActions();
        return;
    }
    const auto state = std::make_shared<GpsExtractionState>();
    m_gpsExtractionState = state;
    m_gpsExtractionButton->setEnabled(false);
    AppendLog(tr("GPS correction extraction started: %1").arg(input));
    AppendLog(tr("All senders and fragments are concatenated in log order; no RTCM reassembly or validation."));
    AppendLog(tr("Only recorded packets can be extracted. Older Qt logs may omit this station’s "
                 "transmitted corrections; current logs include submitted MAVLink packets while recording is enabled."));
    auto *progress = new QProgressDialog(tr("Extracting recorded GPS correction bytes…"),
        tr("Cancel"), 0, 1000, this);
    progress->setObjectName(QStringLiteral("DeveloperGpsProgressDialog"));
    progress->setWindowTitle(tr("Extract GPS Corrections"));
    // A modeless progress dialog keeps setValue() from pumping nested GUI
    // events. The page's operation gate disables conflicting actions itself.
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_gpsExtractionProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });
    using Result = GpsCorrectionExtractor::Result;
    auto *watcher = new QFutureWatcher<Result>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this, [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress || m_gpsExtractionState != state ||
            state->cancelled.load(std::memory_order_relaxed))
            return;
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0)
            guardedProgress->setValue(int(qBound(0.0L, 1000.0L * done / total, 1000.0L)));
    });
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, state, watcher, timer, guardedProgress, output]() {
        timer->stop();
        const Result result = watcher->result();
        watcher->deleteLater();
        if (m_gpsExtractionState != state)
            return;
        m_gpsExtractionState.reset();
        m_gpsExtractionProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (m_fileToolsClosing)
            return;
        if (result.cancelled)
            AppendLog(tr("GPS correction extraction cancelled; no output was published."));
        else if (!result.success)
            AppendLog(tr("GPS correction extraction failed: %1").arg(result.error));
        else if (result.messagesWritten == 0)
            AppendLog(tr("GPS correction extraction completed: no GPS correction messages found; "
                         "an empty stream was written to %1.").arg(output));
        else
            AppendLog(tr("GPS correction extraction completed: %1 messages, %2 bytes written to %3.")
                          .arg(result.messagesWritten).arg(result.bytesWritten).arg(output));
        if (result.skippedBytes || result.rejectedFrames || result.truncatedTail)
            AppendLog(tr("GPS correction extraction warning: %1 skipped bytes, %2 rejected frames, "
                         "truncated tail: %3. Output may be incomplete; this is not RTCM fragment reconstruction.")
                          .arg(result.skippedBytes).arg(result.rejectedFrames)
                          .arg(result.truncatedTail ? tr("yes") : tr("no")));
        RefreshVehicleActions();
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run([input, output, state]() {
        try {
            return GpsCorrectionExtractor::Extract(input, output,
                [state]() { return state->cancelled.load(std::memory_order_relaxed); },
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed, std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Result result;
            result.error = QString::fromUtf8(error.what());
            return result;
        } catch (...) {
            Result result;
            result.error = QStringLiteral("Unexpected file extraction error.");
            return result;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::CancelSplit()
{
    ++m_splitPromptRevision;
    if (m_splitState)
        m_splitState->cancelled.store(true, std::memory_order_relaxed);
    const QPointer<QDialog> prompt = m_splitPrompt;
    m_splitPrompt.clear();
    if (prompt)
        prompt->reject();
    if (m_splitProgress)
        m_splitProgress->cancel();
}

void ConfigDeveloperToolsView::PickSplitInput()
{
    if (m_fileToolsClosing || m_splitState || m_splitPrompt
        || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    const quint64 revision = ++m_splitPromptRevision;
    auto *dialog = new QFileDialog(this, tr("Select DataFlash log"));
    dialog->setObjectName(QStringLiteral("DeveloperSplitInputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilter(tr("DataFlash logs (*.bin *.BIN *.log *.LOG)"));
    m_splitPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_fileToolsClosing || revision != m_splitPromptRevision)
            return;
        m_splitPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            PickSplitCount(files.first(), revision);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickSplitCount(const QString &input,
                                               quint64 revision)
{
    if (m_fileToolsClosing || revision != m_splitPromptRevision)
        return;
    auto *dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("DeveloperSplitCountDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Split DataFlash Log"));
    dialog->setLabelText(tr("Number of output parts (2 to 1000)"));
    dialog->setInputMode(QInputDialog::IntInput);
    dialog->setIntRange(2, 1000);
    dialog->setIntValue(10);
    dialog->setIntStep(1);
    m_splitPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, input, revision](int result) {
        if (m_fileToolsClosing || revision != m_splitPromptRevision)
            return;
        m_splitPrompt.clear();
        if (result == QDialog::Accepted)
            ConfirmSplit(input, dialog->intValue(), revision);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ConfirmSplit(const QString &input, int pieces,
                                             quint64 revision)
{
    if (m_fileToolsClosing || revision != m_splitPromptRevision)
        return;
    const QStringList outputs = DataFlashLogSplitter::OutputPaths(input, pieces);
    QString outputDescription;
    if (!outputs.isEmpty()) {
        outputDescription = outputs.size() <= 8
            ? outputs.join(QLatin1Char('\n'))
            : tr("%1\n%2\n…\n%3\n(%4 output files beside the input)")
                  .arg(outputs.at(0), outputs.at(1), outputs.constLast())
                  .arg(outputs.size());
    }
    const QString warning = tr(
        "Split this DataFlash log into %1 independently readable parts?\n\n"
        "Outputs:\n%2\n\n"
        "Complete records and required format metadata are copied into each part. "
        "Existing output files are never overwritten. All parts are staged before "
        "publication, and cancellation is honored before publication starts. "
        "Publishing several files is not group-atomic: a late rename failure can "
        "leave earlier listed outputs published.")
        .arg(pieces).arg(outputDescription);
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Confirm DataFlash log split"), warning,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("DeveloperSplitConfirmDialog"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_splitPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, input, pieces, revision](int result) {
        if (m_fileToolsClosing || revision != m_splitPromptRevision)
            return;
        m_splitPrompt.clear();
        if (result == QMessageBox::Yes)
            SplitDataFlashLog(input, pieces);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::SplitDataFlashLog(const QString &input,
                                                  int pieces)
{
    if (m_fileToolsClosing)
        return;
    if (m_splitState || m_splitPrompt || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_dashWareState || m_dashWarePrompt
        || ApjEmbeddingBusy() || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy() || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("DataFlash log split: another file selection, offline operation, or vehicle operation is already active."));
        RefreshOfflineFileActions();
        return;
    }
    if (input.trimmed().isEmpty() || pieces < 2 || pieces > 1000) {
        AppendLog(tr("DataFlash log split: select a .bin or .log input and 2 to 1000 parts."));
        RefreshOfflineFileActions();
        return;
    }

    const auto state = std::make_shared<SplitState>();
    m_splitState = state;
    m_splitButton->setEnabled(false);
    AppendLog(tr("DataFlash log split started: %1 into %2 parts.")
                  .arg(input).arg(pieces));
    AppendLog(tr("Outputs are staged before publication and never overwrite existing files; multi-file publication is not group-atomic."));

    auto *progress = new QProgressDialog(
        tr("Splitting complete DataFlash records…"), tr("Cancel"),
        0, 1000, this);
    progress->setObjectName(QStringLiteral("DeveloperSplitProgressDialog"));
    progress->setWindowTitle(tr("Split DataFlash Log"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_splitProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });

    using Result = DataFlashLogSplitter::Result;
    auto *watcher = new QFutureWatcher<Result>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this,
            [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress
            || m_splitState != state
            || state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0) {
            guardedProgress->setValue(int(qBound(
                0.0L, 1000.0L * done / total, 1000.0L)));
        }
    });
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, state, watcher, timer, guardedProgress, input, pieces]() {
        timer->stop();
        const Result result = watcher->result();
        watcher->deleteLater();
        if (m_splitState != state)
            return;
        m_splitState.reset();
        m_splitProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (m_fileToolsClosing)
            return;

        if (result.cancelled) {
            AppendLog(result.outputs.isEmpty()
                ? tr("DataFlash log split cancelled before publication; no output was published.")
                : tr("DataFlash log split cancelled after publishing: %1")
                      .arg(result.outputs.join(QStringLiteral(", "))));
        } else if (!result.success) {
            AppendLog(tr("DataFlash log split failed: %1").arg(result.error));
            if (!result.outputs.isEmpty()) {
                AppendLog(tr("Already published outputs (multi-file publication is not group-atomic):\n%1")
                              .arg(result.outputs.join(QLatin1Char('\n'))));
            }
        } else {
            AppendLog(tr("DataFlash log split completed: %1 files, %2 records (%3 data records), %4 bytes published from %5.")
                          .arg(result.outputs.size()).arg(result.recordsRead)
                          .arg(result.dataRecords).arg(result.bytesWritten)
                          .arg(input));
            if (result.outputs.size() != pieces) {
                AppendLog(tr("DataFlash log split warning: requested %1 parts but %2 paths were reported.")
                              .arg(pieces).arg(result.outputs.size()));
            }
        }
        for (const QString &warning : result.warnings)
            AppendLog(tr("DataFlash log split warning: %1").arg(warning));
        RefreshVehicleActions();
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run([input, pieces, state]() {
        try {
            return DataFlashLogSplitter::Split(
                input, pieces,
                [state]() {
                    return state->cancelled.load(std::memory_order_relaxed);
                },
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed,
                                           std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Result result;
            result.error = QString::fromUtf8(error.what());
            return result;
        } catch (...) {
            Result result;
            result.error = QStringLiteral("Unexpected DataFlash split error.");
            return result;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::CancelDashWareExport()
{
    ++m_dashWarePromptRevision;
    if (m_dashWareState)
        m_dashWareState->cancelled.store(true, std::memory_order_relaxed);
    const QPointer<QDialog> prompt = m_dashWarePrompt;
    m_dashWarePrompt.clear();
    if (prompt)
        prompt->reject();
    if (m_dashWareProgress)
        m_dashWareProgress->cancel();
}

void ConfigDeveloperToolsView::PickDashWareInput()
{
    if (m_fileToolsClosing || m_dashWareState || m_dashWarePrompt
        || ApjEmbeddingBusy() || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy()
        || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    const quint64 revision = ++m_dashWarePromptRevision;
    auto *dialog = new QFileDialog(this, tr("Select DataFlash log"));
    dialog->setObjectName(QStringLiteral("DeveloperDashWareInputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilter(tr("DataFlash logs (*.bin *.BIN *.log *.LOG)"));
    m_dashWarePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_fileToolsClosing || revision != m_dashWarePromptRevision)
            return;
        m_dashWarePrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            PickDashWareTypes(files.first(), revision);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickDashWareTypes(const QString &input,
                                                  quint64 revision)
{
    if (m_fileToolsClosing || revision != m_dashWarePromptRevision)
        return;
    auto *dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("DeveloperDashWareTypesDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Create DashWare CSV"));
    dialog->setLabelText(tr("Message types separated by semicolons (empty includes all declared types)"));
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setTextValue(QStringLiteral("GPS;ATT;NTUN;CTUN;MODE;BAT"));
    m_dashWarePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, input, revision](int result) {
        if (m_fileToolsClosing || revision != m_dashWarePromptRevision)
            return;
        m_dashWarePrompt.clear();
        if (result == QDialog::Accepted) {
            const QStringList types = normalizedDashWareTypes(
                dialog->textValue().split(QLatin1Char(';'),
                                          Qt::KeepEmptyParts));
            PickDashWareOutput(input, types, revision);
        } else {
            RefreshVehicleActions();
        }
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickDashWareOutput(
    const QString &input, const QStringList &types, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_dashWarePromptRevision)
        return;
    const QFileInfo source(input);
    auto *dialog = new QFileDialog(
        this, tr("Save DashWare CSV"), source.absolutePath());
    dialog->setObjectName(QStringLiteral("DeveloperDashWareOutputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setOption(QFileDialog::DontConfirmOverwrite, false);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter(tr("CSV files (*.csv)"));
    dialog->setDefaultSuffix(QStringLiteral("csv"));
    dialog->selectFile(source.completeBaseName()
                       + QStringLiteral("-dashware.csv"));
    m_dashWarePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, input, types, revision](int result) {
        if (m_fileToolsClosing || revision != m_dashWarePromptRevision)
            return;
        m_dashWarePrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            ExportDashWareCsv(input, files.first(), types);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ExportDashWareCsv(
    QString input, QString output, QStringList types)
{
    if (m_fileToolsClosing)
        return;
    if (m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy()
        || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_vehiclePrompt || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("DashWare CSV export: another file selection, offline operation, or vehicle operation is already active."));
        RefreshOfflineFileActions();
        return;
    }
    if (input.trimmed().isEmpty() || output.trimmed().isEmpty()) {
        AppendLog(tr("DashWare CSV export: input and output paths are required."));
        RefreshOfflineFileActions();
        return;
    }

    const QStringList selectedTypes = normalizedDashWareTypes(types);
    const auto state = std::make_shared<DashWareState>();
    m_dashWareState = state;
    m_dashWareButton->setEnabled(false);
    AppendLog(tr("DashWare CSV export started: %1 to %2 (%3).")
                  .arg(input, output,
                       selectedTypes.isEmpty()
                           ? tr("all declared message types")
                           : selectedTypes.join(QLatin1Char(';'))));

    auto *progress = new QProgressDialog(
        tr("Exporting DataFlash rows to DashWare CSV…"), tr("Cancel"),
        0, 1000, this);
    progress->setObjectName(QStringLiteral("DeveloperDashWareProgressDialog"));
    progress->setWindowTitle(tr("Create DashWare CSV"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_dashWareProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });

    using Result = DataFlashDashWareCsvExporter::Result;
    auto *watcher = new QFutureWatcher<Result>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this,
            [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress
            || m_dashWareState != state
            || state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0) {
            guardedProgress->setValue(int(qBound(
                0.0L, 1000.0L * done / total, 1000.0L)));
        }
    });
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, state, watcher, timer, guardedProgress, output]() {
        timer->stop();
        const Result result = watcher->result();
        watcher->deleteLater();
        if (m_dashWareState != state)
            return;
        m_dashWareState.reset();
        m_dashWareProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (m_fileToolsClosing)
            return;

        if (result.cancelled) {
            AppendLog(tr("DashWare CSV export cancelled; no output was published."));
        } else if (!result.success) {
            AppendLog(tr("DashWare CSV export failed: %1").arg(result.error));
        } else {
            AppendLog(tr("DashWare CSV export completed: %1 rows, %2 columns, %3 bytes written to %4.")
                          .arg(result.rowsWritten).arg(result.columns)
                          .arg(result.bytesWritten).arg(output));
        }
        for (const QString &warning : result.warnings)
            AppendLog(tr("DashWare CSV export warning: %1").arg(warning));
        RefreshVehicleActions();
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run(
        [input, output, selectedTypes, state]() {
        try {
            return DataFlashDashWareCsvExporter::Export(
                input, output, selectedTypes,
                [state]() {
                    return state->cancelled.load(std::memory_order_relaxed);
                },
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed,
                                           std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Result result;
            result.error = QString::fromUtf8(error.what());
            return result;
        } catch (...) {
            Result result;
            result.error = QStringLiteral("Unexpected DashWare CSV export error.");
            return result;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::CancelApjEmbedding()
{
    ++m_apjPromptRevision;
    if (m_apjState)
        m_apjState->cancelled.store(true, std::memory_order_relaxed);
    const QPointer<QDialog> prompt = m_apjPrompt;
    m_apjPrompt.clear();
    if (prompt)
        prompt->reject();
    if (m_apjProgress)
        m_apjProgress->cancel();
}

void ConfigDeveloperToolsView::PickApjFirmware()
{
    if (m_fileToolsClosing || ApjEmbeddingBusy() || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy()
        || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    const quint64 revision = ++m_apjPromptRevision;
    auto *dialog = new QFileDialog(this, tr("Select APJ firmware"));
    dialog->setObjectName(QStringLiteral("DeveloperApjFirmwareDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({tr("ArduPilot firmware (*.apj *.APJ)"),
                            tr("All files (*)")});
    m_apjPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_fileToolsClosing || revision != m_apjPromptRevision)
            return;
        m_apjPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result == QDialog::Accepted && files.size() == 1)
            PickApjDefaults(files.first(), revision);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::PickApjDefaults(
    const QString &firmware, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_apjPromptRevision)
        return;
    const QFileInfo source(firmware);
    auto *dialog = new QFileDialog(
        this, tr("Select parameter defaults"), source.absolutePath());
    dialog->setObjectName(QStringLiteral("DeveloperApjDefaultsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({tr(
        "Parameter defaults (*.param *.parm *.PARAM *.PARM)"),
        tr("All files (*)")});
    m_apjPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, firmware, revision](int result) {
        if (m_fileToolsClosing || revision != m_apjPromptRevision)
            return;
        m_apjPrompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result != QDialog::Accepted || files.size() != 1) {
            RefreshVehicleActions();
            return;
        }
        const QString parameters = files.first();
        if (QFileInfo::exists(
                ApjDefaultsEmbedder::SuggestedOutputPath(firmware))) {
            ConfirmApjOverwrite(firmware, parameters, revision);
        } else {
            EmbedDefaultsInApj(firmware, parameters, false);
        }
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ConfirmApjOverwrite(
    const QString &firmware, const QString &parameters, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_apjPromptRevision)
        return;
    const QString output = ApjDefaultsEmbedder::SuggestedOutputPath(firmware);
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Replace embedded-defaults output"),
        tr("The output already exists:\n\n%1\n\n"
           "Replace this local APJ file with a newly generated image? "
           "The source firmware is preserved. The generated file is not uploaded or flashed.")
            .arg(output),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("DeveloperApjOverwriteConfirmDialog"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_apjPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, firmware, parameters, revision](int result) {
        if (m_fileToolsClosing || revision != m_apjPromptRevision)
            return;
        m_apjPrompt.clear();
        if (result == QMessageBox::Yes)
            EmbedDefaultsInApj(firmware, parameters, true);
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::EmbedDefaultsInApj(
    QString firmware, QString parameters, bool overwriteExisting)
{
    if (m_fileToolsClosing)
        return;
    if (ApjEmbeddingBusy() || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy()
        || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("APJ defaults embedding: another file selection, offline operation, or vehicle operation is already active."));
        RefreshOfflineFileActions();
        return;
    }
    if (firmware.trimmed().isEmpty() || parameters.trimmed().isEmpty()) {
        AppendLog(tr("APJ defaults embedding: firmware and parameter-defaults paths are required."));
        RefreshOfflineFileActions();
        return;
    }

    const QString output = ApjDefaultsEmbedder::SuggestedOutputPath(firmware);
    const auto state = std::make_shared<ApjEmbeddingState>();
    m_apjState = state;
    m_apjButton->setEnabled(false);
    AppendLog(tr("APJ defaults embedding started: %1 + %2 -> %3.")
                  .arg(firmware, parameters, output));
    AppendLog(tr("This is a local file transformation only; the generated APJ is not uploaded or flashed."));

    auto *progress = new QProgressDialog(
        tr("Embedding parameter defaults in APJ firmware…"), tr("Cancel"),
        0, 1000, this);
    progress->setObjectName(QStringLiteral("DeveloperApjProgressDialog"));
    progress->setWindowTitle(tr("Embed Defaults in APJ"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_apjProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });

    using Result = ApjDefaultsEmbedder::Result;
    auto *watcher = new QFutureWatcher<Result>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this,
            [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress
            || m_apjState != state
            || state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0) {
            guardedProgress->setValue(int(qBound(
                0.0L, 1000.0L * done / total, 1000.0L)));
        }
    });
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, state, watcher, timer, guardedProgress, output]() {
        timer->stop();
        const Result result = watcher->result();
        watcher->deleteLater();
        if (m_apjState != state)
            return;
        m_apjState.reset();
        m_apjProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (m_fileToolsClosing)
            return;

        if (result.cancelled) {
            AppendLog(tr("APJ defaults embedding cancelled; no new output was published."));
        } else if (!result.success) {
            AppendLog(tr("APJ defaults embedding failed: %1").arg(result.error));
        } else {
            AppendLog(tr("APJ defaults embedding completed: %1 parameter bytes embedded (capacity %2), %3 image bytes, %4 output bytes written to %5.")
                          .arg(result.defaultsBytes)
                          .arg(result.maximumDefaultsBytes)
                          .arg(result.imageBytes)
                          .arg(result.outputBytes)
                          .arg(result.outputPath.isEmpty()
                                   ? output : result.outputPath));
            if (result.repairedDescriptors > 0) {
                AppendLog(tr("APJ defaults embedding recalculated both CRC values in %1 unsigned firmware descriptor(s).")
                              .arg(result.repairedDescriptors));
            }
        }
        for (const QString &warning : result.warnings)
            AppendLog(tr("APJ defaults embedding warning: %1").arg(warning));
        RefreshVehicleActions();
    });
    timer->start();
    const ApjDefaultsEmbedder::Options options{overwriteExisting};
    watcher->setFuture(QtConcurrent::run(
        [firmware, parameters, options, state]() {
        try {
            return ApjDefaultsEmbedder::Embed(
                firmware, parameters, options,
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed,
                                           std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                },
                [state]() {
                    return state->cancelled.load(std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Result result;
            result.error = QString::fromUtf8(error.what());
            return result;
        } catch (...) {
            Result result;
            result.error = QStringLiteral("Unexpected APJ defaults embedding error.");
            return result;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::CancelLogOrganizer()
{
    const bool retainExecutionReport =
        m_logOrganizerState && m_logOrganizerState->executing;
    if (!retainExecutionReport)
        ++m_logOrganizerRevision;
    if (m_logOrganizerState)
        m_logOrganizerState->cancelled.store(true, std::memory_order_relaxed);
    const QPointer<QDialog> prompt = m_logOrganizerPrompt;
    const QPointer<QProgressDialog> progress = m_logOrganizerProgress;
    m_logOrganizerPrompt.clear();
    if (prompt) {
        const QSignalBlocker blocker(prompt);
        prompt->reject();
    }
    if (progress) {
        const QSignalBlocker blocker(progress);
        progress->cancel();
    }
}

void ConfigDeveloperToolsView::PickLogOrganizerDirectory()
{
    if (m_fileToolsClosing || LogOrganizerBusy() || ApjEmbeddingBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy()
        || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        return;
    }
    const quint64 revision = ++m_logOrganizerRevision;
    auto *dialog = new QFileDialog(this, tr("Select log directory"));
    dialog->setObjectName(
        QStringLiteral("DeveloperLogOrganizerDirectoryDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::Directory);
    // Qt 5 resets ShowDirsOnly while changing fileMode.
    dialog->setOption(QFileDialog::ShowDirsOnly);
    m_logOrganizerPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_fileToolsClosing || revision != m_logOrganizerRevision)
            return;
        m_logOrganizerPrompt.clear();
        const QStringList directories = dialog->selectedFiles();
        if (result == QDialog::Accepted && directories.size() == 1)
            AnalyzeLogDirectory(directories.first());
        else
            RefreshVehicleActions();
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::AnalyzeLogDirectory(QString root)
{
    if (m_fileToolsClosing)
        return;
    if (LogOrganizerBusy() || ApjEmbeddingBusy() || MavFtpDownloadBusy()
        || ParameterRecoveryBusy()
        || m_gpsExtractionState || m_gpsExtractionPrompt
        || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || m_vehiclePrompt
        || (m_vehicleTools && m_vehicleTools->busy())) {
        AppendLog(tr("Log directory organizer: another file selection, offline operation, or vehicle operation is already active."));
        RefreshOfflineFileActions();
        return;
    }
    if (root.trimmed().isEmpty()) {
        AppendLog(tr("Log directory organizer: select a directory to analyze."));
        RefreshOfflineFileActions();
        return;
    }

    const quint64 revision = ++m_logOrganizerRevision;
    const auto state = std::make_shared<LogOrganizerState>();
    m_logOrganizerState = state;
    AppendLog(tr("Log directory analysis started: %1").arg(root));
    AppendLog(tr("Analysis is read-only. No files are moved or deleted before the exact plan is reviewed and explicitly executed."));

    auto *progress = new QProgressDialog(
        tr("Analyzing the log directory…"), tr("Cancel"), 0, 1000, this);
    progress->setObjectName(
        QStringLiteral("DeveloperLogOrganizerProgressDialog"));
    progress->setWindowTitle(tr("Organize Log Directory"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_logOrganizerProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });
    progress->show();

    using Analysis = FlightLogOrganizer::Analysis;
    auto *watcher = new QFutureWatcher<Analysis>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this,
            [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress
            || m_logOrganizerState != state
            || state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0) {
            guardedProgress->setValue(int(qBound(
                0.0L, 1000.0L * done / total, 1000.0L)));
        }
    });
    connect(watcher, &QFutureWatcher<Analysis>::finished, this,
            [this, state, watcher, timer, guardedProgress, revision]() {
        timer->stop();
        const Analysis analysis = watcher->result();
        watcher->deleteLater();
        if (m_logOrganizerState != state)
            return;
        m_logOrganizerState.reset();
        m_logOrganizerProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (m_fileToolsClosing || revision != m_logOrganizerRevision)
            return;

        if (analysis.cancelled) {
            AppendLog(tr("Log directory analysis cancelled; no files were changed."));
            RefreshVehicleActions();
            return;
        }
        if (!analysis.success || !analysis.plan.isValid()) {
            AppendLog(tr("Log directory analysis failed: %1")
                          .arg(analysis.error.isEmpty()
                                   ? tr("the organizer returned an invalid plan")
                                   : analysis.error));
            RefreshVehicleActions();
            return;
        }
        AppendLog(tr("Log directory analysis completed: %1 candidate files, %2 planned filesystem changes under %3.")
                      .arg(analysis.plan.candidateCount())
                      .arg(analysis.plan.entries().size())
                      .arg(analysis.plan.root()));
        for (const QString &warning : analysis.plan.warnings())
            AppendLog(tr("Log directory analysis warning: %1").arg(warning));
        if (analysis.plan.entries().isEmpty()) {
            AppendLog(tr("No changes were planned; the directory was left unchanged. Review any analysis warnings above."));
            RefreshVehicleActions();
            return;
        }
        ShowLogOrganizerPlan(analysis.plan, revision);
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run([root, state]() {
        try {
            return FlightLogOrganizer::Analyze(
                root,
                [state]() {
                    return state->cancelled.load(std::memory_order_relaxed);
                },
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed,
                                           std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Analysis analysis;
            analysis.error = QString::fromUtf8(error.what());
            return analysis;
        } catch (...) {
            Analysis analysis;
            analysis.error = QStringLiteral(
                "Unexpected log directory analysis error.");
            return analysis;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ShowLogOrganizerPlan(
    FlightLogOrganizer::Plan plan, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_logOrganizerRevision
        || LogOrganizerBusy() || !plan.isValid()) {
        if (!m_fileToolsClosing)
            RefreshVehicleActions();
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("DeveloperLogOrganizerPlanDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Review Log Organization Plan"));
    dialog->setWindowModality(Qt::NonModal);
    dialog->resize(940, 540);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(
        tr("Root: %1\nCandidates examined: %2\nPlanned changes: %3\n\n"
           "Review every exact path below. Stop recording and close all log writers before executing: active writers are not detected or locked. "
           "Empty log deletion is permanent, and moves never overwrite an existing destination. "
           "Corrected content classification can relocate logs that an older Mission Planner organization pass had already sorted.")
            .arg(plan.root())
            .arg(plan.candidateCount())
            .arg(plan.entries().size()),
        dialog);
    summary->setObjectName(
        QStringLiteral("DeveloperLogOrganizerPlanSummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *tree = new QTreeWidget(dialog);
    tree->setObjectName(QStringLiteral("DeveloperLogOrganizerPlanTree"));
    tree->setColumnCount(4);
    tree->setHeaderLabels({tr("Operation"), tr("Source (relative to root)"),
                           tr("Destination (relative to root)"), tr("Bytes")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    for (const FlightLogOrganizer::Entry &entry : plan.entries()) {
        const bool deletion =
            entry.operation == FlightLogOrganizer::Operation::DeleteEmpty;
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, deletion ? tr("Permanently delete empty log")
                                  : tr("Move"));
        item->setText(1, QDir(plan.root()).relativeFilePath(entry.source));
        item->setText(2, deletion ? tr("(deleted)")
            : QDir(plan.root()).relativeFilePath(entry.destination));
        item->setData(1, Qt::UserRole, entry.source);
        item->setData(2, Qt::UserRole, entry.destination);
        item->setText(3, QString::number(entry.bytes));
        item->setToolTip(1, entry.source);
        item->setToolTip(2, deletion ? tr("This zero-byte log will be deleted permanently.")
                                     : entry.destination);
    }
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(tree, 1);

    if (!plan.warnings().isEmpty()) {
        auto *warnings = new QPlainTextEdit(dialog);
        warnings->setObjectName(
            QStringLiteral("DeveloperLogOrganizerPlanWarnings"));
        warnings->setReadOnly(true);
        warnings->setMaximumHeight(120);
        warnings->setPlainText(
            tr("Warnings:\n%1").arg(plan.warnings().join(QLatin1Char('\n'))));
        layout->addWidget(warnings);
    }

    auto *buttons = new QDialogButtonBox(dialog);
    auto *execute = buttons->addButton(tr("Execute Plan"),
                                       QDialogButtonBox::AcceptRole);
    auto *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    execute->setObjectName(
        QStringLiteral("DeveloperLogOrganizerExecuteButton"));
    cancel->setObjectName(
        QStringLiteral("DeveloperLogOrganizerCancelButton"));
    execute->setEnabled(plan.isValid() && !plan.entries().isEmpty());
    execute->setDefault(false);
    execute->setAutoDefault(false);
    cancel->setDefault(true);
    cancel->setAutoDefault(true);
    connect(execute, &QPushButton::clicked, dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    m_logOrganizerPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, plan, revision](int result) {
        if (m_fileToolsClosing || revision != m_logOrganizerRevision)
            return;
        m_logOrganizerPrompt.clear();
        if (result == QDialog::Accepted)
            ExecuteLogOrganizerPlan(plan, revision);
        else {
            AppendLog(tr("Log organization plan cancelled; no planned changes were executed."));
            RefreshVehicleActions();
        }
    });
    dialog->show();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ExecuteLogOrganizerPlan(
    FlightLogOrganizer::Plan plan, quint64 revision)
{
    if (m_fileToolsClosing || revision != m_logOrganizerRevision
        || LogOrganizerBusy() || !plan.isValid()
        || plan.entries().isEmpty()) {
        RefreshVehicleActions();
        return;
    }

    const auto state = std::make_shared<LogOrganizerState>();
    state->executing = true;
    m_logOrganizerState = state;
    AppendLog(tr("Executing %1 explicitly confirmed log organization changes under %2. Empty-log deletions are permanent; completed changes are not rolled back on a later failure or cancellation.")
                  .arg(plan.entries().size()).arg(plan.root()));
    auto *progress = new QProgressDialog(
        tr("Executing the confirmed log organization plan…"), tr("Cancel"),
        0, 1000, this);
    progress->setObjectName(
        QStringLiteral("DeveloperLogOrganizerProgressDialog"));
    progress->setWindowTitle(tr("Organize Log Directory"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_logOrganizerProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [state]() {
        state->cancelled.store(true, std::memory_order_relaxed);
    });
    progress->show();

    using Result = FlightLogOrganizer::Result;
    auto *watcher = new QFutureWatcher<Result>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(100);
    const QPointer<QProgressDialog> guardedProgress(progress);
    connect(timer, &QTimer::timeout, this,
            [this, state, guardedProgress]() {
        if (m_fileToolsClosing || !guardedProgress
            || m_logOrganizerState != state
            || state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        const qint64 total = state->total.load(std::memory_order_relaxed);
        const qint64 done = state->processed.load(std::memory_order_relaxed);
        if (total > 0) {
            guardedProgress->setValue(int(qBound(
                0.0L, 1000.0L * done / total, 1000.0L)));
        }
    });
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, state, watcher, timer, guardedProgress, revision]() {
        timer->stop();
        const Result result = watcher->result();
        watcher->deleteLater();
        if (m_logOrganizerState != state)
            return;
        m_logOrganizerState.reset();
        m_logOrganizerProgress.clear();
        if (guardedProgress)
            guardedProgress->deleteLater();
        if (revision != m_logOrganizerRevision)
            return;

        for (const FlightLogOrganizer::Entry &entry : result.completed) {
            if (entry.operation == FlightLogOrganizer::Operation::DeleteEmpty) {
                AppendLog(tr("Permanently deleted planned zero-byte log: %1")
                              .arg(entry.source));
            } else {
                AppendLog(tr("Moved planned log file: %1 -> %2 (%3 bytes)")
                              .arg(entry.source, entry.destination)
                              .arg(entry.bytes));
            }
        }
        for (const QString &warning : result.warnings)
            AppendLog(tr("Log organizer warning: %1").arg(warning));
        if (result.cancelled) {
            AppendLog(tr("Log organization cancelled after %1 completed changes; %2 planned changes remain. Completed moves or deletions were not rolled back.")
                          .arg(result.completed.size()).arg(result.remaining));
        } else if (!result.success) {
            AppendLog(tr("Log organization failed after %1 completed changes; %2 planned changes remain: %3. Completed moves or deletions were not rolled back.")
                          .arg(result.completed.size()).arg(result.remaining)
                          .arg(result.error));
        } else {
            AppendLog(tr("Log organization completed: %1 changes applied; %2 remain.")
                          .arg(result.completed.size()).arg(result.remaining));
        }
        if (!m_fileToolsClosing)
            RefreshVehicleActions();
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run([plan, state]() {
        try {
            return FlightLogOrganizer::Execute(
                plan,
                [state]() {
                    return state->cancelled.load(std::memory_order_relaxed);
                },
                [state](qint64 processed, qint64 total) {
                    state->processed.store(processed,
                                           std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                });
        } catch (const std::exception &error) {
            Result result;
            result.error = QString::fromUtf8(error.what());
            result.remaining = plan.entries().size();
            return result;
        } catch (...) {
            Result result;
            result.error = QStringLiteral(
                "Unexpected log organization execution error.");
            result.remaining = plan.entries().size();
            return result;
        }
    }));
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::StartVehicleAction(VehicleAction action)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    if (m_fileToolsClosing || !service || m_vehiclePrompt || m_gpsExtractionState
        || m_gpsExtractionPrompt || m_splitState || m_splitPrompt
        || m_dashWareState || m_dashWarePrompt || ApjEmbeddingBusy()
        || LogOrganizerBusy()
        || MavFtpDownloadBusy() || ParameterRecoveryBusy())
        return;
    const quint64 revision = ++m_promptRevision;
    VehiclePlan plan;
    QString error;
    const bool prepared = service->prepare(action, &plan, &error);
    if (!guard || !service || m_fileToolsClosing
        || m_vehicleTools != service || revision != m_promptRevision)
        return;
    if (!prepared) {
        AppendLog(tr("Cannot prepare vehicle operation: %1").arg(error));
        RefreshVehicleActions();
        return;
    }
    if (action == VehicleAction::UpgradeBootloader) {
        ConfirmUpgradeBootloaderSource(plan, revision);
        return;
    }
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

void ConfigDeveloperToolsView::ConfirmUpgradeBootloaderSource(
    const VehiclePlan &plan, quint64 revision)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    if (m_fileToolsClosing || !service || revision != m_promptRevision
        || m_vehiclePrompt)
        return;
    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_fileToolsClosing || m_vehicleTools != service
        || revision != m_promptRevision || m_vehiclePrompt) {
        return;
    }
    if (!valid) {
        AppendLog(tr("Bootloader upgrade cancelled: %1").arg(error));
        RefreshVehicleActions();
        return;
    }

    const QString warning =
        tr("Upgrade Bootloader\n"
           "Target: link %1, system %2, component %3 (%4).\n\n"
           "This operation asks the selected ArduPilot vehicle to flash the board-specific bootloader embedded in its currently installed firmware. It does not select or download a bootloader file. Unsupported boards or firmware builds may reject the request.\n\n"
           "Continue to the final flash-safety confirmation?")
            .arg(plan.target.endpoint.linkId)
            .arg(plan.target.endpoint.systemId)
            .arg(plan.target.endpoint.componentId)
            .arg(plan.target.endpoint.linkName);
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Confirm Embedded Bootloader Source"), warning,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(
        QStringLiteral("DeveloperUpgradeBootloaderSourceConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_vehiclePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, plan, revision](int result) {
        if (!guard || m_fileToolsClosing || revision != m_promptRevision
            || m_vehicleTools != service) {
            return;
        }
        m_vehiclePrompt.clear();
        if (result != QMessageBox::Yes || !service) {
            RefreshVehicleActions();
            return;
        }
        ConfirmUpgradeBootloaderFlash(plan, revision);
    });
    dialog->open();
    RefreshVehicleActions();
}

void ConfigDeveloperToolsView::ConfirmUpgradeBootloaderFlash(
    const VehiclePlan &plan, quint64 revision)
{
    const QPointer<ConfigDeveloperToolsView> guard(this);
    const QPointer<DeveloperVehicleToolService> service(m_vehicleTools);
    if (m_fileToolsClosing || !service || revision != m_promptRevision
        || m_vehiclePrompt)
        return;
    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_fileToolsClosing || m_vehicleTools != service
        || revision != m_promptRevision || m_vehiclePrompt) {
        return;
    }
    if (!valid) {
        AppendLog(tr("Bootloader upgrade cancelled: %1").arg(error));
        RefreshVehicleActions();
        return;
    }

    const QString warning =
        tr("FINAL BOOTLOADER FLASH WARNING\n"
           "Target: link %1, system %2, component %3 (%4).\n\n"
           "This permanently writes the vehicle's bootloader flash. Power loss or interruption can brick the flight controller. Keep stable power and the data connection connected for at least five minutes. This tool never retries the flash command automatically.\n\n"
           "Heartbeats and telemetry may pause for several seconds during flashing, and the application may show a lost-link indication. Keep power and the data link connected.\n\n"
           "An accepted response can mean the bootloader was updated or was already current. A missing acknowledgement leaves the outcome uncertain; do not automatically retry or power-cycle the vehicle. Execute this one bootloader flash request now?")
            .arg(plan.target.endpoint.linkId)
            .arg(plan.target.endpoint.systemId)
            .arg(plan.target.endpoint.componentId)
            .arg(plan.target.endpoint.linkName);
    auto *dialog = new QMessageBox(
        QMessageBox::Critical, tr("Final Bootloader Flash Confirmation"),
        warning, QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(
        QStringLiteral("DeveloperUpgradeBootloaderFlashConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_vehiclePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, plan, revision](int result) {
        if (!guard || m_fileToolsClosing || revision != m_promptRevision
            || m_vehicleTools != service) {
            return;
        }
        m_vehiclePrompt.clear();
        if (result != QMessageBox::Yes || !service) {
            RefreshVehicleActions();
            return;
        }
        QString error;
        const bool valid = service->validate(plan, &error);
        if (!guard || !service || m_fileToolsClosing
            || m_vehicleTools != service
            || revision != m_promptRevision || m_vehiclePrompt) {
            return;
        }
        if (!valid) {
            AppendLog(tr("Bootloader upgrade cancelled: %1").arg(error));
            RefreshVehicleActions();
            return;
        }
        const auto submitResult = service->execute(plan, 0.0, &error);
        if (!guard || !service || m_vehicleTools != service
            || revision != m_promptRevision) {
            return;
        }
        if (submitResult != DeveloperVehicleToolService::SubmitResult::Started)
            AppendLog(tr("Bootloader upgrade was not started: %1").arg(error));
        RefreshVehicleActions();
    });
    dialog->open();
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
    case VehicleAction::UpgradeBootloader:
        // Preserve the two-stage contract even if a future caller reaches the
        // generic helper directly.
        ConfirmUpgradeBootloaderSource(plan, revision);
        return;
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
