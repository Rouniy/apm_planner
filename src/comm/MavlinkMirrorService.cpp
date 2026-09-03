#include "MavlinkMirrorService.h"

MavlinkMirrorService::OutputFactory MavlinkMirrorService::ProductionOutputFactory()
{
    return [](const MavlinkMirrorSettings &settings, QString *error) {
        return MavlinkMirrorOutput::Create(settings, error);
    };
}

QString MavlinkMirrorService::StoppedText()
{
    return QStringLiteral("Stopped.");
}

QString MavlinkMirrorService::PickPortText()
{
    return QStringLiteral("Pick a port first.");
}

QString MavlinkMirrorService::ErrorConnectingText(const QString &reason)
{
    return QStringLiteral("Error connecting: %1").arg(reason);
}

QString MavlinkMirrorService::SourceRemovedText(const QString &linkName)
{
    return QStringLiteral("Stopped: source link %1 was removed.").arg(linkName);
}

QString MavlinkMirrorService::WriteBackFailedText()
{
    return QStringLiteral("Write back failed: the vehicle link rejected bytes; write back disabled.");
}

MavlinkMirrorService::MavlinkMirrorService(RawWriter rawWriter, OutputFactory outputFactory,
                                           QObject *parent)
    : QObject(parent)
    , m_rawWriter(std::move(rawWriter))
    , m_outputFactory(std::move(outputFactory))
    , m_text(StoppedText())
{
    if (!m_outputFactory) {
        m_outputFactory = ProductionOutputFactory();
    }
}

MavlinkMirrorService::~MavlinkMirrorService()
{
    // No signals from a destructor. Release the unique_ptr before ~QObject
    // deletes the parented output, so nothing is deleted twice.
    m_queue.clear();
    m_pendingBytes = 0;
    releaseOutput();
}

void MavlinkMirrorService::setUdpPortGuard(UdpPortGuard guard)
{
    m_udpPortGuard = std::move(guard);
}

MavlinkMirrorService::Status MavlinkMirrorService::status() const
{
    Status status;
    status.state = m_state;
    status.text = m_text;
    if (m_output && m_droppedBytes > 0) {
        status.text += QStringLiteral(" (dropped %1 bytes)").arg(m_droppedBytes);
    }
    if (m_writeBackFailed) {
        status.text += QLatin1Char(' ') + WriteBackFailedText();
    }
    status.selection = m_settings.portSelection;
    status.linkId = m_linkId;
    status.linkName = m_linkName;
    status.allowWriteBack = m_allowWriteBack;
    status.writeBackFailed = m_writeBackFailed;
    status.txBytes = m_txBytes;
    status.rxBytes = m_rxBytes;
    status.droppedBytes = m_droppedBytes;
    status.droppedFrames = m_droppedFrames;
    status.pendingBytes = m_pendingBytes;
    return status;
}

void MavlinkMirrorService::resetCounters()
{
    m_txBytes = 0;
    m_rxBytes = 0;
    m_droppedBytes = 0;
    m_droppedFrames = 0;
    m_writeBackFailed = false;
    m_queue.clear();
    m_pendingBytes = 0;
}

bool MavlinkMirrorService::start(int linkId, const QString &linkName,
                                 const MavlinkMirrorSettings &settings, QString *error)
{
    if (m_output) {
        stop();
    }
    resetCounters();
    m_settings = settings;
    m_settings.portSelection = settings.portSelection.trimmed();
    m_linkId = linkId;
    m_linkName = linkName;
    m_allowWriteBack = settings.allowWriteBack;

    const auto refuse = [this, error](State state, const QString &text) {
        if (error) {
            *error = text;
        }
        m_linkId = -1;
        m_state = state;
        m_text = text;
        emit statusChanged();
        return false;
    };

    if (m_settings.portSelection.isEmpty()) {
        return refuse(State::Stopped, PickPortText());   // MP10: status only, nothing starts
    }
    if (linkId < 0) {
        return refuse(State::Failed, ErrorConnectingText(
            QStringLiteral("No current link: select a vehicle target or connect a link.")));
    }
    if (MavlinkMirrorOutput::IsUdpHostSelection(m_settings.portSelection) && m_udpPortGuard) {
        QString reason;
        if (!m_udpPortGuard(MavlinkMirrorOutput::DefaultHostPort, &reason)) {
            if (reason.isEmpty()) {
                reason = QStringLiteral("UDP port %1 is not available.")
                             .arg(MavlinkMirrorOutput::DefaultHostPort);
            }
            return refuse(State::Failed, ErrorConnectingText(reason));
        }
    }

    QString reason;
    std::unique_ptr<MavlinkMirrorOutput> output = m_outputFactory(m_settings, &reason);
    if (!output) {
        return refuse(State::Failed, ErrorConnectingText(
            reason.isEmpty() ? QStringLiteral("no output for %1").arg(m_settings.portSelection)
                             : reason));
    }
    if (!output->open(&reason)) {
        output->close();
        return refuse(State::Failed, ErrorConnectingText(
            reason.isEmpty() ? QStringLiteral("cannot open %1").arg(m_settings.portSelection)
                             : reason));
    }

    // Parent the live output to the service: releaseOutput() hands it to
    // deleteLater, and if the service dies before the event loop runs, QObject
    // ownership reclaims it anyway. The unique_ptr is always released first.
    output->setParent(this);
    m_output = std::move(output);
    connect(m_output.get(), &MavlinkMirrorOutput::peerChanged, this, [this]() {
        if (m_output && !m_output->hasPeer()) {
            m_queue.clear();   // MP10 has no queue: nothing stale reaches the next peer
            m_pendingBytes = 0;
        }
        refreshState();
        emit statusChanged();
        drain();
    });
    connect(m_output.get(), &MavlinkMirrorOutput::bytesWritten, this, [this](qint64) {
        if (m_draining) {
            m_drainRequested = true;
        } else {
            drain();
        }
    });
    connect(m_output.get(), &MavlinkMirrorOutput::peerBytesReceived, this,
            &MavlinkMirrorService::handlePeerBytes);
    connect(m_output.get(), &MavlinkMirrorOutput::errorOccurred, this, [this](const QString &text) {
        fail(ErrorConnectingText(text));
    });
    refreshState();
    emit statusChanged();
    return true;
}

