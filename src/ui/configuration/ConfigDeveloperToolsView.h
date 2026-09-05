#ifndef CONFIGDEVELOPERTOOLSVIEW_H
#define CONFIGDEVELOPERTOOLSVIEW_H

#include "ActionPageView.h"
#include "services/DeveloperVehicleToolService.h"

#include <QMap>
#include <QPointer>
#include <memory>

class QCloseEvent;
class QDialog;
class QObject;
class QPushButton;
class QProgressDialog;
class QShowEvent;

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

public slots:
    void DecodeMavlinkInput(const QString &input);
    void DecodeHardwareIdInput(const QString &input,
                               const QString &parameterName = QString());
    // Paths already selected/confirmed by the caller; never sends telemetry.
    void ExtractGpsCorrections(const QString &input, const QString &output);
    void SplitDataFlashLog(const QString &input, int pieces);

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
    void RefreshOfflineFileActions();
    struct GpsExtractionState;
    struct SplitState;

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
    bool m_fileToolsClosing = false;
};

#endif // CONFIGDEVELOPERTOOLSVIEW_H
