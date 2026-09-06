#include "RemoteDataFlashLogWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <iterator>

namespace {
enum Stop { Running = 0, Discard = 1, QueueOverflow = 2, InvalidBlock = 3, Shutdown = 4, PreservePartial = 5 };
QString stoppedError(int reason)
{
    if (reason == QueueOverflow) return QStringLiteral("Remote log disk queue exceeded 256 pending blocks; nothing was published.");
    if (reason == InvalidBlock) return QStringLiteral("Remote log blocks must contain exactly 200 bytes and fit the 512 MiB limit; nothing was published.");
    if (reason == Shutdown || reason == PreservePartial)
        return QStringLiteral("Remote log capture stopped unexpectedly; no final log was published.");
    return QStringLiteral("Remote log capture discarded; nothing was published.");
}
}

struct RemoteDataFlashLogWriter::Control {
    std::atomic<int> stop{Running};
    std::atomic<bool> explicitDiscard{false};
    quint64 epoch = 0, generation = 0;
};

class RemoteDataFlashLogWriter::Worker final : public QObject {
public:
    explicit Worker(RemoteDataFlashLogWriter *owner) : owner(owner) {}

    void open(const QString &directory, const QString &stem, const std::shared_ptr<Control> &next)
    {
        cleanup(); control = next;
        if (stopped()) return;
        const QFileInfo info(directory);
        if (!info.exists() || !info.isDir() || info.canonicalFilePath().isEmpty()) {
            fail(QStringLiteral("Choose an existing log directory; no directories were created.")); return;
        }
        static const QRegularExpression validStem(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,95}$"));
        if (!validStem.match(stem).hasMatch()) {
            fail(QStringLiteral("Invalid remote log filename stem (1–96 ASCII letters/digits/_/- required).")); return;
        }
        finalBase = QDir(info.canonicalFilePath()).filePath(stem + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces));
        file.reset(new QTemporaryFile(QDir(info.canonicalFilePath()).filePath(
            '.' + QFileInfo(finalBase).fileName() + QStringLiteral(".XXXXXX.part"))));
        file->setAutoRemove(true);
        if (!file->open()) { fail(file->errorString()); return; } // QTemporaryFile opens ReadWrite.
        if (stopped()) return;
        const auto token = control;
        const QString path = file->fileName();
        QMetaObject::invokeMethod(owner, [facade = owner, token, path] {
            if (!facade->m_busy || facade->m_epoch != token->epoch || token->stop.load() != Running) return;
            facade->m_opened = true;
            emit facade->opened(token->generation, path);
        }, Qt::QueuedConnection);
    }

    void append(quint32 sequence, const QByteArray &data, const std::shared_ptr<Control> &token)
    {
        if (control != token || !file || stopped()) return;
        const quint64 extent = (quint64(sequence) + 1) * BlockBytes;
        if (data.size() != BlockBytes || extent > quint64(MaximumFileBytes)) {
            fail(stoppedError(InvalidBlock)); return;
        }
        const quint64 nextSequence = hasBlocks ? quint64(highest) + 1 : 0;
        if (quint64(sequence) > nextSequence + MaximumForwardGap) {
            fail(QStringLiteral("Remote log forward gap exceeds 4096 blocks; nothing was published.")); return;
        }
        auto after = intervals.upperBound(sequence);
        const bool duplicate = after != intervals.begin() && std::prev(after).value() >= sequence;
        if (!file->seek(qint64(sequence) * BlockBytes)) { fail(file->errorString()); return; }
        if (duplicate) {
            const QByteArray original = file->read(BlockBytes);
            if (original.size() != BlockBytes) { fail(QStringLiteral("Cannot read the original remote log block for duplicate verification.")); return; }
            if (original != data) { fail(QStringLiteral("Conflicting duplicate remote log block %1; nothing was published.").arg(sequence)); return; }
            ++duplicates;
        } else {
            if (file->write(data) != BlockBytes || !file->flush()) { fail(file->errorString()); return; }
            // Store disjoint inclusive intervals, not one allocation per block.
            quint32 first = sequence, last = sequence;
            if (after != intervals.begin()) {
                auto before = std::prev(after);
                if (quint64(before.value()) + 1 == sequence) {
                    first = before.key(); intervals.erase(before);
                }
            }
            after = intervals.upperBound(sequence);
            if (after != intervals.end() && quint64(sequence) + 1 == after.key()) {
                last = after.value(); intervals.erase(after);
            }
            intervals.insert(first, last);
            ++blocks; highest = hasBlocks ? qMax(highest, sequence) : sequence; hasBlocks = true;
            if (intervals.size() > MaximumIntervals) {
                fail(QStringLiteral("Remote log interval metadata exceeded its bounded capacity; nothing was published.")); return;
            }
        }
        if (stopped()) return;
        QMetaObject::invokeMethod(owner, [facade = owner, token, sequence, duplicate] {
            if (!facade->m_busy || facade->m_epoch != token->epoch) return;
            if (facade->m_pending > 0) --facade->m_pending;
            if (token->stop.load() != Running) return;
            emit facade->blockStored(token->generation, sequence, duplicate);
        }, Qt::QueuedConnection);
    }

    void finish(bool save, const std::shared_ptr<Control> &token)
    {
        if (control != token || stopped()) return;
        if (!save) { fail(stoppedError(Discard), true); return; }
        if (!file || !hasBlocks) { fail(QStringLiteral("Cannot save an empty remote log capture.")); return; }
        Result result = statistics();
        // Explicitly write zeros: do not depend on sparse-file gap behaviour
        // on the selected filesystem. This pass is bounded and cancellable.
        const QByteArray zeros(64 * 1024, '\0');
        for (const auto &range : result.missingRanges) {
            if (!file->seek(qint64(range.first) * BlockBytes)) { fail(file->errorString()); return; }
            qint64 remaining = (qint64(range.last) - range.first + 1) * BlockBytes;
            while (remaining > 0) {
                if (stopped()) return;
                const qint64 count = qMin<qint64>(remaining, zeros.size());
                if (file->write(zeros.constData(), count) != count) { fail(file->errorString()); return; }
                remaining -= count;
            }
        }
        if (!file->flush()) { fail(file->errorString()); return; }
        if (stopped()) return;
        const QString destination = finalBase + (result.missingBlocks ? QStringLiteral(".partial.bin") : QStringLiteral(".bin"));
        // No callbacks or cancellation checks after this publication boundary.
        // QFile::rename never overwrites an existing destination.
        if (!file->rename(destination)) { fail(QStringLiteral("Cannot publish remote log without overwriting a file: %1").arg(file->errorString())); return; }
        file->setAutoRemove(false);
        result.success = result.published = true; result.path = destination;
        result.warnings << QStringLiteral("Remote logging has no EOF/flush acknowledgement; this file is a captured prefix, not proof of a complete flight log.");
        if (result.missingBlocks)
            result.warnings << QStringLiteral("Incomplete capture: %1 missing blocks in %2 ranges were zero-filled; published as .partial.bin.")
                .arg(result.missingBlocks).arg(result.missingRanges.size());
        complete(result);
    }

    void stop(const std::shared_ptr<Control> &token) { if (control == token) stopped(); }
    void shutdown() {
        if (control) {
            int running = Running; control->stop.compare_exchange_strong(running, Shutdown);
            stopped();
        }
        cleanup();
    }

