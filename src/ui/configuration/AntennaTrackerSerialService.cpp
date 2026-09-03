#include "AntennaTrackerSerialService.h"

#include <QSerialPort>
#include <QTimer>

#include <cmath>
#include <utility>

// --- AntennaTrackerSerialTransport -----------------------------------------------------

AntennaTrackerSerialTransport::AntennaTrackerSerialTransport(QObject *parent)
    : QObject(parent)
{
}

AntennaTrackerSerialTransport::~AntennaTrackerSerialTransport() = default;

// --- AntennaTrackerSerialPortTransport -------------------------------------------------

AntennaTrackerSerialPortTransport::AntennaTrackerSerialPortTransport(QObject *parent)
    : AntennaTrackerSerialTransport(parent)
    , m_port(new QSerialPort(this))
{
    applyFraming();
    connect(m_port, &QSerialPort::bytesWritten, this,
            &AntennaTrackerSerialTransport::bytesWritten);
    // The tracker protocols never read; keep the read buffer from growing.
    connect(m_port, &QSerialPort::readyRead, this, [this]() { m_port->readAll(); });
    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
        // TimeoutError only comes from the blocking waitFor* calls, which the
        // service never uses; NoError is Qt clearing a previous error.
        if (m_muted || error == QSerialPort::NoError || error == QSerialPort::TimeoutError) {
            return;
        }
        QString message = m_port->errorString();
        if (message.isEmpty()) {
            message = tr("serial port error %1").arg(static_cast<int>(error));
        }
        emit errorOccurred(message);
    });
}

AntennaTrackerSerialPortTransport::~AntennaTrackerSerialPortTransport()
{
    m_muted = true;
    if (m_port->isOpen()) {
        m_port->close();
    }
}

bool AntennaTrackerSerialPortTransport::applyFraming()
{
    return m_port->setDataBits(QSerialPort::Data8) && m_port->setParity(QSerialPort::NoParity)
        && m_port->setStopBits(QSerialPort::OneStop)
        && m_port->setFlowControl(QSerialPort::NoFlowControl);
}

bool AntennaTrackerSerialPortTransport::open(const QString &portName, int baudRate,
                                             QString *error)
{
    if (m_port->isOpen()) {
        if (error) {
            *error = tr("serial port is already open");
        }
        return false;
    }
    m_muted = true;
    m_port->setPortName(portName);
    // Re-assert 8N1 in case a setup-time caller inspected and changed port().
    const bool opened = m_port->setBaudRate(baudRate) && applyFraming()
        && m_port->open(QIODevice::ReadWrite);
    if (!opened) {
        if (error) {
            *error = m_port->errorString();
            if (error->isEmpty()) {
                *error = tr("could not open %1").arg(portName);
            }
        }
        if (m_port->isOpen()) {
            m_port->close();
        }
        m_port->clearError();
        m_muted = false;
        return false;
    }
    m_port->clearError();
    m_muted = false;
    if (error) {
        error->clear();
    }
    return true;
}

void AntennaTrackerSerialPortTransport::close()
{
    if (!m_port->isOpen()) {
        return;
    }
    // Closing a disconnected USB adapter is best effort, as in MP10 Close().
    m_muted = true;
    m_port->close();
    m_port->clearError();
    m_muted = false;
}

bool AntennaTrackerSerialPortTransport::isOpen() const
{
    return m_port->isOpen();
}

bool AntennaTrackerSerialPortTransport::discardInput()
{
    return m_port->isOpen() && m_port->clear(QSerialPort::Input);
}

qint64 AntennaTrackerSerialPortTransport::write(const QByteArray &bytes)
{
    if (!m_port->isOpen()) {
        return -1;
    }
    return m_port->write(bytes);
}

qint64 AntennaTrackerSerialPortTransport::bytesToWrite() const
{
    return m_port->isOpen() ? m_port->bytesToWrite() : 0;
}

// --- AntennaTrackerSerialSettings ------------------------------------------------------

QStringList AntennaTrackerSerialSettings::Bauds()
{
    return {QStringLiteral("4800"),  QStringLiteral("9600"),  QStringLiteral("14400"),
            QStringLiteral("19200"), QStringLiteral("28800"), QStringLiteral("38400"),
            QStringLiteral("57600"), QStringLiteral("115200")};
}

QStringList AntennaTrackerSerialSettings::Interfaces()
{
    return AntennaTrackerOutputFactory::InterfaceNames();
}

