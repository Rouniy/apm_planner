#include "NmeaOutputService.h"

#include <QTimer>

#include <cmath>
#include <utility>

QString NmeaOutputService::TcpHostSelection()
{
    return QStringLiteral("TCP Host - 14551");
}

QString NmeaOutputService::UdpHostSelection()
{
    return QStringLiteral("UDP Host - 14551");
}

bool NmeaOutputService::IsTcpHostSelection(const QString &selection)
{
    return selection == TcpHostSelection();
}

bool NmeaOutputService::IsUdpHostSelection(const QString &selection)
{
    return selection == UdpHostSelection();
}

QStringList NmeaOutputService::Selections(const QStringList &serialPorts)
{
    QStringList selections;
    for (const QString &port : serialPorts) {
        if (!port.isEmpty() && !selections.contains(port)) {
            selections.append(port);
        }
    }
    selections.append(TcpHostSelection());
    selections.append(UdpHostSelection());
    return selections;
}

NmeaOutputService::OutputFactory NmeaOutputService::ProductionOutputFactory()
{
    return [](const NmeaOutputSettings &settings, QString *error) {
        const QString selection = settings.portSelection.trimmed();
        if (selection.isEmpty()) {
            if (error) {
                *error = QStringLiteral("No port selected.");
            }
            return std::unique_ptr<MavlinkMirrorOutput>();
        }
        if (IsTcpHostSelection(selection)) {
            return std::unique_ptr<MavlinkMirrorOutput>(
                new TcpHostMirrorOutput(DefaultHostPort));
        }
        if (IsUdpHostSelection(selection)) {
            return std::unique_ptr<MavlinkMirrorOutput>(
                new UdpHostMirrorOutput(DefaultHostPort));
        }
        if (settings.baud <= 0) {
            if (error) {
                *error = QStringLiteral("Invalid baud rate %1.")
                             .arg(settings.baud);
            }
            return std::unique_ptr<MavlinkMirrorOutput>();
        }
        return std::unique_ptr<MavlinkMirrorOutput>(
            new SerialMirrorOutput(selection, settings.baud));
    };
}

QString NmeaOutputService::StoppedText()
{
    return QStringLiteral("Stopped.");
}

QString NmeaOutputService::PickPortText()
{
    return QStringLiteral("Pick a port first.");
}

QString NmeaOutputService::ErrorConnectingText(const QString &reason)
{
    return QStringLiteral("Error connecting: %1").arg(reason);
}

QString NmeaOutputService::SourceRemovedText(const QString &linkName)
{
    return QStringLiteral("Stopped: source link %1 was removed.").arg(linkName);
}

NmeaOutputService::NmeaOutputService(OutputFactory outputFactory, Clock clock,
                                     QObject *parent)
    : QObject(parent)
    , m_outputFactory(std::move(outputFactory))
    , m_clock(std::move(clock))
    , m_text(StoppedText())
{
    if (!m_outputFactory) {
        m_outputFactory = ProductionOutputFactory();
    }
    if (!m_clock) {
        m_clock = []() { return QDateTime::currentDateTimeUtc(); };
    }
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &NmeaOutputService::emitNow);
}

NmeaOutputService::~NmeaOutputService()
{
    m_timer->stop();
    releaseOutput();
}

void NmeaOutputService::setUdpPortGuard(UdpPortGuard guard)
{
    m_udpPortGuard = std::move(guard);
}

NmeaOutputService::Status NmeaOutputService::status() const
{
    Status result;
    result.state = m_state;
    result.text = m_text;
    if (m_output && m_droppedSentences > 0) {
        result.text += QStringLiteral(" (dropped %1 sentences)")
                           .arg(m_droppedSentences);
    }
    result.selection = m_settings.portSelection;
    result.endpoint = m_endpoint;
    result.linkName = m_linkName;
    result.lastSentence = m_lastSentence;
    result.sentencesSent = m_sentencesSent;
    result.droppedSentences = m_droppedSentences;
    result.ticks = m_ticks;
    return result;
}

