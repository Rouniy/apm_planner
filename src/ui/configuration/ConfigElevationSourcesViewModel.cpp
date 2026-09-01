#include "ConfigElevationSourcesViewModel.h"

#include <QFileInfo>
#include <QSettings>

ConfigElevationSourcesViewModel::ConfigElevationSourcesViewModel(QObject *parent)
    : ConfigElevationSourcesViewModel(ElevationSourceService::instance(),
                                      nullptr, parent)
{
}

ConfigElevationSourcesViewModel::ConfigElevationSourcesViewModel(
    ElevationSourceService *service, QSettings *settings, QObject *parent)
    : QObject(parent),
      m_service(service ? service : ElevationSourceService::instance()),
      m_settings(settings ? settings : new QSettings),
      m_ownsSettings(!settings),
      m_directoryPath(ElevationSourceService::savedDirectory(m_settings)),
      m_status(tr("Choose a directory containing local elevation or raster-map files.")),
      m_nativeGdalStatus(
          tr("Checking the optional native GDAL raster backend…"))
{
    m_settings->setFallbacksEnabled(false);
    connect(m_service, &ElevationSourceService::scanStarted,
            this, &ConfigElevationSourcesViewModel::serviceScanStarted);
    connect(m_service, &ElevationSourceService::scanProgress,
            this, &ConfigElevationSourcesViewModel::serviceScanProgress);
    connect(m_service, &ElevationSourceService::scanFinished,
            this, &ConfigElevationSourcesViewModel::serviceScanFinished);
    connect(m_service, &ElevationSourceService::scanFailed,
            this, &ConfigElevationSourcesViewModel::serviceScanFailed);
    connect(m_service, &ElevationSourceService::scanCancelled,
            this, &ConfigElevationSourcesViewModel::serviceScanCancelled);
    connect(m_service, &ElevationSourceService::busyChanged,
            this, &ConfigElevationSourcesViewModel::serviceBusyChanged);

    if (m_service->hasLastResult()) {
        applyResult(m_service->lastResult());
    } else {
        m_nativeGdalStatus = tr("Raster map: %1")
            .arg(m_service->backendStatus());
        const QString startupError = m_service->startupError();
        if (!startupError.isEmpty()) {
            m_status = tr("Saved elevation directory failed to load: %1")
                .arg(startupError);
        }
    }
    if (m_service->isBusy()) {
        m_isBusy = true;
        m_status = tr("Restoring the saved local elevation and raster directory in the background…");
    }
}

ConfigElevationSourcesViewModel::~ConfigElevationSourcesViewModel()
{
    if (m_ownsActiveScan && m_isBusy) {
        m_service->cancelScan();
    }
    if (m_ownsSettings) {
        delete m_settings;
    }
}

void ConfigElevationSourcesViewModel::SetDirectoryPath(const QString &path)
{
    if (m_directoryPath == path) {
        return;
    }
    m_directoryPath = path;
    emit directoryPathChanged(path);
}

void ConfigElevationSourcesViewModel::SelectAndScan(
    const QString &directory)
{
    if (!m_isBusy) {
        saveAndScan(directory);
    }
}

void ConfigElevationSourcesViewModel::Rescan()
{
    if (!m_isBusy) {
        saveAndScan(m_directoryPath);
    }
}

void ConfigElevationSourcesViewModel::Cancel()
{
    if (!m_isBusy) {
        return;
    }
    setStatus(tr("Cancellation requested; finishing the current file…"));
    m_service->cancelScan();
}

void ConfigElevationSourcesViewModel::ClearSaved(bool confirmed)
{
    if (m_isBusy || !confirmed) {
        return;
    }
    QString error;
    if (!ElevationSourceService::clearSavedDirectory(m_settings, &error)) {
        setStatus(tr("Local-source scan failed: %1").arg(error));
        return;
    }
    m_service->unloadNativeRasters();
    SetDirectoryPath(QString());
    m_rasterFiles.clear();
    emit rasterFilesChanged();
    setNativeGdalStatus(tr("Raster map unloaded. %1")
                            .arg(m_service->backendStatus()));
    setStatus(tr("Saved elevation directory cleared. Restart to unload files already indexed in this session."));
}

void ConfigElevationSourcesViewModel::serviceScanStarted(bool startup)
{
    Q_UNUSED(startup)
    m_elevationCompleted = 0;
    m_elevationTotal = 0;
    m_rasterCompleted = 0;
    m_rasterTotal = 0;
    setProgress(0, 1);
    setBusy(true);
    setStatus(startup
        ? tr("Restoring the saved local elevation and raster directory in the background…")
        : tr("Discovering local elevation and GDAL raster-map files…"));
    setNativeGdalStatus(
        tr("Scanning the directory with the optional native GDAL backend…"));
}

void ConfigElevationSourcesViewModel::serviceScanProgress(
    const ElevationSourcesScanProgress &progress, bool startup)
{
    Q_UNUSED(startup)
    if (progress.stage == ElevationSourcesScanProgress::Stage::Elevation) {
        m_elevationCompleted = progress.completed;
        m_elevationTotal = progress.total;
        setStatus(progress.completed >= progress.total
            ? tr("Finishing elevation index…")
            : tr("Indexing %1/%2: %3")
                  .arg(progress.completed + 1)
                  .arg(progress.total)
                  .arg(QFileInfo(progress.currentFile).fileName()));
    } else {
        m_rasterCompleted = progress.completed;
        m_rasterTotal = progress.total;
        setNativeGdalStatus(progress.completed >= progress.total
            ? tr("Finishing native GDAL raster index…")
            : tr("GDAL %1/%2: %3")
                  .arg(progress.completed + 1)
                  .arg(progress.total)
                  .arg(QFileInfo(progress.currentFile).fileName()));
    }
    setProgress(m_elevationCompleted + m_rasterCompleted,
                qMax(1, m_elevationTotal + m_rasterTotal));
}

