#include "DataFlashGpxExporter.h"

#include "DataFlashRawReader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryFile>
#include <QXmlStreamWriter>
#include <QtEndian>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {

using Result = DataFlashGpxExporter::Result;

constexpr qint64 ReadChunkBytes = 64 * 1024;
constexpr qint64 TicksPerMillisecond = 10000;
constexpr qint64 TicksPerSecond = 10000000;
constexpr int CurrentGpsUtcOffsetSeconds = 18;
constexpr int InstanceClockRecordLimit = 2001;

bool pathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool publishNoReplace(const QString &stagedPath, const QString &destination,
                      QString *error)
{
#ifdef Q_OS_UNIX
    const QByteArray sourceName = QFile::encodeName(stagedPath);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::link(sourceName.constData(), destinationName.constData()) != 0) {
        *error = QStringLiteral(
            "Could not publish the GPX file without overwriting an existing file.");
        return false;
    }
    QFile::remove(stagedPath);
    return true;
#elif defined(Q_OS_WIN)
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(stagedPath.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        *error = QStringLiteral(
            "Could not publish the GPX file without overwriting an existing file.");
        return false;
    }
    return true;
#else
    Q_UNUSED(stagedPath)
    Q_UNUSED(destination)
    *error = QStringLiteral(
        "Atomic no-overwrite GPX publication is unavailable on this platform.");
    return false;
#endif
}

bool sourceStillMatches(const QString &requestedPath, const QString &canonical,
                        qint64 size, const QDateTime &modifiedUtc)
{
    const QFileInfo current(requestedPath);
    return !current.isSymLink() && current.isFile()
        && current.canonicalFilePath() == canonical
        && current.size() == size
        && current.lastModified().toUTC() == modifiedUtc;
}

bool cancelled(const DataFlashGpxExporter::CancelCheck &cancel,
               Result *result)
{
    if (cancel && cancel()) {
        result->cancelled = true;
        return true;
    }
    return false;
}

class ProgressReporter final
{
public:
    ProgressReporter(qint64 sourceSize,
                     const DataFlashGpxExporter::Progress &callback)
        : m_sourceSize(sourceSize)
        , m_total(qMax<qint64>(1, sourceSize))
        , m_callback(callback)
        , m_minimumStep(qMax<qint64>(1, m_total / 1000))
    {
    }

    qint64 total() const { return m_total; }

    void begin()
    {
        reportValue(0, true);
    }

    void update(int pass, qint64 position, bool force = false)
    {
        if (m_sourceSize <= 0) {
            reportValue(pass >= 3 ? m_total : 0, force);
            return;
        }
        const long double numerator =
            static_cast<long double>(pass) * m_sourceSize
            + qBound<qint64>(0, position, m_sourceSize);
        const qint64 value = qBound<qint64>(
            0, static_cast<qint64>(numerator / 4.0L), m_total);
        reportValue(value, force);
    }

    void finish()
    {
        reportValue(m_total, true);
    }

private:
    void reportValue(qint64 value, bool force)
    {
        if (!m_callback || (!force && m_last >= 0
                            && value - m_last < m_minimumStep)) {
            return;
        }
        if (m_last >= 0 && value < m_last) {
            value = m_last;
        }
        if (value == m_last && !force) {
            return;
        }
        m_last = value;
        m_callback(value, m_total);
    }

    qint64 m_sourceSize = 0;
    qint64 m_total = 1;
    const DataFlashGpxExporter::Progress &m_callback;
    qint64 m_minimumStep = 1;
    qint64 m_last = -1;
};

int fieldIndex(const DataFlashRaw::Definition &definition,
               const QList<QByteArray> &names)
{
    for (const QByteArray &name : names) {
        const int index = definition.columns.indexOf(name);
        if (index >= 0) {
            return index;
        }
    }
    return -1;
}

