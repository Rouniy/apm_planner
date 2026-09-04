#ifndef OSDVIDEOOVERLAYCORE_H
#define OSDVIDEOOVERLAYCORE_H

#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

class QIODevice;

/** Immutable HUD state selected from a Mission Planner timestamped .tlog. */
struct OsdVideoTelemetrySample
{
    qint64 timestampUsec = 0;
    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    double altitude = 0.0;
    double airSpeed = 0.0;
    double groundSpeed = 0.0;
    double verticalSpeed = 0.0;
    double satelliteCount = 0.0;
    bool armed = false;
    bool prearmOk = false;
    int gpsFixType = 0;
    QString mode = QStringLiteral("—");
    double batteryVoltage = 0.0;
    int batteryRemaining = 0;
    double currentAmps = 0.0;
    double navBearing = 0.0;
    double targetAltitude = 0.0;
    double targetSpeed = 0.0;
    double windDirection = 0.0;
    double windSpeed = 0.0;
    double aoa = 0.0;
    double ssa = 0.0;
    double xtrackError = 0.0;
    double turnRate = 0.0;
    double batteryVoltage2 = 0.0;
    int batteryRemaining2 = 0;
    double currentAmps2 = 0.0;
    double throttlePercent = 0.0;
    bool failsafe = false;
    bool safetyActive = false;
    double linkQuality = 0.0;
    double waypointDistance = 0.0;
    int waypointNumber = 0;
};

class OsdVideoTelemetryTimeline
{
public:
    enum class LoadStatus { Ok, Cancelled, Error };
    using CancelRequested = std::function<bool()>;
    using Progress = std::function<void(qint64 bytesProcessed,
                                        qint64 bytesTotal)>;

    explicit OsdVideoTelemetryTimeline(
        QVector<OsdVideoTelemetrySample> samples = {});

    static OsdVideoTelemetryTimeline Load(
        QIODevice *device,
        LoadStatus *status = nullptr,
        QString *errorMessage = nullptr,
        const CancelRequested &cancel = {},
        const Progress &progress = {});

    bool isEmpty() const { return m_samples.isEmpty(); }
    int count() const { return m_samples.size(); }
    qint64 startTimeUsec() const;
    qint64 endTimeUsec() const;
    const QVector<OsdVideoTelemetrySample> &samples() const {
        return m_samples;
    }

    /** Latest state at/before start + max(videoPosition, 0) + offset. */
    OsdVideoTelemetrySample sampleAt(qint64 videoPositionUsec,
                                     qint64 offsetUsec) const;

    static qint64 BucketStartUsec(qint64 timestampUsec);

private:
    QVector<OsdVideoTelemetrySample> m_samples;
};

struct OsdVideoExportOptions
{
    QString videoPath;
    QString tlogPath;
    QString outputPath;
    int timeOffsetSeconds = 0;
    bool fullResolution = false;
    int previewWidth = 960;
    int jpegQuality = 85;
};

class OsdVideoOverlayCore
{
public:
    static constexpr int MinimumOffsetSeconds = -900;
    static constexpr int MaximumOffsetSeconds = 900;
    static constexpr int DefaultFramesPerSecond = 25;

    /** Empty string means valid. */
    static QString Validate(const OsdVideoExportOptions &options);
    static QString DefaultOutputPath(const QString &videoPath);
    static QSize OutputSize(int sourceWidth, int sourceHeight,
                            bool fullResolution, int previewWidth = 960);
    static int ClampFramesPerSecond(double framesPerSecond);
    /** Nearest CFR slot for a non-negative presentation timestamp. */
    static qint64 FrameIndexForTimestamp(qint64 timestampUsec,
                                         int framesPerSecond);
};

#endif // OSDVIDEOOVERLAYCORE_H
