#ifndef DATAFLASHFFTCORE_H
#define DATAFLASHFFTCORE_H

#include <QString>
#include <QVector>

/** Dependency-free variable-size FFT math matching Mission Planner FFT2.rin. */
class DataFlashFftCore final
{
public:
    static constexpr int MinimumBins = 4;
    static constexpr int MaximumBins = 14;

    static bool IsValidBins(int bins);
    static QVector<double> FrequencyTable(int sampleCount,
                                          double sampleRateHz);

    /**
     * Applies MP10's normalized Hann window and returns N/2 magnitudes.
     * outputLog selects 20*log10(magnitude), matching FFT2.rin.
     */
    static bool Transform(const QVector<double> &samples,
                          int bins,
                          bool outputLog,
                          QVector<double> *values,
                          QString *error = nullptr);

private:
    DataFlashFftCore() = delete;
};

#endif // DATAFLASHFFTCORE_H
