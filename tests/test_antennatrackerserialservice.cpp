#include <QtTest>

#include "ui/configuration/AntennaTrackerSerialService.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QSerialPort>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>

using State = AntennaTrackerSerialService::State;

namespace {

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray result;
    for (const int value : values) {
        result.append(static_cast<char>(value));
    }
    return result;
}

// MP10 default Maestro setup for speed 100 / acceleration 5, then the initial
// PanAndTilt(0, 0): tilt 1500 us * 4 = 6000 = 0x1770 -> 0x70, 0x2E; pan the same.
QByteArray maestroSetupBytes()
{
    return bytes({0x87, 0x00, 0x64, 0x00, 0x87, 0x01, 0x64, 0x00,
                  0x89, 0x00, 0x05, 0x00, 0x89, 0x01, 0x05, 0x00});
}

QByteArray maestroCenterBytes()
{
    return bytes({0x84, 0x01, 0x70, 0x2E, 0x84, 0x00, 0x70, 0x2E});
}

// Observations that outlive the transport object, which the service deletes.
struct FakeTransportLog
{
    QString portName;
    int baudRate = 0;
    int opens = 0;
    int closes = 0;
    int discards = 0;
    int writeCalls = 0;
    QByteArray wire;          // bytes accepted by write(), in order
    QList<int> discardOffsets; // wire.size() when each discard happened
    bool destroyed = false;
};

class FakeTransport final : public AntennaTrackerSerialTransport
{
public:
    FakeTransport(std::shared_ptr<FakeTransportLog> log, QObject *parent)
        : AntennaTrackerSerialTransport(parent)
        , m_log(std::move(log))
    {
    }

    ~FakeTransport() override { m_log->destroyed = true; }

    // knobs
    bool failOpen = false;
    QString openError = QStringLiteral("boom");
    int acceptLimit = -1;   // maximum bytes accepted per write() call
    bool autoDrain = true;  // report accepted bytes written from the event loop
    bool syncDrain = false; // report them from inside write() (re-entrancy)
    bool failWrites = false;

    bool open(const QString &portName, int baudRate, QString *error) override
    {
        ++m_log->opens;
        m_log->portName = portName;
        m_log->baudRate = baudRate;
        if (failOpen) {
            if (error) {
                *error = openError;
            }
            return false;
        }
        m_open = true;
        return true;
    }

    void close() override
    {
        if (m_open) {
            m_open = false;
            ++m_log->closes;
        }
    }

    bool isOpen() const override { return m_open; }

    bool discardInput() override
    {
        if (!m_open) {
            return false;
        }
        ++m_log->discards;
        m_log->discardOffsets.append(m_log->wire.size());
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        ++m_log->writeCalls;
        if (!m_open) {
            return -1;
        }
        if (failWrites) {
            emit errorOccurred(QStringLiteral("write failed"));
            return -1;
        }
        const int accepted = acceptLimit >= 0 ? qMin(acceptLimit, data.size()) : data.size();
        m_log->wire.append(data.left(accepted));
        m_pending += accepted;
        if (accepted > 0) {
            if (syncDrain) {
                drain();
            } else if (autoDrain) {
                QTimer::singleShot(0, this, [this]() { drain(); });
            }
        }
        return accepted;
    }

    qint64 bytesToWrite() const override { return m_pending; }

    // Test controls.
    void drain(qint64 count = -1)
    {
        const qint64 amount = count < 0 ? m_pending : qMin(count, m_pending);
        m_pending -= amount;
        if (amount > 0) {
            emit bytesWritten(amount);
        }
    }

    void unplug(const QString &message) { emit errorOccurred(message); }

private:
    std::shared_ptr<FakeTransportLog> m_log;
    bool m_open = false;
    qint64 m_pending = 0;
};

struct Fixture
{
    std::shared_ptr<FakeTransportLog> log = std::make_shared<FakeTransportLog>();
    QPointer<FakeTransport> transport;
    int factoryCalls = 0;
    std::function<void(FakeTransport *)> configure;

    AntennaTrackerSerialService::TransportFactory factory()
    {
        return [this](QObject *parent) -> AntennaTrackerSerialTransport * {
            ++factoryCalls;
            auto *created = new FakeTransport(log, parent);
            if (configure) {
                configure(created);
            }
            transport = created;
            return created;
        };
    }
};

class Recorder final : public QObject
{
public:
    QList<State> states;
    QList<quint64> generations;
    QStringList statuses;
    QStringList errors;
    QList<QPair<double, double>> targets;

    explicit Recorder(AntennaTrackerSerialService *service)
        : QObject(service)
    {
        connect(service, &AntennaTrackerSerialService::stateChanged, this,
                [this](State state, quint64 generation) {
            states.append(state);
            generations.append(generation);
        });
        connect(service, &AntennaTrackerSerialService::statusChanged, this,
                [this](const QString &status) { statuses.append(status); });
        connect(service, &AntennaTrackerSerialService::errorOccurred, this,
                [this](quint64, const QString &message) { errors.append(message); });
        connect(service, &AntennaTrackerSerialService::targetWritten, this,
                [this](quint64, double pan, double tilt) { targets.append({pan, tilt}); });
    }
};

AntennaTrackerSerialSettings settingsFor(const QString &interfaceName)
{
    AntennaTrackerSerialSettings settings;
    settings.interfaceName = interfaceName;
    settings.portName = QStringLiteral("/dev/ttyTRACKER0");
    return settings;
}

void flushDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

} // namespace

class AntennaTrackerSerialServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void settingsDefaultsValidationAndRanges();
    void metaTypesAreQueueable();
    void connectRunsMaestroSetupAndCentersBeforeConnected();
    void textProtocolsSkipSetupAndDiscard();
    void rejectsPortOwnedByMainLink();
    void rejectsInvalidRequestsWithMp10Texts();
    void openFailureReleasesTransport();
    void latestTargetWinsWhileAFrameIsInFlight();
    void partialWritesResumeInOrderWithDiscardBoundaries();
    void backpressureWithoutProgressTimesOut();
    void synchronousDrainIsReentrantSafe();
    void writeErrorsUseFrameSpecificTexts();
    void unplugFailsClosedAndAllowsReconnect();
    void cancelDuringConnectingAndReconnect();
    void disconnectDropsPendingTargetAndIgnoresLateEvents();
    void reentrantReconnectKeepsTheNewGenerationStatus();
    void disconnectInsideTargetWrittenIsSafe();
    void trimAndReverseApplyLive();
    void destructionClosesTransportSilently();
    void serialPortTransportUsesEightNoneOne();
    void runsInsideAWorkerThreadThroughQueuedSlots();
};

