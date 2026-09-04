#include "DataFlashSpectrogramAnalyzer.h"

#include <QColor>
#include <QHash>
#include <QMap>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <initializer_list>
#include <limits>
#include <utility>

namespace {

constexpr double kMicrosecondsToSeconds = 1.0e-6;
constexpr int kMaximumPendingBatches = 64;

struct SensorSelection {
    int type{-1};       // ArduPilot ISBH: 0 = accelerometer, 1 = gyroscope
    int instance{-1};   // ArduPilot instance is zero based
    QString name;
};

bool isFiniteNumber(double value)
{
    return std::isfinite(value);
}

bool parseSensorName(const QString &sensorName, SensorSelection *selection)
{
    const QString name = sensorName.trimmed().toUpper();
    int type = -1;
    QString suffix;
    if (name.startsWith(QStringLiteral("ACC"))) {
        type = 0;
        suffix = name.mid(3);
    } else if (name.startsWith(QStringLiteral("GYR"))) {
        type = 1;
        suffix = name.mid(3);
    } else {
        return false;
    }

    bool ok = false;
    const int oneBasedInstance = suffix.toInt(&ok);
    if (!ok || oneBasedInstance < 1 || oneBasedInstance > 5
        || suffix != QString::number(oneBasedInstance)) {
        return false;
    }

    if (selection) {
        selection->type = type;
        selection->instance = oneBasedInstance - 1;
        selection->name = (type == 0 ? QStringLiteral("ACC")
                                     : QStringLiteral("GYR"))
                          + QString::number(oneBasedInstance);
    }
    return true;
}

bool parseDirectType(const QString &typeName, int *sensorType, int *instance,
                     bool *hasExplicitInstance = nullptr)
{
    const QString name = typeName.trimmed().toUpper();
    int type = -1;
    QString suffix;
    if (name.startsWith(QStringLiteral("ACC"))) {
        type = 0;
        suffix = name.mid(3);
    } else if (name.startsWith(QStringLiteral("GYR"))) {
        type = 1;
        suffix = name.mid(3);
    } else {
        return false;
    }

    int zeroBasedInstance = 0;
    const bool explicitInstance = !suffix.isEmpty();
    if (explicitInstance) {
        bool ok = false;
        const int oneBasedInstance = suffix.toInt(&ok);
        if (!ok || oneBasedInstance < 1 || oneBasedInstance > 5
            || suffix != QString::number(oneBasedInstance)) {
            return false;
        }
        zeroBasedInstance = oneBasedInstance - 1;
    }

    if (sensorType) {
        *sensorType = type;
    }
    if (instance) {
        *instance = zeroBasedInstance;
    }
    if (hasExplicitInstance) {
        *hasExplicitInstance = explicitInstance;
    }
    return true;
}

const QVariant *findValue(const QList<QPair<QString, QVariant>> &values,
                          std::initializer_list<const char *> names)
{
    // The candidate order expresses semantic priority (for example SampleUS
    // is the first sample timestamp while TimeUS is only a fallback).
    for (const char *name : names) {
        for (const auto &entry : values) {
            if (entry.first.compare(QLatin1String(name), Qt::CaseInsensitive)
                == 0) {
                return &entry.second;
            }
        }
    }
    return nullptr;
}

bool variantToFiniteDouble(const QVariant *variant, double *result)
{
    if (!variant) {
        return false;
    }
    bool ok = false;
    const double value = variant->toDouble(&ok);
    if (!ok || !isFiniteNumber(value)) {
        return false;
    }
    if (result) {
        *result = value;
    }
    return true;
}

bool variantToInteger(const QVariant *variant, int minimum, int maximum,
                      int *result)
{
    double value = 0.0;
    if (!variantToFiniteDouble(variant, &value)
        || std::floor(value) != value || value < minimum || value > maximum) {
        return false;
    }
    if (result) {
        *result = static_cast<int>(value);
    }
    return true;
}

bool variantToBatchNumber(const QVariant *variant, quint64 *result)
{
    if (!variant) {
        return false;
    }
    bool ok = false;
    const quint64 value = variant->toULongLong(&ok);
    if (!ok) {
        return false;
    }
    if (result) {
        *result = value;
    }
    return true;
}

bool variantToArray(const QVariant *variant, QVector<double> *result)
{
    if (!variant || !result) {
        return false;
    }

    const QVariantList values = variant->toList();
    if (values.isEmpty()) {
        return false;
    }

    QVector<double> converted;
    converted.reserve(values.size());
    for (const QVariant &value : values) {
        double number = 0.0;
        if (!variantToFiniteDouble(&value, &number)) {
            return false;
        }
        converted.append(number);
    }
    *result = std::move(converted);
    return true;
}

QString errorMessage(DataFlashSpectrogramAnalyzer::ErrorCode error)
{
    using ErrorCode = DataFlashSpectrogramAnalyzer::ErrorCode;
    switch (error) {
    case ErrorCode::None:
        return QString();
    case ErrorCode::InvalidSensor:
        return QStringLiteral("Sensor must be ACC1..ACC5 or GYR1..GYR5.");
    case ErrorCode::InvalidOptions:
        return QStringLiteral("Spectrogram options are invalid.");
    case ErrorCode::InvalidRecord:
        return QStringLiteral("Selected-sensor records are malformed.");
    case ErrorCode::InvalidDecibelRange:
        return QStringLiteral("Min dB must be finite and smaller than Max dB.");
    case ErrorCode::InvalidSampleRate:
        return QStringLiteral("The selected sensor has no valid sample rate.");
    case ErrorCode::NoData:
        return QStringLiteral("No records were found for the selected sensor.");
    case ErrorCode::NotEnoughSamples:
        return QStringLiteral("At least 1024 contiguous samples are required.");
    case ErrorCode::TooManySamples:
        return QStringLiteral("The selected sensor exceeds the input sample limit.");
    case ErrorCode::Cancelled:
        return QStringLiteral("Spectrogram generation was cancelled.");
    }
    return QStringLiteral("Spectrogram generation failed.");
}

bool ratesMatch(double first, double second)
{
    if (!isFiniteNumber(first) || !isFiniteNumber(second) || first <= 0.0 || second <= 0.0) {
        return false;
    }
    return std::abs(first - second)
           <= std::max(0.01, std::max(first, second) * 1.0e-6);
}

std::array<double, DataFlashSpectrogramAnalyzer::FftSize> hannWindow()
{
    std::array<double, DataFlashSpectrogramAnalyzer::FftSize> window{};
    const double pi = std::acos(-1.0);
    for (int i = 0; i < DataFlashSpectrogramAnalyzer::FftSize; ++i) {
        // This is the normalization used by Mission Planner's FFT2.rin().
        window[static_cast<std::size_t>(i)] =
            (4.0 / DataFlashSpectrogramAnalyzer::FftSize) * 0.5
            * (1.0 - std::cos(2.0 * pi * i
                              / DataFlashSpectrogramAnalyzer::FftSize));
    }
    return window;
}

std::array<double, DataFlashSpectrogramAnalyzer::FrequencyBinCount>
fftDecibels(const DataFlashSpectrogramAnalyzer::SampleSegment &segment,
            int start, int axis)
{
    using Analyzer = DataFlashSpectrogramAnalyzer;
    using Complex = std::complex<double>;
    static const std::array<double, Analyzer::FftSize> window = hannWindow();

    std::array<Complex, Analyzer::FftSize> values{};
    for (int i = 0; i < Analyzer::FftSize; ++i) {
        const Analyzer::SampleFrame &sample = segment.samples.at(start + i);
        const double component = axis == 0 ? sample.x : axis == 1 ? sample.y
                                                                  : sample.z;
        values[static_cast<std::size_t>(i)] =
            Complex(component * window[static_cast<std::size_t>(i)], 0.0);
    }

    // Iterative radix-2 Cooley-Tukey FFT. The fixed 1024-point transform keeps
    // this dependency-free and deterministic on every Qt 5 platform.
    for (int i = 1, reversed = 0; i < Analyzer::FftSize; ++i) {
        int bit = Analyzer::FftSize >> 1;
        for (; reversed & bit; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (i < reversed) {
            std::swap(values[static_cast<std::size_t>(i)],
                      values[static_cast<std::size_t>(reversed)]);
        }
    }

    const double pi = std::acos(-1.0);
    for (int length = 2; length <= Analyzer::FftSize; length <<= 1) {
        const Complex root = std::polar(1.0, -2.0 * pi / length);
        for (int offset = 0; offset < Analyzer::FftSize; offset += length) {
            Complex twiddle(1.0, 0.0);
            for (int i = 0; i < length / 2; ++i) {
                const Complex even = values[static_cast<std::size_t>(offset + i)];
                const Complex odd =
                    values[static_cast<std::size_t>(offset + i + length / 2)]
                    * twiddle;
                values[static_cast<std::size_t>(offset + i)] = even + odd;
                values[static_cast<std::size_t>(offset + i + length / 2)] =
                    even - odd;
                twiddle *= root;
            }
        }
    }

    std::array<double, Analyzer::FrequencyBinCount> decibels{};
    for (int i = 0; i < Analyzer::FrequencyBinCount; ++i) {
        const double magnitude = std::abs(values[static_cast<std::size_t>(i)]);
        decibels[static_cast<std::size_t>(i)] =
            20.0 * std::log10(std::max(magnitude,
                                      std::numeric_limits<double>::min()));
    }
    return decibels;
}

std::array<QRgb, 256> colorPalette()
{
    std::array<QRgb, 256> palette{};
    for (int scale = 0; scale < 256; ++scale) {
        double hue = (255.0 - scale) / 255.0;
        // HSL hue 1 is the same hue as 0. Explicit wrapping avoids the gray
        // minimum-value pixel produced by the reference switch implementation.
        if (hue >= 1.0) {
            hue = 0.0;
        }
        palette[static_cast<std::size_t>(scale)] =
            QColor::fromHslF(hue, 0.5, 0.5).rgba();
    }
    return palette;
}

struct WindowReference {
    int segment{0};
    int start{0};
};

} // namespace

class DataFlashSpectrogramAnalyzer::RecordBuilder::Private
{
public:
    struct BatchPacket {
        QVector<double> x;
        QVector<double> y;
        QVector<double> z;
    };

