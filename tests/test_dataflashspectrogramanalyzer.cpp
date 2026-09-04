#include "ui/Loghandling/DataFlashSpectrogramAnalyzer.h"

#include <QtTest>

#include <cmath>
#include <limits>
#include <utility>

namespace {

using Analyzer = DataFlashSpectrogramAnalyzer;
using Row = QList<QPair<QString, QVariant>>;

constexpr double kPi = 3.14159265358979323846;

double sine(int sample, double frequencyHz, double sampleRateHz,
            double amplitude = 1.0)
{
    return amplitude
           * std::sin(2.0 * kPi * frequencyHz * sample / sampleRateHz);
}

Row directRow(double timeUs, double x, double y, double z)
{
    return {{QStringLiteral("TimeUS"), QVariant(timeUs)},
            {QStringLiteral("AccX"), QVariant(x)},
            {QStringLiteral("AccY"), QVariant(y)},
            {QStringLiteral("AccZ"), QVariant(z)}};
}

Analyzer::BuildResult directSource(int sampleCount = 2048,
                                   double sampleRateHz = 1024.0)
{
    Analyzer::RecordBuilder builder(QStringLiteral("ACC1"));
    for (int sample = 0; sample < sampleCount; ++sample) {
        const Analyzer::RecordDisposition disposition = builder.addRecord(
            QStringLiteral("ACC1"),
            directRow(sample * 1000000.0 / sampleRateHz,
                      sine(sample, 64.0, sampleRateHz),
                      sine(sample, 128.0, sampleRateHz),
                      sine(sample, 192.0, sampleRateHz)));
        if (disposition != Analyzer::RecordDisposition::Accepted) {
            return builder.finish();
        }
    }
    return builder.finish();
}

Row batchHeader(quint64 number, int type, int instance, double rate,
                double multiplier, int count, double sampleTimeUs)
{
    return {{QStringLiteral("TimeUS"), QVariant(sampleTimeUs - 10.0)},
            {QStringLiteral("N"), QVariant::fromValue(number)},
            {QStringLiteral("type"), QVariant(type)},
            {QStringLiteral("instance"), QVariant(instance)},
            {QStringLiteral("mul"), QVariant(multiplier)},
            {QStringLiteral("smp_rate"), QVariant(rate)},
            {QStringLiteral("smp_cnt"), QVariant(count)},
            {QStringLiteral("SampleUS"), QVariant(sampleTimeUs)}};
}

Row batchData(quint64 number, int sequence, double rate)
{
    QVariantList x;
    QVariantList y;
    QVariantList z;
    x.reserve(32);
    y.reserve(32);
    z.reserve(32);
    for (int offset = 0; offset < 32; ++offset) {
        const int sample = sequence * 32 + offset;
        x.append(qRound(sine(sample, 40.0, rate, 1000.0)));
        y.append(qRound(sine(sample, 80.0, rate, 1000.0)));
        z.append(qRound(sine(sample, 120.0, rate, 1000.0)));
    }
    return {{QStringLiteral("N"), QVariant::fromValue(number)},
            {QStringLiteral("seqno"), QVariant(sequence)},
            {QStringLiteral("x"), QVariant(x)},
            {QStringLiteral("y"), QVariant(y)},
            {QStringLiteral("z"), QVariant(z)}};
}

} // namespace

class DataFlashSpectrogramAnalyzerTest final : public QObject
{
    Q_OBJECT

private slots:
    void directRowsProduceDistinctAxisSpectra();
    void batchRowsMapFifthSensorAndReconstructSamples();
    void incompleteBatchFallsBackToDirectRows();
    void validatesDecibelRangeAndFiniteSamples();
    void cancellationIsTyped();
    void rasterWidthIsBoundedWithoutBlankColumns();
    void rasterPoolingRetainsTransientWindows();
    void builderInputLimitIsTyped();
};

