#ifndef CONFIGDEFAULTSETTINGSVIEW_H
#define CONFIGDEFAULTSETTINGSVIEW_H

#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterStore.h"
#include "ui/configuration/FrameDefaultCatalogService.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWidget>

class ConfigRawParams;
class QComboBox;
class QLabel;
class QPushButton;
class QResizeEvent;

/*
 * Mission Planner 10 SETUP > Default Settings page
 * (GCSViews/ConfigurationView/ConfigDefaultSettingsView.axaml with the
 * "ArduPilot frame defaults" row of Views/RawParamsView.axaml).
 *
 * The page lists the official Tools/Frame_params profiles through a
 * caller-owned FrameDefaultCatalogService, downloads the selected profile
 * into the frame-defaults cache and hands the file to the embedded
 * ConfigRawParams, whose Compare Params dialog only STAGES the selected
 * differences. Nothing is written to the vehicle by this page; Write Params
 * remains a separate, confirmed ConfigRawParams action.
 */
class ConfigDefaultSettingsView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigDefaultSettingsView(
        FrameDefaultCatalogService *service,
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr, bool enforceMetadataRanges = false);
    ~ConfigDefaultSettingsView() override;

    ConfigRawParams *rawParams() const { return m_rawParams; }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = false);
    void setConnected(bool connected);
    bool isConnected() const { return m_connected; }
    void setParameterSnapshot(const QList<ParameterRecord> &records,
                              int preferredComponent = 1);

    // Root of the frame-defaults cache (FrameDefaultCatalogService::
    // GetCachePath adds "frame-defaults/<catalog path>"); defaults to
    // FrameDefaultCatalogService::DefaultCacheRoot().
    QString cacheRoot() const { return m_cacheRoot; }
    void setCacheRoot(const QString &root);

    bool isActive() const { return m_active; }
    bool isBusy() const { return m_busy != Busy::Idle; }
    QString statusText() const { return m_status; }
    int frameDefaultCount() const { return m_items.size(); }
    QVector<FrameDefaultCatalogItem> frameDefaults() const { return m_items; }
    QString selectedFrameDefaultPath() const;
    quint64 targetRevision() const { return m_targetRevision; }

public slots:
    // Page shown: loads the (cached) catalog when no list is available yet.
    void activate();
    // Page hidden: cancels the operation this page owns.
    void deactivate();
    // Exact parameter target changed: discards an in-flight download and
    // forwards to the embedded ConfigRawParams.
    void parameterTargetChanged();
    // "Load / refresh list": forces a new catalog walk.
    void refreshFrameDefaults();
    // "Compare / Stage…": downloads the selected profile for the current
    // vehicle and opens ConfigRawParams' Compare Params dialog.
    void compareAndStage();

signals:
    // Forwarded from the embedded ConfigRawParams so SetupView can wire the
    // page like every other parameter page.
    void refreshRequested(int componentId);
    void writeRequested(int componentId, QVariantList changes);
    void statusChanged(const QString &status);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Busy { Idle, LoadingList, Downloading };

    void buildUi(const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges);
    void setStatus(const QString &status);
    void updateStatusLabel();
    void setBusy(Busy busy);
    void updateControls();
    void requestCatalog(bool forceRefresh);
    void catalogReady(const QVector<FrameDefaultCatalogItem> &items, bool fromCache);
    void catalogFailed(const QString &error, bool cancelled);
    void downloadFinished(const QString &catalogPath, const QByteArray &bytes);
    void downloadFailed(const QString &catalogPath, const QString &error,
                        bool cancelled);
    // Cancels the download this page owns (if any) and reports `status`.
    void discardDownload(const QString &status);
    QString displayNameFor(const QString &catalogPath) const;

    QPointer<FrameDefaultCatalogService> m_service;
    ConfigRawParams *m_rawParams = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_warning = nullptr;
    QLabel *m_rowLabel = nullptr;
    QComboBox *m_combo = nullptr;
    QPushButton *m_compareButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    QString m_status;
    QString m_cacheRoot;
    QVector<FrameDefaultCatalogItem> m_items;
    bool m_active = false;
    bool m_connected = false;
    Busy m_busy = Busy::Idle;
    bool m_waitingCatalogRequest = false;
    bool m_ownsCatalogRequest = false;
    FrameDefaultCatalogService::OperationId m_catalogOperationId =
        FrameDefaultCatalogService::InvalidOperationId;
    QString m_downloadPath;          // catalog path of the download this page owns
    FrameDefaultCatalogService::OperationId m_downloadOperationId =
        FrameDefaultCatalogService::InvalidOperationId;
    QString m_pendingDiscardStatus;  // status to show when an owned download is cancelled
    quint64 m_targetRevision = 1;
    quint64 m_downloadRevision = 0;
};

#endif