int NmeaOutputService::timerIntervalMs() const
{
    const double rate = std::max(0.1, m_settings.rateHz);
    return qBound(1, qRound(1000.0 / rate), 4000);
}

void NmeaOutputService::resetSession()
{
    m_vehicleState = NmeaVehicleState();
    m_lastSentence.clear();
    m_sentencesSent = 0;
    m_droppedSentences = 0;
    m_ticks = 0;
}

bool NmeaOutputService::start(const VehicleEndpoint &endpoint,
                              const QString &linkName,
                              const NmeaOutputSettings &settings,
                              QString *error)
{
    if (m_output) {
        stop();
    }
    resetSession();
    m_settings = settings;
    m_settings.portSelection = settings.portSelection.trimmed();
    m_endpoint = endpoint;
    m_linkName = linkName.trimmed().isEmpty()
        ? endpoint.linkName : linkName.trimmed();

    const auto refuse = [this, error](State state, const QString &text) {
        if (error) {
            *error = text;
        }
        m_endpoint = VehicleEndpoint();
        m_state = state;
        m_text = text;
        emit statusChanged();
        return false;
    };

    if (m_settings.portSelection.isEmpty()) {
        return refuse(State::Stopped, PickPortText());
    }
    if (!endpoint.isValid()) {
        return refuse(State::Failed, ErrorConnectingText(
            QStringLiteral("No current vehicle target: select a vehicle or connect a link.")));
    }
    if (!std::isfinite(m_settings.rateHz) || m_settings.rateHz <= 0.0) {
        return refuse(State::Failed, ErrorConnectingText(
            QStringLiteral("Invalid update rate.")));
    }
    if (IsUdpHostSelection(m_settings.portSelection) && m_udpPortGuard) {
        QString reason;
        if (!m_udpPortGuard(DefaultHostPort, &reason)) {
            if (reason.isEmpty()) {
                reason = QStringLiteral("UDP port %1 is not available.")
                             .arg(DefaultHostPort);
            }
            return refuse(State::Failed, ErrorConnectingText(reason));
        }
    }

    QString reason;
    std::unique_ptr<MavlinkMirrorOutput> output =
        m_outputFactory(m_settings, &reason);
    if (!output) {
        return refuse(State::Failed, ErrorConnectingText(
            reason.isEmpty()
                ? QStringLiteral("no output for %1").arg(m_settings.portSelection)
                : reason));
    }
    if (!output->open(&reason)) {
        output->close();
        return refuse(State::Failed, ErrorConnectingText(
            reason.isEmpty()
                ? QStringLiteral("cannot open %1").arg(m_settings.portSelection)
                : reason));
    }

    output->setParent(this);
    m_output = std::move(output);
    connect(m_output.get(), &MavlinkMirrorOutput::peerChanged,
            this, [this]() {
        refreshOutputStatus();
        emit statusChanged();
    });
    connect(m_output.get(), &MavlinkMirrorOutput::errorOccurred,
            this, [this](const QString &text) {
        fail(ErrorConnectingText(text));
    });
    // Mirror transports drain incoming bytes before emitting this signal. NMEA
    // Output deliberately ignores them; UDP uses only the sender as its peer.
    connect(m_output.get(), &MavlinkMirrorOutput::peerBytesReceived,
            this, [](const QByteArray &) {});

    m_state = State::Emitting;
    refreshOutputStatus();
    updateTimerInterval();
    m_timer->start();
    emit statusChanged();
    emitNow(); // MP10's worker attempts its first tick immediately.
    return true;
}

void NmeaOutputService::refreshOutputStatus()
{
    if (!m_output) {
        return;
    }
    if (IsTcpHostSelection(m_settings.portSelection)
        && m_output->hasPeer()) {
        const auto *tcp = qobject_cast<const TcpHostMirrorOutput *>(
            m_output.get());
        const QString peer = tcp ? tcp->peerDescription() : QString();
        m_text = peer.isEmpty()
            ? QStringLiteral("Emitting NMEA on %1.").arg(m_settings.portSelection)
            : QStringLiteral("Emitting NMEA to TCP client %1.").arg(peer);
        return;
    }
    m_text = QStringLiteral("Emitting NMEA on %1.")
                 .arg(m_settings.portSelection);
}

