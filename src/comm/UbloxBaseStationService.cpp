#include "UbloxBaseStationService.h"

#include <QMetaType>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr int MaximumReceiverChunkBytes = 256 * 1024;

void assignError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

QString modeName(UbloxBaseStationService::Mode mode)
{
    switch (mode) {
    case UbloxBaseStationService::Mode::Receiver:
        return QStringLiteral("receiver");
    case UbloxBaseStationService::Mode::SurveyIn:
        return QStringLiteral("survey-in");
    case UbloxBaseStationService::Mode::Fixed:
        return QStringLiteral("fixed-base");
    }
    return QStringLiteral("receiver");
}
}

bool UbloxBaseStationService::FixedPosition::isValid() const noexcept
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitudeMeters) && std::isfinite(accuracyMeters)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && altitudeMeters >= -10000.0 && altitudeMeters <= 100000.0
        && accuracyMeters >= MinimumSurveyAccuracyMeters
        && accuracyMeters <= MaximumSurveyAccuracyMeters;
}

UbloxBaseStationService::UbloxBaseStationService(
    GpsCorrectionSource *source, QObject *parent)
    : QObject(parent)
    , m_source(source)
    , m_sequenceTimer(new QTimer(this))
    , m_status(QStringLiteral("Connect a serial correction receiver first."))
{
    qRegisterMetaType<Mode>();
    qRegisterMetaType<State>();
    qRegisterMetaType<Outcome>();
    qRegisterMetaType<FixedPosition>();
    qRegisterMetaType<Report>();

    m_sequenceTimer->setSingleShot(true);
    m_sequenceTimer->setTimerType(Qt::PreciseTimer);
    connect(m_sequenceTimer, &QTimer::timeout,
            this, &UbloxBaseStationService::handleSequenceTimer);
    if (m_source) {
        connect(m_source, &GpsCorrectionSource::receiverBytes,
                this, &UbloxBaseStationService::handleReceiverBytes);
        connect(m_source, &GpsCorrectionSource::stateChanged,
                this, [this](bool, bool) { handleSourceStateChanged(); });
        connect(m_source, &QObject::destroyed,
                this, [this]() { handleSourceStateChanged(); });
        if (available()) {
            m_status = QStringLiteral("Serial correction receiver is ready.");
        }
    }
}

UbloxBaseStationService::~UbloxBaseStationService()
{
    m_shuttingDown = true;
    if (m_sequenceTimer) {
        m_sequenceTimer->stop();
    }
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }
}

bool UbloxBaseStationService::available() const noexcept
{
    return !m_shuttingDown && m_source
        && m_source->thread() == thread()
        && m_source->canConfigureReceiver()
        && m_source->receiverSession() != 0
        && m_source->receiverBaudRate() > 0;
}

UbloxBaseStationService::State UbloxBaseStationService::state() const noexcept
{
    if (m_busy) {
        return State::Configuring;
    }
    if (m_configuredSession != 0 && m_source
        && m_source->receiverSession() == m_configuredSession
        && m_source->canConfigureReceiver()) {
        switch (m_mode) {
        case Mode::Receiver:
            return State::Configured;
        case Mode::SurveyIn:
            return State::SurveyIn;
        case Mode::Fixed:
            return State::Fixed;
        }
    }
    return available() ? State::Ready : State::Unavailable;
}

bool UbloxBaseStationService::configureReceiver(
    bool m8p130Plus, quint64 *operationIdOut, QString *error)
{
    if (operationIdOut) {
        *operationIdOut = 0;
    }
    if (error) {
        error->clear();
    }
    if (!available()) {
        assignError(error,
                    QStringLiteral("An open writable serial correction receiver is required."));
        return false;
    }
    QVector<UbloxBaseStationProtocol::Command> commands =
        UbloxBaseStationProtocol::autoConfigure(
            m_source->receiverBaudRate(), m8p130Plus);
    if (commands.isEmpty()) {
        assignError(error,
                    QStringLiteral("The receiver setup sequence is unavailable."));
        return false;
    }
    return begin(Mode::Receiver, std::move(commands),
                 operationIdOut, error);
}

