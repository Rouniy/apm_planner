#ifndef ANTENNATRACKERSERIALSERVICE_H
#define ANTENNATRACKERSERIALSERVICE_H

#include "AntennaTrackerOutputs.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QSerialPort;
class QTimer;

/*
 * Raw-serial service behind Mission Planner 10's `Antenna Tracker (Serial)`
 * and `Antenna Tracker (Live)` pages (AntennaTrackerUIViewModel Connect /
 * StartLoop / StopLoop / Dispose over ICommsSerial). The service owns one
 * dedicated serial transport, the selected IAntennaTrackerOutput codec and the
 * write pipeline between them. It never touches LinkManager: the view model
 * injects the "port already owned by the main MAVLink link" check.
 *
 * Threading: the service is a plain event-driven QObject. Every method must be
 * called on the thread that owns the service. Runtime operations are slots so
 * a view model may host the service in a worker thread and reach it through
 * queued invocations; factory/check/watchdog setters are setup-time methods.
 * Signals are queued-connection safe. The only blocking transport calls are
 * open() and close(), exactly like MP10's SerialPort.Open.
 *
 * Write pipeline: a "frame" is one MP10 Setup() or PanAndTilt() call, i.e. the
 * ordered list of commands the codec hands to the injected writer. Commands are
 * written one at a time; the next command is started only after the transport
 * reports the previous bytes drained, so a Maestro "discard input first" always
 * precedes its own command bytes. Partial acceptance by the transport keeps the
 * remainder in the service and resumes on bytesWritten(). At most one frame is
 * in flight and at most one target waits behind it: a newer target replaces the
 * waiting one (latest target wins), nothing is queued without bound. A write
 * that makes no progress within the write timeout fails the connection.
 */

// Serial boundary. The production implementation wraps one QSerialPort; unit
// tests inject a fake. Implementations report asynchronous failures through
// errorOccurred() and drained bytes through bytesWritten(); both may be emitted
// synchronously from inside write()/discardInput().
class AntennaTrackerSerialTransport : public QObject
{
    Q_OBJECT

public:
    explicit AntennaTrackerSerialTransport(QObject *parent = nullptr);
    ~AntennaTrackerSerialTransport() override;

    // Opens the device at baudRate with 8 data bits, no parity, one stop bit
    // and no flow control. On failure returns false and fills *error.
    virtual bool open(const QString &portName, int baudRate, QString *error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // MP10 Serial.DiscardInBuffer(): drops unread input; false on failure.
    virtual bool discardInput() = 0;
    // Queues bytes for transmission. Returns the number of bytes accepted
    // (0..bytes.size()) or -1 on failure; accepted bytes are reported back
    // through bytesWritten() once they leave the transport.
    virtual qint64 write(const QByteArray &bytes) = 0;
    // Bytes accepted by write() and not yet reported through bytesWritten().
    virtual qint64 bytesToWrite() const = 0;

signals:
    void bytesWritten(qint64 bytes);
    void errorOccurred(const QString &message);
};

// The dedicated QSerialPort transport: 8N1, no flow control, input discarded
// as it arrives (the tracker protocols never read). Errors raised while open()
// or close() run are returned synchronously instead of being signalled.
class AntennaTrackerSerialPortTransport final : public AntennaTrackerSerialTransport
{
    Q_OBJECT

public:
    explicit AntennaTrackerSerialPortTransport(QObject *parent = nullptr);
    ~AntennaTrackerSerialPortTransport() override;

    QSerialPort *port() const { return m_port; }

    bool open(const QString &portName, int baudRate, QString *error) override;
    void close() override;
    bool isOpen() const override;
    bool discardInput() override;
    qint64 write(const QByteArray &bytes) override;
    qint64 bytesToWrite() const override;

private:
    bool applyFraming();

    QSerialPort *m_port = nullptr;
    bool m_muted = false;
};

// One MP10 Connect() request: the page settings the view model collects.
// Defaults are the MP10 AntennaTrackerUIViewModel defaults.
struct AntennaTrackerSerialSettings
{
    Q_DECLARE_TR_FUNCTIONS(AntennaTrackerSerialSettings)

public:
    QString interfaceName = AntennaTrackerOutputFactory::Maestro();
    QString portName;
    int baudRate = 9600;

    int panRange = 360;
    int panPwmRange = 1000;
    int panPwmCenter = 1500;
    int panSpeed = 100;
    int panAccel = 5;
    double panTrim = 0.0;
    bool panReverse = false;

    int tiltRange = 90;
    int tiltPwmRange = 1000;
    int tiltPwmCenter = 1500;
    int tiltSpeed = 100;
    int tiltAccel = 5;
    double tiltTrim = 0.0;
    bool tiltReverse = false;

    // MP10 Bauds list and InterfaceNames, in MP10 order.
    static QStringList Bauds();
    static QStringList Interfaces();
    static int DefaultBaud() { return 9600; }

