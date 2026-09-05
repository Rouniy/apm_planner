#ifndef DATAFLASHFFTANALYZER_H
#define DATAFLASHFFTANALYZER_H

#include <QString>
#include <QVector>

#include <functional>

/** Pure, bounded Mission Planner FFT line-spectrum analysis. */
class DataFlashFftAnalyzer final
{
public:
    static constexpr int MaximumInputSamples = 2 * 1024 * 1024;
    using CancellationCheck = std::function<bool()>;

    struct Options
    {
        int bins = 10;
        double startFrequencyHz = 5.0;
        bool magnitude = false;
        int maximumInputSamples = MaximumInputSamples;
        CancellationCheck isCancelled;
    };

    struct Series
    {
        QString label;
        QVector<double> frequenciesHz;
        QVector<double> values;
        double sampleRateHz = 0.0;
    };

    struct Result
    {
        bool succeeded = false;
        bool cancelled = false;
        QString error;
        QString source;
        double sampleRateHz = 0.0;
        double suggestedNotchHz = 0.0;
        QVector<Series> series;
    };

    struct Segment
    {
        double sampleRateHz = 0.0;
        double startSeconds = 0.0;
        QVector<double> x;
        QVector<double> y;
        QVector<double> z;
    };

    struct SensorData
    {
        QString label;
        bool batch = false;
        QVector<Segment> segments;
    };

    static Result Analyze(const QVector<SensorData> &sensors);
    static Result Analyze(const QVector<SensorData> &sensors,
                          const Options &options);

private:
    DataFlashFftAnalyzer() = delete;
};

#endif // DATAFLASHFFTANALYZER_H