void AntennaTrackerSerialServiceTest::settingsDefaultsValidationAndRanges()
{
    AntennaTrackerSerialSettings settings;
    QCOMPARE(settings.interfaceName, QStringLiteral("Maestro"));
    QCOMPARE(settings.baudRate, 9600);
    QCOMPARE(settings.panRange, 360);
    QCOMPARE(settings.panPwmRange, 1000);
    QCOMPARE(settings.panPwmCenter, 1500);
    QCOMPARE(settings.panSpeed, 100);
    QCOMPARE(settings.panAccel, 5);
    QCOMPARE(settings.panTrim, 0.0);
    QVERIFY(!settings.panReverse);
    QCOMPARE(settings.tiltRange, 90);
    QCOMPARE(settings.tiltPwmRange, 1000);
    QCOMPARE(settings.tiltPwmCenter, 1500);
    QCOMPARE(settings.tiltSpeed, 100);
    QCOMPARE(settings.tiltAccel, 5);
    QCOMPARE(settings.tiltTrim, 0.0);
    QVERIFY(!settings.tiltReverse);
    QCOMPARE(AntennaTrackerSerialSettings::Bauds(),
             (QStringList{QStringLiteral("4800"), QStringLiteral("9600"),
                          QStringLiteral("14400"), QStringLiteral("19200"),
                          QStringLiteral("28800"), QStringLiteral("38400"),
                          QStringLiteral("57600"), QStringLiteral("115200")}));
    QCOMPARE(AntennaTrackerSerialSettings::Interfaces(),
             AntennaTrackerOutputFactory::InterfaceNames());
    QCOMPARE(AntennaTrackerSerialSettings::DefaultBaud(), 9600);

    QCOMPARE(settings.validate(), QStringLiteral("No serial port selected."));
    settings.portName = QStringLiteral("COM7");
    QCOMPARE(settings.validate(), QString());

    settings.baudRate = 0;
    QCOMPARE(settings.validate(),
             QStringLiteral("Error connecting: baud rate is below the safe minimum."));
    settings.baudRate = 9600;
    settings.interfaceName = QStringLiteral("Nope");
    QCOMPARE(settings.validate(),
             QStringLiteral("Error selecting tracker interface: "
                            "Unknown antenna tracker interface."));
    settings.interfaceName = QStringLiteral("Maestro");
    settings.panRange = 0;
    QCOMPARE(settings.validate(),
             QStringLiteral("Invalid number entered: pan range is below the safe minimum."));
    settings.panRange = 360;
    settings.tiltAccel = -1;
    QCOMPARE(settings.validate(),
             QStringLiteral("Invalid number entered: "
                            "tilt acceleration is below the safe minimum."));
    settings.tiltAccel = 0;
    QCOMPARE(settings.validate(), QString());
    settings.panTrim = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(settings.validate(),
             QStringLiteral("Invalid number entered: trim must be a finite angle."));
    settings.panTrim = 12.5;
    QCOMPARE(settings.validate(), QString());

    // MP10 driver setup: C# integer division on both halves of an odd range.
    settings.panRange = 361;
    settings.tiltRange = 91;
    settings.panReverse = true;
    settings.tiltTrim = -3.0;
    settings.panSpeed = 7;
    settings.tiltAccel = 9;
    MaestroAntennaTrackerOutput output;
    settings.applyTo(output);
    QCOMPARE(output.panStartRange(), -180);
    QCOMPARE(output.panEndRange(), 180);
    QCOMPARE(output.tiltStartRange(), -45);
    QCOMPARE(output.tiltEndRange(), 45);
    QCOMPARE(output.trimPan(), 12.5);
    QCOMPARE(output.trimTilt(), -3.0);
    QVERIFY(output.panReverse());
    QVERIFY(!output.tiltReverse());
    QCOMPARE(output.panPwmRange(), 1000);
    QCOMPARE(output.tiltPwmCenter(), 1500);
    QCOMPARE(output.panSpeed(), 7);
    QCOMPARE(output.tiltAccel(), 9);
}

void AntennaTrackerSerialServiceTest::metaTypesAreQueueable()
{
    AntennaTrackerSerialService service;
    QVERIFY(QMetaType::type("AntennaTrackerSerialService::State") != QMetaType::UnknownType);
    QVERIFY(QMetaType::type("AntennaTrackerSerialSettings") != QMetaType::UnknownType);
    QCOMPARE(service.state(), State::Disconnected);
    QVERIFY(!service.isRunning());
    QVERIFY(!service.isWriting());
    QCOMPARE(service.generation(), quint64(0));
    QCOMPARE(service.status(), QStringLiteral("Disconnected."));
    QCOMPARE(service.writeTimeoutMs(), 1000);
    QVERIFY(AntennaTrackerSerialService::DefaultTransportFactory());
    QVERIFY(!service.setTarget(0, 0));
    service.disconnectFromTracker();
    service.cancel();
    QCOMPARE(service.state(), State::Disconnected);
}

