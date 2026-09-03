#include "SerialPassThroughViewModel.h"

#include <QTimer>

#include <utility>

SerialPassThroughViewModel::SerialPassThroughViewModel(
    MavlinkMirrorService *service, Dependencies dependencies, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_dependencies(std::move(dependencies))
    , m_statusText(MavlinkMirrorService::StoppedText())
{
    refreshPorts();

    auto *poll = new QTimer(this);
    poll->setInterval(500);
    connect(poll, &QTimer::timeout, this,
            &SerialPassThroughViewModel::refreshStatus);
    poll->start();
}

SerialPassThroughViewModel::~SerialPassThroughViewModel()
{
    stop();
}

QString SerialPassThroughViewModel::connectButtonText() const
{
    return isRunning() ? tr("Stop") : tr("Start");
}

bool SerialPassThroughViewModel::isRunning() const
{
    return m_service && m_service->isRunning();
}

void SerialPassThroughViewModel::refreshPorts()
{
    const QString previous = m_selectedPort;
    const QStringList serialPorts = m_dependencies.enumeratePorts
        ? m_dependencies.enumeratePorts() : QStringList();
    m_ports = MavlinkMirrorService::Selections(serialPorts);
    m_selectedPort = m_ports.contains(previous)
        ? previous : (m_ports.isEmpty() ? QString() : m_ports.first());
    emit changed();
}

void SerialPassThroughViewModel::setSelectedPort(const QString &selection)
{
    if (m_selectedPort == selection) {
        return;
    }
    m_selectedPort = selection;
    emit changed();
}

void SerialPassThroughViewModel::setSelectedBaud(int baud)
{
    if (!m_bauds.contains(baud) || m_selectedBaud == baud) {
        return;
    }
    m_selectedBaud = baud;
    emit changed();
}

void SerialPassThroughViewModel::setAllowWriteBack(bool enabled)
{
    if (m_allowWriteBack == enabled) {
        return;
    }
    m_allowWriteBack = enabled;
    if (m_service) {
        m_service->setAllowWriteBack(enabled);
    }
    refreshStatus();
}

void SerialPassThroughViewModel::toggleConnection()
{
    if (!m_service) {
        setLocalStatus(tr("MAVLink mirror service is unavailable."));
        return;
    }
    if (m_service->isRunning()) {
        m_localStatus.clear();
        m_service->stop();
        refreshStatus();
        return;
    }
    m_localStatus.clear();
    if (m_selectedPort.isEmpty()) {
        setLocalStatus(MavlinkMirrorService::PickPortText());
        return;
    }
    if (m_dependencies.validateSelection) {
        const QString rejection =
            m_dependencies.validateSelection(m_selectedPort);
        if (!rejection.isEmpty()) {
            setLocalStatus(MavlinkMirrorService::ErrorConnectingText(rejection));
            return;
        }
    }
    const MavlinkMirrorSource source = m_dependencies.resolveSource
        ? m_dependencies.resolveSource() : MavlinkMirrorSource();
    if (!source.isValid()) {
        setLocalStatus(tr("No current link: select a vehicle target or connect a link."));
        return;
    }

    MavlinkMirrorSettings settings;
    settings.portSelection = m_selectedPort;
    settings.baud = m_selectedBaud;
    settings.allowWriteBack = m_allowWriteBack;
    QString error;
    m_service->start(source.linkId, source.linkName, settings, &error);
    refreshStatus();
}

void SerialPassThroughViewModel::stop()
{
    if (m_service) {
        m_localStatus.clear();
        m_service->stop();
        refreshStatus();
    }
}

void SerialPassThroughViewModel::refreshStatus()
{
    if (!m_service) {
        return;
    }
    if (!m_localStatus.isEmpty() && !m_service->isRunning()) {
        return;
    }
    const MavlinkMirrorService::Status status = m_service->status();
    const QString text = status.text;
    const quint64 tx = status.txBytes;
    const quint64 rx = status.rxBytes;
    const quint64 dropped = status.droppedBytes;
    const bool allowWriteBack = status.allowWriteBack;
    if (m_statusText == text && m_txBytes == tx && m_rxBytes == rx
        && m_droppedBytes == dropped
        && m_allowWriteBack == allowWriteBack) {
        return;
    }
    m_statusText = text;
    m_txBytes = tx;
    m_rxBytes = rx;
    m_droppedBytes = dropped;
    m_allowWriteBack = allowWriteBack;
    emit changed();
}

void SerialPassThroughViewModel::setLocalStatus(const QString &text)
{
    m_localStatus = text;
    m_statusText = text;
    m_txBytes = 0;
    m_rxBytes = 0;
    m_droppedBytes = 0;
    emit changed();
}
