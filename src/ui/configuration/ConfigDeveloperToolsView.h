#ifndef CONFIGDEVELOPERTOOLSVIEW_H
#define CONFIGDEVELOPERTOOLSVIEW_H

#include "ActionPageView.h"
#include "services/DeveloperVehicleToolService.h"
#include "ui/Loghandling/FlightLogOrganizer.h"

#include <QMap>
#include <QPointer>
#include <memory>

class QCloseEvent;
class QDialog;
class QObject;
class QPushButton;
class QProgressDialog;
class QShowEvent;
class MavFtpServiceInterface;
class MavFtpFileDownload;
class VehicleTargetManager;

/** Mission Planner 10 Developer Tools action page. */
class ConfigDeveloperToolsView final : public ActionPageView
{
    Q_OBJECT

public:
    explicit ConfigDeveloperToolsView(QObject *actionSource = nullptr,
                                      QWidget *parent = nullptr);
    ~ConfigDeveloperToolsView() override;

    int ImplementedActionCount() const;
    void setVehicleToolService(DeveloperVehicleToolService *service);
    void setMavFtpDownloadServices(MavFtpServiceInterface *service,
                                   VehicleTargetManager *targets);

public slots:
    void DecodeMavlinkInput(const QString &input);
    void DecodeHardwareIdInput(const QString &input,
                               const QString &parameterName = QString());
    // Paths already selected by the caller; these operations never send telemetry.
    void ExtractGpsCorrections(const QString &input, const QString &output);
    void SplitDataFlashLog(const QString &input, int pieces);
    void ExportDashWareCsv(QString input, QString output, QStringList types);
    void EmbedDefaultsInApj(QString firmware, QString parameters,
                            bool overwriteExisting = false);
    // Starts read-only analysis. Executing the returned immutable plan always
    // requires confirmation in DeveloperLogOrganizerPlanDialog.
    void AnalyzeLogDirectory(QString root);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    using VehicleAction = DeveloperVehicleToolService::Action;
    using VehiclePlan = DeveloperVehicleToolService::Plan;
    void AddVehicleAction(const QString &label, const QString &objectName,
                          VehicleAction action);
    void RefreshVehicleActions();
    void StartVehicleAction(VehicleAction action);
    void ConfirmVehicleAction(const VehiclePlan &plan, double value,
                              quint64 promptRevision);
    void CancelVehiclePrompt();
    QPushButton *AddToolAction(const QString &label,
                               const QString &buttonObjectName,
                               const QString &actionObjectName);
    void DecodePacket();
    void DecodeHardwareId();
    void PickGpsCorrectionInput();
    void PickGpsCorrectionOutput(const QString &input, quint64 revision);
    void CancelGpsExtraction();
    void PickSplitInput();
    void PickSplitCount(const QString &input, quint64 revision);
    void ConfirmSplit(const QString &input, int pieces, quint64 revision);
    void CancelSplit();
    void PickDashWareInput();
    void PickDashWareTypes(const QString &input, quint64 revision);
    void PickDashWareOutput(const QString &input, const QStringList &types,
                            quint64 revision);
    void CancelDashWareExport();
    void PickApjFirmware();
    void PickApjDefaults(const QString &firmware, quint64 revision);
    void ConfirmApjOverwrite(const QString &firmware,
                             const QString &parameters, quint64 revision);
    void CancelApjEmbedding();
    bool ApjEmbeddingBusy() const;
    void PickLogOrganizerDirectory();
    void ShowLogOrganizerPlan(FlightLogOrganizer::Plan plan,
                              quint64 revision);
    void ExecuteLogOrganizerPlan(FlightLogOrganizer::Plan plan,
                                 quint64 revision);
    void CancelLogOrganizer();
    bool LogOrganizerBusy() const;
    void RefreshOfflineFileActions();
    void StartMavFtpDownload();
    bool MavFtpDownloadBusy() const;
    struct GpsExtractionState;
    struct SplitState;
    struct DashWareState;
    struct ApjEmbeddingState;
    struct LogOrganizerState;

    QPointer<QObject> m_actionSource;
    QPointer<DeveloperVehicleToolService> m_vehicleTools;
    QMap<VehicleAction, QPushButton *> m_vehicleButtons;
    QPointer<QDialog> m_vehiclePrompt;
    QStringList m_seenHistory;
    QString m_seenStatus;
    quint64 m_promptRevision = 0;
    bool m_refreshingVehicleActions = false;
    int m_implementedActionCount = 0;
    QPushButton *m_gpsExtractionButton = nullptr;
    QPointer<QDialog> m_gpsExtractionPrompt;
    QPointer<QProgressDialog> m_gpsExtractionProgress;
    std::shared_ptr<GpsExtractionState> m_gpsExtractionState;
    quint64 m_gpsPromptRevision = 0;
    QPushButton *m_splitButton = nullptr;
    QPointer<QDialog> m_splitPrompt;
    QPointer<QProgressDialog> m_splitProgress;
    std::shared_ptr<SplitState> m_splitState;
    quint64 m_splitPromptRevision = 0;
    QPushButton *m_dashWareButton = nullptr;
    QPointer<QDialog> m_dashWarePrompt;
    QPointer<QProgressDialog> m_dashWareProgress;
    std::shared_ptr<DashWareState> m_dashWareState;
    quint64 m_dashWarePromptRevision = 0;
    QPushButton *m_apjButton = nullptr;
    QPointer<QDialog> m_apjPrompt;
    QPointer<QProgressDialog> m_apjProgress;
    std::shared_ptr<ApjEmbeddingState> m_apjState;
    quint64 m_apjPromptRevision = 0;
    QPushButton *m_logOrganizerButton = nullptr;
    QPointer<QDialog> m_logOrganizerPrompt;
    QPointer<QProgressDialog> m_logOrganizerProgress;
    std::shared_ptr<LogOrganizerState> m_logOrganizerState;
    quint64 m_logOrganizerRevision = 0;
    QPointer<MavFtpServiceInterface> m_mavFtpService;
    QPointer<VehicleTargetManager> m_mavFtpTargets;
    QPointer<MavFtpFileDownload> m_mavFtpDownload;
    QPushButton *m_mavFtpButton = nullptr;
    bool m_fileToolsClosing = false;
};

#endif // CONFIGDEVELOPERTOOLSVIEW_H