    struct PendingBatch {
        quint64 number{0};
        double sampleRateHz{0.0};
        double multiplier{0.0};
        int sampleCount{0};
        double startSeconds{0.0};
        int packetLength{0};
        QMap<int, BatchPacket> packets;
    };

    explicit Private(const QString &requestedSensor,
                     const BuildOptions &requestedOptions)
        : options(requestedOptions)
    {
        if (!parseSensorName(requestedSensor, &sensor)) {
            fatalError = ErrorCode::InvalidSensor;
        }
        if (options.maximumInputSamples <= 0) {
            fatalError = ErrorCode::InvalidOptions;
        } else {
            options.maximumInputSamples =
                std::min(options.maximumInputSamples, MaximumInputSamples);
        }
    }

    bool cancelled() const
    {
        return options.isCancelled && options.isCancelled();
    }

    RecordDisposition preflight()
    {
        if (cancelled()) {
            fatalError = ErrorCode::Cancelled;
            return RecordDisposition::Cancelled;
        }
        if (fatalError == ErrorCode::TooManySamples) {
            return RecordDisposition::LimitExceeded;
        }
        if (fatalError != ErrorCode::None) {
            return RecordDisposition::Rejected;
        }
        if (finished) {
            fatalError = ErrorCode::InvalidOptions;
            return RecordDisposition::Rejected;
        }
        return RecordDisposition::Accepted;
    }