void DataFlashSpectrogramAnalyzerTest::directRowsProduceDistinctAxisSpectra()
{
    const Analyzer::BuildResult build = directSource();
    QVERIFY2(build.ok(), qPrintable(build.message));
    QCOMPARE(static_cast<int>(build.source.kind),
             static_cast<int>(Analyzer::SourceKind::Direct));
    QCOMPARE(build.source.sensorName, QStringLiteral("ACC1"));
    QCOMPARE(build.source.sampleCount, 2048);
    QCOMPARE(build.source.segments.size(), 1);
    QVERIFY(std::abs(build.source.sampleRateHz - 1024.0) < 1.0e-6);

    Analyzer::AnalysisOptions options;
    options.minimumDb = -120.0;
    options.maximumDb = 20.0;
    const Analyzer::AnalysisResult result =
        Analyzer::Analyze(build.source, options);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.frequencyBinsHz.size(), Analyzer::FrequencyBinCount);
    QCOMPARE(result.axes[0].image.size(),
             QSize(result.renderedWindowCount, Analyzer::FrequencyBinCount));
    QCOMPARE(result.axes[1].image.size(), result.axes[0].image.size());
    QCOMPARE(result.axes[2].image.size(), result.axes[0].image.size());
    QVERIFY(result.axes[0].image != result.axes[1].image);
    QVERIFY(result.axes[1].image != result.axes[2].image);

    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 64.0) < 1.0e-6);
    QVERIFY(std::abs(result.axes[1].strongestFrequencyHz - 128.0) < 1.0e-6);
    QVERIFY(std::abs(result.axes[2].strongestFrequencyHz - 192.0) < 1.0e-6);
    QVERIFY(std::abs(result.axes[0].strongestDecibels) < 0.05);
    QCOMPARE(result.axes[0].peakFrequenciesHz.size(),
             result.renderedWindowCount);
}

void DataFlashSpectrogramAnalyzerTest::batchRowsMapFifthSensorAndReconstructSamples()
{
    constexpr double sampleRate = 1024.0;
    Analyzer::RecordBuilder builder(QStringLiteral("GYR5"));

    // Sensor 4 must not be mistaken for sensor 5 when N is later reused.
    QCOMPARE(static_cast<int>(builder.addRecord(
                 QStringLiteral("ISBH"),
                 batchHeader(42, 1, 3, sampleRate, 100.0,
                             Analyzer::FftSize, 1000000.0))),
             static_cast<int>(Analyzer::RecordDisposition::Ignored));
    QCOMPARE(static_cast<int>(builder.addRecord(
                 QStringLiteral("ISBD"), batchData(42, 0, sampleRate))),
             static_cast<int>(Analyzer::RecordDisposition::Ignored));

    QCOMPARE(static_cast<int>(builder.addRecord(
                 QStringLiteral("ISBH"),
                 batchHeader(42, 1, 4, sampleRate, 100.0,
                             Analyzer::FftSize, 2000000.0))),
             static_cast<int>(Analyzer::RecordDisposition::Accepted));
    for (int sequence = 0; sequence < Analyzer::FftSize / 32; ++sequence) {
        QCOMPARE(static_cast<int>(builder.addRecord(
                     QStringLiteral("ISBD"),
                     batchData(42, sequence, sampleRate))),
                 static_cast<int>(Analyzer::RecordDisposition::Accepted));
    }

    const Analyzer::BuildResult build = builder.finish();
    QVERIFY2(build.ok(), qPrintable(build.message));
    QCOMPARE(static_cast<int>(build.source.kind),
             static_cast<int>(Analyzer::SourceKind::Batch));
    QCOMPARE(build.source.sensorName, QStringLiteral("GYR5"));
    QCOMPARE(build.source.sampleCount, Analyzer::FftSize);
    QCOMPARE(build.source.segments.size(), 1);
    QCOMPARE(build.source.segments.constFirst().samples.size(),
             Analyzer::FftSize);
    QCOMPARE(build.source.segments.constFirst().samples.constFirst().timeSeconds,
             2.0);

    Analyzer::AnalysisOptions options;
    options.minimumDb = -100.0;
    options.maximumDb = 30.0;
    const Analyzer::AnalysisResult result =
        Analyzer::Analyze(build.source, options);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.renderedWindowCount, 1);
    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 40.0) < 1.0e-6);
    QVERIFY(std::abs(result.axes[1].strongestFrequencyHz - 80.0) < 1.0e-6);
    QVERIFY(std::abs(result.axes[2].strongestFrequencyHz - 120.0) < 1.0e-6);
    QVERIFY(result.axes[0].image != result.axes[1].image);
    QVERIFY(result.axes[1].image != result.axes[2].image);
}