void NmeaOutputService::updateTimerInterval()
{
    m_timer->setInterval(timerIntervalMs());
}

void NmeaOutputService::setRateHz(double rateHz)
{
    if (!std::isfinite(rateHz) || rateHz <= 0.0
        || qFuzzyCompare(m_settings.rateHz, rateHz)) {
        return;
    }
    m_settings.rateHz = rateHz;
    updateTimerInterval();
    emit statusChanged();
}

void NmeaOutputService::observeMessage(int linkId,
                                       const mavlink_message_t &message)
{
    if (!m_output || linkId != m_endpoint.linkId
        || message.sysid != m_endpoint.systemId
        || message.compid != m_endpoint.componentId) {
        return;
    }
    NmeaSentenceBuilder::Apply(m_vehicleState, message);
}

void NmeaOutputService::emitNow()
{
    if (!m_output || !m_vehicleState.hasPosition) {
        return;
    }
    const bool isUdp = IsUdpHostSelection(m_settings.portSelection);
    if (!isUdp && !m_output->hasPeer()) {
        return; // MP10's TCP stream is closed until a client connects.
    }

    const QStringList bodies = NmeaSentenceBuilder::Tick(
        m_vehicleState, m_clock().toUTC());
    QList<QByteArray> lines;
    lines.reserve(bodies.size());
    qint64 tickBytes = 0;
    for (const QString &body : bodies) {
        const QByteArray line = NmeaSentenceBuilder::Line(body);
        tickBytes += line.size();
        lines.append(line);
    }
    ++m_ticks;

    // With UDP and no learned sender, MP10 still formats the tick and updates
    // LastSentence while its EndPointList silently sends to nobody.
    if (isUdp && !m_output->hasPeer()) {
        if (!lines.isEmpty()) {
            m_lastSentence = QString::fromLatin1(lines.last()).trimmed();
        }
        emit statusChanged();
        return;
    }

    if (m_output->pendingBytes() + tickBytes
        > MavlinkMirrorOutput::OutputPendingLimitBytes) {
        m_droppedSentences += static_cast<quint64>(lines.size());
        emit statusChanged();
        return;
    }

    for (int index = 0; index < lines.size(); ++index) {
        const QByteArray &line = lines.at(index);
        const qint64 written = m_output->write(line);
        if (!m_output) {
            return;
        }
        if (written < 0 || (written > 0 && written != line.size())) {
            fail(ErrorConnectingText(
                QStringLiteral("cannot write to %1")
                    .arg(m_settings.portSelection)));
            return;
        }
        if (written == 0) {
            m_droppedSentences += static_cast<quint64>(lines.size() - index);
            emit statusChanged();
            return;
        }
        ++m_sentencesSent;
        m_lastSentence = QString::fromLatin1(line).trimmed();
    }
    emit statusChanged();
}

void NmeaOutputService::releaseOutput()
{
    if (!m_output) {
        return;
    }
    MavlinkMirrorOutput *output = m_output.release();
    disconnect(output, nullptr, this, nullptr);
    output->close();
    output->deleteLater();
}

void NmeaOutputService::setStopped(const QString &text)
{
    m_timer->stop();
    releaseOutput();
    m_endpoint = VehicleEndpoint();
    m_state = State::Stopped;
    m_text = text;
    emit statusChanged();
}

void NmeaOutputService::fail(const QString &text)
{
    m_timer->stop();
    releaseOutput();
    m_endpoint = VehicleEndpoint();
    m_state = State::Failed;
    m_text = text;
    emit statusChanged();
}

void NmeaOutputService::stop()
{
    if (!m_output && m_state == State::Stopped && m_text == StoppedText()) {
        return;
    }
    setStopped(StoppedText());
}

void NmeaOutputService::forgetLink(int linkId)
{
    if (!m_output || linkId != m_endpoint.linkId) {
        return;
    }
    setStopped(SourceRemovedText(m_linkName));
}

void NmeaOutputService::clear()
{
    stop();
}