void AntennaTrackerSerialServiceTest::connectRunsMaestroSetupAndCentersBeforeConnected()
{
    Fixture fixture;
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    const quint64 generation = service.connectToTracker(settingsFor(QStringLiteral("Maestro")));
    QCOMPARE(generation, quint64(1));
    QCOMPARE(service.generation(), generation);
    QVERIFY(service.isRunning());
    QCOMPARE(fixture.factoryCalls, 1);
    QCOMPARE(fixture.log->portName, QStringLiteral("/dev/ttyTRACKER0"));
    QCOMPARE(fixture.log->baudRate, 9600);
    QCOMPARE(fixture.log->opens, 1);
    // The first setup command is handed over synchronously; the rest wait for
    // the transport to drain, so Connected is reached through the event loop.
    QCOMPARE(service.state(), State::Connecting);
    QVERIFY(service.isWriting());
    QCOMPARE(fixture.log->wire, maestroSetupBytes().left(4));

    QTRY_COMPARE(service.state(), State::Connected);
    QVERIFY(!service.isWriting());
    QCOMPARE(fixture.log->wire, maestroSetupBytes() + maestroCenterBytes());
    QCOMPARE(fixture.log->discards, 6);
    QCOMPARE(fixture.log->discardOffsets, (QList<int>{0, 4, 8, 12, 16, 20}));
    QCOMPARE(service.status(), QStringLiteral("Connected (Maestro)."));
    QCOMPARE(recorder.states, (QList<State>{State::Connecting, State::Connected}));
    QCOMPARE(recorder.generations, (QList<quint64>{1, 1}));
    QCOMPARE(recorder.statuses, QStringList{QStringLiteral("Connected (Maestro).")});
    QVERIFY(recorder.errors.isEmpty());
    QVERIFY(recorder.targets.isEmpty()); // the initial centre is part of connecting
    QCOMPARE(service.writtenTargetCount(), 0);

    // A second connect while running is refused without touching the port.
    QCOMPARE(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))), quint64(0));
    QCOMPARE(recorder.errors, QStringList{QStringLiteral("Antenna tracker is already connected.")});
    QCOMPARE(fixture.factoryCalls, 1);
    QCOMPARE(service.state(), State::Connected);

    // MP10 loop tick: tilt first, then pan (10, 10) -> 1611*4 and 1527*4.
    QVERIFY(service.setTarget(10, 10));
    QTRY_COMPARE(service.writtenTargetCount(), 1);
    QCOMPARE(fixture.log->wire.mid(24), bytes({0x84, 0x01, 0x2C, 0x32, 0x84, 0x00, 0x5C, 0x2F}));
    QCOMPARE(recorder.targets, (QList<QPair<double, double>>{{10.0, 10.0}}));

    service.disconnectFromTracker();
    QCOMPARE(service.state(), State::Disconnected);
    QCOMPARE(service.status(), QStringLiteral("Disconnected."));
    QCOMPARE(fixture.log->closes, 1);
    flushDeferredDeletes();
    QVERIFY(fixture.log->destroyed);
    QVERIFY(fixture.transport.isNull());
}

void AntennaTrackerSerialServiceTest::textProtocolsSkipSetupAndDiscard()
{
    Fixture fixture;
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("DegreeTracker"))) > 0);
    QTRY_COMPARE(service.state(), State::Connected);
    QCOMPARE(fixture.log->wire, QByteArrayLiteral("!!!PAN:0000,TLT:0000\n"));
    QCOMPARE(fixture.log->discards, 0);
    QCOMPARE(service.status(), QStringLiteral("Connected (DegreeTracker)."));
    QVERIFY(service.setTarget(12.34, -5.67));
    QTRY_COMPARE(service.writtenTargetCount(), 1);
    QCOMPARE(fixture.log->wire.mid(21), QByteArrayLiteral("!!!PAN:0123,TLT:-0056\n"));
    service.disconnectFromTracker();

    Fixture ardu;
    AntennaTrackerSerialService arduService;
    arduService.setTransportFactory(ardu.factory());
    QVERIFY(arduService.connectToTracker(settingsFor(QStringLiteral("ArduTracker"))) > 0);
    QTRY_COMPARE(arduService.state(), State::Connected);
    QCOMPARE(ardu.log->wire, QByteArrayLiteral("!!!PAN:1500,TLT:1500\n"));
    QCOMPARE(ardu.log->discards, 0);
}

void AntennaTrackerSerialServiceTest::rejectsPortOwnedByMainLink()
{
    Fixture fixture;
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);
    QStringList queried;
    bool owned = true;
    service.setPortOwnershipCheck([&queried, &owned](const QString &portName) {
        queried.append(portName);
        return owned;
    });

    const quint64 generation = service.connectToTracker(settingsFor(QStringLiteral("Maestro")));
    QCOMPARE(generation, quint64(1));
    QCOMPARE(queried, QStringList{QStringLiteral("/dev/ttyTRACKER0")});
    QCOMPARE(service.state(), State::Failed);
    QVERIFY(!service.isRunning());
    QCOMPARE(service.status(),
             QStringLiteral("Could not open port: selected serial port is already in use by a "
                            "vehicle link."));
    QCOMPARE(service.lastError(), service.status());
    QCOMPARE(recorder.errors, QStringList{service.status()});
    QCOMPARE(recorder.states, QList<State>{State::Failed});
    QCOMPARE(fixture.factoryCalls, 0); // the port is never opened

    // Released by the main link: the same request now connects.
    owned = false;
    QCOMPARE(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))), quint64(2));
    QCOMPARE(queried.size(), 2);
    QCOMPARE(fixture.factoryCalls, 1);
    QTRY_COMPARE(service.state(), State::Connected);
    QCOMPARE(recorder.generations.last(), quint64(2));
}

void AntennaTrackerSerialServiceTest::rejectsInvalidRequestsWithMp10Texts()
{
    Fixture fixture;
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    AntennaTrackerSerialSettings settings = settingsFor(QStringLiteral("Maestro"));
    settings.portName.clear();
    QCOMPARE(service.connectToTracker(settings), quint64(1));
    QCOMPARE(service.state(), State::Failed);
    QCOMPARE(service.status(), QStringLiteral("No serial port selected."));

    settings = settingsFor(QStringLiteral("Unknown"));
    QCOMPARE(service.connectToTracker(settings), quint64(2));
    QCOMPARE(service.status(),
             QStringLiteral("Error selecting tracker interface: "
                            "Unknown antenna tracker interface."));

    settings = settingsFor(QStringLiteral("Maestro"));
    settings.tiltPwmCenter = 0;
    QCOMPARE(service.connectToTracker(settings), quint64(3));
    QCOMPARE(service.status(),
             QStringLiteral("Invalid number entered: tilt PWM center is below the safe minimum."));

    // A range of 1 degree passes the MP10 minimum but collapses to 0..0, which
    // the codec's Init() reports before the port is opened.
    settings = settingsFor(QStringLiteral("ArduTracker"));
    settings.tiltRange = 1;
    QCOMPARE(service.connectToTracker(settings), quint64(4));
    QCOMPARE(service.status(), QStringLiteral("Invalid tilt range."));

    QCOMPARE(fixture.factoryCalls, 0);
    QCOMPARE(recorder.errors.size(), 4);
    QCOMPARE(recorder.states, QList<State>{State::Failed}); // Failed only once, no flapping
    QCOMPARE(service.generation(), quint64(4));

    // Targets and trims are ignored while nothing is connected.
    QVERIFY(!service.setTarget(1, 2));
    QVERIFY(!service.centerTracker());
    service.setTrim(5, 6);
    service.setReverse(true, true);
    QCOMPARE(service.settings().panTrim, 5.0);
    QVERIFY(service.settings().tiltReverse);
}