void DataFlashSpectrogramAnalyzerTest::incompleteBatchFallsBackToDirectRows()
{
    constexpr double sampleRate = 1024.0;
    Analyzer::RecordBuilder builder(QStringLiteral("ACC1"));
    for (int sample = 0; sample < Analyzer::FftSize; ++sample) {
        QCOMPARE(static_cast<int>(builder.addRecord(
                     QStringLiteral("ACC1"),
                     directRow(sample * 1000000.0 / sampleRate,
                               sine(sample, 32.0, sampleRate),
                               sine(sample, 48.0, sampleRate),
                               sine(sample, 64.0, sampleRate)))),
                 static_cast<int>(Analyzer::RecordDisposition::Accepted));
    }
    QCOMPARE(static_cast<int>(builder.addRecord(
                 QStringLiteral("ISBH"),
                 batchHeader(9, 0, 0, sampleRate, 100.0,
                             Analyzer::FftSize, 3000000.0))),
             static_cast<int>(Analyzer::RecordDisposition::Accepted));
    // Sequence zero is absent, so these packets cannot form a contiguous FFT.
    for (int sequence = 1; sequence < Analyzer::FftSize / 32; ++sequence) {
        QCOMPARE(static_cast<int>(builder.addRecord(
                     QStringLiteral("ISBD"),
                     batchData(9, sequence, sampleRate))),
                 static_cast<int>(Analyzer::RecordDisposition::Accepted));
    }

    const Analyzer::BuildResult build = builder.finish();
    QVERIFY2(build.ok(), qPrintable(build.message));
    QCOMPARE(static_cast<int>(build.source.kind),
             static_cast<int>(Analyzer::SourceKind::Direct));
    QCOMPARE(build.source.sampleCount, Analyzer::FftSize);
}

void DataFlashSpectrogramAnalyzerTest::validatesDecibelRangeAndFiniteSamples()
{
    const Analyzer::BuildResult build = directSource(Analyzer::FftSize);
    QVERIFY2(build.ok(), qPrintable(build.message));

    Analyzer::AnalysisOptions options;
    options.minimumDb = -20.0;
    options.maximumDb = -20.0;
    Analyzer::AnalysisResult result = Analyzer::Analyze(build.source, options);
    QCOMPARE(static_cast<int>(result.error),
             static_cast<int>(Analyzer::ErrorCode::InvalidDecibelRange));

    options.minimumDb = -80.0;
    options.maximumDb = std::numeric_limits<double>::infinity();
    result = Analyzer::Analyze(build.source, options);
    QCOMPARE(static_cast<int>(result.error),
             static_cast<int>(Analyzer::ErrorCode::InvalidDecibelRange));

    Analyzer::RecordBuilder invalidBuilder(QStringLiteral("ACC1"));
    QCOMPARE(static_cast<int>(invalidBuilder.addDirectRecord(
                 QStringLiteral("ACC1"), 0.0,
                 std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0)),
             static_cast<int>(Analyzer::RecordDisposition::Rejected));
    const Analyzer::BuildResult invalid = invalidBuilder.finish();
    QCOMPARE(static_cast<int>(invalid.error),
             static_cast<int>(Analyzer::ErrorCode::InvalidRecord));
}

void DataFlashSpectrogramAnalyzerTest::cancellationIsTyped()
{
    const Analyzer::BuildResult build = directSource(Analyzer::FftSize);
    QVERIFY2(build.ok(), qPrintable(build.message));

    Analyzer::AnalysisOptions options;
    options.isCancelled = [] { return true; };
    const Analyzer::AnalysisResult result =
        Analyzer::Analyze(build.source, options);
    QCOMPARE(static_cast<int>(result.error),
             static_cast<int>(Analyzer::ErrorCode::Cancelled));
    QVERIFY(result.axes[0].image.isNull());
}

