#ifndef QTAUDIOOUTPUT_H
#define QTAUDIOOUTPUT_H

#include <QObject>
#include <QQueue>
#include <QStringList>

class QSoundEffect;
class QTextToSpeech;
class QueuedSpeechController;

class QtAudioOutput final : public QObject
{
public:
    explicit QtAudioOutput(QObject *parent = nullptr);

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

private:
    bool ensureSoundEffect();
    void playNextFile();
    void selectVoice(bool female);

    QQueue<QString> m_pendingFiles;
    bool m_playing = false;
    QSoundEffect *m_soundEffect = nullptr;
    QTextToSpeech *m_speech = nullptr;
    QueuedSpeechController *m_speechQueue = nullptr;
};

#endif
