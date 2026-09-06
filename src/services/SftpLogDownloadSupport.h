#pragma once
#include "SftpLogSession.h"
#include <QStringList>

class SftpLogDownloadSupport {
public:
    using Cancel = SftpLogSession::Cancel;
    using Progress = std::function<void(qint64 completed, qint64 total, const QString &status)>;
    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        int savedLogs = 0;
        QStringList publishedPaths;
        QStringList warnings;
    };
    static QString safeBinName(const QString &remoteName, int fallbackIndex = 0);
    // Worker only; preserves completed BIN/LOG/KML on failure/cancel, never
    // replaces existing files. Source entry paths are immutable listed paths.
    static Result download(SftpLogSession &session, const QVector<SftpLogEntry> &entries,
                           const QString &destination, bool createKml,
                           Cancel cancel = {}, Progress progress = {});
};
