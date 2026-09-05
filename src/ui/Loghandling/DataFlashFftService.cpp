#include "DataFlashFftService.h"

#include "AP2DataPlotStatus.h"
#include "AsciiLogParser.h"
#include "BinLogParser.h"
#include "DataFlashFftCore.h"
#include "ILogdataSink.h"
#include "ILogParser.h"
#include "IParserCallback.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSharedPointer>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>

namespace
{
using Analyzer = DataFlashFftAnalyzer;

constexpr int SensorInstances = 6;
constexpr int BatchSensorCount = SensorInstances * 2;
constexpr int DirectSensorCount = 6;
constexpr int BatchPacketLength = 32;
constexpr int MaximumPendingBatches = 16;

Analyzer::Result failure(const QString &message, bool cancelled = false)
{
    Analyzer::Result result;
    result.cancelled = cancelled;
    result.error = message;
    return result;
}

bool cancellationRequested(const Analyzer::Options &options)
{
    return options.isCancelled && options.isCancelled();
}

const QVariant *findValue(const QList<QPair<QString, QVariant>> &values,
                          std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        for (const auto &entry : values) {
            if (entry.first.compare(QLatin1String(name),
                                    Qt::CaseInsensitive) == 0) {
                return &entry.second;
            }
        }
    }
    return nullptr;
}

bool finiteDouble(const QVariant *value, double *result)
{
    if (!value) {
        return false;
    }
    bool ok = false;
    const double converted = value->toDouble(&ok);
    if (!ok || !std::isfinite(converted)) {
        return false;
    }
    if (result) {
        *result = converted;
    }
    return true;
}

bool integerValue(const QVariant *value, int minimum, int maximum,
                  int *result)
{
    double converted = 0.0;
    if (!finiteDouble(value, &converted) || std::floor(converted) != converted
        || converted < minimum || converted > maximum) {
        return false;
    }
    if (result) {
        *result = static_cast<int>(converted);
    }
    return true;
}

bool unsignedValue(const QVariant *value, quint64 *result)
{
    if (!value) {
        return false;
    }
    bool ok = false;
    const quint64 converted = value->toULongLong(&ok);
    if (!ok) {
        return false;
    }
    if (result) {
        *result = converted;
    }
    return true;
}

bool arrayValue(const QVariant *value, QVector<double> *result)
{
    if (!value || !result) {
        return false;
    }
    const QVariantList list = value->toList();
    if (list.isEmpty()) {
        return false;
    }
    QVector<double> converted;
    converted.reserve(list.size());
    for (const QVariant &entry : list) {
        double number = 0.0;
        if (!finiteDouble(&entry, &number)) {
            return false;
        }
        converted.append(number);
    }
    *result = std::move(converted);
    return true;
}

struct AxisPacket
{
    QVector<double> x;
    QVector<double> y;
    QVector<double> z;
};

struct PendingBatch
{
    quint64 number = 0;
    int sensorType = -1;
    int instance = -1;
    double sampleRateHz = 0.0;
    double multiplier = 0.0;
    int sampleCount = 0;
    double startSeconds = 0.0;
    quint64 arrivalOrder = 0;
    int retainedSamples = 0;
    QMap<int, AxisPacket> packets;
};

struct TimedSample
{
    double microseconds = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

class FftLogSink final : public ILogdataSink
{
public:
    explicit FftLogSink(const Analyzer::Options &options)
        : m_options(options), m_fftSize(1 << options.bins)
    {
        for (int instance = 0; instance < SensorInstances; ++instance) {
            m_batchSensors[instance].label =
                QStringLiteral("ACC%1").arg(instance);
            m_batchSensors[instance].batch = true;
            m_batchSensors[SensorInstances + instance].label =
                QStringLiteral("GYR%1").arg(instance);
            m_batchSensors[SensorInstances + instance].batch = true;
        }
    }

    bool addDataType(const QString &, quint32, int, const QString &,
                     const QStringList &, int) override
    {
        return true;
    }

    bool addDataRow(
        const QString &typeName,
        const QList<QPair<QString, QVariant>> &values) override
    {
        if (cancellationRequested(m_options)) {
            m_cancelled = true;
            m_error = QStringLiteral("FFT calculation was cancelled.");
            return false;
        }
        const QString type = typeName.trimmed().toUpper();
        if (type.startsWith(QStringLiteral("ISBH"))) {
            addBatchHeader(values);
        } else if (type.startsWith(QStringLiteral("ISBD"))) {
            addBatchData(values);
        } else {
            const int instance = directInstance(type);
            if (instance >= 0 && !m_batchUsable) {
                addDirectRow(instance, values);
            }
        }
        return m_error.isEmpty();
    }

