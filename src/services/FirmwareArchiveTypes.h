#ifndef FIRMWAREARCHIVETYPES_H
#define FIRMWAREARCHIVETYPES_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <functional>

namespace FirmwareArchive {
// Trusted observers only: callbacks must not mutate archive paths/content.
// Callbacks remain cancellable during verification; filesystem stamps and
// hashes do not provide exclusion against concurrent external writers.
using Cancel = std::function<bool()>;
using Progress = std::function<void(int completed, int total, const QString &item)>;
using ChunkSink = std::function<bool(const QByteArray &)>;
enum class Failure { None, Network, Policy, Limit, LocalIo, Cancelled };
struct FetchResult {
    Failure failure = Failure::None;
    QString error;
    qint64 bytes = 0;
    bool success() const { return failure == Failure::None; }
};
// Synchronous worker-only streaming transport. May be called concurrently by
// at most four workers. httpsOnly also governs every redirect, not only input.
using Fetch = std::function<FetchResult(const QUrl &, qint64 maximumBytes,
    bool httpsOnly, const Cancel &, const ChunkSink &)>;
struct Result {
    bool success = false, cancelled = false;
    // retainedStaging is the last known path, not a relocated-file locator.
    QString directory, error, retainedStaging;
    QUrl manifestSource;
    int fileCount = 0, failedFiles = 0;
    qint64 bytesDownloaded = 0;
    QStringList warnings;
};
constexpr int MaximumManifestBytes = 8 * 1024 * 1024;
constexpr qint64 MaximumFirmwareBytes = 256LL * 1024 * 1024;
constexpr int MaximumDownloads = 20000;
constexpr int MaximumParallelDownloads = 4;
}
#endif