    RecordDisposition reject()
    {
        ++rejectedRecords;
        return RecordDisposition::Rejected;
    }

    bool reserveSamples(int count)
    {
        if (count < 0 || count > options.maximumInputSamples - storedSamples) {
            fatalError = ErrorCode::TooManySamples;
            return false;
        }
        storedSamples += count;
        return true;
    }

    RecordDisposition appendDirect(double timestampMicroseconds,
                                   double x, double y, double z)
    {
        const RecordDisposition state = preflight();
        if (state != RecordDisposition::Accepted) {
            return state;
        }
        if (!isFiniteNumber(timestampMicroseconds) || !isFiniteNumber(x) || !isFiniteNumber(y)
            || !isFiniteNumber(z)) {
            return reject();
        }
        if (!reserveSamples(1)) {
            return RecordDisposition::LimitExceeded;
        }
        directSamples.append(
            {timestampMicroseconds * kMicrosecondsToSeconds, x, y, z});
        return RecordDisposition::Accepted;
    }

    void discardPending(quint64 number)
    {
        const auto iterator = pending.find(number);
        if (iterator == pending.end()) {
            return;
        }
        for (const BatchPacket &packet : iterator->packets) {
            storedSamples -= packet.x.size();
        }
        pending.erase(iterator);
    }

    bool finalizeIfComplete(quint64 number)
    {
        auto iterator = pending.find(number);
        if (iterator == pending.end() || iterator->packetLength <= 0) {
            return false;
        }
        PendingBatch &batch = iterator.value();
        const int expectedPackets =
            (batch.sampleCount + batch.packetLength - 1) / batch.packetLength;
        for (int sequence = 0; sequence < expectedPackets; ++sequence) {
            if (!batch.packets.contains(sequence)) {
                return false;
            }
        }

        SampleSegment segment;
        segment.sampleRateHz = batch.sampleRateHz;
        segment.samples.reserve(batch.sampleCount);
        int sampleIndex = 0;
        int packetStorage = 0;
        for (int sequence = 0; sequence < expectedPackets; ++sequence) {
            const BatchPacket &packet = batch.packets[sequence];
            packetStorage += packet.x.size();
            for (int index = 0;
                 index < packet.x.size() && sampleIndex < batch.sampleCount;
                 ++index, ++sampleIndex) {
                segment.samples.append(
                    {batch.startSeconds + sampleIndex / batch.sampleRateHz,
                     packet.x.at(index) / batch.multiplier,
                     packet.y.at(index) / batch.multiplier,
                     packet.z.at(index) / batch.multiplier});
            }
        }

        storedSamples -= packetStorage;
        storedSamples += segment.samples.size();
        completedBatches.append(std::move(segment));
        pending.erase(iterator);
        return true;
    }

