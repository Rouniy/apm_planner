#ifndef LOGANONYMIZER_H
#define LOGANONYMIZER_H

#include <QString>
#include <QStringList>
#include <functional>

class QIODevice;

// Shared worker-only contract. Backends write an uncommitted staging device;
// only the file facade may publish it. No transport or global MAVLink state.
struct LogAnonymizeOptions
{
    double latitudeOffset = 0.0;
    double longitudeOffset = 0.0;
};

struct LogAnonymizeResult
{
    bool success = false;
    bool cancelled = false;
    QString error;
    QStringList warnings;
    qint64 inputBytes = 0;
    qint64 outputBytes = 0;
    qint64 records = 0;
    qint64 coordinateFields = 0;
    qint64 patchedValues = 0;
    qint64 strippedSignatures = 0;
};

using LogAnonymizeCancel = std::function<bool()>;
using LogAnonymizeProgress = std::function<void(qint64, qint64)>;

class LogAnonymizer final
{
public:
    enum class Format { BinaryDataFlash, TextDataFlash, Telemetry };
    static bool formatForPath(const QString &path, Format *format);
    static double generateRandomOffset();
    static bool parseOffset(const QString &text, double *value);
    static QString privacyWarning();
    static LogAnonymizeResult anonymizeFile(
        const QString &input, const QString &output,
        const LogAnonymizeOptions &options,
        const LogAnonymizeCancel &cancel = {},
        const LogAnonymizeProgress &progress = {});
};

#endif
