#ifndef GEOREFSERVICE_H
#define GEOREFSERVICE_H

#include <QDateTime>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

class GeoRefService final
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    enum class Mode { Cam, Trig, TimeOffset };
    struct Options {
        QString logPath, photoDirectory;
        QString outputDirectory; // Empty means photoDirectory/geotagged.
        Mode mode = Mode::Cam;
        double timeOffsetSeconds = 0;
        bool useGps2 = false;
        int shutterLagMilliseconds = 0;
        bool useAmslAltitude = false, useGpsAltitude = false;
        double baseAltitudeAdjustmentMeters = 0;
    };
    struct Match {
        QString sourcePath, outputPath;
        QDateTime timeUtc;
        double latitude = 0, longitude = 0, altitude = 0;
        double roll = 0, pitch = 0, yaw = 0;
    };
    class Plan {
    public:
        Plan();
        bool isValid() const;
        Options options() const;
        QVector<Match> matches() const;
        QStringList outputPaths() const;
        QStringList warnings() const;
    private:
        struct Data;
        std::shared_ptr<const Data> d;
        explicit Plan(std::shared_ptr<const Data> data);
        friend class GeoRefService;
    };
    struct PlanResult {
        bool success = false, cancelled = false;
        QString error;
        QStringList warnings;
        std::shared_ptr<const Plan> plan;
    };
    struct EstimateResult {
        bool success = false, cancelled = false, hasEstimate = false;
        double offsetSeconds = 0;
        QString error;
        QStringList warnings;
    };
    struct Result {
        bool success = false, cancelled = false;
        QString error;
        QStringList publishedPaths, failedPaths, warnings;
        QVector<Match> matches;
        int taggedPhotos = 0, failedPhotos = 0;
    };
    static PlanResult Prepare(const Options &options, const Cancel &cancel = {}, const Progress &progress = {});
    static EstimateResult Estimate(const Options &options, const Cancel &cancel = {}, const Progress &progress = {});
    static Result Execute(const Plan &plan, const Cancel &cancel = {}, const Progress &progress = {});
};

#endif
