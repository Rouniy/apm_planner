#include "MicrodroneDownlinkService.h"

#include <limits>
#include <utility>

MicrodroneDownlinkService::MicrodroneDownlinkService(Dependencies dependencies, QObject *parent)
    : QObject(parent), m_dependencies(std::move(dependencies))
{
    if (!m_dependencies.clock)
        m_dependencies.clock = [] { return QDateTime::currentDateTimeUtc(); };
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &MicrodroneDownlinkService::emitNow);
}

MicrodroneDownlinkService::~MicrodroneDownlinkService()
{
    m_destroying = true;
    finish(State::Stopped, QString());
}

MicrodroneDownlinkService::OutputFactory MicrodroneDownlinkService::ProductionOutputFactory()
{
    return [](const MicrodroneOutputSettings &settings, QString *) {
        return std::unique_ptr<MavlinkMirrorOutput>(
            new SerialMirrorOutput(settings.port, settings.baud));
    };
}

QList<int> MicrodroneDownlinkService::Bauds()
{
    return {4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200};
}

QString MicrodroneDownlinkService::TargetChangedText()
{
    return tr("The active modem or vehicle changed. MicroDrone output was stopped; "
              "start it again after verifying the selected telemetry source.");
}

QString MicrodroneDownlinkService::sourceDescription() const
{
    if (!m_source.isValid())
        return tr("No connected telemetry source.");
    return tr("Telemetry source: %1:%2 on %3.")
        .arg(m_source.selection.endpoint.systemId)
        .arg(m_source.selection.endpoint.componentId)
        .arg(m_source.linkName.isEmpty() ? tr("the active modem") : m_source.linkName);
}

bool MicrodroneDownlinkService::resolveCurrent(MicrodroneSource *source)
{
    const auto resolver = m_dependencies.resolveSource;
    QPointer<MicrodroneDownlinkService> guard(this);
    *source = resolver ? resolver() : MicrodroneSource();
    return guard && source->isValid();
}

bool MicrodroneDownlinkService::sourceStillCurrent(quint64 revision)
{
    QPointer<MicrodroneDownlinkService> guard(this);
    MicrodroneSource current;
    const bool valid = resolveCurrent(&current);
    if (!guard || revision != m_revision)
        return false;
    if (!valid || !m_source.sameSource(current)) {
        m_source = current;
        m_telemetry.clear();
        finish(State::Stopped, TargetChangedText());
        return false;
    }
    return true;
}

void MicrodroneDownlinkService::finish(State state, const QString &text)
{
    ++m_revision;
    const quint64 revision = m_revision;
    m_timer.stop();
    m_state = state;
    m_text = text;
    m_pending.clear();
    m_offset = 0;
    m_draining = false;
    // Detach before closing: a synchronous close callback cannot close a
    // successor session or resurrect this one. Keep the signal sender alive.
    auto output = std::move(m_output);
    QPointer<MicrodroneDownlinkService> guard(this);
    if (output) {
        disconnect(output.get(), nullptr, this, nullptr);
        output->close();
    }
    if (guard && revision == m_revision && !m_destroying)
        emit changed();
}

void MicrodroneDownlinkService::stop()
{
    finish(State::Stopped, tr("Stopped."));
}

bool MicrodroneDownlinkService::start(const MicrodroneOutputSettings &requestedSettings, QString *error)
{
    const MicrodroneOutputSettings settings = requestedSettings;
    if (m_destroying || busy() || isRunning()) {
        if (error) *error = tr("MicroDrone output is already opening or running.");
        return false;
    }
    QPointer<MicrodroneDownlinkService> guard(this);
    const quint64 revision = ++m_revision;
    m_state = State::Opening;
    m_text = tr("Opening serial output...");
    auto fail = [guard, revision, error](const QString &reason) {
        if (error) *error = reason;
        if (guard && guard->m_revision == revision)
            guard->finish(State::Failed, reason);
        return false;
    };
    if (settings.port.trimmed().isEmpty())
        return fail(tr("Select a serial port."));
    if (!Bauds().contains(settings.baud))
        return fail(tr("Select one of the supported baud rates."));
    const auto validator = m_dependencies.validatePort;
    const QString conflict = validator ? validator(settings.port) : QString();
    if (!guard || revision != m_revision) return false;
    if (!conflict.isEmpty()) return fail(conflict);
    MicrodroneSource current;
    const bool valid = resolveCurrent(&current);
    if (!guard || revision != m_revision) return false;
    if (!valid) return fail(tr("Connect and select a vehicle before starting MicroDrone output."));
    if (!m_source.sameSource(current)) m_telemetry.clear();
    m_source = current;
    m_counter = 0;
    m_frames = m_dropped = 0;
    m_lastLine.clear();
    emit changed();
    if (!guard || revision != m_revision || !sourceStillCurrent(revision)) return false;
    const auto factory = m_dependencies.outputFactory;
    QString reason;
    auto owned = factory ? factory(settings, &reason) : nullptr;
    // QObject output deletion is deferred even if the callback deletes us while
    // open/write is on the stack; each call site additionally pins ownership.
    std::shared_ptr<MavlinkMirrorOutput> output(owned.release(), [](MavlinkMirrorOutput *value) {
        if (value) value->deleteLater();
    });
    if (!guard || revision != m_revision) {
        if (output) output->close();
        return false;
    }
    if (!output) return fail(reason.isEmpty() ? tr("Serial output is unavailable.") : reason);
    m_output = output;
    connect(output.get(), &MavlinkMirrorOutput::errorOccurred, this,
            [this, revision](const QString &message) {
        if (revision == m_revision) finish(State::Failed, message);
    });
    connect(output.get(), &MavlinkMirrorOutput::bytesWritten, this,
            [this, revision](qint64) {
        if (revision == m_revision) drain();
    });
    const bool opened = output->open(&reason);
    if (!guard || revision != m_revision) {
        output->close();
        return false;
    }
    if (!opened) return fail(reason.isEmpty() ? tr("Cannot open serial output.") : reason);
    if (!sourceStillCurrent(revision)) return false;
    m_state = State::Emitting;
    m_text = tr("Emitting MicroDrone telemetry on %1 at %2 baud.").arg(settings.port).arg(settings.baud);
    m_timer.start();
    emit changed();
    if (!guard || revision != m_revision) return false;
    emitNow();
    if (guard && m_state == State::Failed && error) *error = m_text;
    return guard && revision == m_revision && isRunning();
}

