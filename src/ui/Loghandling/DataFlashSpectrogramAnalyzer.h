#ifndef DATAFLASHSPECTROGRAMANALYZER_H
#define DATAFLASHSPECTROGRAMANALYZER_H

#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariant>
#include <QVector>

#include <array>
#include <functional>
#include <memory>

/**
 * Offline, parser-independent DataFlash spectrogram analysis.
 *
 * RecordBuilder is deliberately shaped like LogdataStorage::addDataRow(): a
 * parser adapter can forward decoded rows without retaining the complete log.
 * The analyzer itself has no QObject, parser, vehicle, or UI dependency and is
 * safe to run in a worker thread.
 */
class DataFlashSpectrogramAnalyzer final
{
public:
    static constexpr int FftSize = 1024;
    static constexpr int FrequencyBinCount = FftSize / 2;
    static constexpr int AxisCount = 3;

    // About 64 MiB of sample payload. This is a hard ceiling; tests and callers
    // may request a smaller limit, but never a larger one.
    static constexpr int MaximumInputSamples = 2 * 1024 * 1024;
    static constexpr int MaximumRasterWidth = 4096;
    static constexpr int DefaultRasterWidth = 2048;

    using CancellationCheck = std::function<bool()>;

    enum class ErrorCode {
        None,
        InvalidSensor,
        InvalidOptions,
        InvalidRecord,
        InvalidDecibelRange,
        InvalidSampleRate,
        NoData,
        NotEnoughSamples,
        TooManySamples,
        Cancelled
    };

    enum class SourceKind {
        Direct,
        Batch
    };

    enum class RecordDisposition {
        Ignored,
        Accepted,
        Rejected,
        LimitExceeded,
        Cancelled
    };

    struct SampleFrame {
        double timeSeconds{0.0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    struct SampleSegment {
        double sampleRateHz{0.0};
        QVector<SampleFrame> samples;
    };

    struct Source {
        QString sensorName;
        SourceKind kind{SourceKind::Direct};
        double sampleRateHz{0.0};
        int sampleCount{0};
        int rejectedRecordCount{0};
        QVector<SampleSegment> segments;
    };

    struct BuildResult {
        ErrorCode error{ErrorCode::None};
        QString message;
        Source source;

        bool ok() const { return error == ErrorCode::None; }
    };

    struct BuildOptions {
        int maximumInputSamples{MaximumInputSamples};
        CancellationCheck isCancelled;
    };

    struct AnalysisOptions {
        double minimumDb{-80.0};
        double maximumDb{-20.0};
        int maximumRasterWidth{DefaultRasterWidth};
        CancellationCheck isCancelled;
    };

    struct AxisResult {
        QImage image;
        QVector<double> peakFrequenciesHz;
        QVector<double> peakDecibels;
        double strongestFrequencyHz{0.0};
        double strongestDecibels{0.0};
    };

    struct AnalysisResult {
        ErrorCode error{ErrorCode::None};
        QString message;
        QString sensorName;
        SourceKind sourceKind{SourceKind::Direct};
        double sampleRateHz{0.0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        double maximumFrequencyHz{0.0};
        int inputSampleCount{0};
        int availableWindowCount{0};
        int renderedWindowCount{0};
        QVector<double> frequencyBinsHz;
        QVector<double> windowTimesSeconds;
        std::array<AxisResult, AxisCount> axes;

        bool ok() const { return error == ErrorCode::None; }
    };

    class RecordBuilder final
    {
    public:
        explicit RecordBuilder(const QString &sensorName);
        RecordBuilder(const QString &sensorName, const BuildOptions &options);
        ~RecordBuilder();

        RecordBuilder(RecordBuilder &&other) noexcept;
        RecordBuilder &operator=(RecordBuilder &&other) noexcept;

        RecordBuilder(const RecordBuilder &) = delete;
        RecordBuilder &operator=(const RecordBuilder &) = delete;

        /**
         * Accepts a decoded DataFlash row. Unrelated rows are ignored. Direct
         * ACC/GYR rows use SampleUS (or TimeUS) and AccX/Y/Z or GyrX/Y/Z.
         * ISBH/ISBD rows use N, type, instance, smp_rate, mul, smp_cnt,
         * SampleUS/TimeUS, seqno, and x/y/z list fields.
         */
        RecordDisposition addRecord(
            const QString &typeName,
            const QList<QPair<QString, QVariant>> &values);

        RecordDisposition addDirectRecord(const QString &typeName,
                                          double timestampMicroseconds,
                                          double x,
                                          double y,
                                          double z);
        RecordDisposition addBatchHeader(quint64 batchNumber,
                                         int sensorType,
                                         int instance,
                                         double sampleRateHz,
                                         double multiplier,
                                         int sampleCount,
                                         double sampleTimestampMicroseconds);
        RecordDisposition addBatchData(quint64 batchNumber,
                                       int sequenceNumber,
                                       const QVector<double> &x,
                                       const QVector<double> &y,
                                       const QVector<double> &z);

        /** Moves the bounded sample source out of the builder. */
        BuildResult finish();

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

    static bool IsSupportedSensor(const QString &sensorName);
    static AnalysisResult Analyze(const Source &source);
    static AnalysisResult Analyze(const Source &source,
                                  const AnalysisOptions &options);
};

#endif // DATAFLASHSPECTROGRAMANALYZER_H