void AntennaTrackerSerialServiceTest::openFailureReleasesTransport()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) {
        transport->failOpen = true;
        transport->openError = QStringLiteral("Permission denied");
    };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    QCOMPARE(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))), quint64(1));
    QCOMPARE(service.state(), State::Failed);
    QCOMPARE(service.status(), QStringLiteral("Error connecting: Permission denied"));
    QCOMPARE(recorder.states, (QList<State>{State::Connecting, State::Failed}));
    QCOMPARE(recorder.errors, QStringList{service.status()});
    QCOMPARE(fixture.log->opens, 1);
    QCOMPARE(fixture.log->closes, 0);
    QVERIFY(fixture.log->wire.isEmpty());
    flushDeferredDeletes();
    QVERIFY(fixture.log->destroyed);

    // A factory that returns nothing is reported instead of dereferenced.
    service.setTransportFactory([](QObject *) -> AntennaTrackerSerialTransport * {
        return nullptr;
    });
    QCOMPARE(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))), quint64(2));
    QCOMPARE(service.status(),
             QStringLiteral("Error connecting: Serial port factory returned no port."));
    QCOMPARE(service.state(), State::Failed);
}

void AntennaTrackerSerialServiceTest::latestTargetWinsWhileAFrameIsInFlight()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("DegreeTracker"))) > 0);
    QCOMPARE(service.state(), State::Connecting);
    // Targets sent while connecting wait behind the initial centre; only the
    // newest survives.
    QVERIFY(service.setTarget(1, 1));
    QVERIFY(service.setTarget(2, 2));
    QVERIFY(service.hasPendingTarget());
    QCOMPARE(service.droppedTargetCount(), 1);
    QCOMPARE(fixture.log->wire, QByteArrayLiteral("!!!PAN:0000,TLT:0000\n"));

    fixture.transport->drain();
    QCOMPARE(service.state(), State::Connected);
    QVERIFY(!service.hasPendingTarget());
    QVERIFY(service.isWriting()); // the surviving target went out right away
    QCOMPARE(fixture.log->wire.mid(21), QByteArrayLiteral("!!!PAN:0020,TLT:0020\n"));
    fixture.transport->drain();
    QVERIFY(!service.isWriting());
    QCOMPARE(recorder.targets, (QList<QPair<double, double>>{{2.0, 2.0}}));
    QCOMPARE(service.writtenTargetCount(), 1);

    // One frame in flight, three more ticks: the middle ones are dropped.
    QVERIFY(service.setTarget(10, 10));
    QCOMPARE(fixture.log->wire.mid(42), QByteArrayLiteral("!!!PAN:0100,TLT:0100\n"));
    QVERIFY(service.setTarget(20, 20));
    QVERIFY(service.setTarget(30, 30));
    QCOMPARE(service.droppedTargetCount(), 2);
    QCOMPARE(fixture.log->writeCalls, 3);
    QCOMPARE(fixture.log->wire.size(), 63);
    fixture.transport->drain();
    QCOMPARE(recorder.targets, (QList<QPair<double, double>>{{2.0, 2.0}, {10.0, 10.0}}));
    QVERIFY(!service.hasPendingTarget());
    QVERIFY(service.isWriting());
    QCOMPARE(fixture.log->wire.mid(63), QByteArrayLiteral("!!!PAN:0300,TLT:0300\n"));
    fixture.transport->drain();
    QCOMPARE(fixture.log->wire.size(), 84);
    QCOMPARE(service.writtenTargetCount(), 3);
    QVERIFY(!service.isWriting());
    QCOMPARE(recorder.targets.last(), (QPair<double, double>{30.0, 30.0}));

    // Non-finite angles never reach the codec.
    QVERIFY(!service.setTarget(std::nan(""), 0));
    QVERIFY(!service.setTarget(0, std::numeric_limits<double>::infinity()));
    QCOMPARE(fixture.log->wire.size(), 84);
    QVERIFY(!service.isWriting());
}

void AntennaTrackerSerialServiceTest::partialWritesResumeInOrderWithDiscardBoundaries()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) {
        transport->autoDrain = false;
        transport->acceptLimit = 1;
    };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QCOMPARE(fixture.log->wire.size(), 1);
    QCOMPARE(fixture.log->discardOffsets, QList<int>{0});

    // Each drained byte lets exactly one more byte through; the next command
    // (and its discard) starts only after the previous one fully drained.
    for (int expected = 2; expected <= 4; ++expected) {
        fixture.transport->drain();
        QCOMPARE(fixture.log->wire.size(), expected);
        QCOMPARE(fixture.log->discards, 1);
    }
    fixture.transport->drain();
    QCOMPARE(fixture.log->discards, 2);
    QCOMPARE(fixture.log->wire.size(), 5);
    QCOMPARE(service.state(), State::Connecting);

    while (service.isWriting()) {
        fixture.transport->drain();
    }
    QCOMPARE(service.state(), State::Connected);
    QCOMPARE(fixture.log->wire, maestroSetupBytes() + maestroCenterBytes());
    QCOMPARE(fixture.log->discardOffsets, (QList<int>{0, 4, 8, 12, 16, 20}));
    QCOMPARE(fixture.log->writeCalls, 24);

    // Automatic draining with the same one-byte transport converges as well.
    fixture.transport->autoDrain = true;
    QVERIFY(service.setTarget(10, 10));
    QTRY_COMPARE(service.writtenTargetCount(), 1);
    QCOMPARE(fixture.log->wire.mid(24), bytes({0x84, 0x01, 0x2C, 0x32, 0x84, 0x00, 0x5C, 0x2F}));
    QCOMPARE(fixture.log->discardOffsets.size(), 8);
}

