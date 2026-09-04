#include "HudDisplaySettings.h"

#include <QSettings>

namespace {
const QString kOverlayEnabledKey = QStringLiteral("CHK_hudshow");
}

HudDisplaySettings::HudDisplaySettings(QSettings *settings, QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }

    m_settings->setFallbacksEnabled(false);
    m_overlayEnabled = m_settings->value(kOverlayEnabledKey, true).toBool();
}

HudDisplaySettings::~HudDisplaySettings() = default;

HudDisplaySettings *HudDisplaySettings::instance()
{
    static HudDisplaySettings settings;
    return &settings;
}

void HudDisplaySettings::setOverlayEnabled(bool enabled)
{
    if (m_overlayEnabled == enabled) {
        return;
    }

    m_settings->setValue(kOverlayEnabledKey, enabled);
    m_settings->sync();
    m_overlayEnabled = enabled;
    emit overlayEnabledChanged(enabled);
}

void HudDisplaySettings::reload()
{
    m_settings->sync();
    const bool enabled = m_settings->value(kOverlayEnabledKey, true).toBool();
    if (m_overlayEnabled == enabled) {
        return;
    }

    m_overlayEnabled = enabled;
    emit overlayEnabledChanged(enabled);
}
