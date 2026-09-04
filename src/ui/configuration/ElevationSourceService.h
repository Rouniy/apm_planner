#ifndef ELEVATIONSOURCESERVICE_H
#define ELEVATIONSOURCESERVICE_H

#include <QList>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

class QSettings;
class QThread;

struct ElevationSourceFile
{
    QString FullPath;
    QString Format;
    bool Indexed = false;
    QString Coverage;
    QString Error;

    QString Name() const;
    QString State() const;
};

struct NativeGdalRasterFile
{
    QString FullPath;
    QString Driver;
    bool Indexed = false;
    QString Size;
    QString Coverage;
    QString Error;

    QString Name() const;
    QString State() const;
};

struct ElevationSourcesScanProgress
{
    enum class Stage {
        Elevation,
        NativeGdal
    };

    Stage stage = Stage::Elevation;
    int completed = 0;
    int total = 0;
    QString currentFile;
};

struct ElevationSourcesScanResult
{
    QString Directory;
    QList<ElevationSourceFile> Files;
    QList<NativeGdalRasterFile> RasterFiles;
    QString Backend;
    int ExaminedRasterFiles = 0;
    int UnrecognizedRasterFiles = 0;
    bool NativeGdalAvailable = false;
    bool Cancelled = false;
    QString Error;

    int IndexedCount() const;
    int ErrorCount() const;
    int GeoTiffCount() const;
    int DtedCount() const;
    int RasterIndexedCount() const;
    int RasterErrorCount() const;
};

Q_DECLARE_METATYPE(ElevationSourcesScanProgress)
Q_DECLARE_METATYPE(ElevationSourcesScanResult)

class ElevationSourceService : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *SettingsKey = "GDALImageDir";
    static constexpr const char *MapType = "GDAL Custom";

    explicit ElevationSourceService(QObject *parent = nullptr);
    ~ElevationSourceService() override;

    static ElevationSourceService *instance();

    static QString savedDirectory(QSettings *settings = nullptr);
    static QString normalizeExistingDirectory(const QString &directory,
                                              QString *error = nullptr);
    static bool saveDirectory(QSettings *settings, const QString &directory,
                              QString *normalized, QString *error = nullptr);
    static bool clearSavedDirectory(QSettings *settings,
                                    QString *error = nullptr);
    static QStringList findSupportedFiles(const QString &directory,
                                          QString *error = nullptr);
    static QStringList findNativeRasterCandidates(const QString &directory,
                                                  QString *error = nullptr);
    static QStringList nativeGdalLibraryCandidates();

    virtual bool isBusy() const;
    virtual bool hasLastResult() const;
    virtual ElevationSourcesScanResult lastResult() const;
    virtual QString startupError() const;
    virtual bool requiresRestartToSwitch(const QString &directory) const;
    virtual QString backendStatus() const;
    virtual bool isNativeGdalAvailable() const;
    virtual bool sampleAltitude(double latitude, double longitude,
                                double *altitude) const;
    virtual bool sampleAltitudeCached(double latitude, double longitude,
                                      double *altitude) const;
    virtual bool requestSrtmTile(double latitude, double longitude) const;
    virtual QString srtmCacheDirectory() const;
    virtual bool srtmAutoDownloadEnabled() const;
    virtual void setSrtmAutoDownloadEnabled(bool enabled);
    virtual QByteArray renderRasterTile(int tileX, int tileY, int zoom,
                                        int tileSize = 256) const;

    virtual bool startScan(const QString &directory, bool startup = false);
    virtual void cancelScan();
    virtual void unloadNativeRasters();
    virtual void initializeFromSettings();
    virtual void shutdown();

signals:
    void scanStarted(bool startup);
    void scanProgress(const ElevationSourcesScanProgress &progress,
                      bool startup);
    void scanFinished(const ElevationSourcesScanResult &result,
                      bool startup);
    void scanFailed(const QString &error, bool startup);
    void scanCancelled(bool startup);
    void busyChanged(bool busy);
    void nativeRastersChanged();
    void srtmTileAvailable(const QString &tileName);
    void srtmDownloadFailed(const QString &tileName, const QString &error);

private:
    class Private;
    std::unique_ptr<Private> d;
};

#endif
