#ifndef DATAFLASHLOGSPLITTER_H
#define DATAFLASHLOGSPLITTER_H

#include <QString>
#include <QStringList>
#include <functional>

class DataFlashLogSplitter final
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QStringList outputs; // Published files, including partial publication on error.
        QStringList warnings;
        qint64 recordsRead = 0;
        qint64 dataRecords = 0;
        qint64 bytesWritten = 0; // Bytes in actually published outputs.
    };
    static QStringList OutputPaths(const QString &inputPath, int pieces);
    // Stages and validates every part before publication. Existing destinations
    // are never overwritten. Publication is not group-atomic; cancellation is
    // honored before publication, not between its individual no-overwrite renames.
    // Blank text lines and only a known, nonmetadata binary record truncated at
    // EOF are omitted with exact warnings. Their original bytes remain hashed.
    static Result Split(const QString &inputPath, int pieces,
                        CancelCheck cancel = {}, Progress progress = {});
};

#endif
