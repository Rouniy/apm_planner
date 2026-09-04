#include "SpeechSettings.h"

#include <QSettings>

SpeechSettings::SpeechSettings(QSettings *settings, QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }

    // Construction observes existing policy without creating an implicit
    // setting. The first user change is what persists the canonical key.
    m_enabled = m_settings->value(settingsKey(), false).toBool();
}

SpeechSettings::~SpeechSettings() = default;

SpeechSettings *SpeechSettings::instance()
{
    static SpeechSettings service;
    return &service;
}

QString SpeechSettings::settingsKey()
{
    return QStringLiteral("speechenable");
}

void SpeechSettings::setEnabled(bool enabled)
{
    // Persist even when the cached default already has this value. This keeps
    // an explicit user choice distinct from the constructor's missing-key
    // default while avoiding a spurious live-state signal.
    m_settings->setValue(settingsKey(), enabled);
    m_settings->sync();

    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    emit enabledChanged(m_enabled);
}

void SpeechSettings::reload()
{
    m_settings->sync();
    const bool enabled = m_settings->value(settingsKey(), false).toBool();
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    emit enabledChanged(m_enabled);
}
