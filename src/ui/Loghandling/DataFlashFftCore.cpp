#include "DataFlashFftCore.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

bool DataFlashFftCore::IsValidBins(int bins)
{
    return bins >= MinimumBins && bins <= MaximumBins;
}

QVector<double> DataFlashFftCore::FrequencyTable(int sampleCount,
                                                double sampleRateHz)
{
    QVector<double> frequencies;
    if (sampleCount < (1 << MinimumBins)
        || sampleCount > (1 << MaximumBins)
        || (sampleCount & (sampleCount - 1)) != 0
        || !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0) {
        return frequencies;
    }
    frequencies.resize(sampleCount / 2);
    for (int index = 0; index < frequencies.size(); ++index) {
        frequencies[index] = static_cast<double>(index) * sampleRateHz
            / static_cast<double>(sampleCount);
    }
    return frequencies;
}

bool DataFlashFftCore::Transform(const QVector<double> &samples,
                                 int bins,
                                 bool outputLog,
                                 QVector<double> *values,
                                 QString *error)
{
    if (values) {
        values->clear();
    }
    if (!values) {
        if (error) {
            *error = QStringLiteral("FFT output is missing.");
        }
        return false;
    }
    if (!IsValidBins(bins)) {
        if (error) {
            *error = QStringLiteral("FFT bins must be between 4 and 14.");
        }
        return false;
    }
    const int sampleCount = 1 << bins;
    if (samples.size() != sampleCount) {
        if (error) {
            *error = QStringLiteral("FFT requires exactly %1 samples.")
                         .arg(sampleCount);
        }
        return false;
    }

    using Complex = std::complex<double>;
    QVector<Complex> spectrum(sampleCount);
    const double pi = std::acos(-1.0);
    for (int index = 0; index < sampleCount; ++index) {
        const double sample = samples.at(index);
        if (!std::isfinite(sample)) {
            if (error) {
                *error = QStringLiteral("FFT input contains a non-finite sample.");
            }
            return false;
        }
        // Exact Mission Planner FFT2.rin normalization.
        const double window = (4.0 / sampleCount) * 0.5
            * (1.0 - std::cos(2.0 * pi * index / sampleCount));
        spectrum[index] = Complex(window * sample, 0.0);
    }

    for (int index = 1, reversed = 0; index < sampleCount; ++index) {
        int bit = sampleCount >> 1;
        while (reversed & bit) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(spectrum[index], spectrum[reversed]);
        }
    }
    for (int length = 2; length <= sampleCount; length <<= 1) {
        const Complex root = std::polar(1.0, -2.0 * pi / length);
        for (int offset = 0; offset < sampleCount; offset += length) {
            Complex twiddle(1.0, 0.0);
            for (int index = 0; index < length / 2; ++index) {
                const Complex even = spectrum[offset + index];
                const Complex odd = spectrum[offset + index + length / 2]
                    * twiddle;
                spectrum[offset + index] = even + odd;
                spectrum[offset + index + length / 2] = even - odd;
                twiddle *= root;
            }
        }
    }

    values->resize(sampleCount / 2);
    const double scale = 20.0 / std::log(10.0);
    for (int index = 0; index < values->size(); ++index) {
        double magnitude = std::abs(spectrum.at(index));
        if (outputLog) {
            // C# double.Epsilon is the smallest positive subnormal value.
            magnitude = scale * std::log(
                magnitude + std::numeric_limits<double>::denorm_min());
        }
        (*values)[index] = magnitude;
    }
    if (error) {
        error->clear();
    }
    return true;
}
