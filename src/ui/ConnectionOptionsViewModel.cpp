#include "ConnectionOptionsViewModel.h"

#include <QSettings>

namespace {
const char kBaudKey[] = "baudrate";
const char kHeartbeatKey[] = "CHK_GCSheartbeat";
const char kGcsSysidKey[] = "gcsid";
const char kLegacyGcsSysidKey[] = "GCS_sysid";
}

ConnectionOptionsViewModel::ConnectionOptionsViewModel(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    m_selectedBaud = settings.value(
        QString::fromLatin1(kBaudKey), 115200).toInt();
    m_sendGcsHeartbeat = settings.value(
        QString::fromLatin1(kHeartbeatKey), true).toBool();
    const int fallbackSysid = settings.value(
        QString::fromLatin1(kLegacyGcsSysidKey), 255).toInt();
    const int savedSystemId = settings.value(
        QString::fromLatin1(kGcsSysidKey), fallbackSysid).toInt();
    m_gcsSysid = savedSystemId >= 1 && savedSystemId <= 255
        ? savedSystemId
        : 255;
}

QList<int> ConnectionOptionsViewModel::availableBaudRates()
{
    return {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
}

QList<int> ConnectionOptionsViewModel::Bauds() const
{
    return availableBaudRates();
}

int ConnectionOptionsViewModel::SelectedBaud() const
{
    return m_selectedBaud;
}

bool ConnectionOptionsViewModel::SendGcsHeartbeat() const
{
    return m_sendGcsHeartbeat;
}

int ConnectionOptionsViewModel::GcsSysid() const
{
    return m_gcsSysid;
}

QString ConnectionOptionsViewModel::Status() const
{
    return m_status;
}

void ConnectionOptionsViewModel::setSelectedBaud(int baud)
{
    // Values loaded from older profiles are deliberately preserved until the
    // operator selects one of MP10's eight choices. New UI selections are
    // restricted to the reference catalog.
    if (!availableBaudRates().contains(baud) || m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit SelectedBaudChanged(baud);
}

void ConnectionOptionsViewModel::setSendGcsHeartbeat(bool enabled)
{
    if (m_sendGcsHeartbeat == enabled) {
        return;
    }
    m_sendGcsHeartbeat = enabled;
    emit SendGcsHeartbeatChanged(enabled);
}

void ConnectionOptionsViewModel::setGcsSysid(int systemId)
{
    if (m_gcsSysid == systemId) {
        return;
    }
    m_gcsSysid = systemId;
    emit GcsSysidChanged(systemId);
}

void ConnectionOptionsViewModel::Apply()
{
    const int systemId = m_gcsSysid >= 1 && m_gcsSysid <= 255
        ? m_gcsSysid
        : 255;
    if (m_gcsSysid != systemId) {
        m_gcsSysid = systemId;
        emit GcsSysidChanged(systemId);
    }

    QSettings settings;
    settings.setValue(QString::fromLatin1(kBaudKey), m_selectedBaud);
    settings.setValue(QString::fromLatin1(kHeartbeatKey), m_sendGcsHeartbeat);
    settings.setValue(QString::fromLatin1(kGcsSysidKey), systemId);
    settings.sync();

    m_status = tr("Saved.");
    emit StatusChanged(m_status);
    emit settingsApplied(m_selectedBaud, m_sendGcsHeartbeat, systemId);
}
