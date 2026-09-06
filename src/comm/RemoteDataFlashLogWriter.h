#ifndef REMOTEDATAFLASHLOGWRITER_H
#define REMOTEDATAFLASHLOGWRITER_H

#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <memory>

class QThread;

// Owner-thread facade. All file access, including opening and removal, belongs
// to its dedicated worker. Destroying the facade preserves nonempty .part
// staging (unless explicitly discarded), aborts and joins that worker.
class RemoteDataFlashLogWriter final : public QObject {
    Q_OBJECT
public:
    struct MissingRange { quint32 first = 0, last = 0; };
    struct Result {
        bool success = false, cancelled = false, published = false, hasBlocks = false;
        // path is either the explicitly published .bin/.partial.bin or a
        // recoverable nonfinal .part on error; inspect published, not path alone.
        QString path, error;
        QStringList warnings;
        // bytes is the captured extent including holes; blocks counts distinct
        // fully flushed blocks. An I/O failure may leave a partial final block.
        qint64 bytes = 0, blocks = 0, duplicateBlocks = 0, missingBlocks = 0;
        quint32 highestSequence = 0;
        QVector<MissingRange> missingRanges;
    };
    static constexpr int BlockBytes = 200;
    static constexpr int MaximumPendingBlocks = 256;
    static constexpr qint64 MaximumFileBytes = 512LL * 1024 * 1024;
    static constexpr quint32 MaximumForwardGap = 4096;
    static constexpr int MaximumIntervals = 65536;

    explicit RemoteDataFlashLogWriter(QObject *parent = nullptr);
    ~RemoteDataFlashLogWriter() override;
    // Existing directory only; stem is 1..96 ASCII alphanumeric/_/- characters
    // beginning with an alphanumeric. generation must be nonzero. open never
    // reuses or overwrites an existing path. opened() reports the staging path.
    bool open(const QString &directory, const QString &stem, quint64 generation);
    bool append(quint32 sequence, const QByteArray &data);
    // Save drains admitted blocks before publication; discard aborts promptly.
    // preservePartial retains a nonfinal .part capture on unexpected loss;
    // automatic disk/protocol failures likewise preserve nonempty staging.
    // Empty save fails. Gaps are explicit zero-filled ranges in .partial.bin.
    // No EOF exists in this protocol: even gap-free output is only a capture.
    // Once the final rename begins, a concurrent discard/destruction cannot
    // revoke a publication already authorized by finish(true).
    bool finish(bool save, bool preservePartial = false);
    bool busy() const { return m_busy; }

signals:
    void opened(quint64 generation, const QString &path);
    void blockStored(quint64 generation, quint32 sequence, bool duplicate);
    void failed(quint64 generation, const QString &reason);
    void finished(quint64 generation, const RemoteDataFlashLogWriter::Result &result);

private:
    struct Control;
    class Worker;
    void abort(int reason);
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<Control> m_control;
    quint64 m_epoch = 0;
    int m_pending = 0;
    bool m_busy = false, m_opened = false, m_accepting = false;
};
Q_DECLARE_METATYPE(RemoteDataFlashLogWriter::Result)

#endif
