#ifndef QTAUDIOOUTPUT_H
#define QTAUDIOOUTPUT_H

#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include <functional>

class QSoundEffect;
class QTextToSpeech;
class QueuedSpeechController;

/**
 * Precise reason the speech backend can or cannot speak right now.
 *
 * The states are ordered from "nothing can be done at runtime" to "working":
 * NotCompiled and NoEngine are installation problems, RuntimeError is the
 * engine (for example speech-dispatcher) failing, QueueUnavailable means the
 * engine exists but the queue is not accepting yet or failed closed after a
 * watchdog timeout.
 */
enum class SpeechBackendState {
    NotCompiled,      ///< Built without Qt TextToSpeech.
    NoEngine,         ///< No Qt TextToSpeech engine plugin is available.
    RuntimeError,     ///< The engine reported an error.
    QueueUnavailable, ///< Engine exists but the queue does not accept speech.
    Ready,            ///< Idle and accepting speech.
    Busy              ///< Speaking; still accepting queued speech.
};

struct SpeechBackendDiagnostic
{
    SpeechBackendState state = SpeechBackendState::NotCompiled;
    QString engine; ///< Explicitly selected engine name when known.
    QString detail; ///< Engine-supplied or build-supplied detail, may be empty.

    bool accepting() const noexcept
    {
        return state == SpeechBackendState::Ready
            || state == SpeechBackendState::Busy;
    }
};

class QtAudioOutput final : public QObject
{
public:
    using BackendStateListener = std::function<void()>;

    explicit QtAudioOutput(QObject *parent = nullptr);

    /** True when this binary was compiled with Qt TextToSpeech. */
    static bool speechCompiledIn();
    /** Engine plugin names Qt can load; empty when none or not compiled. */
    static QStringList availableSpeechEngines();

    bool playFile(const QString &fileName);
    // Ready means that a backend exists and can accept queued speech. It stays
    // true while the backend is speaking; use isSpeechIdle() for an empty pump.
    bool isSpeechReady() const;
    bool isSpeechIdle() const;
    bool speak(const QString &text);
    void stopSpeech();
    QStringList availableVoices() const;
    void selectFemaleVoice();
    void selectMaleVoice();

    /** Backend-only diagnostic; application gates (mute etc.) are not included. */
    SpeechBackendDiagnostic speechBackendDiagnostic() const;
    /** Invoked on this object's thread whenever the engine state changes. */
    void setBackendStateListener(BackendStateListener listener);

private:
    bool ensureSoundEffect();
    void playNextFile();
    void selectVoice(bool female);

    QQueue<QString> m_pendingFiles;
    bool m_playing = false;
    QSoundEffect *m_soundEffect = nullptr;
    QTextToSpeech *m_speech = nullptr;
    QueuedSpeechController *m_speechQueue = nullptr;
    BackendStateListener m_backendStateListener;
    QString m_engineName;
    QString m_engineDetail;
    bool m_engineError = false;
};

#endif