    void addUnitData(quint8, const QString &) override {}
    void addMultiplierData(quint8, double) override {}
    void addMsgToUnitAndMultiplierData(
        quint32, const QByteArray &, const QByteArray &) override {}
    void setTimeStamp(const QString &, double) override {}
    QStringList setupUnitData(const QString &, double) override { return {}; }
    QString getError() const override { return m_error; }

    bool wasCancelled() const { return m_cancelled; }

    QVector<Analyzer::SensorData> finish()
    {
        QVector<Analyzer::SensorData> sensors;
        if (m_batchUsable) {
            for (Analyzer::SensorData &sensor : m_batchSensors) {
                if (!sensor.segments.isEmpty()) {
                    sensors.append(std::move(sensor));
                }
            }
            return sensors;
        }

        static const char *const gyroLabels[] = {
            "IMU GYR", "IMU2 GYR", "IMU3 GYR"};
        static const char *const accelLabels[] = {
            "IMU ACC", "IMU2 ACC", "IMU3 ACC"};
        for (int index = 0; index < DirectSensorCount; ++index) {
            if (m_directSensors[index].isEmpty()) {
                continue;
            }
            Analyzer::SensorData sensor;
            sensor.label = QLatin1String(
                index < 3 ? gyroLabels[index] : accelLabels[index - 3]);
            sensor.batch = false;
            sensor.segments = directSegments(m_directSensors[index]);
            if (!sensor.segments.isEmpty()) {
                sensors.append(std::move(sensor));
            }
        }
        return sensors;
    }

private:
    static int directInstance(const QString &type)
    {
        if (type == QStringLiteral("IMU")) {
            return 0;
        }
        if (type == QStringLiteral("IMU2")) {
            return 1;
        }
        if (type == QStringLiteral("IMU3")) {
            return 2;
        }
        return -1;
    }

    bool reserveSamples(int count)
    {
        if (count < 0
            || count > m_options.maximumInputSamples - m_retainedSamples) {
            m_error = QStringLiteral(
                "FFT input exceeds the bounded %1-sample limit.")
                              .arg(m_options.maximumInputSamples);
            return false;
        }
        m_retainedSamples += count;
        return true;
    }

    void releaseSamples(int count)
    {
        m_retainedSamples = std::max(0, m_retainedSamples - count);
    }

    void discardPending(quint64 number)
    {
        const auto iterator = m_pending.find(number);
        if (iterator == m_pending.end()) {
            return;
        }
        releaseSamples(iterator->retainedSamples);
        m_pending.erase(iterator);
    }

    void evictOldestPending()
    {
        while (m_pending.size() >= MaximumPendingBatches) {
            auto oldest = m_pending.begin();
            for (auto candidate = m_pending.begin();
                 candidate != m_pending.end(); ++candidate) {
                if (candidate->arrivalOrder < oldest->arrivalOrder) {
                    oldest = candidate;
                }
            }
            releaseSamples(oldest->retainedSamples);
            m_pending.erase(oldest);
        }
    }

    void addBatchHeader(const QList<QPair<QString, QVariant>> &values)
    {
        quint64 number = 0;
        if (!unsignedValue(findValue(values, {"N"}), &number)) {
            return;
        }
        // A repeated N denotes a new batch even when the new header is
        // malformed. Never let its following data attach to stale metadata.
        discardPending(number);

        int type = -1;
        int instance = -1;
        int sampleCount = 0;
        double sampleRate = 0.0;
        double multiplier = 0.0;
        double timestampUs = 0.0;
        if (!integerValue(findValue(values, {"type"}), 0, 1, &type)
            || !integerValue(findValue(values, {"instance"}), 0, 5,
                             &instance)
            || !integerValue(findValue(values, {"smp_cnt"}), 1,
                             Analyzer::MaximumInputSamples, &sampleCount)
            || !finiteDouble(findValue(values, {"smp_rate"}), &sampleRate)
            || !finiteDouble(findValue(values, {"mul"}), &multiplier)
            || !finiteDouble(findValue(values, {"SampleUS", "TimeUS"}),
                             &timestampUs)
            || sampleRate <= 0.0 || multiplier == 0.0) {
            return;
        }
        // Dropped ISBD rows are common in lossy logs. Keep correlation memory
        // bounded without allowing stale incomplete batches to poison an
        // otherwise usable later spectrum.
        evictOldestPending();
        PendingBatch batch;
        batch.number = number;
        batch.sensorType = type;
        batch.instance = instance;
        batch.sampleRateHz = sampleRate;
        batch.multiplier = multiplier;
        batch.sampleCount = sampleCount;
        batch.startSeconds = timestampUs / 1000000.0;
        batch.arrivalOrder = ++m_batchArrivalOrder;
        m_pending.insert(number, std::move(batch));
    }