QString AntennaTrackerSerialSettings::validate() const
{
    // MP10 Connect() order: port, baud (reported as a connection error),
    // interface, then every numeric field with its safe minimum.
    if (portName.trimmed().isEmpty()) {
        return AntennaTrackerSerialService::NoPortText();
    }
    if (baudRate < 1) {
        return AntennaTrackerSerialService::ErrorConnectingText(
            tr("%1 is below the safe minimum.").arg(tr("baud rate")));
    }
    if (!Interfaces().contains(interfaceName)) {
        return tr("Error selecting tracker interface: %1")
            .arg(AntennaTrackerOutputFactory::UnknownInterfaceText());
    }
    struct Field
    {
        int value;
        int minimum;
        QString name;
    };
    const Field fields[] = {
        {panRange, 1, tr("pan range")},
        {tiltRange, 1, tr("tilt range")},
        {panPwmRange, 1, tr("pan PWM range")},
        {tiltPwmRange, 1, tr("tilt PWM range")},
        {panPwmCenter, 1, tr("pan PWM center")},
        {tiltPwmCenter, 1, tr("tilt PWM center")},
        {panSpeed, 0, tr("pan speed")},
        {panAccel, 0, tr("pan acceleration")},
        {tiltSpeed, 0, tr("tilt speed")},
        {tiltAccel, 0, tr("tilt acceleration")},
    };
    for (const Field &field : fields) {
        if (field.value < field.minimum) {
            return tr("Invalid number entered: %1 is below the safe minimum.").arg(field.name);
        }
    }
    // MP10 accepts any double trim; the Qt codecs refuse non-finite angles, so
    // fail here with a clear message instead of a silent output failure.
    if (!std::isfinite(panTrim) || !std::isfinite(tiltTrim)) {
        return tr("Invalid number entered: trim must be a finite angle.");
    }
    return QString();
}

void AntennaTrackerSerialSettings::applyTo(IAntennaTrackerOutput &output) const
{
    // C# `panRange / 2 * -1` truncates toward zero before negating.
    output.setPanStartRange(panRange / 2 * -1);
    output.setPanEndRange(panRange / 2);
    output.setTrimPan(panTrim);
    output.setTiltStartRange(tiltRange / 2 * -1);
    output.setTiltEndRange(tiltRange / 2);
    output.setTrimTilt(tiltTrim);
    output.setPanReverse(panReverse);
    output.setTiltReverse(tiltReverse);
    output.setPanPwmRange(panPwmRange);
    output.setTiltPwmRange(tiltPwmRange);
    output.setPanPwmCenter(panPwmCenter);
    output.setTiltPwmCenter(tiltPwmCenter);
    output.setPanSpeed(panSpeed);
    output.setPanAccel(panAccel);
    output.setTiltSpeed(tiltSpeed);
    output.setTiltAccel(tiltAccel);
}

// --- AntennaTrackerSerialService: texts ------------------------------------------------

QString AntennaTrackerSerialService::DisconnectedText()
{
    return tr("Disconnected.");
}

QString AntennaTrackerSerialService::ConnectedText(const QString &interfaceName)
{
    return tr("Connected (%1).").arg(interfaceName);
}

QString AntennaTrackerSerialService::NoPortText()
{
    return tr("No serial port selected.");
}

QString AntennaTrackerSerialService::PortInUseText()
{
    return tr("Could not open port: selected serial port is already in use by a vehicle link.");
}

QString AntennaTrackerSerialService::ErrorConnectingText(const QString &reason)
{
    return tr("Error connecting: %1").arg(reason);
}

QString AntennaTrackerSerialService::SetupFailedText()
{
    return tr("Tracker setup failed.");
}

QString AntennaTrackerSerialService::SetupFailedText(const QString &reason)
{
    return tr("Tracker setup failed: %1").arg(reason);
}

QString AntennaTrackerSerialService::InitialTargetFailedText(const QString &reason)
{
    return tr("Failed to set initial pan and tilt: %1").arg(reason);
}

QString AntennaTrackerSerialService::SerialErrorText(const QString &reason)
{
    return tr("Tracker serial error: %1").arg(reason);
}

QString AntennaTrackerSerialService::WriteTimeoutText(int milliseconds)
{
    return tr("write timed out after %1 ms").arg(milliseconds);
}

QString AntennaTrackerSerialService::AlreadyRunningText()
{
    return tr("Antenna tracker is already connected.");
}

QString AntennaTrackerSerialService::NoTransportText()
{
    return ErrorConnectingText(tr("Serial port factory returned no port."));
}

// --- AntennaTrackerSerialService: lifecycle --------------------------------------------