bool UbloxBaseStationService::configureSurveyIn(
    quint32 durationSeconds, double accuracyMeters, bool m8p130Plus,
    quint64 *operationIdOut, QString *error)
{
    if (operationIdOut) {
        *operationIdOut = 0;
    }
    if (error) {
        error->clear();
    }
    if (durationSeconds == 0
        || durationSeconds > MaximumSurveyDurationSeconds
        || !std::isfinite(accuracyMeters)
        || accuracyMeters < MinimumSurveyAccuracyMeters
        || accuracyMeters > MaximumSurveyAccuracyMeters) {
        assignError(error,
                    QStringLiteral("Survey-in duration or accuracy is outside the supported range."));
        return false;
    }
    if (!available()) {
        assignError(error,
                    QStringLiteral("An open writable serial correction receiver is required."));
        return false;
    }

    QString protocolError;
    QVector<UbloxBaseStationProtocol::Command> commands =
        UbloxBaseStationProtocol::restartSurvey(
            m_source->receiverBaudRate(), durationSeconds,
            accuracyMeters, m8p130Plus, &protocolError);
    if (commands.isEmpty()) {
        assignError(error, protocolError.isEmpty()
                        ? QStringLiteral("The survey-in request is invalid.")
                        : protocolError);
        return false;
    }
    return begin(Mode::SurveyIn, std::move(commands),
                 operationIdOut, error);
}

bool UbloxBaseStationService::configureFixed(
    const FixedPosition &position, bool m8p130Plus,
    quint64 *operationIdOut, QString *error)
{
    if (operationIdOut) {
        *operationIdOut = 0;
    }
    if (error) {
        error->clear();
    }
    if (!position.isValid()) {
        assignError(error,
                    QStringLiteral("The fixed base position or accuracy is invalid."));
        return false;
    }
    if (!available()) {
        assignError(error,
                    QStringLiteral("An open writable serial correction receiver is required."));
        return false;
    }

    QString protocolError;
    const QByteArray modeCommand = UbloxBaseStationProtocol::fixedLla(
        position.latitude, position.longitude, position.altitudeMeters,
        position.accuracyMeters, &protocolError);
    if (modeCommand.isEmpty()) {
        assignError(error, protocolError.isEmpty()
                        ? QStringLiteral("The fixed-base request is invalid.")
                        : protocolError);
        return false;
    }
    QVector<UbloxBaseStationProtocol::Command> commands =
        UbloxBaseStationProtocol::autoConfigure(
            m_source->receiverBaudRate(), m8p130Plus);
    commands += UbloxBaseStationProtocol::disableBase();
    commands.append(UbloxBaseStationProtocol::Command{
        modeCommand, 0, 200, 0, QStringLiteral("Set fixed base position")});
    return begin(Mode::Fixed, std::move(commands),
                 operationIdOut, error);
}

bool UbloxBaseStationService::applyFixed(
    const FixedPosition &position, quint64 *operationIdOut, QString *error)
{
    if (operationIdOut) {
        *operationIdOut = 0;
    }
    if (error) {
        error->clear();
    }
    if (!position.isValid()) {
        assignError(error,
                    QStringLiteral("The fixed base position or accuracy is invalid."));
        return false;
    }
    if (!available()) {
        assignError(error,
                    QStringLiteral("An open writable serial correction receiver is required."));
        return false;
    }

    QString protocolError;
    const QByteArray modeCommand = UbloxBaseStationProtocol::fixedLla(
        position.latitude, position.longitude, position.altitudeMeters,
        position.accuracyMeters, &protocolError);
    if (modeCommand.isEmpty()) {
        assignError(error, protocolError.isEmpty()
                        ? QStringLiteral("The fixed-base request is invalid.")
                        : protocolError);
        return false;
    }
    QVector<UbloxBaseStationProtocol::Command> commands;
    commands.reserve(2);
    commands.append(UbloxBaseStationProtocol::Command{
        modeCommand, 0, 200, 0, QStringLiteral("Set fixed base position")});
    commands.append(UbloxBaseStationProtocol::Command{
        UbloxBaseStationProtocol::frame(0x06, 0x71), 0, 0, 10,
        QStringLiteral("Poll time mode")});
    return begin(Mode::Fixed, std::move(commands),
                 operationIdOut, error);
}

