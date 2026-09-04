#ifndef CONFIGVIEW_H
#define CONFIGVIEW_H

#include "ParamField.h"
#include "ConfigRouteProfile.h"

#include <QPointer>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <memory>

class BackstageView;
class ConfigFriendlyParamsView;
class ConfigRawParams;
class ConfigUserDefinedView;
class ParameterMetaDataRepository;
class ParameterMetaDataUpdater;
class QGCUASParamManager;
class UASInterface;
enum class ParameterFirmwareFamily;

class ConfigView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigView(QWidget *parent = nullptr);
    ~ConfigView() override;

signals:
    void advancedModeChanged(bool advanced);

public slots:
    void advModeChanged(bool advanced);

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
    void currentPageChanged(const QString &pageId);
    void firmwareVersionDetected(const QString &versionText);
    void parameterMetadataUpdated(ParameterFirmwareFamily family,
                                  const QString &firmwareVersion);

private:
    void buildPages();
    void refreshPageVisibility();
    void refreshLoadingOverlay();
    void syncConnectionState();
    void bindParameterManager(QGCUASParamManager *manager);
    void resetVehiclePages(bool targetChanged);
    void refreshFriendlyParameterPages();
    void resetParameterProgress();
    void restorePreferredPage();
    QWidget *createFriendlyParamsPage(bool advanced, QWidget *parent);
    QWidget *createFlightModesPage(QWidget *parent);
    QWidget *createExtendedTuningPage(QWidget *parent);
    QWidget *createHeliSetupPage(QWidget *parent);
    QWidget *createOsdPage(QWidget *parent);
    QWidget *createUserDefinedPage(QWidget *parent);
    QWidget *createRawParamsPage(QWidget *parent);
    QList<ConfigFriendlyParameterValue> parameterSnapshot(int componentId) const;
    ParameterFirmwareFamily parameterFirmwareFamily() const;
    ConfigRouteContext routeContext() const;
    bool isHelicopterProfile() const;
    bool hasLegacyHeliSetup() const;
    bool hasConnectedLink() const;
    bool currentPageAllowsPartialParameters() const;

    BackstageView *m_backstage = nullptr;
    std::unique_ptr<ParameterMetaDataRepository> m_metadataRepository;
    std::unique_ptr<ParameterMetaDataUpdater> m_metadataUpdater;
    QPointer<UASInterface> m_uas;
    QPointer<QGCUASParamManager> m_parameterManager;
    QString m_parameterLoadFailure;
    // New values are stable page ids; old installations may still contain a
    // translated page header and remain accepted during restore.
    QString m_preferredPage;
    QString m_firmwareVersion;
    QString m_targetPageToRestore;
    qulonglong m_parameterTargetRevision = 0;
    bool m_parameterLoadingCanceled = false;
    bool m_connected = false;
    bool m_parametersReady = false;
    bool m_parameterRetryPending = false;
    bool m_advanced = false;
    bool m_adjustingSelection = false;
    bool m_officialFirmware = false;
};

#endif
