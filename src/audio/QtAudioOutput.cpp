#include "QtAudioOutput.h"

#include "QueuedSpeechController.h"

#include <QFileInfo>
#include <QUrl>
#include <QtGlobal>

#ifdef APM_HAS_QT_MULTIMEDIA
#include <QMediaPlayer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
#endif

#ifdef APM_HAS_QT_TEXT_TO_SPEECH
#include <QTextToSpeech>
#include <QVoice>
#endif

QtAudioOutput::QtAudioOutput(QObject *parent)
    : QObject(parent)
{
#ifdef APM_HAS_QT_MULTIMEDIA
    m_player = new QMediaPlayer(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
#endif
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) {
                    m_playing = false;
                    playNextFile();
                }
            });
#endif

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
    m_speech = new QTextToSpeech(this);
    const auto updateSpeechState = [this](QTextToSpeech::State state) {
        QueuedSpeechController::BackendState queueState =
            QueuedSpeechController::BackendState::Busy;
        if (state == QTextToSpeech::Ready) {
            queueState = QueuedSpeechController::BackendState::Ready;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        } else if (state == QTextToSpeech::Error) {
#else
        } else if (state == QTextToSpeech::BackendError) {
#endif
            queueState = QueuedSpeechController::BackendState::Error;
        }
        m_speechQueue->backendStateChanged(queueState);
    };
    connect(m_speech, &QTextToSpeech::stateChanged,
            this, updateSpeechState);
    updateSpeechState(m_speech->state());
#endif
}

bool QtAudioOutput::playFile(const QString &fileName)
{
#ifdef APM_HAS_QT_MULTIMEDIA
    if (!QFileInfo::exists(fileName)) {
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
    if (!m_player || m_pendingFiles.isEmpty()) {
        m_playing = false;
        return;
    }

    m_playing = true;
    const QUrl source = QUrl::fromLocalFile(m_pendingFiles.dequeue());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_player->setSource(source);
#else
    m_player->setMedia(source);
#endif
    m_player->play();
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
