#include "MicrodroneDownlinkViewModel.h"

#include <algorithm>
#include <exception>
#include <utility>

MicrodroneDownlinkViewModel::MicrodroneDownlinkViewModel(
    MicrodroneDownlinkService *service, PortEnumerator enumeratePorts,
    QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_enumeratePorts(std::move(enumeratePorts))
    , m_bauds(MicrodroneDownlinkService::Bauds())
{
    refreshPorts();
    if (m_service) {
        connect(m_service, &MicrodroneDownlinkService::changed,
                this, [this]() {
            m_localStatus.clear();
            refreshStatus();
        });
        connect(m_service, &QObject::destroyed, this, [this]() {
            ++m_revision;
            m_localStatus = tr("MicroDrone downlink service is unavailable.");
            refreshStatus();
        });
        m_service->synchronizeSource();
    }
    refreshStatus();
}

MicrodroneDownlinkViewModel::~MicrodroneDownlinkViewModel()
{
    const QPointer<MicrodroneDownlinkService> service(m_service);
    if (service) {
        disconnect(service, nullptr, this, nullptr);
        service->stop();
    }
}

bool MicrodroneDownlinkViewModel::busy() const
{
    return m_service && m_service->busy();
}

bool MicrodroneDownlinkViewModel::isRunning() const
{
    return m_service && m_service->isRunning();
}

QString MicrodroneDownlinkViewModel::connectButtonText() const
{
    return isRunning() ? tr("Stop") : tr("Connect");
}

void MicrodroneDownlinkViewModel::refreshPorts()
{
    if (!canEditSettings())
        return;
    const PortEnumerator enumerate = m_enumeratePorts;
    const quint64 revision = ++m_revision;
    const QPointer<MicrodroneDownlinkViewModel> guard(this);
    QStringList ports;
    try {
        ports = enumerate ? enumerate() : QStringList();
    } catch (const std::exception &exception) {
        if (guard && revision == m_revision) {
            setLocalStatus(tr("Refreshing serial ports failed: %1")
                .arg(QString::fromLocal8Bit(exception.what())));
        }
        return;
    } catch (...) {
        if (guard && revision == m_revision)
            setLocalStatus(tr("Refreshing serial ports failed."));
        return;
    }
    if (!guard || revision != m_revision)
        return;
    ports.removeAll(QString());
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end(),
              [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    const QString previous = m_selectedPort;
    m_ports = ports;
    m_selectedPort = m_ports.contains(previous)
        ? previous : (m_ports.isEmpty() ? QString() : m_ports.first());
    m_localStatus.clear();
    refreshStatus();
}

void MicrodroneDownlinkViewModel::setSelectedPort(const QString &port)
{
    if (!canEditSettings() || m_selectedPort == port)
        return;
    ++m_revision;
    m_selectedPort = port;
    m_localStatus.clear();
    refreshStatus();
}

void MicrodroneDownlinkViewModel::setSelectedBaud(int baud)
{
    if (!canEditSettings() || !m_bauds.contains(baud)
        || m_selectedBaud == baud) {
        return;
    }
    ++m_revision;
    m_selectedBaud = baud;
    m_localStatus.clear();
    refreshStatus();
}

void MicrodroneDownlinkViewModel::toggleConnection()
{
    const QPointer<MicrodroneDownlinkViewModel> guard(this);
    const QPointer<MicrodroneDownlinkService> service(m_service);
    if (!service) {
        setLocalStatus(tr("MicroDrone downlink service is unavailable."));
        return;
    }
    if (service->busy())
        return;
    if (service->isRunning()) {
        stop();
        return;
    }
    if (m_selectedPort.trimmed().isEmpty()) {
        setLocalStatus(tr("Select a serial output port first."));
        return;
    }
    if (!m_bauds.contains(m_selectedBaud)) {
        setLocalStatus(tr("Select a supported baud rate."));
        return;
    }

    MicrodroneOutputSettings settings;
    settings.port = m_selectedPort;
    settings.baud = m_selectedBaud;
    m_localStatus.clear();
    const quint64 revision = ++m_revision;
    QString error;
    const bool started = service->start(settings, &error);
    if (!guard || !service || m_service != service
        || revision != m_revision) {
        return;
    }
    if (!started && !error.isEmpty())
        m_localStatus = error;
    refreshStatus();
}

void MicrodroneDownlinkViewModel::stop()
{
    const QPointer<MicrodroneDownlinkViewModel> guard(this);
    const QPointer<MicrodroneDownlinkService> service(m_service);
    ++m_revision;
    m_localStatus.clear();
    if (service)
        service->stop();
    if (guard)
        refreshStatus();
}

void MicrodroneDownlinkViewModel::refreshStatus()
{
    const QPointer<MicrodroneDownlinkService> service(m_service);
    const QString status = !m_localStatus.isEmpty()
        ? m_localStatus
        : (service ? service->statusText()
                   : tr("MicroDrone downlink service is unavailable."));
    const QString source = service
        ? service->sourceDescription()
        : tr("No connected telemetry source.");
    const QString line = service ? service->lastLine() : QString();
    m_statusText = status;
    m_sourceDescription = source;
    m_lastLine = line;
    emit changed();
}

void MicrodroneDownlinkViewModel::setLocalStatus(const QString &text)
{
    ++m_revision;
    m_localStatus = text;
    refreshStatus();
}
