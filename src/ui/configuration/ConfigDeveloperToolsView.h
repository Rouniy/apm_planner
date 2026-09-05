#ifndef CONFIGDEVELOPERTOOLSVIEW_H
#define CONFIGDEVELOPERTOOLSVIEW_H

#include "ActionPageView.h"
#include "services/DeveloperVehicleToolService.h"

#include <QMap>
#include <QPointer>

class QCloseEvent;
class QDialog;
class QObject;
class QPushButton;

/** Mission Planner 10 Developer Tools action page. */
class ConfigDeveloperToolsView final : public ActionPageView
{
    Q_OBJECT

public:
    explicit ConfigDeveloperToolsView(QObject *actionSource = nullptr,
                                      QWidget *parent = nullptr);

    int ImplementedActionCount() const;
    void setVehicleToolService(DeveloperVehicleToolService *service);

public slots:
    void DecodeMavlinkInput(const QString &input);
    void DecodeHardwareIdInput(const QString &input,
                               const QString &parameterName = QString());

protected:
    void closeEvent(QCloseEvent *event) override;

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

    QPointer<QObject> m_actionSource;
    QPointer<DeveloperVehicleToolService> m_vehicleTools;
    QMap<VehicleAction, QPushButton *> m_vehicleButtons;
    QPointer<QDialog> m_vehiclePrompt;
    QStringList m_seenHistory;
    QString m_seenStatus;
    quint64 m_promptRevision = 0;
    bool m_refreshingVehicleActions = false;
    int m_implementedActionCount = 0;
};

#endif // CONFIGDEVELOPERTOOLSVIEW_H
