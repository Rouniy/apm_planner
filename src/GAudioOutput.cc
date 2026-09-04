#include "GAudioOutput.h"

#include "audio/QtAudioOutput.h"
#include "configuration.h"
#include "logging.h"
#include "services/SpeechSettings.h"

#include <QApplication>
#include <QSettings>
#include <QTimer>

namespace {
const QString kAudioSettingsPrefix = QStringLiteral("QGC_AUDIOOUTPUT_");
}

GAudioOutput *GAudioOutput::instance()
{
    static GAudioOutput *singleton = nullptr;
    if (!singleton) {
        singleton = new GAudioOutput(qApp);
    }
    return singleton;
}

GAudioOutput::GAudioOutput(QObject *parent)
    : QObject(parent),
      audioBackend(new QtAudioOutput(this)),
      speechSettings(SpeechSettings::instance())
{
    QSettings settings;
    muted = settings.value(kAudioSettingsPrefix + QStringLiteral("muted"), false).toBool();

    emergencyTimer = new QTimer(this);
    connect(emergencyTimer, &QTimer::timeout, this, &GAudioOutput::beep);
    connect(speechSettings, &SpeechSettings::enabledChanged, this,
            [this](bool enabled) {
                if (!enabled) {
                    audioBackend->stopSpeech();
                }
                emit speechEnabledChanged(enabled);
            });

    if (voiceIndex == VOICE_FEMALE) {
        selectFemaleVoice();
    } else {
        selectMaleVoice();
    }
}

GAudioOutput::~GAudioOutput()
{
    QLOG_INFO() << "~GAudioOutput()";
}

void GAudioOutput::mute(bool shouldMute)
{
    if (shouldMute == muted) {
        return;
    }
    muted = shouldMute;
    if (muted) {
        audioBackend->stopSpeech();
    }
    QSettings settings;
    settings.setValue(kAudioSettingsPrefix + QStringLiteral("muted"), muted);
    settings.sync();
    emit mutedChanged(muted);
}

bool GAudioOutput::isMuted() const
{
    return muted;
}

bool GAudioOutput::isSpeechEnabled() const
{
    return speechSettings && speechSettings->isEnabled();
}

void GAudioOutput::setSpeechEnabled(bool enabled)
{
    if (speechSettings) {
        speechSettings->setEnabled(enabled);
    }
}

bool GAudioOutput::isSpeechReady() const
{
    return isSpeechEnabled() && !muted && !emergency && audioBackend
        && audioBackend->isSpeechReady();
}

bool GAudioOutput::say(QString text, int severity)
{
    Q_UNUSED(severity)
    if (!isSpeechReady() || text == QStringLiteral("system %1")) {
        return false;
    }
    return audioBackend->speak(text);
}

bool GAudioOutput::sayForVehicle(QString text, bool vehicleArmed, int severity)
{
    if (!speechSettings || !speechSettings->isEnabled()
        || (speechSettings->armedOnly() && !vehicleArmed)) {
        return false;
    }
    return say(text, severity);
}

bool GAudioOutput::alert(QString text)
{
    if (muted) {
        return false;
    }
    beep();
    say(text, 2);
    return true;
}

void GAudioOutput::notifyPositive()
{
    if (!muted) {
        audioBackend->playFile(QGC::shareDirectory() + QStringLiteral("/files/audio/alert.wav"));
    }
}

void GAudioOutput::notifyNegative()
{
    if (!muted) {
        audioBackend->playFile(QGC::shareDirectory() + QStringLiteral("/files/audio/alert.wav"));
    }
}

bool GAudioOutput::startEmergency()
{
    if (!emergency) {
        emergency = true;
        if (!muted) {
            beep();
        }
        emergencyTimer->start(1500);
        QTimer::singleShot(5000, this, &GAudioOutput::stopEmergency);
    }
    return true;
}

bool GAudioOutput::stopEmergency()
{
    emergency = false;
    emergencyTimer->stop();
    return true;
}

void GAudioOutput::beep()
{
    if (!muted) {
        audioBackend->playFile(QGC::shareDirectory() + QStringLiteral("/files/audio/alert.wav"));
    }
}

void GAudioOutput::selectFemaleVoice()
{
    voiceIndex = VOICE_FEMALE;
    audioBackend->selectFemaleVoice();
}

void GAudioOutput::selectMaleVoice()
{
    voiceIndex = VOICE_MALE;
    audioBackend->selectMaleVoice();
}

QStringList GAudioOutput::listVoices() const
{
    return audioBackend->availableVoices();
}
