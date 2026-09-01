#ifndef CONFIGVIEW_H
#define CONFIGVIEW_H

#include "ParamField.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <memory>

class BackstageView;
class ConfigFriendlyParamsView;
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
    void parameterChanged(int uas, int component, int parameterCount,
                          int parameterId, QString parameterName,
                          QVariant value);
    void parameterListUpToDate(int component);
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
    QWidget *createUserDefinedPage(QWidget *parent);
    QList<ConfigFriendlyParameterValue> parameterSnapshot(int componentId) const;
    ParameterFirmwareFamily parameterFirmwareFamily() const;
    bool friendlyParametersSupported() const;
    bool hasConnectedLink() const;
    bool currentPageAllowsPartialParameters() const;

    BackstageView *m_backstage = nullptr;
    std::unique_ptr<ParameterMetaDataRepository> m_metadataRepository;
    std::unique_ptr<ParameterMetaDataUpdater> m_metadataUpdater;
    QPointer<UASInterface> m_uas;
    QPointer<QGCUASParamManager> m_parameterManager;
    QHash<int, QSet<int>> m_receivedParameterIds;
    QHash<int, int> m_expectedParameterCounts;
    QString m_parameterLoadFailure;
    QString m_preferredPageHeader;
    QString m_firmwareVersion;
    int m_parameterProgress = -1;
    bool m_connected = false;
    bool m_parametersReady = false;
    bool m_advanced = false;
    bool m_adjustingSelection = false;
    bool m_officialFirmware = false;
};

#endif
