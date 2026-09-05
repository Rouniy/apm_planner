#ifndef GPSCORRECTIONEXTRACTOR_H
#define GPSCORRECTIONEXTRACTOR_H

#include <QString>

#include <functional>

class GpsCorrectionExtractor
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64 bytesProcessed,
                                        qint64 bytesTotal)>;

    struct Result
    {
        bool success = false;
        bool cancelled = false;
        qint64 messagesWritten = 0;
        qint64 bytesWritten = 0;
        qint64 recordsRead = 0;
        qint64 skippedBytes = 0;
        qint64 rejectedFrames = 0;
        bool truncatedTail = false;
        QString error;
    };

    /**
     * Extracts GPS_INJECT_DATA and GPS_RTCM_DATA payload bytes from every
     * sender in their original tlog order. The destination is atomically
     * replaced only after a complete, non-cancelled scan.
     */
    static Result Extract(const QString &inputPath,
                          const QString &outputPath,
                          CancelCheck cancel = {},
                          Progress progress = {});
};

#endif // GPSCORRECTIONEXTRACTOR_H