bool UbloxBaseStationService::begin(
    Mode mode, QVector<UbloxBaseStationProtocol::Command> commands,
    quint64 *operationIdOut, QString *error)
{
    const QPointer<UbloxBaseStationService> guard(this);
    if (QThread::currentThread() != thread()) {
        assignError(error,
                    QStringLiteral("Receiver configuration must run on its owner thread."));
        return false;
    }
    if (m_shuttingDown || !m_source || m_source->thread() != thread()) {
        assignError(error,
                    QStringLiteral("Receiver configuration is unavailable."));
        return false;
    }
    if (m_busy || m_finishing) {
        assignError(error,
                    QStringLiteral("Another receiver configuration is active."));
        return false;
    }
    if (m_nextOperationId == 0) {
        assignError(error,
                    QStringLiteral("Receiver operation identifiers are exhausted."));
        return false;
    }
    if (commands.isEmpty() || commands.size() > MaximumCommands) {
        assignError(error,
                    QStringLiteral("The receiver configuration sequence has an invalid size."));
        return false;
    }
    qint64 totalBytes = 0;
    for (const UbloxBaseStationProtocol::Command &command : commands) {
        if (command.bytes.isEmpty() || command.delayBeforeMs < 0
            || command.delayAfterMs < 0 || command.baudRate < 0
            || totalBytes > MaximumSequenceBytes - command.bytes.size()) {
            assignError(error,
                        QStringLiteral("The receiver configuration sequence is invalid or too large."));
            return false;
        }
        totalBytes += command.bytes.size();
    }

    const quint64 session = m_source->receiverSession();
    if (!m_source->canConfigureReceiver() || session == 0
        || m_source->receiverBaudRate() <= 0) {
        assignError(error,
                    QStringLiteral("The serial receiver session changed before configuration."));
        return false;
    }

    m_protocol.reset();
    m_commands = std::move(commands);
    m_commandIndex = 0;
    m_submittedCommands = 0;
    m_acceptedAcknowledgements = 0;
    m_rejectedAcknowledgements = 0;
    m_acknowledgementStatus.clear();
    m_mode = mode;
    m_operationSession = session;
    m_configuredSession = 0;
    m_operationId = m_nextOperationId++;
    m_sequenceStep = SequenceStep::Idle;
    m_cancelRequested = false;
    m_sourceLostRequested = false;
    m_writeInFlight = false;
    m_busy = true;
    m_status = QStringLiteral("Submitting u-blox %1 configuration (0/%2)…")
        .arg(modeName(mode)).arg(m_commands.size());
    const quint64 operationId = m_operationId;
    if (operationIdOut) {
        *operationIdOut = operationId;
    }

    emit stateChanged();
    if (!guard || !operationIsCurrent(operationId, session)) {
        return guard && !m_busy;
    }
    scheduleCurrentCommand();
    return true;
}

void UbloxBaseStationService::scheduleCurrentCommand()
{
    if (!m_busy || m_shuttingDown || !m_source) {
        return;
    }
    const quint64 operationId = m_operationId;
    const quint64 session = m_operationSession;
    if (m_source->receiverSession() != session
        || !m_source->canConfigureReceiver()) {
        finish(Outcome::SourceLost,
               QStringLiteral("The serial receiver session changed during configuration."));
        return;
    }
    if (m_commandIndex >= m_commands.size()) {
        finish(Outcome::Submitted,
               QStringLiteral("All u-blox %1 commands were submitted; receiver ACK/NAV status remains diagnostic.")
                   .arg(modeName(m_mode)));
        return;
    }

    const UbloxBaseStationProtocol::Command command =
        m_commands.at(m_commandIndex);
    if (command.baudRate > 0
        && command.baudRate != m_source->receiverBaudRate()) {
        QString baudError;
        const QPointer<UbloxBaseStationService> guard(this);
        const bool changed = m_source->setReceiverBaudRate(
            command.baudRate, session, &baudError);
        if (!guard || !operationIsCurrent(operationId, session)) {
            return;
        }
        if (!changed) {
            finish(m_source && m_source->receiverSession() == session
                       ? Outcome::Rejected : Outcome::SourceLost,
                   baudError.isEmpty()
                       ? QStringLiteral("Unable to change the receiver baud rate.")
                       : baudError);
            return;
        }
    }

    m_sequenceStep = SequenceStep::BeforeWrite;
    if (command.delayBeforeMs > 0) {
        m_sequenceTimer->start(command.delayBeforeMs);
    } else {
        writeCurrentCommand();
    }
}

