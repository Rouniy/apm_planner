#include "ConfigHWBTViewModel.h"

#include <QSet>

ConfigHWBTViewModel::ConfigHWBTViewModel(QObject *parent)
    : QObject(parent)
{
}

QString ConfigHWBTViewModel::Title() const
{
    return tr("Bluetooth Setup");
}

QString ConfigHWBTViewModel::Instructions() const
{
    return tr(
        "Configures an HC-05/HC-06 style serial Bluetooth module via AT "
        "commands. Disconnect the main link first, select the module's "
        "serial port, then click Write.");
}

QStringList ConfigHWBTViewModel::Bauds() const
{
    return {
        QStringLiteral("1200"), QStringLiteral("2400"),
        QStringLiteral("4800"), QStringLiteral("9600"),
        QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200")
    };
}

QHash<int, int> ConfigHWBTViewModel::BaudMap()
{
    return {
        {1200, 1}, {2400, 2}, {4800, 3}, {9600, 4},
        {19200, 5}, {38400, 6}, {57600, 7}, {115200, 8}
    };
}

QList<int> ConfigHWBTViewModel::ProbeBauds()
{
    // Preserve Mission Planner's dictionary insertion order.
    return {57600, 38400, 9600, 19200, 115200, 1200, 2400, 4800};
}

QList<QByteArray> ConfigHWBTViewModel::BuildCommands(
    const QString &name, const QString &selectedBaud,
    const QString &pin)
{
    bool baudOk = false;
    const int baud = selectedBaud.toInt(&baudOk);
    const int baudCode = baudOk ? BaudMap().value(baud, 0) : 0;
    return {
        QByteArrayLiteral("AT"),
        QByteArrayLiteral("AT+VERSION"),
        QStringLiteral("AT+ROLE=0\r\n").toLatin1(),
        QStringLiteral("AT+NAME=%1\r\n").arg(name).toLatin1(),
        QStringLiteral("AT+NAME%1").arg(name).toLatin1(),
        QStringLiteral("AT+BAUD=%1\r\n").arg(selectedBaud).toLatin1(),
        QStringLiteral("AT+BAUD%1").arg(baudCode).toLatin1(),
        QStringLiteral("AT+PSWD=%1\r\n").arg(pin).toLatin1(),
        QStringLiteral("AT+PIN%1").arg(pin).toLatin1(),
        QByteArrayLiteral("AT+RESET")
    };
}

void ConfigHWBTViewModel::setPorts(
    const QStringList &ports, const QString &preferredPort)
{
    QStringList distinct;
    QSet<QString> seen;
    for (const QString &port : ports) {
        const QString candidate = port.trimmed();
        const QString identity = candidate.toCaseFolded();
        if (!candidate.isEmpty() && !seen.contains(identity)) {
            seen.insert(identity);
            distinct.append(candidate);
        }
    }
    const QString previous = m_selectedPort;
    m_ports = distinct;
    if (m_ports.contains(previous)) {
        m_selectedPort = previous;
    } else if (m_ports.contains(preferredPort)) {
        m_selectedPort = preferredPort;
    } else {
        m_selectedPort = m_ports.value(0);
    }
    emit portsChanged();
    emit settingsChanged();
}

void ConfigHWBTViewModel::setSelectedPort(const QString &port)
{
    if (m_selectedPort == port) {
        return;
    }
    m_selectedPort = port;
    emit settingsChanged();
}

void ConfigHWBTViewModel::setName(const QString &name)
{
    if (m_name == name) {
        return;
    }
    m_name = name;
    emit settingsChanged();
}

void ConfigHWBTViewModel::setSelectedBaud(const QString &baud)
{
    if (m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit settingsChanged();
}

void ConfigHWBTViewModel::setPin(const QString &pin)
{
    if (m_pin == pin) {
        return;
    }
    m_pin = pin;
    emit settingsChanged();
}

void ConfigHWBTViewModel::setMainLinkConnected(bool connected)
{
    if (m_mainLinkConnected == connected) {
        return;
    }
    m_mainLinkConnected = connected;
    emit stateChanged();
    if (m_mainLinkConnected && m_isBusy) {
        Cancel();
    }
}

bool ConfigHWBTViewModel::Write()
{
    if (m_isBusy) {
        return false;
    }
    if (m_mainLinkConnected) {
        append(tr(
            "Please disconnect the main link before configuring "
            "Bluetooth."));
        return false;
    }
    if (m_selectedPort.trimmed().isEmpty()) {
        append(tr("No serial port selected."));
        return false;
    }
    bool baudOk = false;
    const int baud = m_selectedBaud.toInt(&baudOk);
    if (!baudOk || !BaudMap().contains(baud)) {
        append(tr("Invalid baud rate."));
        return false;
    }
    if (!isPrintableAscii(m_name, 32)) {
        append(tr("Name must be 1–32 printable ASCII characters."));
        return false;
    }
    if (!isPrintableAscii(m_pin, 16)) {
        append(tr("PIN must be 1–16 printable ASCII characters."));
        return false;
    }

    m_output.clear();
    emit outputChanged();
    m_isBusy = true;
    m_isCanceling = false;
    const quint64 generation = ++m_generation;
    emit stateChanged();
    emit programRequested(
        generation, m_selectedPort, m_name, m_selectedBaud, m_pin);
    return true;
}

void ConfigHWBTViewModel::Cancel()
{
    if (!m_isBusy || m_isCanceling) {
        return;
    }
    m_isCanceling = true;
    append(tr("Canceling operation…"));
    emit stateChanged();
    emit cancelRequested(m_generation);
}

void ConfigHWBTViewModel::operationProgress(
    quint64 generation, const QString &line)
{
    if (!m_isBusy || m_isCanceling || generation != m_generation) {
        return;
    }
    append(line);
}

void ConfigHWBTViewModel::operationFinished(
    quint64 generation, bool success, bool canceled)
{
    if (!m_isBusy || generation != m_generation) {
        return;
    }
    const bool canceledByUser = m_isCanceling;
    m_isBusy = false;
    m_isCanceling = false;
    if (canceled || canceledByUser) {
        append(tr("Operation canceled."));
    } else if (success) {
        append(tr(
            "Programming sequence sent. Review device responses above."));
    } else {
        append(tr("Error setting parameter — no device responded."));
    }
    emit stateChanged();
}

void ConfigHWBTViewModel::append(const QString &line)
{
    m_output.append(line);
    m_output.append(QLatin1Char('\n'));
    emit outputChanged();
}

bool ConfigHWBTViewModel::isPrintableAscii(
    const QString &value, int maximumLength)
{
    if (value.isEmpty() || value.size() > maximumLength) {
        return false;
    }
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}