void MicrodroneDownlinkService::synchronizeSource()
{
    QPointer<MicrodroneDownlinkService> guard(this);
    const quint64 revision = m_revision;
    MicrodroneSource current;
    resolveCurrent(&current);
    if (!guard || revision != m_revision) return;
    if (m_source.sameSource(current)) return;
    ++m_revision; // Idle cache changes invalidate in-flight receipt callbacks too.
    m_source = current;
    m_telemetry.clear();
    if (busy() || isRunning()) finish(State::Stopped, TargetChangedText());
    else emit changed();
}

void MicrodroneDownlinkService::observeMessage(int linkId, quint64 epoch,
                                             const mavlink_message_t &message)
{
    QPointer<MicrodroneDownlinkService> guard(this);
    synchronizeSource();
    if (!guard || !m_source.isValid()) return;
    const auto endpoint = m_source.selection.endpoint;
    if (linkId != endpoint.linkId || epoch != m_source.instance.linkSessionEpoch
        || message.sysid != endpoint.systemId || message.compid != endpoint.componentId) return;
    const quint64 revision = m_revision;
    const auto clock = m_dependencies.clock;
    const QDateTime now = clock();
    if (!guard || revision != m_revision) return;
    m_telemetry.Apply(message, now.toUTC());
}

void MicrodroneDownlinkService::forgetLink(int linkId, quint64 epoch)
{
    if (m_source.selection.endpoint.linkId != linkId
        || m_source.instance.linkSessionEpoch != epoch) return;
    const bool active = busy() || isRunning();
    m_source = MicrodroneSource();
    m_telemetry.clear();
    finish(State::Stopped, active ? TargetChangedText() : tr("Stopped."));
}

void MicrodroneDownlinkService::emitNow()
{
    if (!isRunning() || m_draining) return;
    QPointer<MicrodroneDownlinkService> guard(this);
    const quint64 revision = m_revision;
    if (!sourceStillCurrent(revision)) return;
    if (!m_pending.isEmpty()) {
        ++m_dropped;
        drain();
        return;
    }
    auto output = m_output;
    if (!output) return;
    const qint64 pending = output->pendingBytes();
    if (!guard || revision != m_revision) return;
    if (pending > 0) {
        ++m_dropped;
        emit changed();
        return;
    }
    const auto clock = m_dependencies.clock;
    const QDateTime now = clock();
    if (!guard || revision != m_revision || !sourceStillCurrent(revision)) return;
    QString error;
    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        m_telemetry.snapshot(), now.toUTC(), m_counter, &error);
    if (frame.isEmpty() || frame.size() > 8192 || m_counter == std::numeric_limits<qint64>::max()) {
        finish(State::Failed, error.isEmpty() ? tr("Cannot encode MicroDrone telemetry.") : error);
        return;
    }
    m_pending = frame;
    m_offset = 0;
    ++m_counter; // nominal sample time, like MP10; skipped busy ticks do not advance it
    drain();
}

void MicrodroneDownlinkService::drain()
{
    if (!isRunning() || m_draining || m_pending.isEmpty()) return;
    QPointer<MicrodroneDownlinkService> guard(this);
    const quint64 revision = m_revision;
    if (!sourceStillCurrent(revision)) return;
    auto output = m_output;
    if (!output) return;
    m_draining = true;
    // At most one bounded frame, never a backlog of obsolete telemetry. A
    // short/zero write is resumed on bytesWritten or the next timer tick.
    const QByteArray remaining = m_pending.mid(m_offset);
    const qint64 accepted = output->write(remaining);
    if (!guard || revision != m_revision) return;
    m_draining = false;
    if (!sourceStillCurrent(revision)) return;
    if (accepted < 0 || accepted > remaining.size()) {
        finish(State::Failed, tr("MicroDrone serial output write failed."));
        return;
    }
    m_offset += accepted;
    if (m_offset != m_pending.size()) return;
    const QList<QByteArray> lines = m_pending.trimmed().split('\n');
    m_lastLine = QString::fromLatin1(lines.constLast().trimmed());
    m_pending.clear();
    m_offset = 0;
    ++m_frames;
    emit changed();
}