AntennaTrackerSerialService::AntennaTrackerSerialService(QObject *parent)
    : QObject(parent)
    , m_transportFactory(DefaultTransportFactory())
    , m_writeTimer(new QTimer(this))
{
    qRegisterMetaType<AntennaTrackerSerialService::State>("AntennaTrackerSerialService::State");
    qRegisterMetaType<AntennaTrackerSerialSettings>("AntennaTrackerSerialSettings");
    m_writeTimer->setSingleShot(true);
    m_writeTimer->setInterval(m_writeTimeoutMs);
    connect(m_writeTimer, &QTimer::timeout, this, &AntennaTrackerSerialService::onWriteTimeout);
    m_status = DisconnectedText();
}

AntennaTrackerSerialService::~AntennaTrackerSerialService()
{
    // No signals from a dying object: receivers may already be half torn down.
    m_destroying = true;
    releaseTransport();
}

AntennaTrackerSerialService::TransportFactory AntennaTrackerSerialService::DefaultTransportFactory()
{
    return [](QObject *parent) -> AntennaTrackerSerialTransport * {
        return new AntennaTrackerSerialPortTransport(parent);
    };
}

void AntennaTrackerSerialService::setTransportFactory(TransportFactory factory)
{
    m_transportFactory = factory ? std::move(factory) : DefaultTransportFactory();
}

void AntennaTrackerSerialService::setPortOwnershipCheck(PortOwnershipCheck check)
{
    m_portOwnershipCheck = std::move(check);
}

void AntennaTrackerSerialService::setWriteTimeoutMs(int milliseconds)
{
    m_writeTimeoutMs = milliseconds;
    if (milliseconds > 0) {
        m_writeTimer->setInterval(milliseconds);
    }
    updateWriteTimer();
}

bool AntennaTrackerSerialService::isRunning() const
{
    return m_state == State::Connecting || m_state == State::Connected;
}

bool AntennaTrackerSerialService::isWriting() const
{
    return m_frameKind != Frame::None;
}

quint64 AntennaTrackerSerialService::connectToTracker(const AntennaTrackerSerialSettings &settings)
{
    if (isRunning()) {
        emit errorOccurred(m_generation, AlreadyRunningText());
        return 0;
    }
    const quint64 generation = ++m_generation;
    m_settings = settings;
    m_lastError.clear();
    m_writtenTargets = 0;
    m_droppedTargets = 0;
    m_rejectedTargets = 0;
    m_hasPendingTarget = false;

    QString error = settings.validate();
    if (!error.isEmpty()) {
        fail(error);
        return generation;
    }
    if (m_portOwnershipCheck && m_portOwnershipCheck(settings.portName)) {
        fail(PortInUseText());
        return generation;
    }

    m_output = AntennaTrackerOutputFactory::Create(settings.interfaceName);
    if (!m_output) {
        fail(tr("Error selecting tracker interface: %1")
                 .arg(AntennaTrackerOutputFactory::UnknownInterfaceText()));
        return generation;
    }
    settings.applyTo(*m_output);
    m_output->setWriter([this](const AntennaTrackerCommand &command) { return collect(command); });
    // MP10 Init(out error): range validation before the port is touched.
    if (!m_output->init(&error)) {
        fail(error);
        return generation;
    }

    setState(State::Connecting);
    if (m_generation != generation || m_state != State::Connecting) {
        return generation; // a receiver cancelled from inside stateChanged()
    }

    AntennaTrackerSerialTransport *transport =
        m_transportFactory ? m_transportFactory(this) : nullptr;
    if (!transport) {
        fail(NoTransportText());
        return generation;
    }
    transport->setParent(this);
    m_transport = transport;
    if (!transport->open(settings.portName, settings.baudRate, &error)) {
        fail(ErrorConnectingText(error));
        return generation;
    }
    connect(transport, &AntennaTrackerSerialTransport::bytesWritten, this,
            &AntennaTrackerSerialService::onBytesWritten);
    connect(transport, &AntennaTrackerSerialTransport::errorOccurred, this,
            &AntennaTrackerSerialService::onTransportError);

    // MP10 Setup(): Maestro speed/acceleration, nothing for the text protocols.
    m_collecting = true;
    m_frameCommands.clear();
    const bool prepared = m_output->setup();
    m_collecting = false;
    if (!prepared) {
        m_frameCommands.clear();
        fail(SetupFailedText());
        return generation;
    }
    m_frameKind = Frame::Setup;
    pump();
    return generation;
}