    // MP10 Connect() validation in MP10 order; empty when the request is usable.
    QString validate() const;
    // MP10 driver setup: +/- range/2 with C# integer division, trims, reverse,
    // PWM range/center, speed and acceleration.
    void applyTo(IAntennaTrackerOutput &output) const;
};

Q_DECLARE_METATYPE(AntennaTrackerSerialSettings)

class AntennaTrackerSerialService final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected, // MP10 "Connect" button, nothing owned
        Connecting,   // port open, Setup()/initial PanAndTilt(0, 0) in flight
        Connected,    // MP10 IsRunning with the loop accepting targets
        Failed        // fail-closed after an error; port released
    };
    Q_ENUM(State)

    // Creates the transport for one connection; the service takes ownership.
    using TransportFactory =
        std::function<AntennaTrackerSerialTransport *(QObject *parent)>;
    // True when the port is already owned by a main MAVLink connection. The
    // view model injects the LinkManager/SerialLinkInterface check.
    using PortOwnershipCheck = std::function<bool(const QString &portName)>;

    explicit AntennaTrackerSerialService(QObject *parent = nullptr);
    ~AntennaTrackerSerialService() override;

    static TransportFactory DefaultTransportFactory();

    void setTransportFactory(TransportFactory factory);
    void setPortOwnershipCheck(PortOwnershipCheck check);
    // Milliseconds without transmit progress before the connection fails;
    // zero or negative disables the watchdog.
    void setWriteTimeoutMs(int milliseconds);
    int writeTimeoutMs() const { return m_writeTimeoutMs; }

    State state() const { return m_state; }
    // MP10 IsRunning: Connecting or Connected.
    bool isRunning() const;
    // Increases with every connect request; signals carry it so a view model
    // can ignore late notifications from a previous connection.
    quint64 generation() const { return m_generation; }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    AntennaTrackerSerialSettings settings() const { return m_settings; }
    QString interfaceName() const { return m_settings.interfaceName; }

    bool hasPendingTarget() const { return m_hasPendingTarget; }
    bool isWriting() const;
    int writtenTargetCount() const { return m_writtenTargets; }
    int droppedTargetCount() const { return m_droppedTargets; }
    int rejectedTargetCount() const { return m_rejectedTargets; }

    // MP10 status texts (AntennaTrackerUIViewModel) and Qt-specific ones.
    static QString DisconnectedText();
    static QString ConnectedText(const QString &interfaceName);
    static QString NoPortText();
    static QString PortInUseText();
    static QString ErrorConnectingText(const QString &reason);
    static QString SetupFailedText();
    static QString SetupFailedText(const QString &reason);
    static QString InitialTargetFailedText(const QString &reason);
    static QString SerialErrorText(const QString &reason);
    static QString WriteTimeoutText(int milliseconds);
    static QString AlreadyRunningText();
    static QString NoTransportText();

public slots:
    // MP10 Connect(): validates, rejects a port owned by the main link, opens
    // the port, runs Setup() and PanAndTilt(0, 0), then reports Connected.
    // Returns the new generation, or 0 when a connection is already running.
    quint64 connectToTracker(const AntennaTrackerSerialSettings &settings);
    // MP10 Disconnect: stops the pipeline and releases the port.
    void disconnectFromTracker();
    // Cancels the running connection when generation matches (0 = any).
    void cancel(quint64 generation = 0);
    // MP10 loop tick: latest target wins while a frame is in flight. Returns
    // false when not running, the angles are not finite or the codec refused.
    bool setTarget(double pan, double tilt);
    // MP10 Home / Center.
    bool centerTracker();
    // MP10 live trim / reverse changes while connected.
    void setTrim(double pan, double tilt);
    void setReverse(bool pan, bool tilt);

signals:
    // State/status receivers may reconnect synchronously. Once that advances
    // the generation, the service suppresses the old transition's remaining
    // status/error signals; lastError() is available inside stateChanged.
    void stateChanged(AntennaTrackerSerialService::State state, quint64 generation);
    void statusChanged(const QString &status);
    void errorOccurred(quint64 generation, const QString &message);
    // The frame carrying these angles fully left the transport.
    void targetWritten(quint64 generation, double pan, double tilt);

private:
    enum class Frame { None, Setup, InitialTarget, Target };

    bool collect(const AntennaTrackerCommand &command);
    bool beginTargetFrame(double pan, double tilt, Frame kind);
    void startPendingTarget();
    void pump();
    void finishFrame();
    void onBytesWritten(qint64 bytes);
    void onTransportError(const QString &message);
    void onWriteTimeout();
    void failWithTransportError(const QString &reason);
    void fail(const QString &message);
    void releaseTransport();
    void updateWriteTimer();
    void setState(State state);
    void setStatus(const QString &status);

    TransportFactory m_transportFactory;
    PortOwnershipCheck m_portOwnershipCheck;
    AntennaTrackerSerialTransport *m_transport = nullptr;
    std::unique_ptr<IAntennaTrackerOutput> m_output;
    QTimer *m_writeTimer = nullptr;
    int m_writeTimeoutMs = 1000;

    State m_state = State::Disconnected;
    quint64 m_generation = 0;
    QString m_status;
    QString m_lastError;
    AntennaTrackerSerialSettings m_settings;

    Frame m_frameKind = Frame::None;
    QList<AntennaTrackerCommand> m_frameCommands;
    QByteArray m_remaining;
    double m_framePan = 0.0;
    double m_frameTilt = 0.0;
    bool m_collecting = false;
    bool m_pumping = false;
    bool m_pumpAgain = false;

    bool m_hasPendingTarget = false;
    double m_pendingPan = 0.0;
    double m_pendingTilt = 0.0;
    int m_writtenTargets = 0;
    int m_droppedTargets = 0;
    int m_rejectedTargets = 0;
    bool m_destroying = false;
};

#endif // ANTENNATRACKERSERIALSERVICE_H
