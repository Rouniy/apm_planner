#ifndef QTAUDIOOUTPUT_H
#define QTAUDIOOUTPUT_H

#include <QObject>
#include <QQueue>
#include <QStringList>

class QMediaPlayer;
class QAudioOutput;
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
    void playNextFile();
    void selectVoice(bool female);

    QQueue<QString> m_pendingFiles;
    bool m_playing = false;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QTextToSpeech *m_speech = nullptr;
    QueuedSpeechController *m_speechQueue = nullptr;
};

#endif