void MavlinkMirrorService::refreshState()
{
    if (!m_output) {
        return;
    }
    m_state = m_output->hasPeer() ? State::Mirroring : State::Listening;
    m_text = m_output->statusText();
}

void MavlinkMirrorService::releaseOutput()
{
    if (!m_output) {
        return;
    }
    // Never delete the output while one of its signals may still be on the
    // stack: detach, close, and let the event loop reclaim it.
    MavlinkMirrorOutput *output = m_output.release();
    disconnect(output, nullptr, this, nullptr);
    output->close();
    output->deleteLater();
}

void MavlinkMirrorService::setStopped(const QString &text)
{
    releaseOutput();
    m_queue.clear();
    m_pendingBytes = 0;
    m_linkId = -1;
    m_state = State::Stopped;
    m_text = text;
    emit statusChanged();
}

void MavlinkMirrorService::fail(const QString &text)
{
    releaseOutput();
    m_queue.clear();
    m_pendingBytes = 0;
    m_linkId = -1;
    m_state = State::Failed;
    m_text = text;
    emit statusChanged();
}

void MavlinkMirrorService::stop()
{
    if (!m_output && m_state == State::Stopped && m_text == StoppedText()) {
        return;   // idempotent
    }
    setStopped(StoppedText());
}

void MavlinkMirrorService::clear()
{
    stop();
}

void MavlinkMirrorService::setAllowWriteBack(bool enabled)
{
    if (m_allowWriteBack == enabled) {
        return;
    }
    m_allowWriteBack = enabled;   // live, like MP10 OnAllowWriteBackChanged
    if (enabled) {
        m_writeBackFailed = false;
    }
    emit statusChanged();
}

void MavlinkMirrorService::observeFrame(int linkId, const QByteArray &frame)
{
    if (!m_output || linkId != m_linkId || frame.isEmpty()) {
        return;   // exact pinned source only
    }
    if (!m_output->hasPeer()) {
        return;   // MP10 writes only to an open stream; nobody is listening yet
    }
    // Hard bound over everything the mirror owns: the FIFO plus whatever the
    // output has accepted but not yet transmitted.
    const qint64 outputPending = qMax<qint64>(0, m_output->pendingBytes());
    if (m_pendingBytes + outputPending + frame.size() > PendingLimitBytes) {
        ++m_droppedFrames;   // drop the newest frame
        m_droppedBytes += static_cast<quint64>(frame.size());
        return;
    }
    m_queue.append(frame);
    m_pendingBytes += frame.size();
    drain();
}

void MavlinkMirrorService::drain()
{
    if (m_draining) {
        m_drainRequested = true;
        return;
    }
    m_draining = true;
    do {
        m_drainRequested = false;
        while (m_output && m_output->hasPeer() && !m_queue.isEmpty()) {
            QByteArray &head = m_queue.first();
            const qint64 written = m_output->write(head);
            if (!m_output) {
                m_draining = false;   // the output failed synchronously inside write()
                return;
            }
            if (written < 0) {
                m_draining = false;
                fail(ErrorConnectingText(QStringLiteral("cannot write to %1")
                                             .arg(m_settings.portSelection)));
                return;
            }
            if (written == 0) {
                break;   // output buffer full: wait for bytesWritten
            }
            m_pendingBytes -= written;
            m_txBytes += static_cast<quint64>(written);
            if (written < head.size()) {
                head.remove(0, static_cast<int>(written));   // partial: keep order
                break;
            }
            m_queue.removeFirst();
        }
    } while (m_drainRequested && m_output && !m_queue.isEmpty());
    m_drainRequested = false;
    m_draining = false;
}

void MavlinkMirrorService::handlePeerBytes(const QByteArray &bytes)
{
    if (!m_output || bytes.isEmpty()) {
        return;
    }
    m_rxBytes += static_cast<quint64>(bytes.size());
    if (!m_allowWriteBack) {
        return;
    }
    // MP10 writes peer bytes raw to BaseStream: no parsing, no re-sequencing.
    const bool written = m_rawWriter && m_rawWriter(m_linkId, bytes);
    if (!written) {
        m_allowWriteBack = false;
        m_writeBackFailed = true;
        emit statusChanged();
    }
}

void MavlinkMirrorService::forgetLink(int linkId)
{
    if (!m_output || linkId != m_linkId) {
        return;
    }
    setStopped(SourceRemovedText(m_linkName));
}