void AntennaTrackerSerialService::disconnectFromTracker()
{
    if (!isRunning()) {
        return;
    }
    const quint64 generation = m_generation;
    releaseTransport();
    setState(State::Disconnected);
    // stateChanged() is intentionally reentrant. A receiver may start another
    // connection synchronously; never overwrite that generation's status.
    if (m_generation == generation && m_state == State::Disconnected) {
        setStatus(DisconnectedText());
    }
}

void AntennaTrackerSerialService::cancel(quint64 generation)
{
    if (!isRunning() || (generation != 0 && generation != m_generation)) {
        return;
    }
    disconnectFromTracker();
}

bool AntennaTrackerSerialService::setTarget(double pan, double tilt)
{
    if (!isRunning() || !std::isfinite(pan) || !std::isfinite(tilt)) {
        return false;
    }
    if (m_state == State::Connected && m_frameKind == Frame::None) {
        if (!beginTargetFrame(pan, tilt, Frame::Target)) {
            ++m_rejectedTargets;
            return false;
        }
        return true;
    }
    // Latest target wins: one slot behind the frame in flight, never a queue.
    if (m_hasPendingTarget) {
        ++m_droppedTargets;
    }
    m_hasPendingTarget = true;
    m_pendingPan = pan;
    m_pendingTilt = tilt;
    return true;
}

bool AntennaTrackerSerialService::centerTracker()
{
    return setTarget(0.0, 0.0);
}

void AntennaTrackerSerialService::setTrim(double pan, double tilt)
{
    m_settings.panTrim = pan;
    m_settings.tiltTrim = tilt;
    if (m_output) {
        m_output->setTrimPan(pan);
        m_output->setTrimTilt(tilt);
    }
}

void AntennaTrackerSerialService::setReverse(bool pan, bool tilt)
{
    m_settings.panReverse = pan;
    m_settings.tiltReverse = tilt;
    if (m_output) {
        m_output->setPanReverse(pan);
        m_output->setTiltReverse(tilt);
    }
}

// --- AntennaTrackerSerialService: write pipeline ---------------------------------------

// IAntennaTrackerOutput writer boundary: commands are only accepted while the
// service is collecting one MP10 Setup()/PanAndTilt() frame.
bool AntennaTrackerSerialService::collect(const AntennaTrackerCommand &command)
{
    if (!m_collecting) {
        return false;
    }
    m_frameCommands.append(command);
    return true;
}

bool AntennaTrackerSerialService::beginTargetFrame(double pan, double tilt, Frame kind)
{
    if (!m_output || !m_transport || m_frameKind != Frame::None) {
        return false;
    }
    m_collecting = true;
    m_frameCommands.clear();
    const bool produced = m_output->panAndTilt(pan, tilt);
    m_collecting = false;
    if (!produced) {
        m_frameCommands.clear();
        return false;
    }
    m_frameKind = kind;
    m_framePan = pan;
    m_frameTilt = tilt;
    pump();
    return true;
}

void AntennaTrackerSerialService::startPendingTarget()
{
    if (!m_hasPendingTarget || m_state != State::Connected || m_frameKind != Frame::None) {
        return;
    }
    m_hasPendingTarget = false;
    if (!beginTargetFrame(m_pendingPan, m_pendingTilt, Frame::Target)) {
        ++m_rejectedTargets;
    }
}

void AntennaTrackerSerialService::pump()
{
    if (m_pumping) {
        m_pumpAgain = true; // re-entered from a transport signal or a receiver
        return;
    }
    m_pumping = true;
    do {
        m_pumpAgain = false;
        while (m_transport && m_frameKind != Frame::None) {
            if (!m_remaining.isEmpty()) {
                const QByteArray chunk = m_remaining;
                const qint64 accepted = m_transport->write(chunk);
                if (!m_transport || m_frameKind == Frame::None) {
                    break; // the write failed and was already handled re-entrantly
                }
                if (accepted < 0 || accepted > chunk.size()) {
                    failWithTransportError(tr("write failed"));
                    break;
                }
                if (accepted == 0) {
                    break; // transport full: resume on bytesWritten() or time out
                }
                m_remaining.remove(0, static_cast<int>(accepted));
                if (!m_remaining.isEmpty()) {
                    break; // partial write: resume on bytesWritten()
                }
            }
            if (m_transport->bytesToWrite() > 0) {
                break; // previous command still leaving the transport
            }
            if (m_frameCommands.isEmpty()) {
                finishFrame();
                continue;
            }
            const AntennaTrackerCommand command = m_frameCommands.takeFirst();
            if (command.discardInputFirst && !m_transport->discardInput()) {
                if (m_transport && m_frameKind != Frame::None) {
                    failWithTransportError(tr("could not discard pending input"));
                }
                break;
            }
            if (!m_transport || m_frameKind == Frame::None) {
                break;
            }
            m_remaining = command.payload;
        }
    } while (m_pumpAgain);
    m_pumping = false;
    updateWriteTimer();
}