    void addBatchData(const QList<QPair<QString, QVariant>> &values)
    {
        quint64 number = 0;
        int sequence = -1;
        QVector<double> x;
        QVector<double> y;
        QVector<double> z;
        if (!unsignedValue(findValue(values, {"N"}), &number)
            || !integerValue(findValue(values, {"seqno"}), 0,
                             Analyzer::MaximumInputSamples, &sequence)
            || !arrayValue(findValue(values, {"x"}), &x)
            || !arrayValue(findValue(values, {"y"}), &y)
            || !arrayValue(findValue(values, {"z"}), &z)
            || x.size() != y.size() || x.size() != z.size()) {
            return;
        }
        auto iterator = m_pending.find(number);
        if (iterator == m_pending.end()) {
            return;
        }
        PendingBatch &batch = iterator.value();
        if (batch.packets.contains(sequence)) {
            return;
        }
        const int expectedPackets =
            (batch.sampleCount + BatchPacketLength - 1) / BatchPacketLength;
        const int firstSample = sequence * BatchPacketLength;
        const int required = sequence < expectedPackets
            ? std::min(BatchPacketLength, batch.sampleCount - firstSample)
            : 0;
        if (sequence >= expectedPackets || x.size() < required
            || x.size() > BatchPacketLength
            || (sequence + 1 < expectedPackets
                && x.size() != BatchPacketLength)) {
            return;
        }
        const int useful = std::min(x.size(), batch.sampleCount - firstSample);
        if (useful <= 0 || !reserveSamples(x.size())) {
            return;
        }
        AxisPacket packet;
        packet.x = std::move(x);
        packet.y = std::move(y);
        packet.z = std::move(z);
        batch.retainedSamples += packet.x.size();
        batch.packets.insert(sequence, std::move(packet));

        if (batch.packets.size() != expectedPackets) {
            return;
        }
        for (int index = 0; index < expectedPackets; ++index) {
            if (!batch.packets.contains(index)) {
                return;
            }
        }
        finishBatch(number);
    }

    void finishBatch(quint64 number)
    {
        auto iterator = m_pending.find(number);
        if (iterator == m_pending.end()) {
            return;
        }
        PendingBatch batch = std::move(iterator.value());
        m_pending.erase(iterator);

        Analyzer::Segment segment;
        segment.sampleRateHz = batch.sampleRateHz;
        segment.startSeconds = batch.startSeconds;
        segment.x.reserve(batch.sampleCount);
        segment.y.reserve(batch.sampleCount);
        segment.z.reserve(batch.sampleCount);
        for (auto packet = batch.packets.cbegin();
             packet != batch.packets.cend(); ++packet) {
            const int remaining = batch.sampleCount - segment.x.size();
            const int take = std::min(remaining, packet->x.size());
            for (int index = 0; index < take; ++index) {
                segment.x.append(packet->x.at(index) / batch.multiplier);
                segment.y.append(packet->y.at(index) / batch.multiplier);
                segment.z.append(packet->z.at(index) / batch.multiplier);
            }
        }
        if (segment.x.size() != batch.sampleCount) {
            releaseSamples(batch.retainedSamples);
            return;
        }
        const int unused = batch.retainedSamples - batch.sampleCount;
        if (unused > 0) {
            releaseSamples(unused);
        }

        const int sensorIndex = batch.sensorType * SensorInstances
            + batch.instance;
        appendBatchSegment(&m_batchSensors[sensorIndex], std::move(segment));
        if (!m_batchUsable) {
            for (const Analyzer::SensorData &sensor : m_batchSensors) {
                for (const Analyzer::Segment &candidate : sensor.segments) {
                    if (candidate.x.size() >= m_fftSize) {
                        m_batchUsable = true;
                        clearDirect();
                        return;
                    }
                }
            }
        }
    }