void UbloxBaseStationService::writeCurrentCommand()
{
    if (!m_busy || m_shuttingDown || !m_source
        || m_commandIndex < 0 || m_commandIndex >= m_commands.size()) {
        return;
    }
    const quint64 operationId = m_operationId;
    const quint64 session = m_operationSession;
    if (m_source->receiverSession() != session
        || !m_source->canConfigureReceiver()) {
        finish(Outcome::SourceLost,
               QStringLiteral("The serial receiver session changed before a configuration write."));
        return;
    }

    const UbloxBaseStationProtocol::Command command =
        m_commands.at(m_commandIndex);
    QString writeError;
    const QPointer<UbloxBaseStationService> guard(this);
    m_writeInFlight = true;
    const bool written = m_source->writeReceiverData(
        command.bytes, session, &writeError);
    if (!guard || !operationIsCurrent(operationId, session)) {
        return;
    }
    m_writeInFlight = false;
    if (written) {
        ++m_submittedCommands;
        ++m_commandIndex;
    }
    if (m_cancelRequested) {
        finish(Outcome::Cancelled,
               QStringLiteral("Receiver configuration was cancelled."));
        return;
    }
    if (m_sourceLostRequested) {
        finish(Outcome::SourceLost,
               QStringLiteral("The serial receiver disconnected or was replaced during a configuration write."));
        return;
    }
    if (!written) {
        finish(m_source && m_source->receiverSession() == session
                   ? Outcome::Rejected : Outcome::SourceLost,
               writeError.isEmpty()
                   ? QStringLiteral("The receiver configuration write failed.")
                   : writeError);
        return;
    }

    m_status = QStringLiteral("Submitting u-blox %1 configuration (%2/%3): %4")
        .arg(modeName(m_mode)).arg(m_submittedCommands)
        .arg(m_commands.size()).arg(command.description);
    emit stateChanged();
    if (!guard || !operationIsCurrent(operationId, session)) {
        return;
    }

    m_sequenceStep = SequenceStep::AfterWrite;
    if (command.delayAfterMs > 0) {
        m_sequenceTimer->start(command.delayAfterMs);
    } else {
        scheduleCurrentCommand();
    }
}

void UbloxBaseStationService::handleSequenceTimer()
{
    if (!m_busy || m_shuttingDown) {
        return;
    }
    if (m_sequenceStep == SequenceStep::BeforeWrite) {
        writeCurrentCommand();
    } else if (m_sequenceStep == SequenceStep::AfterWrite) {
        scheduleCurrentCommand();
    }
}

void UbloxBaseStationService::handleReceiverBytes(
    const QByteArray &bytes, quint64 session)
{
    if (m_shuttingDown || bytes.isEmpty() || session == 0
        || bytes.size() > MaximumReceiverChunkBytes
        || (session != m_operationSession
            && session != m_configuredSession)) {
        return;
    }

    bool changed = false;
    for (char byte : bytes) {
        UbloxBaseStationProtocol::Packet packet;
        if (!m_protocol.read(static_cast<quint8>(byte), &packet)) {
            continue;
        }
        if (packet.messageClass == 0x05
            && (packet.messageId == 0x00 || packet.messageId == 0x01)) {
            UbloxBaseStationProtocol::Acknowledgement acknowledgement;
            if (UbloxBaseStationProtocol::decodeAcknowledgement(
                    packet, &acknowledgement)) {
                m_acknowledgementStatus = acknowledgement.accepted
                    ? QStringLiteral("Receiver ACK %1/%2 accepted.")
                          .arg(acknowledgement.messageClass, 2, 16,
                               QLatin1Char('0'))
                          .arg(acknowledgement.messageId, 2, 16,
                               QLatin1Char('0'))
                    : QStringLiteral("Receiver NAK %1/%2 rejected.")
                          .arg(acknowledgement.messageClass, 2, 16,
                               QLatin1Char('0'))
                          .arg(acknowledgement.messageId, 2, 16,
                               QLatin1Char('0'));
                if (m_busy && session == m_operationSession) {
                    acknowledgement.accepted
                        ? ++m_acceptedAcknowledgements
                        : ++m_rejectedAcknowledgements;
                }
                changed = true;
            }
        } else if (packet.messageClass == 0x01
                   && packet.messageId == 0x3b) {
            UbloxBaseStationProtocol::SurveyIn survey;
            if (UbloxBaseStationProtocol::decodeSurveyIn(packet, &survey)) {
                m_surveyStatus = survey;
                changed = true;
            }
        } else if (packet.messageClass == 0x01
                   && packet.messageId == 0x07) {
            UbloxBaseStationProtocol::Position position;
            if (UbloxBaseStationProtocol::decodePosition(packet, &position)) {
                m_position = position;
                changed = true;
            }
        } else if (packet.messageClass == 0x0a
                   && packet.messageId == 0x04) {
            UbloxBaseStationProtocol::Version version;
            if (UbloxBaseStationProtocol::decodeVersion(packet, &version)) {
                m_status = QStringLiteral("u-blox receiver %1 (%2).")
                    .arg(version.software, version.hardware);
                changed = true;
            }
        }
    }
    if (changed) {
        emit stateChanged();
    }
}