void AntennaTrackerSerialServiceTest::backpressureWithoutProgressTimesOut()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    service.setWriteTimeoutMs(40);
    QCOMPARE(service.writeTimeoutMs(), 40);
    Recorder recorder(&service);

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QCOMPARE(service.state(), State::Connecting);
    QTRY_COMPARE(service.state(), State::Failed);
    QCOMPARE(service.status(), QStringLiteral("Tracker setup failed: write timed out after 40 ms"));
    QCOMPARE(recorder.errors, QStringList{service.status()});
    QCOMPARE(fixture.log->closes, 1);
    QCOMPARE(fixture.log->wire.size(), 4); // nothing more was pushed into a stuck port

    // A transport that accepts nothing at all is also caught by the watchdog.
    Fixture stuck;
    stuck.configure = [](FakeTransport *transport) {
        transport->autoDrain = false;
        transport->acceptLimit = 0;
    };
    AntennaTrackerSerialService stuckService;
    stuckService.setTransportFactory(stuck.factory());
    stuckService.setWriteTimeoutMs(40);
    QVERIFY(stuckService.connectToTracker(settingsFor(QStringLiteral("DegreeTracker"))) > 0);
    QTRY_COMPARE(stuckService.state(), State::Failed);
    QCOMPARE(stuckService.status(),
             QStringLiteral("Failed to set initial pan and tilt: write timed out after 40 ms"));
    QVERIFY(stuck.log->wire.isEmpty());

    // Progress restarts the watchdog: a slow but live device stays connected.
    Fixture slow;
    slow.configure = [](FakeTransport *transport) {
        transport->autoDrain = false;
        transport->acceptLimit = 1;
    };
    AntennaTrackerSerialService slowService;
    slowService.setTransportFactory(slow.factory());
    slowService.setWriteTimeoutMs(500);
    QVERIFY(slowService.connectToTracker(settingsFor(QStringLiteral("DegreeTracker"))) > 0);
    // Invariant: total elapsed > timeout > every individual progress gap.
    for (int step = 0; step < 10; ++step) {
        QTest::qWait(100);
        slow.transport->drain();
    }
    QCOMPARE(slowService.state(), State::Connecting);
    while (slowService.isWriting()) {
        slow.transport->drain();
    }
    QCOMPARE(slowService.state(), State::Connected);
    QTest::qWait(600); // idle pipeline: the watchdog is not armed
    QCOMPARE(slowService.state(), State::Connected);

    // Disabling the watchdog keeps a stalled connection open indefinitely.
    Fixture none;
    none.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    AntennaTrackerSerialService noneService;
    noneService.setTransportFactory(none.factory());
    noneService.setWriteTimeoutMs(40);
    noneService.setWriteTimeoutMs(0);
    QVERIFY(noneService.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QTest::qWait(100);
    QCOMPARE(noneService.state(), State::Connecting);
}

void AntennaTrackerSerialServiceTest::synchronousDrainIsReentrantSafe()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->syncDrain = true; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    // bytesWritten() arrives from inside write(): the whole connect completes
    // synchronously without recursion.
    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QCOMPARE(service.state(), State::Connected);
    QCOMPARE(fixture.log->wire, maestroSetupBytes() + maestroCenterBytes());
    QCOMPARE(fixture.log->discardOffsets, (QList<int>{0, 4, 8, 12, 16, 20}));
    QVERIFY(service.setTarget(10, 10));
    QCOMPARE(service.writtenTargetCount(), 1);
    QVERIFY(!service.isWriting());

    // A receiver that reacts to a written target inside the signal keeps the
    // pipeline consistent.
    int chained = 0;
    connect(&service, &AntennaTrackerSerialService::targetWritten, &service,
            [&service, &chained](quint64, double pan, double) {
        if (chained++ == 0) {
            service.setTarget(pan + 1, 0);
        }
    });
    QVERIFY(service.setTarget(20, 0));
    QCOMPARE(service.writtenTargetCount(), 3);
    QCOMPARE(recorder.targets.last(), (QPair<double, double>{21.0, 0.0}));
    QVERIFY(!service.isWriting());
    QVERIFY(!service.hasPendingTarget());
}