void AntennaTrackerSerialService::finishFrame()
{
    const Frame finished = m_frameKind;
    m_frameKind = Frame::None;
    m_remaining.clear();
    switch (finished) {
    case Frame::None:
        break;
    case Frame::Setup:
        // MP10: PanAndTilt(0, 0) right after Setup(), before the loop starts.
        if (!beginTargetFrame(0.0, 0.0, Frame::InitialTarget)) {
            if (m_transport) {
                fail(InitialTargetFailedText(tr("the tracker output refused the command")));
            }
        }
        break;
    case Frame::InitialTarget:
        setState(State::Connected);
        if (m_state == State::Connected) {
            setStatus(ConnectedText(m_settings.interfaceName));
        }
        startPendingTarget();
        break;
    case Frame::Target: {
        ++m_writtenTargets;
        const quint64 generation = m_generation;
        if (!m_destroying) {
            emit targetWritten(generation, m_framePan, m_frameTilt);
        }
        if (m_generation == generation) {
            startPendingTarget();
        }
        break;
    }
    }
}

void AntennaTrackerSerialService::onBytesWritten(qint64 bytes)
{
    if (!m_transport) {
        return;
    }
    if (bytes > 0 && m_writeTimer->isActive()) {
        m_writeTimer->start(); // progress: restart the watchdog
    }
    pump();
}

void AntennaTrackerSerialService::onTransportError(const QString &message)
{
    if (!m_transport) {
        return;
    }
    failWithTransportError(message);
}

void AntennaTrackerSerialService::onWriteTimeout()
{
    if (!m_transport || m_frameKind == Frame::None) {
        return;
    }
    failWithTransportError(WriteTimeoutText(m_writeTimeoutMs));
}

void AntennaTrackerSerialService::failWithTransportError(const QString &reason)
{
    switch (m_frameKind) {
    case Frame::Setup:
        fail(SetupFailedText(reason));
        break;
    case Frame::InitialTarget:
        fail(InitialTargetFailedText(reason));
        break;
    case Frame::None:
    case Frame::Target:
        fail(SerialErrorText(reason));
        break;
    }
}

// Fail closed: MP10's loop swallows exceptions and keeps hammering the port at
// 10 Hz; the Qt port releases the port and reports once.
void AntennaTrackerSerialService::fail(const QString &message)
{
    const quint64 generation = m_generation;
    m_lastError = message;
    releaseTransport();
    setState(State::Failed);
    // A stateChanged(Failed) receiver may reconnect synchronously.
    if (m_generation != generation || m_state != State::Failed) {
        return;
    }
    setStatus(message);
    // statusChanged() is reentrant as well; suppress the obsolete error when
    // its receiver has already moved the service to a new generation.
    if (!m_destroying && m_generation == generation
        && m_state == State::Failed) {
        emit errorOccurred(generation, message);
    }
}

void AntennaTrackerSerialService::releaseTransport()
{
    m_writeTimer->stop();
    m_frameKind = Frame::None;
    m_frameCommands.clear();
    m_remaining.clear();
    m_collecting = false;
    m_hasPendingTarget = false;
    if (m_output) {
        m_output->close();
        m_output.reset();
    }
    AntennaTrackerSerialTransport *transport = m_transport;
    m_transport = nullptr;
    if (!transport) {
        return;
    }
    disconnect(transport, nullptr, this, nullptr);
    if (transport->isOpen()) {
        transport->close();
    }
    if (m_destroying) {
        delete transport;
    } else {
        // This may run inside one of the transport's own signals or write();
        // never delete it synchronously.
        transport->deleteLater();
    }
}

void AntennaTrackerSerialService::updateWriteTimer()
{
    const bool inFlight = m_transport && m_frameKind != Frame::None
        && (!m_remaining.isEmpty() || m_transport->bytesToWrite() > 0);
    if (!inFlight || m_writeTimeoutMs <= 0) {
        m_writeTimer->stop();
    } else if (!m_writeTimer->isActive()) {
        m_writeTimer->start();
    }
}

void AntennaTrackerSerialService::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    if (!m_destroying) {
        emit stateChanged(state, m_generation);
    }
}

void AntennaTrackerSerialService::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    if (!m_destroying) {
        emit statusChanged(status);
    }
}
