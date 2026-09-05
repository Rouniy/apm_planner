#include <QtTest>

#include "ui/Loghandling/DataFlashFftAnalyzer.h"

#include <cmath>

namespace
{
DataFlashFftAnalyzer::Segment sineSegment(
    int count, double sampleRate, double xFrequency,
    double yFrequency, double zFrequency, double xScale = 1.0)
{
    DataFlashFftAnalyzer::Segment segment;
    segment.sampleRateHz = sampleRate;
    segment.x.resize(count);
    segment.y.resize(count);
    segment.z.resize(count);
    const double pi = std::acos(-1.0);
    for (int sample = 0; sample < count; ++sample) {
        segment.x[sample] = xScale * std::sin(
            2.0 * pi * xFrequency * sample / sampleRate);
        segment.y[sample] = std::sin(
            2.0 * pi * yFrequency * sample / sampleRate);
        segment.z[sample] = std::sin(
            2.0 * pi * zFrequency * sample / sampleRate);
    }
    return segment;
}

const DataFlashFftAnalyzer::Series *seriesByLabel(
    const DataFlashFftAnalyzer::Result &result, const QString &label)
{
    for (const auto &series : result.series) {
        if (series.label == label) {
            return &series;
        }
    }
    return nullptr;
}
}

class DataFlashFftAnalyzerTest final : public QObject
{
    Q_OBJECT

private slots:
    void oneFullWindowIsAnalyzedAndSuggestsGyroPeak();
    void includesEveryFullWindowInAverage();
    void averagesDbPerWindow();
    void usableBatchGloballyWinsAndSupportsInstanceFive();
    void incompleteBatchFallsBackToImu();
    void validatesBoundsAndCancellation();
};

void DataFlashFftAnalyzerTest::oneFullWindowIsAnalyzedAndSuggestsGyroPeak()
{
    DataFlashFftAnalyzer::SensorData gyro;
    gyro.label = QStringLiteral("IMU GYR");
    gyro.segments.append(sineSegment(256, 256.0, 40.0, 60.0, 80.0, 3.0));
    DataFlashFftAnalyzer::Options options;
    options.bins = 8;
    options.startFrequencyHz = 20.0;
    options.magnitude = true;

    const auto result = DataFlashFftAnalyzer::Analyze({gyro}, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("IMU"));
    QCOMPARE(result.series.size(), 3);
    QVERIFY(std::abs(result.suggestedNotchHz - 40.0) < 0.01);
    const auto *x = seriesByLabel(result, QStringLiteral("IMU GYR x"));
    QVERIFY(x);
    QCOMPARE(x->values.at(10), 0.0); // 10 Hz is below the cutoff.
    QVERIFY(std::abs(x->values.at(40) - 3.0) < 1.0e-8);
}