float halfToFloat(quint16 bits)
{
    const quint32 sign = quint32(bits & 0x8000u) << 16;
    quint32 exponent = (bits >> 10) & 0x1fu;
    quint32 fraction = bits & 0x03ffu;
    quint32 output = 0;
    if (exponent == 0) {
        if (fraction == 0) {
            output = sign;
        } else {
            int shift = 0;
            while ((fraction & 0x0400u) == 0) {
                fraction <<= 1;
                ++shift;
            }
            fraction &= 0x03ffu;
            output = sign | (quint32(127 - 14 - shift) << 23)
                | (fraction << 13);
        }
    } else if (exponent == 0x1fu) {
        output = sign | 0x7f800000u | (fraction << 13);
    } else {
        output = sign | ((exponent + (127 - 15)) << 23)
            | (fraction << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &output, sizeof(value));
    return value;
}

QByteArray normalizeDotNetExponent(const QByteArray &input)
{
    int marker = input.indexOf('e');
    if (marker < 0) {
        marker = input.indexOf('E');
    }
    if (marker < 0) {
        return input;
    }
    int digit = marker + 1;
    char sign = '+';
    if (digit < input.size()
        && (input.at(digit) == '+' || input.at(digit) == '-')) {
        sign = input.at(digit++);
    }
    return input.left(marker) + 'E' + sign
        + input.mid(digit).rightJustified(2, '0');
}

QByteArray dotNetSingle(float value)
{
    std::array<char, 128> storage{};
    auto converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                   value, std::chars_format::general);
    if (converted.ec != std::errc()) {
        return QString::number(value, 'g', 9).toUpper().toLatin1();
    }
    QByteArray general(storage.data(), int(converted.ptr - storage.data()));
    int marker = general.indexOf('e');
    if (marker < 0) {
        marker = general.indexOf('E');
    }
    if (marker < 0) {
        return general;
    }
    const int exponent = general.mid(marker + 1).toInt();
    if (exponent >= 0 && exponent < 9) {
        converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                  value, std::chars_format::fixed);
        if (converted.ec == std::errc()) {
            return QByteArray(storage.data(), int(converted.ptr - storage.data()));
        }
    }
    return normalizeDotNetExponent(general);
}

bool roundTrips(const QString &text, double value)
{
    const QByteArray bytes = text.toLatin1();
    double parsed = 0.0;
    const auto converted = std::from_chars(
        bytes.constData(), bytes.constData() + bytes.size(), parsed,
        std::chars_format::general);
    return converted.ec == std::errc()
        && converted.ptr == bytes.constData() + bytes.size()
        && parsed == value;
}

QByteArray dotNetDouble(double value)
{
    if (std::isnan(value)) {
        return QByteArrayLiteral("NaN");
    }
    if (std::isinf(value)) {
        return value < 0.0 ? QByteArrayLiteral("-Infinity")
                           : QByteArrayLiteral("Infinity");
    }
    if (value == 0.0) {
        return std::signbit(value) ? QByteArrayLiteral("-0")
                                   : QByteArrayLiteral("0");
    }
    constexpr int maximum = 17;
    for (int precision = 1; precision <= maximum; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general, value)) {
            continue;
        }
        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0) {
            return general.toLatin1();
        }
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < maximum) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed, value)) {
                return fixed.toLatin1();
            }
        }
        return normalizeDotNetExponent(general.toLatin1());
    }
    return normalizeDotNetExponent(
        QString::number(value, 'g', maximum).toLatin1());
}

