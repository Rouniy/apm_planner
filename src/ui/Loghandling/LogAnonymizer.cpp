#include "LogAnonymizer.h"
#include "DataFlashLogAnonymizer.h"
#include "TelemetryLogAnonymizer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRandomGenerator>
#include <QSaveFile>
#include <cmath>
#include <limits>

namespace {
QString resolvedPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool hashInput(QFile &input, QByteArray *digest, qint64 expected,
               const LogAnonymizeCancel &cancel,
               const LogAnonymizeProgress &progress, qint64 base,
               LogAnonymizeResult *result)
{
    if (!input.seek(0)) {
        result->error = input.errorString();
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!input.atEnd()) {
        if (cancel && cancel()) {
            result->cancelled = true;
            return false;
        }
        const QByteArray bytes = input.read(64 * 1024);
        if (bytes.isEmpty()) {
            result->error = QStringLiteral("Could not read input log: %1").arg(input.errorString());
            return false;
        }
        count += bytes.size();
        if (count > expected) {
            result->error = QStringLiteral("The input log changed while it was being processed.");
            return false;
        }
        hash.addData(bytes);
        if (progress) progress(base + count, expected * 3);
    }
    if (count != expected) {
        result->error = QStringLiteral("The input log changed while it was being processed.");
        return false;
    }
    *digest = hash.result();
    if (!input.seek(0)) {
        result->error = input.errorString();
        return false;
    }
    return true;
}
}

bool LogAnonymizer::formatForPath(const QString &path, Format *format)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    Format selected;
    if (suffix == QStringLiteral("bin")) selected = Format::BinaryDataFlash;
    else if (suffix == QStringLiteral("log")) selected = Format::TextDataFlash;
    else if (suffix == QStringLiteral("tlog")) selected = Format::Telemetry;
    else return false;
    if (format) *format = selected;
    return true;
}

double LogAnonymizer::generateRandomOffset()
{
    auto *random = QRandomGenerator::system();
    const double magnitude = 0.5 + random->bounded(1500001u) / 1000000.0;
    return random->bounded(2u) ? magnitude : -magnitude;
}

bool LogAnonymizer::parseOffset(const QString &text, double *value)
{
    if (!value) return false;
    if (text.trimmed().isEmpty()) {
        *value = generateRandomOffset();
        return true;
    }
    // No locale-dependent thousands separators: these are degrees, not groups.
    QLocale locale = QLocale::c();
    locale.setNumberOptions(QLocale::RejectGroupSeparator);
    bool ok = false;
    const double parsed = locale.toDouble(text.trimmed(), &ok);
    if (!ok || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

QString LogAnonymizer::privacyWarning()
{
    return QStringLiteral("Beta: inspect the output before sharing. This tool shifts recognized "
        "coordinate fields; it does NOT fully sanitize a log. Serial identifiers, parameters, "
        "messages, altitude, timestamps, relative tracks and unrecognized location fields may remain. "
        "TLOG output drops opaque GPS correction/file/log payloads, signing-key messages and "
        "commands without an audited coordinate interpretation. These removals are reported; "
        "they do not guarantee anonymity. One known location can reveal the translation. Keep the "
        "offsets private. Modified signed MAVLink frames are exported unsigned, not re-signed.");
}

LogAnonymizeResult LogAnonymizer::anonymizeFile(
    const QString &inputPath, const QString &outputPath,
    const LogAnonymizeOptions &options, const LogAnonymizeCancel &cancel,
    const LogAnonymizeProgress &progress)
{
    LogAnonymizeResult result;
    Format format;
    if (inputPath.trimmed().isEmpty() || outputPath.trimmed().isEmpty()) {
        result.error = QStringLiteral("Select both an input log and an output file.");
        return result;
    }
    if (!formatForPath(inputPath, &format)) {
        result.error = QStringLiteral("Supported input formats are .bin, .log and .tlog.");
        return result;
    }
    Format outputFormat;
    if (!formatForPath(outputPath, &outputFormat) || outputFormat != format) {
        result.error = QStringLiteral("The output must keep the input format (.bin, .log or .tlog).");
        return result;
    }
    if (!std::isfinite(options.latitudeOffset) || !std::isfinite(options.longitudeOffset)) {
        result.error = QStringLiteral("Coordinate offsets must be finite numbers.");
        return result;
    }
    if (resolvedPath(inputPath) == resolvedPath(outputPath)) {
        result.error = QStringLiteral("The output must not replace the source log.");
        return result;
    }
    const QFileInfo sourceInfo(inputPath);
    if (!sourceInfo.isFile()) {
        result.error = QStringLiteral("The input must be an existing regular log file.");
        return result;
    }
    const QString inputCanonical = sourceInfo.canonicalFilePath();
    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        result.error = input.errorString();
        return result;
    }
    const qint64 size = input.size();
    if (size <= 0 || size > std::numeric_limits<qint64>::max() / 3) {
        result.error = QStringLiteral("The input log is empty or too large.");
        return result;
    }
    QByteArray before, after;
    if (!hashInput(input, &before, size, cancel, progress, 0, &result)) return result;
    QSaveFile output(outputPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        result.error = output.errorString();
        return result;
    }
    const auto backendProgress = [&](qint64 done, qint64 total) {
        if (progress) {
            const long double ratio = total > 0
                ? qBound(0.0L, static_cast<long double>(done) / total, 1.0L) : 0;
            progress(size + static_cast<qint64>(ratio * size), size * 3);
        }
    };
    switch (format) {
    case Format::BinaryDataFlash:
        result = DataFlashLogAnonymizer::anonymize(&input, &output, options, cancel, backendProgress);
        break;
    case Format::TextDataFlash:
        result = TelemetryLogAnonymizer::anonymizeText(&input, &output, options, cancel, backendProgress);
        break;
    case Format::Telemetry:
        result = TelemetryLogAnonymizer::anonymizeTlog(&input, &output, options, cancel, backendProgress);
        break;
    }
    const bool transformed = result.success;
    result.success = false; // Success is publication, never just staged bytes.
    if (!transformed || result.cancelled) return result;
    if (!hashInput(input, &after, size, cancel, progress, size * 2, &result)) return result;
    if (before != after || QFileInfo(inputPath).canonicalFilePath() != inputCanonical
        || resolvedPath(inputPath) == resolvedPath(outputPath)) {
        result.error = QStringLiteral("The input log or selected paths changed during processing.");
        return result;
    }
    if (cancel && cancel()) {
        result.cancelled = true;
        return result;
    }
    result.inputBytes = size;
    result.outputBytes = output.pos();
    result.warnings.prepend(privacyWarning());
    if (!output.commit()) {
        result.error = output.errorString();
        return result;
    }
    result.success = true;
    return result;
}