void AntennaTrackerSerialServiceTest::writeErrorsUseFrameSpecificTexts()
{
    Fixture setup;
    setup.configure = [](FakeTransport *transport) { transport->failWrites = true; };
    AntennaTrackerSerialService setupService;
    setupService.setTransportFactory(setup.factory());
    Recorder setupRecorder(&setupService);
    QVERIFY(setupService.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QCOMPARE(setupService.state(), State::Failed);
    QCOMPARE(setupService.status(), QStringLiteral("Tracker setup failed: write failed"));
    QCOMPARE(setupRecorder.states, (QList<State>{State::Connecting, State::Failed}));
    QCOMPARE(setupRecorder.errors.size(), 1); // the synchronous -1 is not reported twice
    QCOMPARE(setup.log->closes, 1);

    Fixture initial;
    initial.configure = [](FakeTransport *transport) { transport->failWrites = true; };
    AntennaTrackerSerialService initialService;
    initialService.setTransportFactory(initial.factory());
    QVERIFY(initialService.connectToTracker(settingsFor(QStringLiteral("ArduTracker"))) > 0);
    QCOMPARE(initialService.status(),
             QStringLiteral("Failed to set initial pan and tilt: write failed"));

    Fixture running;
    AntennaTrackerSerialService runningService;
    runningService.setTransportFactory(running.factory());
    Recorder runningRecorder(&runningService);
    QVERIFY(runningService.connectToTracker(settingsFor(QStringLiteral("ArduTracker"))) > 0);
    QTRY_COMPARE(runningService.state(), State::Connected);
    running.transport->failWrites = true;
    QVERIFY(runningService.setTarget(1, 1)); // accepted, then the port fails
    QCOMPARE(runningService.state(), State::Failed);
    QCOMPARE(runningService.status(), QStringLiteral("Tracker serial error: write failed"));
    QCOMPARE(runningRecorder.errors.size(), 1);
    QVERIFY(!runningService.setTarget(2, 2));
}

void AntennaTrackerSerialServiceTest::unplugFailsClosedAndAllowsReconnect()
{
    Fixture fixture;
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QTRY_COMPARE(service.state(), State::Connected);
    QVERIFY(service.setTarget(5, 5));
    QVERIFY(service.setTarget(6, 6)); // pending behind the frame in flight
    QVERIFY(service.hasPendingTarget());

    fixture.transport->unplug(QStringLiteral("Resource error"));
    QCOMPARE(service.state(), State::Failed);
    QVERIFY(!service.isRunning());
    QVERIFY(!service.isWriting());
    QVERIFY(!service.hasPendingTarget());
    QCOMPARE(service.status(), QStringLiteral("Tracker serial error: Resource error"));
    QCOMPARE(service.lastError(), service.status());
    QCOMPARE(recorder.errors, QStringList{service.status()});
    QCOMPARE(recorder.states, (QList<State>{State::Connecting, State::Connected, State::Failed}));
    QCOMPARE(fixture.log->closes, 1);
    flushDeferredDeletes();
    QVERIFY(fixture.log->destroyed);
    QVERIFY(!service.setTarget(7, 7));
    QCOMPARE(recorder.targets.size(), 0);

    // The user can reconnect after replugging; late events from the dead
    // transport are impossible because it is gone.
    const int errorsBefore = recorder.errors.size();
    QCOMPARE(service.connectToTracker(settingsFor(QStringLiteral("Maestro"))), quint64(2));
    QTRY_COMPARE(service.state(), State::Connected);
    QCOMPARE(fixture.log->opens, 2);
    QCOMPARE(recorder.errors.size(), errorsBefore);
    QCOMPARE(recorder.generations.last(), quint64(2));
    QCOMPARE(service.writtenTargetCount(), 0);
    QCOMPARE(service.droppedTargetCount(), 0);
}

void AntennaTrackerSerialServiceTest::cancelDuringConnectingAndReconnect()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    const quint64 first = service.connectToTracker(settingsFor(QStringLiteral("Maestro")));
    QCOMPARE(service.state(), State::Connecting);
    QVERIFY(service.setTarget(3, 3));

    service.cancel(first + 1); // a stale or foreign generation is ignored
    QCOMPARE(service.state(), State::Connecting);
    service.cancel(first);
    QCOMPARE(service.state(), State::Disconnected);
    QCOMPARE(service.status(), QStringLiteral("Disconnected."));
    QVERIFY(!service.isWriting());
    QVERIFY(!service.hasPendingTarget());
    QCOMPARE(fixture.log->closes, 1);
    QCOMPARE(fixture.log->wire.size(), 4);
    QVERIFY(recorder.errors.isEmpty());
    flushDeferredDeletes();
    QVERIFY(fixture.log->destroyed);

    // The abandoned transport can no longer feed the service.
    const quint64 second = service.connectToTracker(settingsFor(QStringLiteral("Maestro")));
    QCOMPARE(second, first + 1);
    QCOMPARE(fixture.factoryCalls, 2);
    QCOMPARE(service.state(), State::Connecting);
    while (service.isWriting()) {
        fixture.transport->drain();
    }
    QCOMPARE(service.state(), State::Connected);
    QCOMPARE(recorder.states,
             (QList<State>{State::Connecting, State::Disconnected, State::Connecting,
                           State::Connected}));
    QCOMPARE(recorder.generations, (QList<quint64>{first, first, second, second}));

    // cancel() with the default generation stops whatever is running.
    service.cancel();
    QCOMPARE(service.state(), State::Disconnected);
    QCOMPARE(fixture.log->closes, 2);
}

void AntennaTrackerSerialServiceTest::disconnectDropsPendingTargetAndIgnoresLateEvents()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    Recorder recorder(&service);

    QVERIFY(service.connectToTracker(settingsFor(QStringLiteral("DegreeTracker"))) > 0);
    fixture.transport->drain();
    QCOMPARE(service.state(), State::Connected);
    QVERIFY(service.setTarget(1, 1));
    QVERIFY(service.setTarget(2, 2));
    QVERIFY(service.hasPendingTarget());

    QPointer<FakeTransport> old = fixture.transport;
    service.disconnectFromTracker();
    QCOMPARE(service.state(), State::Disconnected);
    QVERIFY(!service.hasPendingTarget());
    QCOMPARE(fixture.log->wire.size(), 42); // centre + target 1 only
    QVERIFY(!old.isNull()); // deferred, never deleted from inside a callback

    // Events from the released transport are not delivered any more.
    old->drain();
    old->unplug(QStringLiteral("late"));
    QCOMPARE(service.state(), State::Disconnected);
    QVERIFY(recorder.errors.isEmpty());
    QCOMPARE(recorder.targets.size(), 0);
    flushDeferredDeletes();
    QVERIFY(old.isNull());
    QVERIFY(fixture.log->destroyed);

    // A receiver that disconnects from inside stateChanged(Connecting) wins.
    Fixture eager;
    AntennaTrackerSerialService eagerService;
    eagerService.setTransportFactory(eager.factory());
    connect(&eagerService, &AntennaTrackerSerialService::stateChanged, &eagerService,
            [&eagerService](State state, quint64) {
        if (state == State::Connecting) {
            eagerService.disconnectFromTracker();
        }
    });
    QVERIFY(eagerService.connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QCOMPARE(eagerService.state(), State::Disconnected);
    QCOMPARE(eager.factoryCalls, 0);
}

