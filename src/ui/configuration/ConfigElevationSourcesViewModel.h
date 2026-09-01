#ifndef CONFIGELEVATIONSOURCESVIEWMODEL_H
#define CONFIGELEVATIONSOURCESVIEWMODEL_H

#include "ElevationSourceService.h"

#include <QObject>
#include <QString>

class QSettings;

class ConfigElevationSourcesViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigElevationSourcesViewModel(QObject *parent = nullptr);
    ConfigElevationSourcesViewModel(ElevationSourceService *service,
                                    QSettings *settings,
                                    QObject *parent = nullptr);
    ~ConfigElevationSourcesViewModel() override;

    QString DirectoryPath() const { return m_directoryPath; }
    QString Status() const { return m_status; }
    QString NativeGdalStatus() const { return m_nativeGdalStatus; }
    int Progress() const { return m_progress; }
    int ProgressMaximum() const { return m_progressMaximum; }
    bool IsBusy() const { return m_isBusy; }
    QList<ElevationSourceFile> Files() const { return m_files; }
    QList<NativeGdalRasterFile> RasterFiles() const { return m_rasterFiles; }

public slots:
    void SetDirectoryPath(const QString &path);
    void SelectAndScan(const QString &directory);
    void Rescan();
    void Cancel();
    void ClearSaved(bool confirmed = true);

signals:
    void directoryPathChanged(const QString &path);
    void statusChanged(const QString &status);
    void nativeGdalStatusChanged(const QString &status);
    void progressChanged(int value, int maximum);
    void busyChanged(bool busy);
    void filesChanged();
    void rasterFilesChanged();
    void gdalMapRefreshRequested();

private slots:
    void serviceScanStarted(bool startup);
    void serviceScanProgress(const ElevationSourcesScanProgress &progress,
                             bool startup);
    void serviceScanFinished(const ElevationSourcesScanResult &result,
                             bool startup);
    void serviceScanFailed(const QString &error, bool startup);
    void serviceScanCancelled(bool startup);
    void serviceBusyChanged(bool busy);

private:
    void saveAndScan(const QString &directory);
    void applyResult(const ElevationSourcesScanResult &result);
    void setStatus(const QString &status);
    void setNativeGdalStatus(const QString &status);
    void setProgress(int value, int maximum);
    void setBusy(bool busy);

    ElevationSourceService *m_service = nullptr;
    QSettings *m_settings = nullptr;
    bool m_ownsSettings = false;
    bool m_ownsActiveScan = false;
    QString m_directoryPath;
    QString m_status;
    QString m_nativeGdalStatus;
    int m_progress = 0;
    int m_progressMaximum = 1;
    bool m_isBusy = false;
    int m_elevationCompleted = 0;
    int m_elevationTotal = 0;
    int m_rasterCompleted = 0;
    int m_rasterTotal = 0;
    QList<ElevationSourceFile> m_files;
    QList<NativeGdalRasterFile> m_rasterFiles;
};

#endif