void DataFlashFftAnalyzerTest::includesEveryFullWindowInAverage()
{
    DataFlashFftAnalyzer::Segment segment;
    segment.sampleRateHz = 64.0;
    segment.x = QVector<double>(128, 0.0);
    segment.y = QVector<double>(128, 0.0);
    segment.z = QVector<double>(128, 0.0);
    const double pi = std::acos(-1.0);
    for (int index = 0; index < 64; ++index) {
        segment.x[64 + index] = std::sin(2.0 * pi * 8.0 * index / 64.0);
    }
    DataFlashFftAnalyzer::SensorData sensor;
    sensor.label = QStringLiteral("IMU ACC");
    sensor.segments.append(segment);
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = true;
    options.startFrequencyHz = 0.0;

    const auto result = DataFlashFftAnalyzer::Analyze({sensor}, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    const auto *x = seriesByLabel(result, QStringLiteral("IMU ACC x"));
    QVERIFY(x);
    // MP10 accidentally skipped the final complete window. Qt deliberately
    // includes both, so zero + unit magnitude averages to one half.
    QVERIFY(std::abs(x->values.at(8) - 0.5) < 1.0e-8);
}

void DataFlashFftAnalyzerTest::averagesDbPerWindow()
{
    DataFlashFftAnalyzer::Segment segment;
    segment.sampleRateHz = 64.0;
    segment.x = QVector<double>(128, 0.0);
    segment.y = QVector<double>(128, 0.0);
    segment.z = QVector<double>(128, 0.0);
    const double pi = std::acos(-1.0);
    for (int index = 0; index < 64; ++index) {
        const double sample = std::sin(2.0 * pi * 8.0 * index / 64.0);
        segment.x[index] = sample;
        segment.x[64 + index] = sample * 0.1;
    }
    DataFlashFftAnalyzer::SensorData sensor;
    sensor.label = QStringLiteral("IMU ACC");
    sensor.segments.append(segment);
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = false;
    options.startFrequencyHz = 0.0;

    const auto result = DataFlashFftAnalyzer::Analyze({sensor}, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    const auto *x = seriesByLabel(result, QStringLiteral("IMU ACC x"));
    QVERIFY(x);
    // Match MP's arithmetic averaging of each window's dB spectrum: the
    // average of 0 dB and -20 dB is -10 dB (not 20*log10(0.55)).
    QVERIFY(std::abs(x->values.at(8) + 10.0) < 1.0e-8);
}

void DataFlashFftAnalyzerTest::usableBatchGloballyWinsAndSupportsInstanceFive()
{
    DataFlashFftAnalyzer::SensorData direct;
    direct.label = QStringLiteral("IMU GYR");
    direct.segments.append(sineSegment(64, 64.0, 8.0, 9.0, 10.0));
    DataFlashFftAnalyzer::SensorData batch;
    batch.label = QStringLiteral("GYR5");
    batch.batch = true;
    batch.segments.append(sineSegment(64, 64.0, 12.0, 14.0, 16.0));
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = true;

    const auto result = DataFlashFftAnalyzer::Analyze(
        {direct, batch}, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("ISBH/ISBD"));
    QCOMPARE(result.series.size(), 3);
    QVERIFY(seriesByLabel(result, QStringLiteral("GYR5 x")));
    QVERIFY(!seriesByLabel(result, QStringLiteral("IMU GYR x")));
}

void DataFlashFftAnalyzerTest::incompleteBatchFallsBackToImu()
{
    DataFlashFftAnalyzer::SensorData batch;
    batch.label = QStringLiteral("ACC0");
    batch.batch = true;
    batch.segments.append(sineSegment(32, 64.0, 8.0, 9.0, 10.0));
    DataFlashFftAnalyzer::SensorData direct;
    direct.label = QStringLiteral("IMU ACC");
    direct.segments.append(sineSegment(64, 64.0, 8.0, 9.0, 10.0));
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;

    const auto result = DataFlashFftAnalyzer::Analyze(
        {batch, direct}, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("IMU"));
    QVERIFY(seriesByLabel(result, QStringLiteral("IMU ACC x")));
}

void DataFlashFftAnalyzerTest::validatesBoundsAndCancellation()
{
    DataFlashFftAnalyzer::SensorData sensor;
    sensor.label = QStringLiteral("IMU ACC");
    sensor.segments.append(sineSegment(64, 64.0, 8.0, 9.0, 10.0));
    DataFlashFftAnalyzer::Options options;
    options.bins = 3;
    QVERIFY(!DataFlashFftAnalyzer::Analyze({sensor}, options).succeeded);
    options.bins = 6;
    options.startFrequencyHz = 1001.0;
    QVERIFY(!DataFlashFftAnalyzer::Analyze({sensor}, options).succeeded);
    options.startFrequencyHz = 5.0;
    options.maximumInputSamples = 32;
    QVERIFY(DataFlashFftAnalyzer::Analyze({sensor}, options).error.contains(
        QStringLiteral("bounded")));
    options.maximumInputSamples = 64;
    options.isCancelled = []() { return true; };
    const auto cancelled = DataFlashFftAnalyzer::Analyze({sensor}, options);
    QVERIFY(cancelled.cancelled);
    QVERIFY(!cancelled.succeeded);
}

QTEST_APPLESS_MAIN(DataFlashFftAnalyzerTest)

#include "test_dataflashfftanalyzer.moc"