void AntennaTrackerSerialServiceTest::reentrantReconnectKeepsTheNewGenerationStatus()
{
    // A receiver may reconnect synchronously from Disconnected. The old
    // disconnect call must not restore "Disconnected." over the new status.
    Fixture disconnected;
    disconnected.configure = [](FakeTransport *transport) {
        transport->syncDrain = true;
    };
    AntennaTrackerSerialService disconnectedService;
    disconnectedService.setTransportFactory(disconnected.factory());
    QVERIFY(disconnectedService.connectToTracker(
                settingsFor(QStringLiteral("Maestro"))) > 0);
    quint64 disconnectedReconnectGeneration = 0;
    connect(&disconnectedService,
            &AntennaTrackerSerialService::stateChanged,
            &disconnectedService,
            [&disconnectedService, &disconnectedReconnectGeneration](
                    State state, quint64) {
        if (state == State::Disconnected
            && disconnectedReconnectGeneration == 0) {
            disconnectedReconnectGeneration =
                disconnectedService.connectToTracker(
                    settingsFor(QStringLiteral("DegreeTracker")));
        }
    });
    disconnectedService.disconnectFromTracker();
    QCOMPARE(disconnectedReconnectGeneration, quint64(2));
    QCOMPARE(disconnectedService.state(), State::Connected);
    QCOMPARE(disconnectedService.generation(), quint64(2));
    QCOMPARE(disconnectedService.status(),
             QStringLiteral("Connected (DegreeTracker)."));

    // The same guard applies to stateChanged(Failed). An error belonging to
    // generation 1 must not overwrite or signal after generation 2 connects.
    Fixture failed;
    failed.configure = [](FakeTransport *transport) {
        transport->syncDrain = true;
    };
    AntennaTrackerSerialService failedService;
    failedService.setTransportFactory(failed.factory());
    Recorder recorder(&failedService);
    QVERIFY(failedService.connectToTracker(
                settingsFor(QStringLiteral("Maestro"))) > 0);
    QPointer<FakeTransport> oldTransport = failed.transport;
    quint64 failedReconnectGeneration = 0;
    connect(&failedService, &AntennaTrackerSerialService::stateChanged,
            &failedService,
            [&failedService, &failedReconnectGeneration](State state, quint64) {
        if (state == State::Failed && failedReconnectGeneration == 0) {
            failedReconnectGeneration = failedService.connectToTracker(
                settingsFor(QStringLiteral("ArduTracker")));
        }
    });
    QVERIFY(oldTransport);
    oldTransport->unplug(QStringLiteral("old generation unplugged"));
    QCOMPARE(failedReconnectGeneration, quint64(2));
    QCOMPARE(failedService.state(), State::Connected);
    QCOMPARE(failedService.generation(), quint64(2));
    QCOMPARE(failedService.status(),
             QStringLiteral("Connected (ArduTracker)."));
    QCOMPARE(failedService.lastError(), QString());
    QVERIFY(recorder.errors.isEmpty());

    // Reconnecting from statusChanged() likewise suppresses the obsolete
    // errorOccurred signal that would otherwise arrive after generation 2.
    Fixture statusChanged;
    statusChanged.configure = [](FakeTransport *transport) {
        transport->syncDrain = true;
    };
    AntennaTrackerSerialService statusChangedService;
    statusChangedService.setTransportFactory(statusChanged.factory());
    Recorder statusChangedRecorder(&statusChangedService);
    QVERIFY(statusChangedService.connectToTracker(
                settingsFor(QStringLiteral("Maestro"))) > 0);
    QPointer<FakeTransport> statusChangedOldTransport = statusChanged.transport;
    quint64 statusChangedReconnectGeneration = 0;
    connect(&statusChangedService,
            &AntennaTrackerSerialService::statusChanged,
            &statusChangedService,
            [&statusChangedService, &statusChangedReconnectGeneration](
                    const QString &status) {
        if (status.startsWith(QStringLiteral("Tracker serial error:"))
            && statusChangedReconnectGeneration == 0) {
            statusChangedReconnectGeneration =
                statusChangedService.connectToTracker(
                    settingsFor(QStringLiteral("DegreeTracker")));
        }
    });
    QVERIFY(statusChangedOldTransport);
    statusChangedOldTransport->unplug(
        QStringLiteral("old status generation unplugged"));
    QCOMPARE(statusChangedReconnectGeneration, quint64(2));
    QCOMPARE(statusChangedService.state(), State::Connected);
    QCOMPARE(statusChangedService.status(),
             QStringLiteral("Connected (DegreeTracker)."));
    QCOMPARE(statusChangedService.lastError(), QString());
    QVERIFY(statusChangedRecorder.errors.isEmpty());
}

void AntennaTrackerSerialServiceTest::disconnectInsideTargetWrittenIsSafe()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) {
        transport->syncDrain = true;
    };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());
    QVERIFY(service.connectToTracker(
                settingsFor(QStringLiteral("DegreeTracker"))) > 0);

    int writtenSignals = 0;
    connect(&service, &AntennaTrackerSerialService::targetWritten,
            &service, [&service, &writtenSignals](quint64, double, double) {
        ++writtenSignals;
        service.disconnectFromTracker();
    });
    QVERIFY(service.setTarget(12.0, -3.0));
    QCOMPARE(writtenSignals, 1);
    QCOMPARE(service.state(), State::Disconnected);
    QCOMPARE(service.status(), QStringLiteral("Disconnected."));
    QVERIFY(!service.isWriting());
    QVERIFY(!service.hasPendingTarget());
    QCOMPARE(fixture.log->closes, 1);
}

void AntennaTrackerSerialServiceTest::trimAndReverseApplyLive()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->syncDrain = true; };
    AntennaTrackerSerialService service;
    service.setTransportFactory(fixture.factory());

    AntennaTrackerSerialSettings settings = settingsFor(QStringLiteral("ArduTracker"));
    QVERIFY(service.connectToTracker(settings) > 0);
    QCOMPARE(service.state(), State::Connected);
    QVERIFY(service.setTarget(10, 0));
    QCOMPARE(fixture.log->wire.mid(21), QByteArrayLiteral("!!!PAN:1527,TLT:1500\n"));

    service.setTrim(10, 0);
    QCOMPARE(service.settings().panTrim, 10.0);
    QVERIFY(service.setTarget(10, 0));
    QCOMPARE(fixture.log->wire.mid(42), QByteArrayLiteral("!!!PAN:1500,TLT:1500\n"));

    service.setTrim(0, 0);
    service.setReverse(true, false);
    QVERIFY(service.settings().panReverse);
    QVERIFY(service.setTarget(10, 0));
    QCOMPARE(fixture.log->wire.mid(63), QByteArrayLiteral("!!!PAN:1472,TLT:1500\n"));

    QVERIFY(service.centerTracker());
    QCOMPARE(fixture.log->wire.mid(84), QByteArrayLiteral("!!!PAN:1500,TLT:1500\n"));
    QCOMPARE(service.writtenTargetCount(), 4);
}