    Source makeBatchSource() const
    {
        Source source;
        source.sensorName = sensor.name;
        source.kind = SourceKind::Batch;
        source.rejectedRecordCount = rejectedRecords;

        double bestRate = 0.0;
        int bestWindowableSamples = 0;
        for (const SampleSegment &candidate : completedBatches) {
            if (candidate.samples.size() < FftSize) {
                continue;
            }
            int matchingSamples = 0;
            for (const SampleSegment &segment : completedBatches) {
                if (segment.samples.size() >= FftSize
                    && ratesMatch(candidate.sampleRateHz,
                                  segment.sampleRateHz)) {
                    matchingSamples += segment.samples.size();
                }
            }
            if (matchingSamples > bestWindowableSamples) {
                bestWindowableSamples = matchingSamples;
                bestRate = candidate.sampleRateHz;
            }
        }

        if (bestWindowableSamples == 0) {
            return source;
        }
        source.sampleRateHz = bestRate;
        for (const SampleSegment &segment : completedBatches) {
            if (segment.samples.size() >= FftSize
                && ratesMatch(bestRate, segment.sampleRateHz)) {
                source.sampleCount += segment.samples.size();
                source.segments.append(segment);
            }
        }
        return source;
    }

    Source makeDirectSource(ErrorCode *directError) const
    {
        Source source;
        source.sensorName = sensor.name;
        source.kind = SourceKind::Direct;
        source.rejectedRecordCount = rejectedRecords;
        if (directError) {
            *directError = ErrorCode::None;
        }
        if (directSamples.size() < 2) {
            return source;
        }

        QVector<double> deltas;
        deltas.reserve(directSamples.size() - 1);
        for (int i = 1; i < directSamples.size(); ++i) {
            const double delta = directSamples.at(i).timeSeconds
                                 - directSamples.at(i - 1).timeSeconds;
            if (isFiniteNumber(delta) && delta > 0.0) {
                deltas.append(delta);
            }
        }
        if (deltas.isEmpty()) {
            if (directError) {
                *directError = ErrorCode::InvalidSampleRate;
            }
            return source;
        }

        std::sort(deltas.begin(), deltas.end());
        const int middle = deltas.size() / 2;
        const double medianDelta = deltas.size() % 2
                                       ? deltas.at(middle)
                                       : 0.5 * (deltas.at(middle - 1)
                                                + deltas.at(middle));
        if (!isFiniteNumber(medianDelta) || medianDelta <= 0.0) {
            if (directError) {
                *directError = ErrorCode::InvalidSampleRate;
            }
            return source;
        }

        source.sampleRateHz = 1.0 / medianDelta;
        if (!isFiniteNumber(source.sampleRateHz) || source.sampleRateHz <= 0.0) {
            if (directError) {
                *directError = ErrorCode::InvalidSampleRate;
            }
            return source;
        }

        SampleSegment current;
        current.sampleRateHz = source.sampleRateHz;
        current.samples.reserve(directSamples.size());
        const double maximumGap = medianDelta * 4.0;
        for (const SampleFrame &sample : directSamples) {
            if (!current.samples.isEmpty()) {
                const double gap = sample.timeSeconds
                                   - current.samples.constLast().timeSeconds;
                if (gap <= 0.0 || gap > maximumGap) {
                    if (!current.samples.isEmpty()) {
                        source.sampleCount += current.samples.size();
                        source.segments.append(std::move(current));
                    }
                    current = SampleSegment{};
                    current.sampleRateHz = source.sampleRateHz;
                }
            }
            current.samples.append(sample);
        }
        if (!current.samples.isEmpty()) {
            source.sampleCount += current.samples.size();
            source.segments.append(std::move(current));
        }
        return source;
    }

