#include <QtTest>

#include "ui/Loghandling/DataFlashFftCore.h"

#include <algorithm>
#include <cmath>
#include <limits>

class DataFlashFftCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void frequencyTableMatchesMp10();
    void hannTransformFindsMagnitudeAndDbPeak();
    void rejectsInvalidInputs();
};

void DataFlashFftCoreTest::frequencyTableMatchesMp10()
{
    const QVector<double> frequencies =
        DataFlashFftCore::FrequencyTable(16, 160.0);
    QCOMPARE(frequencies.size(), 8);
    QCOMPARE(frequencies.at(0), 0.0);
    QCOMPARE(frequencies.at(3), 30.0);
    QCOMPARE(frequencies.last(), 70.0);
    // MP10 performs this calculation with integers and truncates 18.75 Hz.
    // Preserve the physically correct fractional bin centre in the Qt port.
    QCOMPARE(DataFlashFftCore::FrequencyTable(16, 100.0).at(3), 18.75);
    QVERIFY(DataFlashFftCore::FrequencyTable(15, 160.0).isEmpty());
    QVERIFY(DataFlashFftCore::FrequencyTable(1 << 15, 160.0).isEmpty());
    QVERIFY(DataFlashFftCore::FrequencyTable(16, 0.0).isEmpty());
}

void DataFlashFftCoreTest::hannTransformFindsMagnitudeAndDbPeak()
{
    constexpr int bins = 10;
    constexpr int count = 1 << bins;
    constexpr int peakBin = 64;
    QVector<double> samples(count);
    const double pi = std::acos(-1.0);
    for (int index = 0; index < count; ++index) {
        samples[index] = std::sin(2.0 * pi * peakBin * index / count);
    }

    QVector<double> magnitude;
    QString error;
    QVERIFY2(DataFlashFftCore::Transform(
                 samples, bins, false, &magnitude, &error),
             qPrintable(error));
    QCOMPARE(magnitude.size(), count / 2);
    const auto maximum = std::max_element(
        magnitude.cbegin(), magnitude.cend());
    QCOMPARE(static_cast<int>(maximum - magnitude.cbegin()), peakBin);
    QVERIFY(std::abs(*maximum - 1.0) < 1.0e-9);

    QVector<double> decibels;
    QVERIFY(DataFlashFftCore::Transform(
        samples, bins, true, &decibels, &error));
    QVERIFY(std::abs(decibels.at(peakBin)) < 1.0e-8);
}

void DataFlashFftCoreTest::rejectsInvalidInputs()
{
    QVector<double> output;
    QString error;
    QVERIFY(!DataFlashFftCore::Transform(
        QVector<double>(16), 3, false, &output, &error));
    QVERIFY(error.contains(QStringLiteral("between 4 and 14")));
    QVERIFY(!DataFlashFftCore::Transform(
        QVector<double>(15), 4, false, &output, &error));
    QVector<double> invalid(16, 0.0);
    invalid[3] = std::numeric_limits<double>::infinity();
    QVERIFY(!DataFlashFftCore::Transform(
        invalid, 4, false, &output, &error));
    QVERIFY(!DataFlashFftCore::Transform(
        QVector<double>(16), 4, false, nullptr, &error));
}

QTEST_APPLESS_MAIN(DataFlashFftCoreTest)

#include "test_dataflashfftcore.moc"