void AntennaTrackerSerialServiceTest::destructionClosesTransportSilently()
{
    Fixture fixture;
    fixture.configure = [](FakeTransport *transport) { transport->autoDrain = false; };
    auto *service = new AntennaTrackerSerialService;
    service->setTransportFactory(fixture.factory());
    int lateSignals = 0;
    QObject watcher;
    connect(service, &AntennaTrackerSerialService::stateChanged, &watcher,
            [&lateSignals](State, quint64) { ++lateSignals; });
    connect(service, &AntennaTrackerSerialService::statusChanged, &watcher,
            [&lateSignals](const QString &) { ++lateSignals; });
    connect(service, &AntennaTrackerSerialService::errorOccurred, &watcher,
            [&lateSignals](quint64, const QString &) { ++lateSignals; });

    QVERIFY(service->connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    QVERIFY(service->setTarget(1, 1));
    QCOMPARE(service->state(), State::Connecting);
    QCOMPARE(lateSignals, 1); // Connecting

    delete service;
    QCOMPARE(lateSignals, 1);
    QCOMPARE(fixture.log->closes, 1);
    QVERIFY(fixture.log->destroyed);
    QVERIFY(fixture.transport.isNull());

    // Destroying an idle or failed service is equally quiet.
    Fixture idle;
    auto *idleService = new AntennaTrackerSerialService;
    idleService->setTransportFactory(idle.factory());
    delete idleService;
    QCOMPARE(idle.factoryCalls, 0);

    Fixture failed;
    failed.configure = [](FakeTransport *transport) { transport->failOpen = true; };
    auto *failedService = new AntennaTrackerSerialService;
    failedService->setTransportFactory(failed.factory());
    QVERIFY(failedService->connectToTracker(settingsFor(QStringLiteral("Maestro"))) > 0);
    delete failedService;
    QVERIFY(failed.log->destroyed);
    QCOMPARE(failed.log->closes, 0);
}

void AntennaTrackerSerialServiceTest::serialPortTransportUsesEightNoneOne()
{
    const AntennaTrackerSerialService::TransportFactory factory =
        AntennaTrackerSerialService::DefaultTransportFactory();
    QObject parent;
    AntennaTrackerSerialTransport *created = factory(&parent);
    QVERIFY(created);
    QCOMPARE(created->parent(), &parent);
    auto *transport = dynamic_cast<AntennaTrackerSerialPortTransport *>(created);
    QVERIFY(transport);
    QSerialPort *port = transport->port();
    QVERIFY(port);
    QCOMPARE(port->dataBits(), QSerialPort::Data8);
    QCOMPARE(port->parity(), QSerialPort::NoParity);
    QCOMPARE(port->stopBits(), QSerialPort::OneStop);
    QCOMPARE(port->flowControl(), QSerialPort::NoFlowControl);
    QVERIFY(!transport->isOpen());
    QCOMPARE(transport->bytesToWrite(), qint64(0));
    QVERIFY(!transport->discardInput());
    QCOMPARE(transport->write(QByteArrayLiteral("x")), qint64(-1));

    // A missing device fails synchronously through *error, never through the
    // asynchronous errorOccurred() path the service reserves for a live port.
    int asyncErrors = 0;
    connect(transport, &AntennaTrackerSerialTransport::errorOccurred, &parent,
            [&asyncErrors](const QString &) { ++asyncErrors; });
    QString error;
    QVERIFY(!transport->open(QStringLiteral("/dev/apm-planner-no-such-tracker-port"), 9600,
                             &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!transport->isOpen());
    QCOMPARE(asyncErrors, 0);
    QCOMPARE(port->dataBits(), QSerialPort::Data8);
    QCOMPARE(port->flowControl(), QSerialPort::NoFlowControl);
    transport->close(); // idempotent while closed
    QCOMPARE(asyncErrors, 0);

    // The service reports the same failure with the MP10 prefix.
    AntennaTrackerSerialService service;
    AntennaTrackerSerialSettings settings = settingsFor(QStringLiteral("Maestro"));
    settings.portName = QStringLiteral("/dev/apm-planner-no-such-tracker-port");
    QVERIFY(service.connectToTracker(settings) > 0);
    QCOMPARE(service.state(), State::Failed);
    QVERIFY(service.status().startsWith(QStringLiteral("Error connecting: ")));
    QVERIFY(service.status().size() > QStringLiteral("Error connecting: ").size());
}

void AntennaTrackerSerialServiceTest::runsInsideAWorkerThreadThroughQueuedSlots()
{
    auto log = std::make_shared<FakeTransportLog>();
    QThread thread;
    thread.start();
    QObject anchor;
    anchor.moveToThread(&thread);

    AntennaTrackerSerialService *service = nullptr;
    QMetaObject::invokeMethod(&anchor, [&service, log]() {
        service = new AntennaTrackerSerialService;
        service->setTransportFactory([log](QObject *parent) -> AntennaTrackerSerialTransport * {
            return new FakeTransport(log, parent);
        });
    }, Qt::BlockingQueuedConnection);
    QVERIFY(service);
    QCOMPARE(service->thread(), &thread);

    // Observe from the test thread through queued connections only.
    QObject receiver;
    QList<State> states;
    QStringList statuses;
    QList<QPair<double, double>> targets;
    connect(service, &AntennaTrackerSerialService::stateChanged, &receiver,
            [&states](State state, quint64) { states.append(state); }, Qt::QueuedConnection);
    connect(service, &AntennaTrackerSerialService::statusChanged, &receiver,
            [&statuses](const QString &status) { statuses.append(status); },
            Qt::QueuedConnection);
    connect(service, &AntennaTrackerSerialService::targetWritten, &receiver,
            [&targets](quint64, double pan, double tilt) { targets.append({pan, tilt}); },
            Qt::QueuedConnection);

    const AntennaTrackerSerialSettings settings = settingsFor(QStringLiteral("DegreeTracker"));
    QVERIFY(QMetaObject::invokeMethod(service, "connectToTracker", Qt::QueuedConnection,
                                      Q_ARG(AntennaTrackerSerialSettings, settings)));
    QTRY_VERIFY(states.contains(State::Connected));
    QCOMPARE(states, (QList<State>{State::Connecting, State::Connected}));
    QCOMPARE(statuses, QStringList{QStringLiteral("Connected (DegreeTracker).")});
    QVERIFY(QMetaObject::invokeMethod(service, "setTarget", Qt::QueuedConnection,
                                      Q_ARG(double, 12.34), Q_ARG(double, -4.5)));
    QTRY_COMPARE(targets.size(), 1);
    QCOMPARE(targets.first(), (QPair<double, double>{12.34, -4.5}));

    // Teardown on the owning thread while connected, then join.
    QMetaObject::invokeMethod(&anchor, [&service]() {
        delete service;
        service = nullptr;
    }, Qt::BlockingQueuedConnection);
    thread.quit();
    QVERIFY(thread.wait(5000));
    QVERIFY(!service);
    QVERIFY(log->destroyed);
    QCOMPARE(log->opens, 1);
    QCOMPARE(log->closes, 1);
    QCOMPARE(log->wire, QByteArrayLiteral("!!!PAN:0000,TLT:0000\n!!!PAN:0123,TLT:-0045\n"));
}

QTEST_GUILESS_MAIN(AntennaTrackerSerialServiceTest)
#include "test_antennatrackerserialservice.moc"
