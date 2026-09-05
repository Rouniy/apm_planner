#include "AddConnectionViewModel.h"

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

AddConnectionViewModel::AddConnectionViewModel(QObject *parent)
    : AddConnectionViewModel([] {
          QStringList ports;
          const auto available = QSerialPortInfo::availablePorts();
          ports.reserve(available.size());
          for (const QSerialPortInfo &port : available) {
              ports.append(port.portName());
          }
          return ports;
      }(), parent)
{}

AddConnectionViewModel::AddConnectionViewModel(
    const QStringList &serialPorts, QObject *parent)
    : QObject(parent),
      m_connections(availableConnections(serialPorts))
{
    if (!m_connections.isEmpty()) {
        m_selectedConnection = m_connections.first();
    }
}

QList<int> AddConnectionViewModel::availableBaudRates()
{
    // Retained legacy ConnectionOptions catalog, in display order.
    return {1200, 2400, 4800, 9600, 19200, 28800, 38400, 57600,
            111100, 115200, 230400, 460800, 500000, 625000,
            921600, 1000000, 1500000};
}

QStringList AddConnectionViewModel::availableConnections(
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

bool AddConnectionViewModel::isNetworkConnection(
    const QString &connection)
{
    return kNetworkConnections.contains(connection);
}

QStringList AddConnectionViewModel::Connections() const
{
    return m_connections;
}

QList<int> AddConnectionViewModel::Bauds() const
{
    return availableBaudRates();
}

QString AddConnectionViewModel::SelectedConnection() const
{
    return m_selectedConnection;
}

int AddConnectionViewModel::SelectedBaud() const
{
    return m_selectedBaud;
}

bool AddConnectionViewModel::BaudEnabled() const
{
    return !m_selectedConnection.isEmpty()
        && !isNetworkConnection(m_selectedConnection);
}

void AddConnectionViewModel::setSelectedConnection(
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

void AddConnectionViewModel::setSelectedBaud(int baud)
{
    if (!availableBaudRates().contains(baud) || m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit SelectedBaudChanged(baud);
}

bool AddConnectionViewModel::Connect()
{
    if (m_selectedConnection.isEmpty()) {
        return false;
    }
    emit connectRequested(m_selectedConnection, m_selectedBaud);
    return true;
}
