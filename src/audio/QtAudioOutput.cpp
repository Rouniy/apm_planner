#include "QtAudioOutput.h"

#include "QueuedSpeechController.h"

#include <QFileInfo>
#include <QUrl>
#include <QtGlobal>
#include <utility>

#ifdef APM_HAS_QT_MULTIMEDIA
#include <QSoundEffect>
#endif

#ifdef APM_HAS_QT_TEXT_TO_SPEECH
#include <QTextToSpeech>
#include <QVoice>
#endif

QtAudioOutput::QtAudioOutput(QObject *parent)
    : QObject(parent)
{
    m_speechQueue = new QueuedSpeechController(
        [this](const QString &text) {
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
            if (m_speech) {
                m_speech->say(text);
            }
#else
            Q_UNUSED(text)
#endif
        },
        [this]() {
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
            if (m_speech) {
                m_speech->stop();
            }
#endif
        },
        QueuedSpeechController::UtteranceTimeoutMs, this);

#ifdef APM_HAS_QT_TEXT_TO_SPEECH
    const QStringList engines = QTextToSpeech::availableEngines();
    if (engines.isEmpty()) {
        // Constructing QTextToSpeech without any engine plugin only yields a
        // BackendError plus a Qt warning. Leave the backend absent so the
        // diagnostic reports the installation problem precisely.
        m_engineDetail = QStringLiteral(
            "QTextToSpeech::availableEngines() is empty");
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    // Qt 6.4+ can report the engine it actually selected, so keep Qt's own
    // default preference and read the exact name back.
    m_speech = new QTextToSpeech(this);
    m_engineName = m_speech->engine();
#else
    // Qt 5 has no engine() getter: request the first available engine
    // explicitly so the diagnostic names the engine that really loaded.
    m_engineName = engines.first();
    m_speech = new QTextToSpeech(m_engineName, this);
#endif
    const auto updateSpeechState = [this](QTextToSpeech::State state) {
        QueuedSpeechController::BackendState queueState =
            QueuedSpeechController::BackendState::Busy;
        m_engineError = false;
        if (state == QTextToSpeech::Ready) {
            queueState = QueuedSpeechController::BackendState::Ready;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        } else if (state == QTextToSpeech::Error) {
#else
        } else if (state == QTextToSpeech::BackendError) {
#endif
            queueState = QueuedSpeechController::BackendState::Error;
            m_engineError = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
            m_engineDetail = m_speech->errorString();
#else
            m_engineDetail = QStringLiteral(
                "QTextToSpeech reported BackendError");
#endif
        }
        m_speechQueue->backendStateChanged(queueState);
        if (m_backendStateListener) {
            m_backendStateListener();
        }
    };
    connect(m_speech, &QTextToSpeech::stateChanged,
            this, updateSpeechState);
    updateSpeechState(m_speech->state());
#endif
}

bool QtAudioOutput::speechCompiledIn()
{
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
    return true;
#else
    return false;
#endif
}

QStringList QtAudioOutput::availableSpeechEngines()
{
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
    return QTextToSpeech::availableEngines();
#else
    return QStringList();
#endif
}

SpeechBackendDiagnostic QtAudioOutput::speechBackendDiagnostic() const
{
    SpeechBackendDiagnostic diagnostic;
#ifndef APM_HAS_QT_TEXT_TO_SPEECH
    diagnostic.state = SpeechBackendState::NotCompiled;
    diagnostic.detail = QStringLiteral(
        "APM_HAS_QT_TEXT_TO_SPEECH is not defined for this build");
    return diagnostic;
#else
    diagnostic.engine = m_engineName;
    diagnostic.detail = m_engineDetail;
    if (!m_speech) {
        diagnostic.state = SpeechBackendState::NoEngine;
        return diagnostic;
    }
    if (m_engineError) {
        diagnostic.state = SpeechBackendState::RuntimeError;
        return diagnostic;
    }
    if (!m_speechQueue->isAvailable()) {
        diagnostic.state = SpeechBackendState::QueueUnavailable;
        return diagnostic;
    }
    diagnostic.state = m_speechQueue->isIdle()
        ? SpeechBackendState::Ready
        : SpeechBackendState::Busy;
    return diagnostic;
#endif
}

void QtAudioOutput::setBackendStateListener(BackendStateListener listener)
{
    m_backendStateListener = std::move(listener);
}

bool QtAudioOutput::ensureSoundEffect()
{
#ifdef APM_HAS_QT_MULTIMEDIA
    if (m_soundEffect) {
        return true;
    }
    // Alerts are short WAV effects. QSoundEffect is the Qt audio-only path;
    // QMediaPlayer needlessly initializes a video renderer and has triggered
    // VA-API driver crashes on headless Intel systems.
    m_soundEffect = new QSoundEffect(this);
    connect(m_soundEffect, &QSoundEffect::playingChanged, this, [this]() {
        if (m_playing && m_soundEffect && !m_soundEffect->isPlaying()
            && m_soundEffect->status() == QSoundEffect::Ready) {
            m_playing = false;
            playNextFile();
        }
    });
    connect(m_soundEffect, &QSoundEffect::statusChanged, this, [this]() {
        if (m_playing && m_soundEffect
            && m_soundEffect->status() == QSoundEffect::Error) {
            m_playing = false;
            playNextFile();
        }
    });
    return true;
#else
    return false;
#endif
}

bool QtAudioOutput::playFile(const QString &fileName)
{
#ifdef APM_HAS_QT_MULTIMEDIA
    if (!QFileInfo::exists(fileName) || !ensureSoundEffect()) {
        return false;
    }
    m_pendingFiles.enqueue(fileName);
    if (!m_playing) {
        playNextFile();
    }
    return true;
#else
    Q_UNUSED(fileName)
    return false;
#endif
}

void QtAudioOutput::playNextFile()
{
#ifdef APM_HAS_QT_MULTIMEDIA
    if (!m_soundEffect || m_pendingFiles.isEmpty()) {
        m_playing = false;
        return;
    }

    m_playing = true;
    const QUrl source = QUrl::fromLocalFile(m_pendingFiles.dequeue());
    m_soundEffect->setSource(source);
    m_soundEffect->play();
#endif
}

bool QtAudioOutput::speak(const QString &text)
{
    const QueuedSpeechController::SubmitResult result =
        m_speechQueue->submit(text);
    return result != QueuedSpeechController::SubmitResult::Rejected;
}

void QtAudioOutput::stopSpeech()
{
    m_speechQueue->stop();
}

bool QtAudioOutput::isSpeechReady() const
{
    return m_speechQueue->isAvailable();
}

bool QtAudioOutput::isSpeechIdle() const
{
    return m_speechQueue->isIdle();
}

QStringList QtAudioOutput::availableVoices() const
{
    QStringList names;
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
    if (!m_speech) {
        return names;
    }
    const auto voices = m_speech->availableVoices();
    for (const QVoice &voice : voices) {
        names.append(voice.name());
    }
#endif
    return names;
}

void QtAudioOutput::selectFemaleVoice()
{
    selectVoice(true);
}

void QtAudioOutput::selectMaleVoice()
{
    selectVoice(false);
}

void QtAudioOutput::selectVoice(bool female)
{
#ifdef APM_HAS_QT_TEXT_TO_SPEECH
    if (!m_speech) {
        return;
    }
    const QVoice::Gender requestedGender = female ? QVoice::Female : QVoice::Male;
    const auto voices = m_speech->availableVoices();
    for (const QVoice &voice : voices) {
        if (voice.gender() == requestedGender) {
            m_speech->setVoice(voice);
            return;
        }
    }
#else
    Q_UNUSED(female)
#endif
}
