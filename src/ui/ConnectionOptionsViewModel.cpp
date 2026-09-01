#include "ConnectionOptionsViewModel.h"

#include <QSettings>

namespace {
constexpr int kDefaultBaud = 115200;
constexpr int kDefaultGcsSystemId = 255;

int validSystemIdOrDefault(int systemId)
{
    return systemId >= 1 && systemId <= 255
        ? systemId
        : kDefaultGcsSystemId;
}

int validBaudOrDefault(int baud)
{
    return ConnectionOptionsViewModel::availableBaudRates().contains(baud)
        ? baud
        : kDefaultBaud;
}
}

ConnectionOptionsViewModel::ConnectionOptionsViewModel(QObject *parent)
    : ConnectionOptionsViewModel(static_cast<QSettings *>(nullptr), parent)
{}

ConnectionOptionsViewModel::ConnectionOptionsViewModel(QSettings *settings,
                                                       QObject *parent)
    : QObject(parent),
      m_settings(settings ? settings : new QSettings(this))
{
    load();
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

int ConnectionOptionsViewModel::GcsSysid() const
{
    return m_gcsSysid;
}

bool ConnectionOptionsViewModel::SendGcsHeartbeat() const
{
    return m_sendGcsHeartbeat;
}

QString ConnectionOptionsViewModel::Status() const
{
    return m_status;
}

void ConnectionOptionsViewModel::setSelectedBaud(int baud)
{
    if (m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit SelectedBaudChanged(baud);
}

void ConnectionOptionsViewModel::setGcsSysid(int systemId)
{
    if (m_gcsSysid == systemId) {
        return;
    }
    m_gcsSysid = systemId;
    emit GcsSysidChanged(systemId);
}

void ConnectionOptionsViewModel::setSendGcsHeartbeat(bool enabled)
{
    if (m_sendGcsHeartbeat == enabled) {
        return;
    }
    m_sendGcsHeartbeat = enabled;
    emit SendGcsHeartbeatChanged(enabled);
}

bool ConnectionOptionsViewModel::Apply()
{
    const int baud = validBaudOrDefault(m_selectedBaud);
    if (baud != m_selectedBaud) {
        setSelectedBaud(baud);
    }
    const int systemId = validSystemIdOrDefault(m_gcsSysid);
    if (systemId != m_gcsSysid) {
        setGcsSysid(systemId);
    }

    // Mission Planner canonical keys.
    m_settings->setValue(QStringLiteral("baudrate"), baud);
    m_settings->setValue(QStringLiteral("CHK_GCSheartbeat"),
                         m_sendGcsHeartbeat);
    m_settings->setValue(QStringLiteral("gcsid"), systemId);

    // Keep the APM Planner 2 settings surfaces converged during the 3.0 port.
    m_settings->setValue(QStringLiteral("GLOBAL_SETTINGS/MAVLINK_ID"),
                         systemId);
    m_settings->setValue(
        QStringLiteral("QGC_MAINWINDOW/HEARTBEATS_ENABLED"),
        m_sendGcsHeartbeat);
    m_settings->sync();

    if (m_settings->status() != QSettings::NoError) {
        setStatus(tr("Save failed."));
        return false;
    }

    setStatus(tr("Saved."));
    emit settingsApplied(baud, m_sendGcsHeartbeat, systemId);
    return true;
}

void ConnectionOptionsViewModel::load()
{
    m_settings->sync();

    m_selectedBaud = m_settings->contains(QStringLiteral("baudrate"))
        ? m_settings->value(QStringLiteral("baudrate")).toInt()
        : m_settings->value(QStringLiteral("SERIALLINK_COMM_BAUD"),
                            kDefaultBaud).toInt();
    m_selectedBaud = validBaudOrDefault(m_selectedBaud);

    m_sendGcsHeartbeat =
        m_settings->contains(QStringLiteral("CHK_GCSheartbeat"))
        ? m_settings->value(QStringLiteral("CHK_GCSheartbeat")).toBool()
        : m_settings->value(
              QStringLiteral("QGC_MAINWINDOW/HEARTBEATS_ENABLED"), true)
              .toBool();

    if (m_settings->contains(QStringLiteral("gcsid"))) {
        m_gcsSysid = m_settings->value(QStringLiteral("gcsid")).toInt();
    } else if (m_settings->contains(QStringLiteral("GCS_sysid"))) {
        m_gcsSysid = m_settings->value(QStringLiteral("GCS_sysid")).toInt();
    } else {
        m_gcsSysid = m_settings->value(
            QStringLiteral("GLOBAL_SETTINGS/MAVLINK_ID"),
            kDefaultGcsSystemId).toInt();
    }
    m_gcsSysid = validSystemIdOrDefault(m_gcsSysid);
}

void ConnectionOptionsViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit StatusChanged(status);
}