void ConfigElevationSourcesViewModel::serviceScanFinished(
    const ElevationSourcesScanResult &result, bool startup)
{
    Q_UNUSED(startup)
    m_ownsActiveScan = false;
    applyResult(result);
    setBusy(false);
    if (m_settings->value(QStringLiteral("MapType")).toString()
        == QString::fromLatin1(ElevationSourceService::MapType)) {
        emit gdalMapRefreshRequested();
    }
}

void ConfigElevationSourcesViewModel::serviceScanFailed(
    const QString &error, bool startup)
{
    m_ownsActiveScan = false;
    setBusy(false);
    setStatus(startup
        ? tr("Saved elevation directory failed to load: %1").arg(error)
        : tr("Local-source scan failed: %1").arg(error));
}

void ConfigElevationSourcesViewModel::serviceScanCancelled(bool startup)
{
    Q_UNUSED(startup)
    m_ownsActiveScan = false;
    setBusy(false);
    setStatus(tr("Local-source indexing cancelled. The previously complete indexes remain active."));
    setNativeGdalStatus(tr("Native GDAL raster indexing cancelled."));
}

void ConfigElevationSourcesViewModel::serviceBusyChanged(bool busy)
{
    setBusy(busy);
}

void ConfigElevationSourcesViewModel::saveAndScan(const QString &directory)
{
    QString normalized;
    QString error;
    if (!ElevationSourceService::saveDirectory(
            m_settings, directory, &normalized, &error)) {
        setStatus(tr("Local-source scan failed: %1").arg(error));
        return;
    }
    SetDirectoryPath(normalized);
    if (m_service->requiresRestartToSwitch(normalized)) {
        setStatus(tr("The new elevation directory is saved. Restart Mission Planner to unload the currently active DEM index and switch sources safely."));
        return;
    }
    m_ownsActiveScan = true;
    if (!m_service->startScan(normalized, false)) {
        m_ownsActiveScan = false;
        setStatus(tr("Local-source scan failed: another local-source scan is already running."));
    }
}

void ConfigElevationSourcesViewModel::applyResult(
    const ElevationSourcesScanResult &result)
{
    SetDirectoryPath(result.Directory);
    m_files = result.Files;
    m_rasterFiles = result.RasterFiles;
    emit filesChanged();
    emit rasterFilesChanged();
    setProgress(result.Files.size(), qMax(1, result.Files.size()));
    if (result.Files.isEmpty()) {
        setStatus(tr("No .tif, .tiff, .dt0, .dt1 or .dt2 files were found in this directory."));
    } else {
        const QString errors = result.ErrorCount() == 0
            ? QStringLiteral(".")
            : tr("; %1 error(s).").arg(result.ErrorCount());
        setStatus(tr("Indexed %1/%2 file(s): %3 GeoTIFF, %4 DTED%5 Local DEM data takes priority over downloaded SRTM.")
                      .arg(result.IndexedCount())
                      .arg(result.Files.size())
                      .arg(result.GeoTiffCount())
                      .arg(result.DtedCount())
                      .arg(errors));
    }

    if (!result.NativeGdalAvailable) {
        setNativeGdalStatus(result.Backend);
        return;
    }
    const QString ignored = result.UnrecognizedRasterFiles == 0
        ? QString()
        : tr("; %1 unrecognized file(s) ignored")
              .arg(result.UnrecognizedRasterFiles);
    if (result.RasterIndexedCount() == 0) {
        setNativeGdalStatus(result.Backend
            + (result.ExaminedRasterFiles == 0
                ? tr(". No candidate files of at least 1 KiB were found.")
                : tr(". No georeferenced raster was indexed%1.").arg(ignored)));
        return;
    }
    QString summary = tr("%1. Indexed %2/%3 raster(s)")
        .arg(result.Backend)
        .arg(result.RasterIndexedCount())
        .arg(result.RasterFiles.size());
    if (result.RasterErrorCount() == 0) {
        summary += ignored + QStringLiteral(".");
    } else {
        summary += tr("; %1 error(s)%2.")
            .arg(result.RasterErrorCount()).arg(ignored);
    }
    summary += tr(" Select GDAL Custom in Flight Planner to overlay them on satellite imagery.");
    setNativeGdalStatus(summary);
}

void ConfigElevationSourcesViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
}

void ConfigElevationSourcesViewModel::setNativeGdalStatus(
    const QString &status)
{
    if (m_nativeGdalStatus == status) {
        return;
    }
    m_nativeGdalStatus = status;
    emit nativeGdalStatusChanged(status);
}

void ConfigElevationSourcesViewModel::setProgress(int value, int maximum)
{
    maximum = qMax(1, maximum);
    value = qBound(0, value, maximum);
    if (m_progress == value && m_progressMaximum == maximum) {
        return;
    }
    m_progress = value;
    m_progressMaximum = maximum;
    emit progressChanged(value, maximum);
}

void ConfigElevationSourcesViewModel::setBusy(bool busy)
{
    if (m_isBusy == busy) {
        return;
    }
    m_isBusy = busy;
    emit busyChanged(busy);
}
