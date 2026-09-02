#include "ConnectionOptionsViewModel.h"

#include <QSerialPortInfo>
#include <QSet>

namespace {
const QStringList kNetworkConnections = {
    QStringLiteral("TCP"),
    QStringLiteral("UDP"),
    QStringLiteral("UDPCl"),
    QStringLiteral("WS")
};
}

ConnectionOptionsViewModel::ConnectionOptionsViewModel(QObject *parent)
    : ConnectionOptionsViewModel([] {
          QStringList ports;
          const auto available = QSerialPortInfo::availablePorts();
          ports.reserve(available.size());
          for (const QSerialPortInfo &port : available) {
              ports.append(port.portName());
          }
          return ports;
      }(), parent)
{}

ConnectionOptionsViewModel::ConnectionOptionsViewModel(
    const QStringList &serialPorts, QObject *parent)
    : QObject(parent),
      m_connections(availableConnections(serialPorts))
{
    if (!m_connections.isEmpty()) {
        m_selectedConnection = m_connections.first();
    }
}

QList<int> ConnectionOptionsViewModel::availableBaudRates()
{
    // MissionPlanner/Controls/ConnectionOptions.resx, in display order.
    return {1200, 2400, 4800, 9600, 19200, 28800, 38400, 57600,
            111100, 115200, 230400, 460800, 500000, 625000,
            921600, 1000000, 1500000};
}

QStringList ConnectionOptionsViewModel::availableConnections(
    const QStringList &serialPorts)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &candidate : serialPorts) {
        const QString port = candidate.trimmed();
        if (!port.isEmpty() && !seen.contains(port)) {
            result.append(port);
            seen.insert(port);
        }
    }
    result.append(kNetworkConnections);
    return result;
}

bool ConnectionOptionsViewModel::isNetworkConnection(
    const QString &connection)
{
    return kNetworkConnections.contains(connection);
}

QStringList ConnectionOptionsViewModel::Connections() const
{
    return m_connections;
}

QList<int> ConnectionOptionsViewModel::Bauds() const
{
    return availableBaudRates();
}

QString ConnectionOptionsViewModel::SelectedConnection() const
{
    return m_selectedConnection;
}

int ConnectionOptionsViewModel::SelectedBaud() const
{
    return m_selectedBaud;
}

bool ConnectionOptionsViewModel::BaudEnabled() const
{
    return !m_selectedConnection.isEmpty()
        && !isNetworkConnection(m_selectedConnection);
}

void ConnectionOptionsViewModel::setSelectedConnection(
    const QString &connection)
{
    if (!m_connections.contains(connection)
        || m_selectedConnection == connection) {
        return;
    }
    const bool oldBaudEnabled = BaudEnabled();
    m_selectedConnection = connection;
    emit SelectedConnectionChanged(connection);
    if (oldBaudEnabled != BaudEnabled()) {
        emit BaudEnabledChanged(BaudEnabled());
    }
}

void ConnectionOptionsViewModel::setSelectedBaud(int baud)
{
    if (!availableBaudRates().contains(baud) || m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit SelectedBaudChanged(baud);
}

bool ConnectionOptionsViewModel::Connect()
{
    if (m_selectedConnection.isEmpty()) {
        return false;
    }
    emit connectRequested(m_selectedConnection, m_selectedBaud);
    return true;
}
