#include "StatusMessageSettings.h"

#include "StatusTextPolicy.h"

#include <QCoreApplication>
#include <QSettings>

namespace {
const QString kSeverityKey = QStringLiteral("severity");
}

StatusMessageSettings::StatusMessageSettings(QSettings *settings,
                                             QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    m_settings->setFallbacksEnabled(false);
    m_severity = readSeverity();
}

StatusMessageSettings::~StatusMessageSettings() = default;

StatusMessageSettings *StatusMessageSettings::instance()
{
    static StatusMessageSettings service;
    return &service;
}

QString StatusMessageSettings::settingsKey()
{
    return kSeverityKey;
}

QStringList StatusMessageSettings::severityNames()
{
    return {
        QCoreApplication::translate("StatusMessageSettings", "Emergency"),
        QCoreApplication::translate("StatusMessageSettings", "Alert"),
        QCoreApplication::translate("StatusMessageSettings", "Critical"),
        QCoreApplication::translate("StatusMessageSettings", "Error"),
        QCoreApplication::translate("StatusMessageSettings", "Warning"),
        QCoreApplication::translate("StatusMessageSettings", "Notice"),
        QCoreApplication::translate("StatusMessageSettings", "Info"),
        QCoreApplication::translate("StatusMessageSettings", "Debug")
    };
}

bool StatusMessageSettings::shouldPromote(const QString &text,
                                          int severity) const
{
    return StatusTextPolicy::shouldPromote(text, severity, m_severity);
}

bool StatusMessageSettings::setSeverity(int severity)
{
    if (!StatusTextPolicy::isValidSeverity(severity)) {
        return false;
    }

    const bool changed = m_severity != severity;
    m_settings->setValue(kSeverityKey, severity);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_severity = readSeverity();
        return false;
    }
    m_severity = severity;
    if (changed) {
        emit severityChanged(severity);
    }
    return true;
}

void StatusMessageSettings::reload()
{
    m_settings->sync();
    const int severity = readSeverity();
    if (severity == m_severity) {
        return;
    }
    m_severity = severity;
    emit severityChanged(severity);
}

int StatusMessageSettings::readSeverity() const
{
    bool ok = false;
    const int severity = m_settings->value(
        kSeverityKey, StatusTextPolicy::DefaultSeverity).toInt(&ok);
    return ok && StatusTextPolicy::isValidSeverity(severity)
        ? severity : StatusTextPolicy::DefaultSeverity;
}
