#ifndef QUEUEDSPEECHCONTROLLER_H
#define QUEUEDSPEECHCONTROLLER_H

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>

#include <functional>

class QTimer;

/**
 * Backend-neutral state machine for serialized speech playback.
 *
 * The controller is independent from QTextToSpeech so queue, cancellation and
 * watchdog behavior can be tested in builds without a speech plugin. All
 * methods must be called on the controller's QObject thread.
 */
class QueuedSpeechController final : public QObject
{
public:
    static constexpr int MaximumPendingUtterances = 4;
    static constexpr int MaximumUtteranceLength = 512;
    static constexpr int UtteranceTimeoutMs = 60000;

    enum class BackendState
    {
        Unavailable,
        Ready,
        Busy,
        Error
    };

    enum class SubmitResult
    {
        Rejected,
        Accepted,
        Coalesced
    };

    using StartCallback = std::function<void(const QString &)>;
    using StopCallback = std::function<void()>;

    explicit QueuedSpeechController(
        StartCallback startCallback,
        StopCallback stopCallback,
        int utteranceTimeoutMs = UtteranceTimeoutMs,
        QObject *parent = nullptr);

    SubmitResult submit(const QString &text);
    void backendStateChanged(BackendState state);
    void stop();

    bool isAvailable() const;
    bool isIdle() const;
    QString currentText() const { return m_currentText; }
    QStringList pendingTexts() const;

private:
    void assertThread() const;
    void clearQueue();
    void playNext();
    void utteranceTimedOut();

    StartCallback m_startCallback;
    StopCallback m_stopCallback;
    BackendState m_backendState = BackendState::Unavailable;
    QString m_currentText;
    QQueue<QString> m_pending;
    QSet<QString> m_pendingSet;
    QTimer *m_watchdog = nullptr;
    bool m_waitingForStopReady = false;
};

#endif // QUEUEDSPEECHCONTROLLER_H
