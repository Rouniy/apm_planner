#include "QueuedSpeechController.h"

#include <QThread>
#include <QTimer>

#include <utility>

QueuedSpeechController::QueuedSpeechController(
    StartCallback startCallback,
    StopCallback stopCallback,
    int utteranceTimeoutMs,
    QObject *parent)
    : QObject(parent)
    , m_startCallback(std::move(startCallback))
    , m_stopCallback(std::move(stopCallback))
    , m_watchdog(new QTimer(this))
{
    Q_ASSERT(utteranceTimeoutMs > 0);
    m_watchdog->setSingleShot(true);
    m_watchdog->setInterval(utteranceTimeoutMs);
    connect(m_watchdog, &QTimer::timeout,
            this, [this]() { utteranceTimedOut(); });
}

QueuedSpeechController::SubmitResult QueuedSpeechController::submit(
    const QString &text)
{
    assertThread();
    const QString normalized =
        text.trimmed().left(MaximumUtteranceLength);
    if (normalized.isEmpty() || !isAvailable()) {
        return SubmitResult::Rejected;
    }
    if (m_currentText == normalized || m_pendingSet.contains(normalized)) {
        return SubmitResult::Coalesced;
    }

    while (m_pending.size() >= MaximumPendingUtterances) {
        m_pendingSet.remove(m_pending.dequeue());
    }
    m_pending.enqueue(normalized);
    m_pendingSet.insert(normalized);
    playNext();
    return SubmitResult::Accepted;
}

void QueuedSpeechController::backendStateChanged(BackendState state)
{
    assertThread();
    m_backendState = state;

    if (state == BackendState::Error
        || state == BackendState::Unavailable) {
        clearQueue();
        return;
    }
    if (state != BackendState::Ready) {
        return;
    }

    if (m_waitingForStopReady) {
        m_waitingForStopReady = false;
        m_watchdog->stop();
        playNext();
        return;
    }
    if (!m_currentText.isEmpty()) {
        m_currentText.clear();
        m_watchdog->stop();
    }
    playNext();
}

void QueuedSpeechController::stop()
{
    assertThread();
    clearQueue();
    if (m_stopCallback) {
        m_stopCallback();
    }
}

bool QueuedSpeechController::isAvailable() const
{
    assertThread();
    return m_backendState == BackendState::Ready
        || m_backendState == BackendState::Busy;
}

bool QueuedSpeechController::isIdle() const
{
    assertThread();
    return m_backendState == BackendState::Ready
        && m_currentText.isEmpty() && m_pending.isEmpty();
}

QStringList QueuedSpeechController::pendingTexts() const
{
    assertThread();
    QStringList result;
    result.reserve(m_pending.size());
    for (const QString &text : m_pending) {
        result.append(text);
    }
    return result;
}

void QueuedSpeechController::assertThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
}

void QueuedSpeechController::clearQueue()
{
    m_watchdog->stop();
    m_currentText.clear();
    m_pending.clear();
    m_pendingSet.clear();
    m_waitingForStopReady = false;
}

void QueuedSpeechController::playNext()
{
    if (m_backendState != BackendState::Ready
        || !m_currentText.isEmpty() || m_pending.isEmpty()) {
        return;
    }

    m_currentText = m_pending.dequeue();
    m_pendingSet.remove(m_currentText);
    // Claim the backend before invoking user code. QTextToSpeech may not emit
    // Speaking synchronously, and another submission must still remain queued.
    m_backendState = BackendState::Busy;
    m_watchdog->start();
    if (m_startCallback) {
        m_startCallback(m_currentText);
    }
}

void QueuedSpeechController::utteranceTimedOut()
{
    assertThread();
    if (m_waitingForStopReady) {
        // The backend did not acknowledge stop within a second watchdog
        // interval. Fail closed instead of starting overlapping speech.
        clearQueue();
        m_backendState = BackendState::Unavailable;
        return;
    }
    if (m_currentText.isEmpty()) {
        return;
    }

    // Preserve later utterances, but never start one until the asynchronous
    // backend confirms Ready. A late Ready from stop must not complete a new
    // utterance that was started speculatively.
    m_currentText.clear();
    m_waitingForStopReady = true;
    if (m_stopCallback) {
        m_stopCallback();
    }
    if (m_waitingForStopReady) {
        m_watchdog->start();
    }
}
