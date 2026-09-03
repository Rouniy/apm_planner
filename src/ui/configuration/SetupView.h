#ifndef SETUPVIEW_H
#define SETUPVIEW_H

#include "ParamField.h"
#include "comm/DroneCanForwardingBroker.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <memory>

class AntennaTrackerUIViewModel;
class BackstageView;
class ConfigGpsInjectView;
class ConfigDroneCanView;
class DroneCanMavlinkTransport;
class FrameDefaultCatalogService;
class LinkInterface;
class ParameterMetaDataRepository;
class QGCUASParamManager;
class UASInterface;

class SetupView final : public QWidget
{
    Q_OBJECT

public:
    explicit SetupView(QWidget *parent = nullptr);
    ~SetupView() override;

signals:
    void advancedModeChanged(bool advanced);
    void connectionStateChanged(bool connected);

public slots:
    void advModeChanged(bool advanced);
    bool showDeveloperTools();

private slots:
    void activeUASSet(UASInterface *uas);
    void vehicleConnected();
    void vehicleDisconnected();
    void parameterTargetChanged();
    void parameterListUpToDate(int component);
    void parameterListLoadStarted();
    void parameterListReadyChanged(bool ready);
    void parameterListLoadFailed(const QString &reason);
    void parameterListLoadCanceled();
    void parameterManagerChanged(QGCUASParamManager *manager);
    void stopParameterLoading();
    void retryParameterLoading();
    void firmwareVersionDetected(const QString &versionText);

private:
    void buildPages();
    void refreshPageVisibility();
    void refreshLoadingOverlay();
    void syncConnectionState();
    void bindParameterManager(QGCUASParamManager *manager);
    void resetConnectionPages(bool restoreSelection = true);
    void resetParameterProgress();
    bool hasConnectedLink() const;
    bool currentPageAllowsPartialParameters() const;
    QWidget *createDefaultSettingsPage(QWidget *parent);
    QWidget *createHWIDPage(QWidget *parent);
    QWidget *createADSBPage(QWidget *parent);
    QWidget *createHWOSDPage(QWidget *parent);
    QWidget *createESP8266Page(QWidget *parent);
    QWidget *createAntennaTrackerSerialPage(QWidget *parent);
    QWidget *createAntennaTrackerLivePage(QWidget *parent);
    // One shared MP10 AntennaTrackerUIViewModel for both tracker pages.
    AntennaTrackerUIViewModel *ensureAntennaTrackerViewModel();
    QWidget *createMotorTestPage(QWidget *parent);
    QWidget *createBluetoothSetupPage(QWidget *parent);
    QWidget *createParachutePage(QWidget *parent);
    QWidget *createGpsInjectPage(QWidget *parent);
    QWidget *createGPSOrderPage(QWidget *parent);
    QWidget *createBatteryMonitoring2Page(QWidget *parent);
    QWidget *createDroneCanPage(QWidget *parent);
    QWidget *createHWCANPage(QWidget *parent);
    QWidget *createEscCalibrationPage(QWidget *parent);
    QWidget *createRadioOutputPage(QWidget *parent);
    QWidget *createSerialPage(QWidget *parent);
    QWidget *createInitialParamsPage(QWidget *parent);
    bool bindDroneCanTransport();
    LinkInterface *selectDroneCanLink() const;
    QList<ConfigFriendlyParameterValue> parameterSnapshot(
        int componentId) const;

    BackstageView *m_backstage = nullptr;
    QPointer<ConfigGpsInjectView> m_gpsInjectPage;
    QPointer<ConfigDroneCanView> m_droneCanPage;
    QPointer<DroneCanForwardingBroker> m_droneCanBroker;
    QPointer<DroneCanMavlinkTransport> m_droneCanTransport;
    DroneCanForwardingBroker::LeaseToken m_droneCanLease;
    QPointer<LinkInterface> m_droneCanLastPrimaryLink;
    std::unique_ptr<ParameterMetaDataRepository> m_metadataRepository;
    QPointer<FrameDefaultCatalogService> m_frameDefaultCatalogService;
    QPointer<AntennaTrackerUIViewModel> m_antennaTrackerViewModel;
    QPointer<UASInterface> m_uas;
    QPointer<QGCUASParamManager> m_parameterManager;
    QString m_parameterLoadFailure;
    QString m_preferredPageHeader;
    QString m_firmwareVersion;
    QString m_targetPageToRestore;
    qulonglong m_parameterTargetRevision = 0;
    bool m_parameterLoadingCanceled = false;
    bool m_connected = false;
    bool m_parametersReady = false;
    bool m_parameterRetryPending = false;
    bool m_advanced = false;
    bool m_officialFirmware = false;
};

#endif
