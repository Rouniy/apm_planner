#ifndef GAUDIOOUTPUT_H
#define GAUDIOOUTPUT_H

#include "audio/QtAudioOutput.h"

#include <QObject>
#include <QStringList>

class QTimer;
class SpeechSettings;

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
    bool isSpeechEnabled() const;
    bool isSpeechReady() const;
    bool isSpeechIdle() const;
    /**
     * Backend-only diagnostic (compiled in, engine plugin, engine error,
     * queue state). Application gates such as mute, the speech enable flag
     * and the emergency tone are reported by isSpeechReady() instead.
     */
    SpeechBackendDiagnostic speechBackendDiagnostic() const;

public slots:
    bool say(QString text, int severity = 1);
    bool sayForVehicle(QString text, int linkId, int systemId,
                       int componentId, int severity = 1);
    bool alert(QString text);
    bool startEmergency();
    bool stopEmergency();
    void selectFemaleVoice();
    void selectMaleVoice();
    void beep();
    void notifyPositive();
    void notifyNegative();
    void mute(bool mute);
    void setSpeechEnabled(bool enabled);
    void stopSpeech();

signals:
    void mutedChanged(bool muted);
    void speechEnabledChanged(bool enabled);
    /** The engine state behind speechBackendDiagnostic() changed. */
    void speechBackendStateChanged();

private:
    explicit GAudioOutput(QObject *parent = nullptr);
    ~GAudioOutput() override;

    int voiceIndex = VOICE_FEMALE;
    bool emergency = false;
    QTimer *emergencyTimer = nullptr;
    bool muted = false;
    QtAudioOutput *audioBackend = nullptr;
    SpeechSettings *speechSettings = nullptr;
};

#endif