    SensorSelection sensor;
    BuildOptions options;
    ErrorCode fatalError{ErrorCode::None};
    bool finished{false};
    int storedSamples{0};
    int rejectedRecords{0};
    QVector<SampleFrame> directSamples;
    QHash<quint64, PendingBatch> pending;
    QVector<SampleSegment> completedBatches;
};

DataFlashSpectrogramAnalyzer::RecordBuilder::RecordBuilder(
    const QString &sensorName)
    : RecordBuilder(sensorName, BuildOptions{})
{
}

DataFlashSpectrogramAnalyzer::RecordBuilder::RecordBuilder(
    const QString &sensorName, const BuildOptions &options)
    : d(new Private(sensorName, options))
{
}

DataFlashSpectrogramAnalyzer::RecordBuilder::~RecordBuilder() = default;

DataFlashSpectrogramAnalyzer::RecordBuilder::RecordBuilder(
    RecordBuilder &&other) noexcept = default;

DataFlashSpectrogramAnalyzer::RecordBuilder &
DataFlashSpectrogramAnalyzer::RecordBuilder::operator=(
    RecordBuilder &&other) noexcept = default;

bool DataFlashSpectrogramAnalyzer::IsSupportedSensor(
    const QString &sensorName)
{
    return parseSensorName(sensorName, nullptr);
}

DataFlashSpectrogramAnalyzer::RecordDisposition
DataFlashSpectrogramAnalyzer::RecordBuilder::addRecord(
    const QString &typeName,
    const QList<QPair<QString, QVariant>> &values)
{
    const RecordDisposition state = d->preflight();
    if (state != RecordDisposition::Accepted) {
        return state;
    }

    const QString normalizedType = typeName.trimmed().toUpper();
    if (normalizedType.startsWith(QStringLiteral("ISBH"))) {
        quint64 batchNumber = 0;
        int sensorType = -1;
        int instance = -1;
        int sampleCount = 0;
        double sampleRate = 0.0;
        double multiplier = 0.0;
        double timestamp = 0.0;
        if (!variantToBatchNumber(findValue(values, {"N"}), &batchNumber)
            || !variantToInteger(findValue(values, {"type"}), 0, 1,
                                 &sensorType)
            || !variantToInteger(findValue(values, {"instance"}), 0, 255,
                                 &instance)
            || !variantToFiniteDouble(findValue(values, {"smp_rate"}),
                                      &sampleRate)
            || !variantToFiniteDouble(findValue(values, {"mul"}),
                                      &multiplier)
            || !variantToInteger(findValue(values, {"smp_cnt"}), 1,
                                 std::numeric_limits<int>::max(), &sampleCount)
            || !variantToFiniteDouble(
                findValue(values, {"SampleUS", "TimeUS"}), &timestamp)) {
            return d->reject();
        }
        return addBatchHeader(batchNumber, sensorType, instance, sampleRate,
                              multiplier, sampleCount, timestamp);
    }

    if (normalizedType.startsWith(QStringLiteral("ISBD"))) {
        quint64 batchNumber = 0;
        int sequenceNumber = -1;
        QVector<double> x;
        QVector<double> y;
        QVector<double> z;
        if (!variantToBatchNumber(findValue(values, {"N"}), &batchNumber)
            || !variantToInteger(findValue(values, {"seqno"}), 0,
                                 MaximumInputSamples, &sequenceNumber)
            || !variantToArray(findValue(values, {"x"}), &x)
            || !variantToArray(findValue(values, {"y"}), &y)
            || !variantToArray(findValue(values, {"z"}), &z)) {
            return d->reject();
        }
        return addBatchData(batchNumber, sequenceNumber, x, y, z);
    }

    int sensorType = -1;
    int instance = -1;
    bool explicitInstance = false;
    if (!parseDirectType(normalizedType, &sensorType, &instance,
                         &explicitInstance)) {
        return RecordDisposition::Ignored;
    }
    if (!explicitInstance) {
        const QVariant *instanceValue =
            findValue(values, {"I", "instance", "Instance"});
        if (instanceValue
            && !variantToInteger(instanceValue, 0, 255, &instance)) {
            return d->reject();
        }
    }
    if (sensorType != d->sensor.type || instance != d->sensor.instance) {
        return RecordDisposition::Ignored;
    }

    double timestamp = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    const bool axesOk = sensorType == 0
        ? variantToFiniteDouble(findValue(values, {"AccX", "X"}), &x)
              && variantToFiniteDouble(findValue(values, {"AccY", "Y"}), &y)
              && variantToFiniteDouble(findValue(values, {"AccZ", "Z"}), &z)
        : variantToFiniteDouble(findValue(values, {"GyrX", "X"}), &x)
              && variantToFiniteDouble(findValue(values, {"GyrY", "Y"}), &y)
              && variantToFiniteDouble(findValue(values, {"GyrZ", "Z"}), &z);
    if (!axesOk
        || !variantToFiniteDouble(
            findValue(values, {"SampleUS", "TimeUS"}), &timestamp)) {
        return d->reject();
    }
    return d->appendDirect(timestamp, x, y, z);
}

DataFlashSpectrogramAnalyzer::RecordDisposition
DataFlashSpectrogramAnalyzer::RecordBuilder::addDirectRecord(
    const QString &typeName, double timestampMicroseconds,
    double x, double y, double z)
{
    const RecordDisposition state = d->preflight();
    if (state != RecordDisposition::Accepted) {
        return state;
    }
    int sensorType = -1;
    int instance = -1;
    if (!parseDirectType(typeName, &sensorType, &instance)
        || sensorType != d->sensor.type || instance != d->sensor.instance) {
        return RecordDisposition::Ignored;
    }
    return d->appendDirect(timestampMicroseconds, x, y, z);
}

DataFlashSpectrogramAnalyzer::RecordDisposition
DataFlashSpectrogramAnalyzer::RecordBuilder::addBatchHeader(
    quint64 batchNumber, int sensorType, int instance, double sampleRateHz,
    double multiplier, int sampleCount, double sampleTimestampMicroseconds)
{
    const RecordDisposition state = d->preflight();
    if (state != RecordDisposition::Accepted) {
        return state;
    }

    // Any header with the same N supersedes stale state. This prevents an ISBD
    // row from being associated with a selected-sensor header after N wraps.
    d->discardPending(batchNumber);
    if (sensorType != d->sensor.type || instance != d->sensor.instance) {
        return RecordDisposition::Ignored;
    }
    if (sensorType < 0 || sensorType > 1 || instance < 0 || instance > 4
        || !isFiniteNumber(sampleRateHz) || sampleRateHz <= 0.0
        || !isFiniteNumber(multiplier) || multiplier == 0.0
        || sampleCount <= 0
        || !isFiniteNumber(sampleTimestampMicroseconds)) {
        return d->reject();
    }
    if (sampleCount > d->options.maximumInputSamples - d->storedSamples) {
        d->fatalError = ErrorCode::TooManySamples;
        return RecordDisposition::LimitExceeded;
    }
    if (d->pending.size() >= kMaximumPendingBatches) {
        return d->reject();
    }

    Private::PendingBatch batch;
    batch.number = batchNumber;
    batch.sampleRateHz = sampleRateHz;
    batch.multiplier = multiplier;
    batch.sampleCount = sampleCount;
    batch.startSeconds = sampleTimestampMicroseconds * kMicrosecondsToSeconds;
    d->pending.insert(batchNumber, std::move(batch));
    return RecordDisposition::Accepted;
}

DataFlashSpectrogramAnalyzer::RecordDisposition
DataFlashSpectrogramAnalyzer::RecordBuilder::addBatchData(
    quint64 batchNumber, int sequenceNumber, const QVector<double> &x,
    const QVector<double> &y, const QVector<double> &z)
{
    const RecordDisposition state = d->preflight();
    if (state != RecordDisposition::Accepted) {
        return state;
    }
    auto iterator = d->pending.find(batchNumber);
    if (iterator == d->pending.end()) {
        return RecordDisposition::Ignored;
    }
    Private::PendingBatch &batch = iterator.value();
    if (sequenceNumber < 0 || x.isEmpty() || x.size() != y.size()
        || x.size() != z.size()) {
        return d->reject();
    }
    for (int i = 0; i < x.size(); ++i) {
        if (!isFiniteNumber(x.at(i)) || !isFiniteNumber(y.at(i)) || !isFiniteNumber(z.at(i))) {
            return d->reject();
        }
    }
    if (batch.packetLength == 0) {
        batch.packetLength = x.size();
    }
    const int expectedPackets =
        (batch.sampleCount + batch.packetLength - 1) / batch.packetLength;
    if (x.size() != batch.packetLength || sequenceNumber >= expectedPackets
        || batch.packets.contains(sequenceNumber)) {
        return d->reject();
    }
    if (!d->reserveSamples(x.size())) {
        return RecordDisposition::LimitExceeded;
    }

    Private::BatchPacket packet;
    packet.x = x;
    packet.y = y;
    packet.z = z;
    batch.packets.insert(sequenceNumber, std::move(packet));
    d->finalizeIfComplete(batchNumber);
    return RecordDisposition::Accepted;
}

DataFlashSpectrogramAnalyzer::BuildResult
DataFlashSpectrogramAnalyzer::RecordBuilder::finish()
{
    BuildResult result;
    if (!d) {
        result.error = ErrorCode::InvalidOptions;
        result.message = errorMessage(result.error);
        return result;
    }
    if (d->cancelled()) {
        d->fatalError = ErrorCode::Cancelled;
    }
    if (d->fatalError != ErrorCode::None) {
        result.error = d->fatalError;
        result.message = errorMessage(result.error);
        d->finished = true;
        return result;
    }
    if (d->finished) {
        result.error = ErrorCode::InvalidOptions;
        result.message = QStringLiteral("The record builder was already finished.");
        return result;
    }
    d->finished = true;

    result.source = d->makeBatchSource();
    if (!result.source.segments.isEmpty()) {
        return result;
    }

    ErrorCode directError = ErrorCode::None;
    result.source = d->makeDirectSource(&directError);
    bool hasWindowableSegment = false;
    for (const SampleSegment &segment : result.source.segments) {
        if (segment.samples.size() >= FftSize) {
            hasWindowableSegment = true;
            break;
        }
    }
    if (hasWindowableSegment) {
        return result;
    }

    if (directError != ErrorCode::None && d->directSamples.size() >= FftSize) {
        result.error = directError;
    } else if (!d->directSamples.isEmpty() || !d->completedBatches.isEmpty()
               || !d->pending.isEmpty()) {
        result.error = ErrorCode::NotEnoughSamples;
    } else if (d->rejectedRecords > 0) {
        result.error = ErrorCode::InvalidRecord;
    } else {
        result.error = ErrorCode::NoData;
    }
    result.message = errorMessage(result.error);
    result.source = Source{};
    result.source.sensorName = d->sensor.name;
    result.source.rejectedRecordCount = d->rejectedRecords;
    return result;
}

DataFlashSpectrogramAnalyzer::AnalysisResult
DataFlashSpectrogramAnalyzer::Analyze(const Source &source)
{
    return Analyze(source, AnalysisOptions{});
}

DataFlashSpectrogramAnalyzer::AnalysisResult
DataFlashSpectrogramAnalyzer::Analyze(const Source &source,
                                      const AnalysisOptions &options)
{
    AnalysisResult result;
    result.sensorName = source.sensorName;
    result.sourceKind = source.kind;

    const auto cancelled = [&options]() {
        return options.isCancelled && options.isCancelled();
    };
    if (cancelled()) {
        result.error = ErrorCode::Cancelled;
        result.message = errorMessage(result.error);
        return result;
    }
    if (!isFiniteNumber(options.minimumDb) || !isFiniteNumber(options.maximumDb)
        || options.minimumDb >= options.maximumDb) {
        result.error = ErrorCode::InvalidDecibelRange;
        result.message = errorMessage(result.error);
        return result;
    }
    if (options.maximumRasterWidth <= 0) {
        result.error = ErrorCode::InvalidOptions;
        result.message = errorMessage(result.error);
        return result;
    }
    if (!isFiniteNumber(source.sampleRateHz) || source.sampleRateHz <= 0.0) {
        result.error = ErrorCode::InvalidSampleRate;
        result.message = errorMessage(result.error);
        return result;
    }

    result.sampleRateHz = source.sampleRateHz;
    result.startSeconds = std::numeric_limits<double>::infinity();
    result.endSeconds = -std::numeric_limits<double>::infinity();

    QVector<WindowReference> windows;
    const int hop = source.kind == SourceKind::Batch ? FftSize : FftSize / 4;
    for (int segmentIndex = 0; segmentIndex < source.segments.size();
         ++segmentIndex) {
        if (cancelled()) {
            result.error = ErrorCode::Cancelled;
            result.message = errorMessage(result.error);
            return result;
        }
        const SampleSegment &segment = source.segments.at(segmentIndex);
        if (!ratesMatch(source.sampleRateHz, segment.sampleRateHz)) {
            result.error = ErrorCode::InvalidSampleRate;
            result.message = errorMessage(result.error);
            return result;
        }
        result.inputSampleCount += segment.samples.size();
        if (result.inputSampleCount > MaximumInputSamples) {
            result.error = ErrorCode::TooManySamples;
            result.message = errorMessage(result.error);
            return result;
        }
        for (const SampleFrame &sample : segment.samples) {
            if (!isFiniteNumber(sample.timeSeconds) || !isFiniteNumber(sample.x)
                || !isFiniteNumber(sample.y) || !isFiniteNumber(sample.z)) {
                result.error = ErrorCode::InvalidRecord;
                result.message = errorMessage(result.error);
                return result;
            }
        }
        for (int start = 0; start + FftSize <= segment.samples.size();
             start += hop) {
            windows.append({segmentIndex, start});
        }
    }

    if (windows.isEmpty()) {
        result.error = result.inputSampleCount == 0 ? ErrorCode::NoData
                                                     : ErrorCode::NotEnoughSamples;
        result.message = errorMessage(result.error);
        return result;
    }
    for (const WindowReference &window : windows) {
        const SampleSegment &segment = source.segments.at(window.segment);
        result.startSeconds = std::min(
            result.startSeconds,
            segment.samples.at(window.start).timeSeconds);
        result.endSeconds = std::max(
            result.endSeconds,
            segment.samples.at(window.start + FftSize - 1).timeSeconds);
    }
    result.availableWindowCount = windows.size();

    const int rasterLimit =
        std::min(options.maximumRasterWidth, MaximumRasterWidth);
    result.renderedWindowCount = std::min(windows.size(), rasterLimit);

    result.frequencyBinsHz.reserve(FrequencyBinCount);
    for (int bin = 0; bin < FrequencyBinCount; ++bin) {
        result.frequencyBinsHz.append(bin * source.sampleRateHz / FftSize);
    }
    result.maximumFrequencyHz = result.frequencyBinsHz.constLast();

    static const std::array<QRgb, 256> palette = colorPalette();
    for (AxisResult &axis : result.axes) {
        axis.image = QImage(result.renderedWindowCount, FrequencyBinCount,
                            QImage::Format_ARGB32);
        axis.image.fill(palette.front());
        axis.peakFrequenciesHz.reserve(result.renderedWindowCount);
        axis.peakDecibels.reserve(result.renderedWindowCount);
        axis.strongestDecibels = -std::numeric_limits<double>::infinity();
    }
    result.windowTimesSeconds.reserve(result.renderedWindowCount);

    const double dbRange = options.maximumDb - options.minimumDb;
    for (int column = 0; column < result.renderedWindowCount; ++column) {
        if (cancelled()) {
            AnalysisResult cancelledResult;
            cancelledResult.error = ErrorCode::Cancelled;
            cancelledResult.message = errorMessage(cancelledResult.error);
            cancelledResult.sensorName = source.sensorName;
            cancelledResult.sourceKind = source.kind;
            return cancelledResult;
        }
        int representativeIndex = 0;
        if (result.renderedWindowCount > 1) {
            const qint64 numerator = static_cast<qint64>(column)
                                     * (windows.size() - 1);
            representativeIndex = static_cast<int>(
                (numerator + (result.renderedWindowCount - 1) / 2)
                / (result.renderedWindowCount - 1));
        }
        const WindowReference &representative = windows.at(
            representativeIndex);
        const SampleSegment &representativeSegment = source.segments.at(
            representative.segment);
        result.windowTimesSeconds.append(
            representativeSegment.samples.at(representative.start).timeSeconds);

        const int bucketBegin = static_cast<int>(
            static_cast<qint64>(column) * windows.size()
            / result.renderedWindowCount);
        const int bucketEnd = static_cast<int>(
            static_cast<qint64>(column + 1) * windows.size()
            / result.renderedWindowCount);

        for (int axisIndex = 0; axisIndex < AxisCount; ++axisIndex) {
            std::array<double, FrequencyBinCount> pooledDecibels{};
            pooledDecibels.fill(-std::numeric_limits<double>::infinity());
            for (int windowIndex = bucketBegin;
                 windowIndex < bucketEnd; ++windowIndex) {
                if (cancelled()) {
                    AnalysisResult cancelledResult;
                    cancelledResult.error = ErrorCode::Cancelled;
                    cancelledResult.message = errorMessage(
                        cancelledResult.error);
                    cancelledResult.sensorName = source.sensorName;
                    cancelledResult.sourceKind = source.kind;
                    return cancelledResult;
                }
                const WindowReference &window = windows.at(windowIndex);
                const SampleSegment &segment = source.segments.at(
                    window.segment);
                const auto decibels = fftDecibels(
                    segment, window.start, axisIndex);
                for (int bin = 0; bin < FrequencyBinCount; ++bin) {
                    pooledDecibels[static_cast<std::size_t>(bin)] = std::max(
                        pooledDecibels[static_cast<std::size_t>(bin)],
                        decibels[static_cast<std::size_t>(bin)]);
                }
            }
            int peakBin = 0;
            double peakDb = pooledDecibels.front();
            for (int bin = 0; bin < FrequencyBinCount; ++bin) {
                const double db = pooledDecibels[
                    static_cast<std::size_t>(bin)];
                if (db > peakDb) {
                    peakDb = db;
                    peakBin = bin;
                }
                const double constrained =
                    std::max(options.minimumDb,
                             std::min(options.maximumDb, db));
                const int scale = qBound(
                    0,
                    static_cast<int>(std::lround(
                        (constrained - options.minimumDb) * 255.0 / dbRange)),
                    255);
                QRgb *scanLine = reinterpret_cast<QRgb *>(
                    result.axes[static_cast<std::size_t>(axisIndex)]
                        .image.scanLine(FrequencyBinCount - 1 - bin));
                scanLine[column] = palette[static_cast<std::size_t>(scale)];
            }

            AxisResult &axis =
                result.axes[static_cast<std::size_t>(axisIndex)];
            const double peakFrequency =
                result.frequencyBinsHz.at(peakBin);
            axis.peakFrequenciesHz.append(peakFrequency);
            axis.peakDecibels.append(peakDb);
            if (peakDb > axis.strongestDecibels) {
                axis.strongestDecibels = peakDb;
                axis.strongestFrequencyHz = peakFrequency;
            }
        }
    }

    return result;
}
