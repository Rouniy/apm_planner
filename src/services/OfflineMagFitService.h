#ifndef OFFLINEMAGFITSERVICE_H
#define OFFLINEMAGFITSERVICE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <functional>

struct MagVector { double x = 0, y = 0, z = 0; };

struct OfflineMagFitResult {
    int compass = 1, sourceSamples = 0, usedSamples = 0, coverageOctants = 0;
    MagVector loggedOffsets, sphereOffsets, offsets, diagonals{1, 1, 1}, offDiagonals;
    double sphereRadius = 0, sphereRmsError = 0, rmsError = 0;
    bool hasEllipsoid = false;
};

struct OfflineMagFitReport {
    bool success = false, cancelled = false;
    QString error, sourcePath;
    bool isTelemetryLog = false;
    int throttleThreshold = 30;
    bool useEllipsoid = true;
    QVector<OfflineMagFitResult> results;
    QStringList warnings;
    // Analysis is valid independently of apply eligibility. Eligibility only
    // establishes the explicit, stable DataFlash compensation context below;
    // the apply service must still bind these device IDs to a fresh target.
    bool applyEligible = false;
    QString applyUnavailableReason;
    QMap<int, quint32> loggedDeviceIds;
    // Exact recorded AHRS_ORIENTATION and COMPASS_ORIENT/EXTERNAL family
    // values. Apply must match the current target's coordinate-frame settings.
    QMap<QString, double> loggedFrameParameters;
};

// Offline computation only: never accesses a vehicle or writes a file. Options
// are copied on entry; callbacks execute on the calling (normally worker) thread.
// Results do not establish sensor identity or the safety of applying a fit.
class OfflineMagFitService {
public:
    struct Options {
        int throttleThreshold = 30;
        bool useEllipsoid = true;
        std::function<bool()> isCancelled;
        std::function<void(double)> progress;
    };
    static constexpr int MaximumSamplesPerCompass = 1000000;
    static OfflineMagFitReport analyze(const QString &path, const Options &options);
    static bool fitSamples(int compass, const QVector<MagVector> &samples,
                           int sourceSamples, const MagVector &loggedOffsets,
                           bool ellipsoid, OfflineMagFitResult *out,
                           QString *error = nullptr,
                           std::function<bool()> isCancelled = {});
    // Exposed for independent reference fixtures. Truncation is toward zero;
    // samples must be finite. Returns empty with error on invalid input/cancel.
    static QVector<MagVector> prepareTelemetrySamples(
        const QVector<MagVector> &samples, QString *error = nullptr,
        std::function<bool()> isCancelled = {});
};

#endif