private:
    Result statistics() const {
        Result result; result.hasBlocks = hasBlocks; result.blocks = blocks;
        result.duplicateBlocks = duplicates; result.highestSequence = highest;
        result.bytes = hasBlocks ? (qint64(highest) + 1) * BlockBytes : 0;
        quint32 next = 0;
        for (auto it = intervals.cbegin(); it != intervals.cend(); ++it) {
            if (it.key() > next) result.missingRanges.append(MissingRange{next, it.key() - 1});
            next = it.value() + 1;
        }
        result.missingBlocks = hasBlocks ? qint64(highest) + 1 - blocks : 0;
        return result;
    }
    bool stopped() {
        if (!control) return true;
        const int reason = control->stop.load();
        if (reason == Running) return false;
        fail(stoppedError(reason), reason == Discard); return true;
    }
    void fail(const QString &reason, bool cancelled = false) {
        if (!control) return;
        Result result = statistics(); result.error = reason; result.cancelled = cancelled;
        complete(result);
    }
    void complete(Result result) {
        const auto token = control;
        if (!result.published && file && file->size() > 0 && !token->explicitDiscard.load()) {
            // Preserve already captured bytes without presenting them as a
            // completed flight log. A failed flush still merits retaining the
            // file for recovery; never overwrite it with a conflicting block.
            const bool flushed = file->flush();
            file->setAutoRemove(false);
            result.path = file->fileName();
            result.bytes = qMax(result.bytes, file->size());
            result.warnings << QStringLiteral("Nonempty capture retained as .part, not a published flight log. It may contain gaps or a partial final block; no remote EOF was confirmed.");
            if (result.missingBlocks)
                result.warnings << QStringLiteral("Retained capture has %1 missing blocks in %2 ranges.")
                    .arg(result.missingBlocks).arg(result.missingRanges.size());
            if (!flushed) result.warnings << QStringLiteral("Disk flush failed; retained bytes may be incomplete.");
        }
        cleanup(); // Removal/preservation happens before terminal notification.
        QMetaObject::invokeMethod(owner, [facade = owner, token, result] {
            if (!facade->m_busy || facade->m_epoch != token->epoch) return;
            QPointer<RemoteDataFlashLogWriter> guard(facade);
            facade->m_opened = facade->m_accepting = false;
            if (!result.success && !result.cancelled) emit facade->failed(token->generation, result.error);
            if (!guard || facade->m_epoch != token->epoch) return;
            facade->m_pending = 0; facade->m_busy = false; facade->m_control.reset();
            emit facade->finished(token->generation, result);
        }, Qt::QueuedConnection);
    }
    void cleanup() {
        file.reset(); control.reset(); intervals.clear(); finalBase.clear();
        blocks = duplicates = 0; highest = 0; hasBlocks = false;
    }
    RemoteDataFlashLogWriter *owner;
    std::shared_ptr<Control> control;
    std::unique_ptr<QTemporaryFile> file;
    QMap<quint32, quint32> intervals;
    QString finalBase;
    qint64 blocks = 0, duplicates = 0;
    quint32 highest = 0;
    bool hasBlocks = false;
};

