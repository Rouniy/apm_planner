#ifndef LOGINDEXTYPES_H
#define LOGINDEXTYPES_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace LogIndex {
using Cancel = std::function<bool()>;
using Progress = std::function<void(int completed, int total, const QString &path)>;
// Must be a worker-safe read-only canonical-cache lookup. No network fallback.
using TileReader = std::function<QByteArray(int x, int y, int zoom)>;

struct FileStamp {
    bool exists = false;
    qint64 sizeBytes = 0;
    QDateTime modifiedUtc;
    bool operator==(const FileStamp &other) const {
        return exists == other.exists && sizeBytes == other.sizeBytes
            && modifiedUtc == other.modifiedUtc;
    }
    bool operator!=(const FileStamp &other) const { return !(*this == other); }
};
struct Point { double latitude = 0; double longitude = 0; };
struct Home {
    bool valid = false;
    double latitude = 0, longitude = 0, altitudeMeters = 0;
};
struct Entry {
    QString fullPath, rootPath;
    FileStamp source, pairedRlog, thumbnail;
    QDateTime dateUtc;
    QString frame = QStringLiteral("Unknown");
    int systemId = 0;
    double durationSeconds = 0, timeInAirSeconds = 0, distanceMeters = 0;
    quint64 cameraMessages = 0;
    Home home;
    QByteArray thumbnailJpeg;
    QString error;
};
struct Analysis {
    Entry entry;
    QVector<Point> track;
    bool cancelled = false;
};
struct ScanResult {
    bool success = false, cancelled = false;
    QString rootPath, error;
    QVector<Entry> entries;
    QStringList warnings;
};
struct Discovery {
    bool success = false, cancelled = false;
    QString rootPath, error;
    QVector<Entry> files;
    QStringList warnings;
};
struct ThumbnailResult {
    QByteArray jpeg;
    QString warning;
    FileStamp sidecar;
    bool cancelled = false;
};
struct DeleteResult {
    bool success = false, cancelled = false;
    QString error;
    QStringList deletedLogs, deletedPaths, warnings;
    int remaining = 0;
};
constexpr int MaximumFiles = 20000;
constexpr int MaximumTrackPoints = 4000;
}

#endif