void DataFlashSpectrogramAnalyzerTest::rasterWidthIsBoundedWithoutBlankColumns()
{
    const Analyzer::BuildResult build = directSource(4096);
    QVERIFY2(build.ok(), qPrintable(build.message));

    Analyzer::AnalysisOptions options;
    options.minimumDb = -120.0;
    options.maximumDb = 20.0;
    options.maximumRasterWidth = 3;
    const Analyzer::AnalysisResult result =
        Analyzer::Analyze(build.source, options);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.availableWindowCount, 13);
    QCOMPARE(result.renderedWindowCount, 3);
    QCOMPARE(result.axes[0].image.width(), 3);
    QCOMPARE(result.windowTimesSeconds.size(), 3);
    QCOMPARE(result.windowTimesSeconds.constFirst(), 0.0);
    QCOMPARE(result.windowTimesSeconds.constLast(), 3.0);
    for (int column = 0; column < result.renderedWindowCount; ++column) {
        const QRgb firstPixel = result.axes[0].image.pixel(column, 0);
        bool containsSpectrum = false;
        for (int row = 1; row < Analyzer::FrequencyBinCount; ++row) {
            if (result.axes[0].image.pixel(column, row) != firstPixel) {
                containsSpectrum = true;
                break;
            }
        }
        QVERIFY(containsSpectrum);
    }
}

void DataFlashSpectrogramAnalyzerTest::rasterPoolingRetainsTransientWindows()
{
    constexpr double sampleRate = 1024.0;
    Analyzer::Source source;
    source.sensorName = QStringLiteral("ACC1");
    source.kind = Analyzer::SourceKind::Batch;
    source.sampleRateHz = sampleRate;
    for (int segmentIndex = 0; segmentIndex < 6; ++segmentIndex) {
        Analyzer::SampleSegment segment;
        segment.sampleRateHz = sampleRate;
        segment.samples.reserve(Analyzer::FftSize);
        for (int sample = 0; sample < Analyzer::FftSize; ++sample) {
            const double value = segmentIndex == 1
                ? sine(sample, 200.0, sampleRate) : 0.0;
            segment.samples.append(
                {segmentIndex * 2.0 + sample / sampleRate,
                 value, value, value});
        }
        source.sampleCount += segment.samples.size();
        source.segments.append(std::move(segment));
    }

    Analyzer::AnalysisOptions options;
    options.minimumDb = -120.0;
    options.maximumDb = 20.0;
    options.maximumRasterWidth = 3;
    const Analyzer::AnalysisResult result = Analyzer::Analyze(source, options);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.availableWindowCount, 6);
    QCOMPARE(result.renderedWindowCount, 3);
    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 200.0) < 1.0e-6);
    QCOMPARE(result.startSeconds, 0.0);
    QVERIFY(std::abs(result.endSeconds - (10.0 + 1023.0 / sampleRate))
            < 1.0e-9);
}

void DataFlashSpectrogramAnalyzerTest::builderInputLimitIsTyped()
{
    Analyzer::BuildOptions options;
    options.maximumInputSamples = 8;
    Analyzer::RecordBuilder builder(QStringLiteral("ACC1"), options);
    for (int sample = 0; sample < 8; ++sample) {
        QCOMPARE(static_cast<int>(builder.addDirectRecord(
                     QStringLiteral("ACC1"), sample * 1000.0,
                     sample, sample, sample)),
                 static_cast<int>(Analyzer::RecordDisposition::Accepted));
    }
    QCOMPARE(static_cast<int>(builder.addDirectRecord(
                 QStringLiteral("ACC1"), 8000.0, 8.0, 8.0, 8.0)),
             static_cast<int>(Analyzer::RecordDisposition::LimitExceeded));
    const Analyzer::BuildResult result = builder.finish();
    QCOMPARE(static_cast<int>(result.error),
             static_cast<int>(Analyzer::ErrorCode::TooManySamples));
    QVERIFY(!result.message.isEmpty());
}

QTEST_APPLESS_MAIN(DataFlashSpectrogramAnalyzerTest)

#include "test_dataflashspectrogramanalyzer.moc"