RemoteDataFlashLogWriter::RemoteDataFlashLogWriter(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<Result>();
    m_thread = new QThread(this);
    m_worker = new Worker(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

RemoteDataFlashLogWriter::~RemoteDataFlashLogWriter()
{
    if (m_control) {
        int running = Running; m_control->stop.compare_exchange_strong(running, Shutdown);
    }
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread->quit(); m_thread->wait();
}

bool RemoteDataFlashLogWriter::open(const QString &directory, const QString &stem, quint64 generation)
{
    if (QThread::currentThread() != thread() || m_busy || !generation) return false;
    if (directory.size() > 4096 || stem.size() > 96 || directory.contains(QChar::Null)) return false;
    m_busy = m_accepting = true; m_opened = false; m_pending = 0;
    m_control = std::make_shared<Control>();
    m_control->generation = generation; m_control->epoch = ++m_epoch;
    const QString copiedDirectory(directory.constData(), directory.size());
    const QString copiedStem(stem.constData(), stem.size());
    const auto token = m_control;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, copiedDirectory, copiedStem, token] {
        worker->open(copiedDirectory, copiedStem, token);
    }, Qt::QueuedConnection);
    return true;
}

bool RemoteDataFlashLogWriter::append(quint32 sequence, const QByteArray &data)
{
    if (QThread::currentThread() != thread() || !m_busy || !m_opened || !m_accepting) return false;
    if (data.size() != BlockBytes || (quint64(sequence) + 1) * BlockBytes > quint64(MaximumFileBytes)) {
        abort(InvalidBlock); return false;
    }
    if (m_pending >= MaximumPendingBlocks) { abort(QueueOverflow); return false; }
    ++m_pending;
    const auto token = m_control;
    const QByteArray copy(data.constData(), data.size()); // Own even fromRawData input.
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, sequence, copy, token] {
        worker->append(sequence, copy, token);
    }, Qt::QueuedConnection);
    return true;
}

void RemoteDataFlashLogWriter::abort(int reason)
{
    if (!m_control) return;
    m_accepting = false;
    int running = Running;
    if (!m_control->stop.compare_exchange_strong(running, reason)) return;
    const auto token = m_control;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, token] { worker->stop(token); }, Qt::QueuedConnection);
}

bool RemoteDataFlashLogWriter::finish(bool save, bool preservePartial)
{
    if (QThread::currentThread() != thread() || !m_busy || !m_control) return false;
    if (!save) {
        if (!preservePartial) m_control->explicitDiscard.store(true);
        abort(preservePartial ? PreservePartial : Discard); return true;
    }
    if (!m_opened || !m_accepting) return false;
    m_accepting = false;
    const auto token = m_control;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, token] { worker->finish(true, token); }, Qt::QueuedConnection);
    return true;
}