void UbloxBaseStationService::handleSourceStateChanged()
{
    if (m_shuttingDown) {
        return;
    }
    const quint64 session = m_source ? m_source->receiverSession() : 0;
    const bool writable = m_source && m_source->canConfigureReceiver();
    if (m_busy
        && (!writable || session == 0 || session != m_operationSession)) {
        if (m_sequenceTimer) {
            m_sequenceTimer->stop();
        }
        if (m_writeInFlight) {
            m_sourceLostRequested = true;
            return;
        }
        finish(Outcome::SourceLost,
               QStringLiteral("The serial receiver disconnected or was replaced."));
        return;
    }
    if (m_configuredSession != 0
        && (!writable || session != m_configuredSession)) {
        m_configuredSession = 0;
        resetObservedState();
        m_status = writable
            ? QStringLiteral("A new serial receiver session is ready; configuration was not replayed.")
            : QStringLiteral("Serial correction receiver disconnected.");
        emit stateChanged();
        return;
    }
    if (!m_busy && m_configuredSession == 0) {
        m_status = writable
            ? QStringLiteral("Serial correction receiver is ready.")
            : QStringLiteral("Connect a serial correction receiver first.");
        emit stateChanged();
    }
}

bool UbloxBaseStationService::cancel(
    quint64 operationId, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!m_busy || operationId == 0 || operationId != m_operationId) {
        assignError(error,
                    QStringLiteral("The receiver configuration operation is no longer active."));
        return false;
    }
    m_cancelRequested = true;
    if (m_sequenceTimer) {
        m_sequenceTimer->stop();
    }
    if (!m_writeInFlight) {
        finish(Outcome::Cancelled,
               QStringLiteral("Receiver configuration was cancelled."));
    }
    return true;
}

void UbloxBaseStationService::stop()
{
    if (m_shuttingDown) {
        return;
    }
    m_configuredSession = 0;
    resetObservedState();
    if (m_busy) {
        cancel(m_operationId);
        return;
    }
    m_status = available()
        ? QStringLiteral("Serial correction receiver is ready.")
        : QStringLiteral("Connect a serial correction receiver first.");
    emit stateChanged();
}

void UbloxBaseStationService::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    if (m_sequenceTimer) {
        m_sequenceTimer->stop();
    }
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }
    m_commands.clear();
    m_commandIndex = 0;
    m_operationId = 0;
    m_operationSession = 0;
    m_configuredSession = 0;
    m_sequenceStep = SequenceStep::Idle;
    m_writeInFlight = false;
    m_cancelRequested = false;
    m_sourceLostRequested = false;
    m_busy = false;
    resetObservedState();
    m_status = QStringLiteral("u-blox base-station service is shut down.");
}

void UbloxBaseStationService::finish(
    Outcome outcome, const QString &description)
{
    if (!m_busy || m_finishing) {
        return;
    }
    const QPointer<UbloxBaseStationService> guard(this);
    m_finishing = true;
    if (m_sequenceTimer) {
        m_sequenceTimer->stop();
    }

    Report report;
    report.operationId = m_operationId;
    report.receiverSession = m_operationSession;
    report.mode = m_mode;
    report.outcome = outcome;
    report.totalCommands = m_commands.size();
    report.submittedCommands = m_submittedCommands;
    report.acceptedAcknowledgements = m_acceptedAcknowledgements;
    report.rejectedAcknowledgements = m_rejectedAcknowledgements;
    report.description = description;
    if (outcome == Outcome::Submitted && m_source
        && m_source->canConfigureReceiver()
        && m_source->receiverSession() == m_operationSession) {
        m_configuredSession = m_operationSession;
    } else {
        m_configuredSession = 0;
    }

    m_lastReport = report;
    m_status = description;
    m_commands.clear();
    m_commandIndex = 0;
    m_operationId = 0;
    m_operationSession = 0;
    m_sequenceStep = SequenceStep::Idle;
    m_writeInFlight = false;
    m_cancelRequested = false;
    m_sourceLostRequested = false;
    m_busy = false;

    emit stateChanged();
    if (!guard) {
        return;
    }
    emit operationFinished(report);
    if (!guard) {
        return;
    }
    m_finishing = false;
    emit stateChanged();
}

void UbloxBaseStationService::resetObservedState()
{
    m_protocol.reset();
    m_surveyStatus = {};
    m_position = {};
    m_acknowledgementStatus.clear();
}

bool UbloxBaseStationService::operationIsCurrent(
    quint64 operationId, quint64 receiverSession) const noexcept
{
    return m_busy && !m_shuttingDown
        && m_operationId == operationId
        && m_operationSession == receiverSession;
}