    static bool segmentsAreContiguous(const Analyzer::Segment &first,
                                      const Analyzer::Segment &second)
    {
        const double tolerance = std::max(
            0.002, 2.0 / std::max(1.0, first.sampleRateHz));
        const double expectedStart = first.startSeconds
            + first.x.size() / first.sampleRateHz;
        return std::abs(first.sampleRateHz - second.sampleRateHz)
                <= std::max(0.01, first.sampleRateHz * 1.0e-6)
            && std::abs(second.startSeconds - expectedStart) <= tolerance;
    }

    static void mergeSegment(Analyzer::Segment *first,
                             const Analyzer::Segment &second)
    {
        first->x += second.x;
        first->y += second.y;
        first->z += second.z;
    }

    static void appendBatchSegment(Analyzer::SensorData *sensor,
                                   Analyzer::Segment segment)
    {
        if (!sensor || segment.x.isEmpty()) {
            return;
        }
        int position = 0;
        while (position < sensor->segments.size()
               && sensor->segments.at(position).startSeconds
                   <= segment.startSeconds) {
            ++position;
        }
        sensor->segments.insert(position, std::move(segment));

        if (position > 0
            && segmentsAreContiguous(sensor->segments.at(position - 1),
                                     sensor->segments.at(position))) {
            mergeSegment(&sensor->segments[position - 1],
                         sensor->segments.at(position));
            sensor->segments.removeAt(position);
            --position;
        }
        while (position + 1 < sensor->segments.size()
               && segmentsAreContiguous(sensor->segments.at(position),
                                        sensor->segments.at(position + 1))) {
            mergeSegment(&sensor->segments[position],
                         sensor->segments.at(position + 1));
            sensor->segments.removeAt(position + 1);
        }
    }

    void addDirectRow(int instance,
                      const QList<QPair<QString, QVariant>> &values)
    {
        double timestamp = 0.0;
        if (!finiteDouble(findValue(values, {"SampleUS", "TimeUS"}),
                          &timestamp)) {
            return;
        }
        const auto appendAxes =
            [this, timestamp](QVector<TimedSample> *destination,
                              const QVariant *xValue,
                              const QVariant *yValue,
                              const QVariant *zValue) {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            if (!finiteDouble(xValue, &x) || !finiteDouble(yValue, &y)
                || !finiteDouble(zValue, &z) || !reserveSamples(1)) {
                return;
            }
            destination->append({timestamp, x, y, z});
        };
        appendAxes(&m_directSensors[instance],
                   findValue(values, {"GyrX"}),
                   findValue(values, {"GyrY"}),
                   findValue(values, {"GyrZ"}));
        if (!m_error.isEmpty()) {
            return;
        }
        appendAxes(&m_directSensors[3 + instance],
                   findValue(values, {"AccX"}),
                   findValue(values, {"AccY"}),
                   findValue(values, {"AccZ"}));
    }

    void clearDirect()
    {
        int released = 0;
        for (QVector<TimedSample> &sensor : m_directSensors) {
            released += sensor.size();
            sensor.clear();
            sensor.squeeze();
        }
        releaseSamples(released);
    }

    static QVector<Analyzer::Segment> directSegments(
        const QVector<TimedSample> &samples)
    {
        QVector<double> deltas;
        deltas.reserve(std::max(0, samples.size() - 1));
        for (int index = 1; index < samples.size(); ++index) {
            const double delta = samples.at(index).microseconds
                - samples.at(index - 1).microseconds;
            if (std::isfinite(delta) && delta > 0.0) {
                deltas.append(delta);
            }
        }
        if (deltas.isEmpty()) {
            return {};
        }
        std::sort(deltas.begin(), deltas.end());
        const double median = deltas.at(deltas.size() / 2);
        if (!std::isfinite(median) || median <= 0.0) {
            return {};
        }
        const double sampleRate = 1000000.0 / median;
        const double maximumGap = median * 4.0;
        QVector<Analyzer::Segment> segments;
        Analyzer::Segment current;
        current.sampleRateHz = sampleRate;
        double lastTime = -std::numeric_limits<double>::infinity();
        for (const TimedSample &sample : samples) {
            if (!std::isfinite(sample.microseconds)
                || sample.microseconds <= lastTime) {
                continue;
            }
            if (!current.x.isEmpty()
                && sample.microseconds - lastTime > maximumGap) {
                segments.append(std::move(current));
                current = Analyzer::Segment();
                current.sampleRateHz = sampleRate;
            }
            if (current.x.isEmpty()) {
                current.startSeconds = sample.microseconds / 1000000.0;
            }
            current.x.append(sample.x);
            current.y.append(sample.y);
            current.z.append(sample.z);
            lastTime = sample.microseconds;
        }
        if (!current.x.isEmpty()) {
            segments.append(std::move(current));
        }
        return segments;
    }

