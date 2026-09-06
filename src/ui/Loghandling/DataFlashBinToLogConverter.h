#ifndef DATAFLASHBINTOLOGCONVERTER_H
#define DATAFLASHBINTOLOGCONVERTER_H

#include <QString>
#include <QStringList>

#include <functional>

/**
 * Bounded, worker-safe DataFlash BIN to Mission Planner ASCII LOG converter.
 *
 * Input is streamed and only the 256 possible DataFlash FMT definitions are
 * retained.  Output is staged beside its destination and published by one
 * no-overwrite rename; cancellation or failure leaves no converter-owned file.
 */
class DataFlashBinToLogConverter final
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64 completedBytes,
                                        qint64 totalBytes)>;

    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QString outputPath;
        QStringList warnings;
        qint64 recordsWritten = 0;
        qint64 recordsSkipped = 0;
        qint64 bytesRead = 0;
        qint64 bytesWritten = 0;
    };

    // Existing destinations are never replaced. The source must be a regular,
    // non-symlink .bin file and output must be a new regular .log path.
    static Result Convert(const QString &inputPath, const QString &outputPath,
                          const CancelCheck &cancel = {},
                          const Progress &progress = {});

private:
    DataFlashBinToLogConverter() = delete;
};

#endif // DATAFLASHBINTOLOGCONVERTER_H
