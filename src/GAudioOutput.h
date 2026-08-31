#ifndef GAUDIOOUTPUT_H
#define GAUDIOOUTPUT_H

#include <QObject>
#include <QStringList>

class QTimer;
class QtAudioOutput;

class GAudioOutput : public QObject
{
    Q_OBJECT
public:
    static GAudioOutput *instance();
    QStringList listVoices() const;

    enum VoiceGender {
        VOICE_MALE = 0,
        VOICE_FEMALE
    } QGVoice;

    bool isMuted() const;

public slots:
    bool say(QString text, int severity = 1);
    bool alert(QString text);
    bool startEmergency();
    bool stopEmergency();
    void selectFemaleVoice();
    void selectMaleVoice();
    void beep();
    void notifyPositive();
    void notifyNegative();
    void mute(bool mute);

signals:
    void mutedChanged(bool muted);

private:
    explicit GAudioOutput(QObject *parent = nullptr);
    ~GAudioOutput() override;

    int voiceIndex = VOICE_FEMALE;
    bool emergency = false;
    QTimer *emergencyTimer = nullptr;
    bool muted = false;
    QtAudioOutput *audioBackend = nullptr;
};

#endif