double binaryScalar(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, int index)
{
    if (index < 0 || index >= definition.format.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const int offset = definition.offsets.at(index);
    const int width = DataFlashRaw::width(definition.format.at(index));
    if (offset < 0 || width <= 0 || offset + width > raw.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + offset);
    switch (definition.format.at(index)) {
    case 'b': return qint8(*at);
    case 'B': case 'M': return *at;
    case 'h': return qFromLittleEndian<qint16>(at);
    case 'H': return qFromLittleEndian<quint16>(at);
    case 'i': return qFromLittleEndian<qint32>(at);
    case 'I': return qFromLittleEndian<quint32>(at);
    case 'q': return static_cast<double>(qFromLittleEndian<qint64>(at));
    case 'Q': return static_cast<double>(qFromLittleEndian<quint64>(at));
    case 'c': return qFromLittleEndian<qint16>(at) / 100.0;
    case 'C': return qFromLittleEndian<quint16>(at) / 100.0;
    case 'e': return qFromLittleEndian<qint32>(at) / 100.0;
    case 'E': return qFromLittleEndian<quint32>(at) / 100.0;
    case 'L': return qFromLittleEndian<qint32>(at) / 10000000.0;
    case 'f': {
        const quint32 bits = qFromLittleEndian<quint32>(at);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        bool ok = false;
        const double roundTripped = dotNetSingle(value).toDouble(&ok);
        return ok ? roundTripped : std::numeric_limits<double>::quiet_NaN();
    }
    case 'd': {
        const quint64 bits = qFromLittleEndian<quint64>(at);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case 'g': {
        const float value = halfToFloat(qFromLittleEndian<quint16>(at));
        bool ok = false;
        const double roundTripped = dotNetSingle(value).toDouble(&ok);
        return ok ? roundTripped : std::numeric_limits<double>::quiet_NaN();
    }
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
}

bool fieldNumber(const QByteArray &raw,
                 const DataFlashRaw::Definition &definition, bool text,
                 const QList<QByteArray> &names, double *value)
{
    const int index = fieldIndex(definition, names);
    if (index < 0) {
        return false;
    }
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (text) {
        const QList<QByteArray> values =
            DataFlashRaw::textValues(raw, definition);
        if (index >= values.size()) {
            return false;
        }
        bool ok = false;
        parsed = values.at(index).trimmed().toDouble(&ok);
        if (!ok) {
            return false;
        }
    } else {
        parsed = binaryScalar(raw, definition, index);
    }
    *value = parsed;
    return true;
}

bool binaryIntegral(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, int index,
                    qint64 *value)
{
    if (index < 0 || index >= definition.format.size()) {
        return false;
    }
    const int offset = definition.offsets.at(index);
    const int width = DataFlashRaw::width(definition.format.at(index));
    if (offset < 0 || width <= 0 || offset + width > raw.size()) {
        return false;
    }
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + offset);
    switch (definition.format.at(index)) {
    case 'b': *value = qint8(*at); return true;
    case 'B': case 'M': *value = *at; return true;
    case 'h': *value = qFromLittleEndian<qint16>(at); return true;
    case 'H': *value = qFromLittleEndian<quint16>(at); return true;
    case 'i': *value = qFromLittleEndian<qint32>(at); return true;
    case 'I': *value = qFromLittleEndian<quint32>(at); return true;
    case 'q': *value = qFromLittleEndian<qint64>(at); return true;
    case 'Q': {
        const quint64 number = qFromLittleEndian<quint64>(at);
        if (number > quint64(std::numeric_limits<qint64>::max())) {
            return false;
        }
        *value = static_cast<qint64>(number);
        return true;
    }
    default: {
        const double number = binaryScalar(raw, definition, index);
        if (!std::isfinite(number) || number != std::trunc(number)
            || number < static_cast<double>(std::numeric_limits<qint64>::min())
            || number >= -static_cast<double>(std::numeric_limits<qint64>::min())) {
            return false;
        }
        *value = static_cast<qint64>(number);
        return true;
    }
    }
}

bool fieldIntegral(const QByteArray &raw,
                   const DataFlashRaw::Definition &definition, bool text,
                   const QList<QByteArray> &names, qint64 *value)
{
    const int index = fieldIndex(definition, names);
    if (index < 0) {
        return false;
    }
    if (!text) {
        return binaryIntegral(raw, definition, index, value);
    }
    const QList<QByteArray> values = DataFlashRaw::textValues(raw, definition);
    if (index >= values.size()) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = values.at(index).trimmed().toLongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

bool fieldIntegralAt(const QByteArray &raw,
                     const DataFlashRaw::Definition &definition, bool text,
                     int index, qint64 *value)
{
    if (index < 0 || index >= definition.columns.size()) {
        return false;
    }
    if (!text) {
        return binaryIntegral(raw, definition, index, value);
    }
    const QList<QByteArray> values = DataFlashRaw::textValues(raw, definition);
    if (index >= values.size()) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = values.at(index).trimmed().toLongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

bool fieldString(const QByteArray &raw,
                 const DataFlashRaw::Definition &definition, bool text,
                 const QList<QByteArray> &names, QByteArray *value)
{
    const int index = fieldIndex(definition, names);
    if (index < 0) {
        return false;
    }
    if (text) {
        const QList<QByteArray> values =
            DataFlashRaw::textValues(raw, definition);
        if (index >= values.size()) {
            return false;
        }
        *value = values.at(index).trimmed();
        return true;
    }
    const char type = definition.format.at(index);
    if (!QByteArrayLiteral("nNZaA").contains(type)) {
        return false;
    }
    *value = DataFlashRaw::ascii(raw.mid(
        definition.offsets.at(index), DataFlashRaw::width(type)));
    return true;
}

struct Clock
{
    bool valid = false;
    QDateTime anchorUtc;
    qint64 offsetMilliseconds = 0;
};

bool clockCandidate(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, bool text,
                    const DataFlashRaw::Definition &primaryGpsLayout,
                    Clock *clock)
{
    // GetTimeGPS deliberately looks up offsets in the primary "GPS" schema
    // even when DFItem's constructor is processing GPS2/GPSB. Preserve that
    // positional quirk for the constructor's FMTU-instance pre-scan.
    qint64 status = 0;
    const int statusIndex = primaryGpsLayout.columns.indexOf(
        QByteArrayLiteral("Status"));
    if (statusIndex >= 0
        && fieldIntegralAt(raw, definition, text, statusIndex, &status)
        && (status == 0 || status == 1 || status == 2)) {
        return false;
    }

    qint64 week = 0;
    qint64 milliseconds = 0;
    int weekIndex = primaryGpsLayout.columns.indexOf(
        QByteArrayLiteral("Week"));
    if (weekIndex < 0) {
        weekIndex = primaryGpsLayout.columns.indexOf(QByteArrayLiteral("GWk"));
    }
    int millisecondsIndex = primaryGpsLayout.columns.indexOf(
        QByteArrayLiteral("TimeMS"));
    if (millisecondsIndex < 0) {
        millisecondsIndex = primaryGpsLayout.columns.indexOf(
            QByteArrayLiteral("GMS"));
    }
    if (!fieldIntegralAt(raw, definition, text, weekIndex, &week)
        || !fieldIntegralAt(raw, definition, text, millisecondsIndex,
                            &milliseconds)
        || week < 0 || week > 5000 || milliseconds < 0
        || milliseconds > 7LL * 24 * 60 * 60 * 1000) {
        return false;
    }

    qint64 offset = 0;
    qint64 value = 0;
    if (definition.columns.contains(QByteArrayLiteral("T"))) {
        if (!fieldIntegral(raw, definition, text, {"T"}, &value)
            || value < std::numeric_limits<int>::min()
            || value > std::numeric_limits<int>::max()) {
            return false;
        }
        offset = value;
    }
    if (definition.columns.contains(QByteArrayLiteral("TimeUS"))) {
        if (!fieldIntegral(raw, definition, text, {"TimeUS"}, &value)) {
            return false;
        }
        offset = value / 1000;
    }

    const QDateTime epoch(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC);
    const QDateTime anchor = epoch.addDays(week * 7)
        .addMSecs(milliseconds).addSecs(-CurrentGpsUtcOffsetSeconds);
    if (!anchor.isValid()) {
        return false;
    }
    clock->valid = true;
    clock->anchorUtc = anchor;
    clock->offsetMilliseconds = offset;
    return true;
}

bool messageMilliseconds(const QByteArray &raw,
                         const DataFlashRaw::Definition &definition,
                         bool text, double *milliseconds)
{
    qint64 value = 0;
    if (definition.columns.contains(QByteArrayLiteral("TimeMS"))) {
        if (!fieldIntegral(raw, definition, text, {"TimeMS"}, &value)) {
            return false;
        }
        *milliseconds = static_cast<double>(value);
        return true;
    }
    if (definition.columns.contains(QByteArrayLiteral("TimeUS"))) {
        if (!fieldIntegral(raw, definition, text, {"TimeUS"}, &value)) {
            return false;
        }
        *milliseconds = static_cast<double>(value) / 1000.0;
        return true;
    }
    if (definition.columns.contains(QByteArrayLiteral("T"))) {
        if (!fieldIntegral(raw, definition, text, {"T"}, &value)) {
            return false;
        }
        *milliseconds = static_cast<double>(value);
        return true;
    }
    *milliseconds = 0.0;
    return true;
}

qint64 floorDivide(qint64 numerator, qint64 denominator)
{
    qint64 quotient = numerator / denominator;
    const qint64 remainder = numerator % denominator;
    if (remainder < 0) {
        --quotient;
    }
    return quotient;
}

bool pointTime(const Clock &clock, double messageMs, QString *text)
{
    if (!std::isfinite(messageMs)) {
        return false;
    }
    // DFItem performs this expression in Double before the Int64 cast.
    const double relative =
        (messageMs - static_cast<double>(clock.offsetMilliseconds))
        * static_cast<double>(TicksPerMillisecond);
    if (!std::isfinite(relative)
        || relative < static_cast<double>(std::numeric_limits<qint64>::min())
        || relative >= -static_cast<double>(std::numeric_limits<qint64>::min())) {
        return false;
    }
    const qint64 relativeTicks = static_cast<qint64>(relative);
    if (!clock.valid && relativeTicks < 0) {
        return false;
    }

    QDateTime timestamp;
    if (clock.valid) {
        const qint64 anchorMilliseconds = clock.anchorUtc.toMSecsSinceEpoch();
        if (anchorMilliseconds > std::numeric_limits<qint64>::max()
                                    / TicksPerMillisecond
            || anchorMilliseconds < std::numeric_limits<qint64>::min()
                                    / TicksPerMillisecond) {
            return false;
        }
        const qint64 anchorTicks = anchorMilliseconds * TicksPerMillisecond;
        if ((relativeTicks > 0
             && anchorTicks > std::numeric_limits<qint64>::max() - relativeTicks)
            || (relativeTicks < 0
                && anchorTicks < std::numeric_limits<qint64>::min()
                                   - relativeTicks)) {
            return false;
        }
        timestamp = QDateTime::fromSecsSinceEpoch(
            floorDivide(anchorTicks + relativeTicks, TicksPerSecond), Qt::UTC);
    } else {
        // DateTime.MinValue is Unspecified in MP10, so ToUniversalTime applies
        // the machine's local-zone policy (including its lower-bound clamp).
        // QDateTime stores milliseconds, while GPX prints whole seconds.
        const QDateTime minimum(QDate(1, 1, 1), QTime(0, 0), Qt::LocalTime);
        const qint64 milliseconds = floorDivide(relativeTicks,
                                                 TicksPerMillisecond);
        timestamp = minimum.addMSecs(milliseconds);
    }
    QDateTime utc = timestamp.toUTC();
    const QDateTime minimumUtc(QDate(1, 1, 1), QTime(0, 0), Qt::UTC);
    if (!clock.valid && (!utc.isValid() || utc < minimumUtc)) {
        // DateTime.ToUniversalTime clamps an underflow at DateTime.MinValue.
        // Qt also represents BCE dates, so an underflow can remain valid.
        utc = minimumUtc;
    }
    if (!utc.isValid() || utc.date().year() < 1 || utc.date().year() > 9999) {
        return false;
    }
    *text = utc.toString(
        QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
    return !text->isEmpty();
}

struct SchemaFacts
{
    QVector<int> instanceTypes;
    QSet<int> instanceTypeSet;
    DataFlashRaw::Definition primaryGpsLayout;
    bool hasPrimaryGpsLayout = false;
    QByteArray digest;
    qint64 blankLines = 0;
    int trailingBytes = 0;
    int trailingType = -1;
};

template<typename Handler>
bool rawPass(const QString &path, bool text, qint64 expectedSize, int pass,
             const DataFlashGpxExporter::CancelCheck &cancel,
             ProgressReporter *progress, Result *result, QByteArray *digest,
             Handler handler)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result->error = QStringLiteral("Could not open the DataFlash input: %1")
                            .arg(file.errorString());
        return false;
    }
    DataFlashRaw::Reader reader(&file, text);
    QByteArray raw;
    int kind = -1;
    while (file.pos() < expectedSize) {
        if (cancelled(cancel, result)) {
            return false;
        }
        if (!reader.next(&raw, &kind)) {
            if (!reader.error.isEmpty()) {
                result->error = reader.error;
            } else {
                result->error = QStringLiteral(
                    "The DataFlash input ended before its snapshotted size.");
            }
            return false;
        }
        if (file.pos() > expectedSize) {
            result->error = QStringLiteral(
                "The DataFlash input grew or changed while it was being read.");
            return false;
        }
        if (!handler(raw, kind, reader)) {
            return false;
        }
        progress->update(pass, file.pos());
    }
    if (file.pos() != expectedSize || file.size() != expectedSize) {
        result->error = QStringLiteral(
            "The DataFlash input changed while it was being read.");
        return false;
    }
    if (digest) {
        *digest = reader.hash.result();
    }
    progress->update(pass, expectedSize, true);
    return true;
}

bool hashFile(const QString &path, qint64 expectedSize,
              const DataFlashGpxExporter::CancelCheck &cancel,
              ProgressReporter *progress, QByteArray *digest, Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result->error = QStringLiteral(
            "Could not reopen the DataFlash input for final verification: %1")
                            .arg(file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (read < expectedSize) {
        if (cancelled(cancel, result)) {
            return false;
        }
        const QByteArray bytes = file.read(
            qMin(ReadChunkBytes, expectedSize - read));
        if (bytes.isEmpty()) {
            result->error = QStringLiteral(
                "The DataFlash input changed or failed during final verification.");
            return false;
        }
        hash.addData(bytes);
        read += bytes.size();
        progress->update(3, read);
    }
    if (file.size() != expectedSize) {
        result->error = QStringLiteral(
            "The DataFlash input changed during final verification.");
        return false;
    }
    *digest = hash.result();
    progress->update(3, expectedSize, true);
    return true;
}

} // namespace

DataFlashGpxExporter::Result DataFlashGpxExporter::Export(
    const QString &inputPath, const QString &outputPath,
    const CancelCheck &cancel, const Progress &progress)
{
    Result result;
    try {
        const QFileInfo requestedSource(inputPath);
        const QString sourceSuffix = requestedSource.suffix().toLower();
        if (inputPath.isEmpty() || outputPath.isEmpty()
            || (sourceSuffix != QLatin1String("bin")
                && sourceSuffix != QLatin1String("log"))
            || QFileInfo(outputPath).suffix().compare(
                   QLatin1String("gpx"), Qt::CaseInsensitive) != 0) {
            result.error = QStringLiteral(
                "Select a DataFlash .bin or .log input and a new .gpx output path.");
            return result;
        }
        if (requestedSource.isSymLink() || !requestedSource.isFile()
            || !requestedSource.isReadable()) {
            result.error = QStringLiteral(
                "The DataFlash input must be a readable regular, non-symlink file.");
            return result;
        }
        const QString sourcePath = requestedSource.canonicalFilePath();
        const qint64 sourceSize = requestedSource.size();
        const QDateTime sourceModified = requestedSource.lastModified().toUTC();
        if (sourcePath.isEmpty() || sourceSize < 0) {
            result.error = QStringLiteral("Could not resolve the DataFlash input file.");
            return result;
        }

        const QFileInfo requestedOutput(outputPath);
        if (pathExists(outputPath)) {
            result.error = QStringLiteral(
                "The GPX output already exists; it was not overwritten.");
            return result;
        }
        const QString parentPath = QFileInfo(requestedOutput.absolutePath())
                                       .canonicalFilePath();
        const QFileInfo parent(parentPath);
        if (parentPath.isEmpty() || !parent.isDir() || !parent.isWritable()
            || requestedOutput.fileName().isEmpty()) {
            result.error = QStringLiteral(
                "The GPX output directory must already exist and be writable.");
            return result;
        }
        const QString destination = QDir(parentPath).filePath(
            requestedOutput.fileName());
        if (destination == sourcePath) {
            result.error = QStringLiteral(
                "The GPX output must be different from the DataFlash input.");
            return result;
        }
        if (cancelled(cancel, &result)) {
            return result;
        }

        const bool text = sourceSuffix == QLatin1String("log");
        ProgressReporter reporter(sourceSize, progress);
        reporter.begin();

        SchemaFacts facts;
        if (!rawPass(sourcePath, text, sourceSize, 0, cancel, &reporter,
                     &result, &facts.digest,
                     [&facts, text](const QByteArray &raw, int kind,
                                    DataFlashRaw::Reader &reader) {
            if (kind == 0
                && reader.currentDefinition.name == QByteArrayLiteral("GPS")) {
                facts.primaryGpsLayout = reader.currentDefinition;
                facts.hasPrimaryGpsLayout = true;
            }
            if (kind == 1) {
                qint64 type = -1;
                QByteArray units;
                if (fieldIntegral(raw, reader.currentDefinition, text,
                                  {"FmtType"}, &type)
                    && type >= 0 && type <= 255
                    && fieldString(raw, reader.currentDefinition, text,
                                   {"UnitIds"}, &units)
                    && units.contains('#')
                    && !facts.instanceTypeSet.contains(int(type))) {
                    facts.instanceTypeSet.insert(int(type));
                    facts.instanceTypes.append(int(type));
                }
            }
            facts.blankLines = reader.blankLines;
            facts.trailingBytes = reader.trailingBytes;
            facts.trailingType = reader.trailingType;
            return true;
        })) {
            return result;
        }
        if (!sourceStillMatches(inputPath, sourcePath, sourceSize,
                                sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed during schema discovery; no GPX file was published.");
            return result;
        }

        QHash<int, int> instanceCounts;
        QHash<int, Clock> instanceClocks;
        Clock lazyGpsClock;
        QByteArray clockDigest;
        if (!rawPass(sourcePath, text, sourceSize, 1, cancel, &reporter,
                     &result, &clockDigest,
                     [&facts, &instanceCounts, &instanceClocks,
                      &lazyGpsClock, text](const QByteArray &raw, int kind,
                                           DataFlashRaw::Reader &reader) {
            if (kind != -1) {
                return true;
            }
            const DataFlashRaw::Definition &definition =
                reader.currentDefinition;
            if (facts.hasPrimaryGpsLayout
                && definition.name == QByteArrayLiteral("GPS")
                && !lazyGpsClock.valid) {
                Clock candidate;
                if (clockCandidate(raw, definition, text,
                                   facts.primaryGpsLayout, &candidate)) {
                    lazyGpsClock = candidate;
                }
            }
            if (!facts.hasPrimaryGpsLayout
                || !definition.name.startsWith(QByteArrayLiteral("GPS"))
                || !facts.instanceTypeSet.contains(definition.id)) {
                return true;
            }
            int &seen = instanceCounts[definition.id];
            if (seen >= InstanceClockRecordLimit) {
                return true;
            }
            ++seen;
            if (!instanceClocks.value(definition.id).valid) {
                Clock candidate;
                if (clockCandidate(raw, definition, text,
                                   facts.primaryGpsLayout, &candidate)) {
                    instanceClocks.insert(definition.id, candidate);
                }
            }
            return true;
        })) {
            return result;
        }
        if (clockDigest != facts.digest
            || !sourceStillMatches(inputPath, sourcePath, sourceSize,
                                   sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed during clock discovery; no GPX file was published.");
            return result;
        }
        Clock clock;
        for (int type : facts.instanceTypes) {
            if (instanceClocks.value(type).valid) {
                clock = instanceClocks.value(type);
                break;
            }
        }
        if (!clock.valid) {
            clock = lazyGpsClock;
        }

        QTemporaryFile staged(QDir(parentPath).filePath(
            QStringLiteral(".apm-dataflash-gpx-XXXXXX.partial")));
        staged.setAutoRemove(true);
        if (!staged.open()) {
            result.error = QStringLiteral(
                "Could not create a private staged GPX file: %1")
                               .arg(staged.errorString());
            return result;
        }
        QXmlStreamWriter xml(&staged);
        xml.setAutoFormatting(true);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("gpx"));
        xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
        xml.writeAttribute(QStringLiteral("creator"),
                           QStringLiteral("Mission Planner"));
        xml.writeDefaultNamespace(
            QStringLiteral("http://www.topografix.com/GPX/1/1"));
        xml.writeStartElement(QStringLiteral("trk"));
        xml.writeStartElement(QStringLiteral("trkseg"));

        qint64 noFix = 0;
        qint64 malformedCoordinates = 0;
        qint64 rejectedCoordinates = 0;
        bool sawGps = false;
        QByteArray exportDigest;
        if (!rawPass(sourcePath, text, sourceSize, 2, cancel, &reporter,
                     &result, &exportDigest,
                     [&result, &xml, &clock, &noFix, &malformedCoordinates,
                      &rejectedCoordinates, &sawGps, text]
                     (const QByteArray &raw, int kind,
                      DataFlashRaw::Reader &reader) {
            if (kind != -1
                || reader.currentDefinition.name != QByteArrayLiteral("GPS")) {
                return true;
            }
            sawGps = true;
            qint64 status = 0;
            if (!fieldIntegral(raw, reader.currentDefinition, text,
                               {"Status"}, &status)
                || status < std::numeric_limits<int>::min()
                || status > std::numeric_limits<int>::max()
                || status < 3) {
                ++noFix;
                return true;
            }
            double latitude = 0.0;
            double longitude = 0.0;
            double altitude = 0.0;
            if (!fieldNumber(raw, reader.currentDefinition, text,
                             {"Lat"}, &latitude)
                || !fieldNumber(raw, reader.currentDefinition, text,
                                {"Lng"}, &longitude)
                || !fieldNumber(raw, reader.currentDefinition, text,
                                {"Alt"}, &altitude)
                || !std::isfinite(latitude) || !std::isfinite(longitude)
                || !std::isfinite(altitude)) {
                ++malformedCoordinates;
                return true;
            }
            if (latitude < -90.0 || latitude > 90.0
                || longitude < -180.0 || longitude > 180.0
                || (latitude == 0.0 && longitude == 0.0)) {
                ++rejectedCoordinates;
                return true;
            }
            double milliseconds = 0.0;
            QString timestamp;
            if (!messageMilliseconds(raw, reader.currentDefinition, text,
                                     &milliseconds)
                || !pointTime(clock, milliseconds, &timestamp)) {
                result.error = QStringLiteral(
                    "A selected GPS record has an invalid Mission Planner timestamp.");
                return false;
            }

            xml.writeStartElement(QStringLiteral("trkpt"));
            xml.writeAttribute(QStringLiteral("lat"),
                               QString::fromLatin1(dotNetDouble(latitude)));
            xml.writeAttribute(QStringLiteral("lon"),
                               QString::fromLatin1(dotNetDouble(longitude)));
            xml.writeTextElement(QStringLiteral("ele"),
                                 QString::fromLatin1(dotNetDouble(altitude)));
            xml.writeTextElement(QStringLiteral("time"), timestamp);
            xml.writeEndElement();
            ++result.pointCount;
            return !xml.hasError();
        })) {
            if (result.error.isEmpty() && !result.cancelled) {
                result.error = QStringLiteral("Could not write the staged GPX document.");
            }
            return result;
        }
        xml.writeEndElement(); // trkseg
        xml.writeEndElement(); // trk
        xml.writeEndElement(); // gpx
        xml.writeEndDocument();
        if (xml.hasError()) {
            result.error = QStringLiteral("Could not write the staged GPX document.");
            return result;
        }
        if (exportDigest != facts.digest
            || !sourceStillMatches(inputPath, sourcePath, sourceSize,
                                   sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed during export; no GPX file was published.");
            return result;
        }

        if (facts.blankLines > 0) {
            result.warnings.append(QStringLiteral(
                "Ignored %1 blank DataFlash text line(s).")
                                       .arg(facts.blankLines));
        }
        if (facts.trailingBytes > 0) {
            result.warnings.append(QStringLiteral(
                "Dropped %1 trailing byte(s) from an incomplete final record of type %2.")
                                       .arg(facts.trailingBytes)
                                       .arg(facts.trailingType));
        }
        if (noFix > 0) {
            result.warnings.append(QStringLiteral(
                "Skipped %1 GPS record(s) without a valid 3D fix.").arg(noFix));
        }
        if (malformedCoordinates > 0) {
            result.warnings.append(QStringLiteral(
                "Skipped %1 GPS record(s) with malformed or non-finite coordinates or altitude.")
                                       .arg(malformedCoordinates));
        }
        if (rejectedCoordinates > 0) {
            result.warnings.append(QStringLiteral(
                "Skipped %1 GPS record(s) outside the GPX coordinate range or at (0,0).")
                                       .arg(rejectedCoordinates));
        }
        if (!clock.valid && sawGps) {
            result.warnings.append(QStringLiteral(
                "No valid GPS week/time anchor was found; timestamps use Mission Planner's year-0001 relative-time fallback."));
        }
        if (result.pointCount == 0) {
            result.warnings.append(QStringLiteral(
                "The input contained no GPS track points accepted by Mission Planner's filter."));
        }

        if (!staged.flush()) {
            result.error = QStringLiteral("Could not flush the staged GPX file: %1")
                               .arg(staged.errorString());
            return result;
        }
        staged.close();
        if (cancelled(cancel, &result)) {
            return result;
        }

        QByteArray finalDigest;
        if (!hashFile(sourcePath, sourceSize, cancel, &reporter,
                      &finalDigest, &result)) {
            return result;
        }
        if (finalDigest != facts.digest
            || !sourceStillMatches(inputPath, sourcePath, sourceSize,
                                   sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed before publication; no GPX file was published.");
            return result;
        }
        const QString finalParent = QFileInfo(requestedOutput.absolutePath())
                                        .canonicalFilePath();
        if (finalParent != parentPath || pathExists(destination)) {
            result.error = QStringLiteral(
                "The GPX destination changed before publication; nothing was overwritten.");
            return result;
        }
        if (cancelled(cancel, &result)) {
            return result;
        }
        reporter.finish();
        if (cancelled(cancel, &result)) {
            return result;
        }
        if (pathExists(destination)
            || !publishNoReplace(staged.fileName(), destination,
                                 &result.error)) {
            return result;
        }

        result.success = true;
        result.outputPath = destination;
        // No callback follows publication. A completed GPX is never removed
        // by this exporter after ownership has transferred to the caller.
    } catch (const std::exception &error) {
        result.error = QStringLiteral("DataFlash GPX export failed: %1")
                           .arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected DataFlash GPX export failure.");
    }
    return result;
}