    Analyzer::Options m_options;
    int m_fftSize = 0;
    int m_retainedSamples = 0;
    bool m_batchUsable = false;
    bool m_cancelled = false;
    quint64 m_batchArrivalOrder = 0;
    QString m_error;
    QMap<quint64, PendingBatch> m_pending;
    std::array<Analyzer::SensorData, BatchSensorCount> m_batchSensors;
    std::array<QVector<TimedSample>, DirectSensorCount> m_directSensors;
};

class ParserCallback final : public IParserCallback
{
public:
    explicit ParserCallback(const Analyzer::Options &options)
        : m_options(options)
    {
    }

    void setParser(ILogParser *parser) { m_parser = parser; }

    void onProgress(qint64, qint64) override
    {
        if (cancellationRequested(m_options) && m_parser) {
            m_parser->stopParsing();
        }
    }

    void onError(const QString &error) override
    {
        m_error = error;
        if (m_parser) {
            m_parser->stopParsing();
        }
    }

    QString error() const { return m_error; }

private:
    Analyzer::Options m_options;
    ILogParser *m_parser = nullptr;
    QString m_error;
};

// LogParserBase otherwise defers the first FMT descriptor until it has
// discovered a timestamp.  A valid single-FMT IMU log must be analyzable too,
// so select ArduPilot's canonical timestamp before parsing the first record.
class FftBinLogParser final : public BinLogParser
{
public:
    using BinLogParser::BinLogParser;

    void presetTimestamp()
    {
        m_activeTimestamp = timeStampType(QStringLiteral("TimeUS"), 1000000.0);
    }
};

class FftAsciiLogParser final : public AsciiLogParser
{
public:
    using AsciiLogParser::AsciiLogParser;

    void presetTimestamp()
    {
        m_activeTimestamp = timeStampType(QStringLiteral("TimeUS"), 1000000.0);
    }
};
}

DataFlashFftAnalyzer::Result DataFlashFftService::Analyze(
    const QString &path, const DataFlashFftAnalyzer::Options &options)
{
    if (cancellationRequested(options)) {
        return failure(QStringLiteral("FFT calculation was cancelled."), true);
    }
    if (!DataFlashFftCore::IsValidBins(options.bins)
        || !std::isfinite(options.startFrequencyHz)
        || options.startFrequencyHz < 0.0
        || options.startFrequencyHz > 1000.0
        || options.maximumInputSamples <= 0
        || options.maximumInputSamples > Analyzer::MaximumInputSamples) {
        return Analyzer::Analyze(QVector<Analyzer::SensorData>(), options);
    }

    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const bool binary = suffix.compare(QStringLiteral("bin"),
                                        Qt::CaseInsensitive) == 0;
    const bool ascii = suffix.compare(QStringLiteral("log"),
                                       Qt::CaseInsensitive) == 0;
    if (!info.exists() || !info.isFile()) {
        return failure(QStringLiteral("The selected DataFlash log does not exist."));
    }
    if (!binary && !ascii) {
        return failure(QStringLiteral("Select a DataFlash .bin or .log file."));
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("Unable to open %1: %2")
                           .arg(info.fileName(), file.errorString()));
    }

    auto sink = QSharedPointer<FftLogSink>::create(options);
    ILogdataSink::Ptr storage = qSharedPointerCast<ILogdataSink>(sink);
    ParserCallback callback(options);
    std::unique_ptr<ILogParser> parser;
    if (binary) {
        auto fftParser = std::make_unique<FftBinLogParser>(storage, &callback);
        fftParser->presetTimestamp();
        parser = std::move(fftParser);
    } else {
        auto fftParser = std::make_unique<FftAsciiLogParser>(storage, &callback);
        fftParser->presetTimestamp();
        parser = std::move(fftParser);
    }
    callback.setParser(parser.get());
    parser->parse(file);

    if (cancellationRequested(options) || sink->wasCancelled()) {
        return failure(QStringLiteral("FFT calculation was cancelled."), true);
    }
    if (!sink->getError().isEmpty()) {
        return failure(sink->getError());
    }
    if (!callback.error().isEmpty()) {
        return failure(callback.error());
    }
    return Analyzer::Analyze(sink->finish(), options);
}
