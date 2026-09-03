#include "SerialOutputNMEAViewModel.h"

#include <utility>

SerialOutputNMEAViewModel::SerialOutputNMEAViewModel(
    NmeaOutputService *service, Dependencies dependencies, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_dependencies(std::move(dependencies))
    , m_statusText(NmeaOutputService::StoppedText())
{
    refreshPorts();
    if (m_service) {
        connect(m_service, &NmeaOutputService::statusChanged,
                this, [this]() {
            m_localStatus.clear();
            refreshStatus();
        });
    }
}

SerialOutputNMEAViewModel::~SerialOutputNMEAViewModel()
{
    stop();
}

QString SerialOutputNMEAViewModel::connectButtonText() const
{
    return isRunning() ? tr("Stop") : tr("Connect");
}

bool SerialOutputNMEAViewModel::isRunning() const
{
    return m_service && m_service->isRunning();
}

void SerialOutputNMEAViewModel::refreshPorts()
{
    const QString previous = m_selectedPort;
    const QStringList serialPorts = m_dependencies.enumeratePorts
        ? m_dependencies.enumeratePorts() : QStringList();
    m_ports = NmeaOutputService::Selections(serialPorts);
    m_selectedPort = m_ports.contains(previous)
        ? previous : (m_ports.isEmpty() ? QString() : m_ports.first());
    emit changed();
}

void SerialOutputNMEAViewModel::setSelectedPort(const QString &selection)
{
    if (m_selectedPort == selection) {
        return;
    }
    m_selectedPort = selection;
    emit changed();
}

void SerialOutputNMEAViewModel::setSelectedBaud(int baud)
{
    if (!m_bauds.contains(baud) || m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit changed();
}

void SerialOutputNMEAViewModel::setSelectedRateHz(double rateHz)
{
    if (!m_rates.contains(rateHz)
        || qFuzzyCompare(m_selectedRateHz, rateHz)) {
        return;
    }
    m_selectedRateHz = rateHz;
    if (m_service && m_service->isRunning()) {
        m_service->setRateHz(rateHz);
    }
    emit changed();
}

void SerialOutputNMEAViewModel::toggleConnection()
{
    if (!m_service) {
        setLocalStatus(NmeaOutputService::ErrorConnectingText(
            tr("NMEA output service is unavailable.")));
        return;
    }
    if (m_service->isRunning()) {
        stop();
        return;
    }
    if (m_selectedPort.isEmpty()) {
        setLocalStatus(NmeaOutputService::PickPortText());
        return;
    }
    if (m_dependencies.validateSelection) {
        const QString reason = m_dependencies.validateSelection(m_selectedPort);
        if (!reason.isEmpty()) {
            setLocalStatus(NmeaOutputService::ErrorConnectingText(reason));
            return;
        }
    }
    const NmeaOutputSource source = m_dependencies.resolveSource
        ? m_dependencies.resolveSource() : NmeaOutputSource();
    if (!source.isValid()) {
        setLocalStatus(NmeaOutputService::ErrorConnectingText(
            tr("No current vehicle target: select a vehicle or connect a link.")));
        return;
    }

    NmeaOutputSettings settings;
    settings.portSelection = m_selectedPort;
    settings.baud = m_selectedBaud;
    settings.rateHz = m_selectedRateHz;
    m_localStatus.clear();
    m_service->start(source.endpoint, source.linkName, settings);
    refreshStatus();
}

void SerialOutputNMEAViewModel::stop()
{
    m_localStatus.clear();
    if (m_service) {
        m_service->stop();
    }
    refreshStatus();
}

void SerialOutputNMEAViewModel::refreshStatus()
{
    QString status = m_localStatus;
    QString last;
    if (m_service) {
        const NmeaOutputService::Status serviceStatus = m_service->status();
        if (status.isEmpty()) {
            status = serviceStatus.text;
        }
        last = serviceStatus.lastSentence;
    }
    if (status == m_statusText && last == m_lastSentence) {
        return;
    }
    m_statusText = status;
    m_lastSentence = last;
    emit changed();
}

void SerialOutputNMEAViewModel::setLocalStatus(const QString &text)
{
    m_localStatus = text;
    m_statusText = text;
    m_lastSentence.clear();
    emit changed();
}
