#ifndef DATAFLASHGPXEXPORTER_H
#define DATAFLASHGPXEXPORTER_H

#include <QString>
#include <QStringList>

#include <functional>

/**
 * Bounded, worker-safe DataFlash BIN/LOG to GPX track exporter.
 *
 * Input is streamed without a whole-log index. Output is staged beside its
 * destination and published with no-overwrite semantics only after the input
 * snapshot has been revalidated. Failure or cancellation leaves no
 * exporter-owned file behind.
 */
class DataFlashGpxExporter final
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64 completedWork,
                                        qint64 totalWork)>;

    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QString outputPath;
        QStringList warnings;
        qint64 pointCount = 0;
    };

    // Existing destinations are never replaced. The source must be a regular,
    // non-symlink .bin or DataFlash ASCII .log file and output must be a new
    // .gpx path. Progress is monotonic and normalized to the snapshotted input
    // size across the bounded clock, export and final verification passes.
    static Result Export(const QString &inputPath, const QString &outputPath,
                         const CancelCheck &cancel = {},
                         const Progress &progress = {});

private:
    DataFlashGpxExporter() = delete;
};

#endif // DATAFLASHGPXEXPORTER_H
