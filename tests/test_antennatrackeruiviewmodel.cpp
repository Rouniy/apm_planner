#include <QtTest>

#include "AntennaTrackerTestFakes.h"
#include "ui/configuration/AntennaTrackerGeometry.h"

#include <QSignalSpy>

#include <cmath>

using Axis = AntennaTrackerUIViewModel::Axis;
using Field = AntennaTrackerUIViewModel::Field;
using State = AntennaTrackerSerialService::State;

namespace {

// DegreeTracker line for the given angles: tenths truncated toward zero.
QByteArray degreeLine(double pan, double tilt)
{
    return IAntennaTrackerOutput::FormatPanTiltLine(IAntennaTrackerOutput::ToInt32(pan * 10),
                                                    IAntennaTrackerOutput::ToInt32(tilt * 10));
}

} // namespace

class AntennaTrackerUIViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchMp10();
    void settingsRoundTrip();
    void connectTogglesTextsAndGating();
    void connectReportsMp10ValidationTexts();
    void loopPointsAtVehicleAndFormatsReadouts();
    void manualModeAndHomeCenter();
    void trimAndReverseApplyLive();
    void serviceFailureResetsControls();
    void activateRefreshesPortsAndDeactivateSaves();
    void findTrimPanFollowsMp10Sweep();
    void shutdownDisconnectsAndSaves();
};

void AntennaTrackerUIViewModelTest::defaultsMatchMp10()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    QCOMPARE(AntennaTrackerUIViewModel::Title(), QStringLiteral("Antenna Tracker"));
    QCOMPARE(vm->selectedInterface(), QStringLiteral("Maestro"));
    QCOMPARE(vm->selectedBaud(), QStringLiteral("9600"));
    QCOMPARE(vm->selectedPort(), QStringLiteral("/dev/ttyTRACKER0")); // first port
    QCOMPARE(vm->ports(), fixture.ports);
    QCOMPARE(vm->interfaces(), (QStringList{QStringLiteral("Maestro"), QStringLiteral("ArduTracker"),
                                            QStringLiteral("DegreeTracker")}));
    QCOMPARE(vm->bauds().size(), 8);
    QCOMPARE(vm->field(Axis::Pan, Field::Range), QStringLiteral("360"));
    QCOMPARE(vm->field(Axis::Pan, Field::PwmRange), QStringLiteral("1000"));
    QCOMPARE(vm->field(Axis::Pan, Field::Center), QStringLiteral("1500"));
    QCOMPARE(vm->field(Axis::Pan, Field::Speed), QStringLiteral("100"));
    QCOMPARE(vm->field(Axis::Pan, Field::Accel), QStringLiteral("5"));
    QCOMPARE(vm->field(Axis::Tilt, Field::Range), QStringLiteral("90"));
    QCOMPARE(vm->field(Axis::Tilt, Field::PwmRange), QStringLiteral("1000"));
    QCOMPARE(vm->field(Axis::Tilt, Field::Center), QStringLiteral("1500"));
    QCOMPARE(vm->field(Axis::Tilt, Field::Speed), QStringLiteral("100"));
    QCOMPARE(vm->field(Axis::Tilt, Field::Accel), QStringLiteral("5"));
    QCOMPARE(vm->trim(Axis::Pan), 0.0);
    QCOMPARE(vm->trim(Axis::Tilt), 0.0);
    QCOMPARE(vm->trimMin(Axis::Pan), -180.0);
    QCOMPARE(vm->trimMax(Axis::Pan), 180.0);
    QCOMPARE(vm->trimMin(Axis::Tilt), -45.0);
    QCOMPARE(vm->trimMax(Axis::Tilt), 45.0);
    QVERIFY(!vm->reverse(Axis::Pan));
    QVERIFY(!vm->reverse(Axis::Tilt));
    QVERIFY(!vm->manualMode());
    QCOMPARE(vm->manualAzimuth(), 0.0);
    QCOMPARE(vm->manualElevation(), 0.0);
    QCOMPARE(vm->vehicleAzimuth(), QStringLiteral("--"));
    QCOMPARE(vm->commandedElevation(), QStringLiteral("--"));
    QCOMPARE(vm->connectText(), QStringLiteral("Connect"));
    QVERIFY(vm->controlsEnabled());
    QVERIFY(vm->speedAccelEnabled());
    QVERIFY(!vm->isRunning());
    QVERIFY(!vm->isSearchingTrim());
    QCOMPARE(vm->status(), QStringLiteral("Disconnected."));
    QCOMPARE(vm->loopIntervalMs(), 100);
    QCOMPARE(vm->trimSearchTiming().settleMs, 4000);
    QCOMPARE(vm->trimSearchTiming().stepMs, 2000);

    // MP10 field names and settings keys.
    QCOMPARE(AntennaTrackerUIViewModel::FieldName(Axis::Tilt, Field::Center),
             QStringLiteral("tilt PWM center"));
    QCOMPARE(AntennaTrackerUIViewModel::FieldName(Axis::Pan, Field::Accel),
             QStringLiteral("pan acceleration"));
    QCOMPARE(AntennaTrackerUIViewModel::SettingsKey(Axis::Pan, Field::PwmRange),
             QStringLiteral("TXT_pwmrangepan"));
    QCOMPARE(AntennaTrackerUIViewModel::SettingsKey(Axis::Tilt, Field::Center),
             QStringLiteral("TXT_centertilt"));
    QCOMPARE(AntennaTrackerUIViewModel::FormatAngle(12.345), QStringLiteral("12.3"));
    QCOMPARE(AntennaTrackerUIViewModel::FormatAngle(-0.04), QStringLiteral("-0.0"));

    // The tilt trim range follows the tilt range with C# integer division.
    QSignalSpy rangeSpy(vm, &AntennaTrackerUIViewModel::trimRangeChanged);
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("91"));
    QCOMPARE(vm->trimMin(Axis::Tilt), -45.0);
    QCOMPARE(vm->trimMax(Axis::Tilt), 45.0);
    QCOMPARE(rangeSpy.count(), 0);
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("180"));
    QCOMPARE(vm->trimMin(Axis::Tilt), -90.0);
    QCOMPARE(vm->trimMax(Axis::Tilt), 90.0);
    QCOMPARE(rangeSpy.count(), 1);
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("abc")); // MP10 ParseInt fallback 90
    QCOMPARE(vm->trimMax(Axis::Tilt), 45.0);
}

void AntennaTrackerUIViewModelTest::settingsRoundTrip()
{
    TrackerFixture fixture;
    {
        AntennaTrackerUIViewModel *vm = fixture.build();
        vm->setSelectedInterface(QStringLiteral("DegreeTracker"));
        vm->setSelectedPort(QStringLiteral("/dev/ttyTRACKER1"));
        vm->setSelectedBaud(QStringLiteral("57600"));
        vm->setField(Axis::Pan, Field::Range, QStringLiteral("270"));
        vm->setField(Axis::Tilt, Field::Accel, QStringLiteral("7"));
        vm->setTrim(Axis::Pan, 12.7);  // stored as (int) -> 12
        vm->setTrim(Axis::Tilt, -3.9); // -> -3
        vm->setReverse(Axis::Pan, true);
        vm->saveSettings();
    }
    QSettings raw(fixture.settingsPath(), QSettings::IniFormat);
    raw.beginGroup(QStringLiteral("AntennaTracker"));
    QCOMPARE(raw.value(QStringLiteral("CMB_interface")).toString(), QStringLiteral("DegreeTracker"));
    QCOMPARE(raw.value(QStringLiteral("CMB_serialport")).toString(), QStringLiteral("/dev/ttyTRACKER1"));
    QCOMPARE(raw.value(QStringLiteral("CMB_baudrate")).toString(), QStringLiteral("57600"));
    QCOMPARE(raw.value(QStringLiteral("TXT_panrange")).toString(), QStringLiteral("270"));
    QCOMPARE(raw.value(QStringLiteral("TXT_tiltaccel")).toString(), QStringLiteral("7"));
    QCOMPARE(raw.value(QStringLiteral("TRK_pantrim")).toInt(), 12);
    QCOMPARE(raw.value(QStringLiteral("TRK_tilttrim")).toInt(), -3);
    QCOMPARE(raw.value(QStringLiteral("CHK_revpan")).toBool(), true);
    QCOMPARE(raw.value(QStringLiteral("CHK_revtilt")).toBool(), false);
    raw.endGroup();

    AntennaTrackerUIViewModel *reloaded = fixture.build();
    QCOMPARE(reloaded->selectedInterface(), QStringLiteral("DegreeTracker"));
    QCOMPARE(reloaded->selectedPort(), QStringLiteral("/dev/ttyTRACKER1"));
    QCOMPARE(reloaded->selectedBaud(), QStringLiteral("57600"));
    QCOMPARE(reloaded->field(Axis::Pan, Field::Range), QStringLiteral("270"));
    QCOMPARE(reloaded->field(Axis::Tilt, Field::Accel), QStringLiteral("7"));
    QCOMPARE(reloaded->trim(Axis::Pan), 12.0);
    QCOMPARE(reloaded->trim(Axis::Tilt), -3.0);
    QVERIFY(reloaded->reverse(Axis::Pan));
    QVERIFY(!reloaded->speedAccelEnabled()); // DegreeTracker
    // Loaded trims/reverse are pushed into the service before any connect.
    QCOMPARE(fixture.service->settings().panTrim, 12.0);
    QVERIFY(fixture.service->settings().panReverse);

    // An unknown stored interface and a vanished port fall back like MP10.
    // Destroy the reloaded view model first: its destructor saves its own state.
    fixture.viewModel.reset();
    raw.beginGroup(QStringLiteral("AntennaTracker"));
    raw.setValue(QStringLiteral("CMB_interface"), QStringLiteral("Nope"));
    raw.setValue(QStringLiteral("CMB_serialport"), QStringLiteral("/dev/gone"));
    raw.endGroup();
    raw.sync();
    AntennaTrackerUIViewModel *fallback = fixture.build();
    QCOMPARE(fallback->selectedInterface(), QStringLiteral("Maestro"));
    QCOMPARE(fallback->selectedPort(), QStringLiteral("/dev/ttyTRACKER0"));
}

void AntennaTrackerUIViewModelTest::connectTogglesTextsAndGating()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    QSignalSpy running(vm, &AntennaTrackerUIViewModel::runningChanged);
    QSignalSpy statuses(vm, &AntennaTrackerUIViewModel::statusChanged);
    vm->setField(Axis::Pan, Field::Center, QStringLiteral(" 1500 ")); // normalised after connect

    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    QVERIFY(vm->isRunning());
    QCOMPARE(vm->connectText(), QStringLiteral("Disconnect"));
    QVERIFY(!vm->controlsEnabled());
    QVERIFY(!vm->speedAccelEnabled());
    QCOMPARE(vm->status(), QStringLiteral("Connected (Maestro)."));
    QCOMPARE(vm->field(Axis::Pan, Field::Center), QStringLiteral("1500"));
    QCOMPARE(fixture.log->portName, QStringLiteral("/dev/ttyTRACKER0"));
    QCOMPARE(fixture.log->baudRate, 9600);
    QCOMPARE(running.count(), 1);
    QCOMPARE(running.last().first().toBool(), true);
    // Connecting saved the settings first (MP10 Connect() -> SaveSettings()).
    QSettings raw(fixture.settingsPath(), QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral("AntennaTracker/CMB_baudrate")).toString(),
             QStringLiteral("9600"));

    // A second call disconnects.
    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Disconnected);
    QVERIFY(!vm->isRunning());
    QCOMPARE(vm->connectText(), QStringLiteral("Connect"));
    QVERIFY(vm->controlsEnabled());
    QVERIFY(vm->speedAccelEnabled());
    QCOMPARE(vm->status(), QStringLiteral("Disconnected."));
    QCOMPARE(fixture.log->closes, 1);
    QCOMPARE(running.count(), 2);

    // Speed/Accel gating follows the interface while disconnected.
    vm->setSelectedInterface(QStringLiteral("ArduTracker"));
    QVERIFY(vm->controlsEnabled());
    QVERIFY(!vm->speedAccelEnabled());
    vm->setSelectedInterface(QStringLiteral("Maestro"));
    QVERIFY(vm->speedAccelEnabled());
}

void AntennaTrackerUIViewModelTest::connectReportsMp10ValidationTexts()
{
    TrackerFixture fixture;
    fixture.ports.clear();
    AntennaTrackerUIViewModel *vm = fixture.build();
    QCOMPARE(vm->selectedPort(), QString());

    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("No serial port selected."));
    QCOMPARE(fixture.factoryCalls, 0);
    QCOMPARE(vm->connectText(), QStringLiteral("Connect"));

    vm->setSelectedPort(QStringLiteral("COM7"));
    vm->setSelectedBaud(QStringLiteral("fast"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("Error connecting: baud rate must be an integer."));
    vm->setSelectedBaud(QStringLiteral("0"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("Error connecting: baud rate is below the safe minimum."));
    vm->setSelectedBaud(QStringLiteral("9600"));

    // MP10 order: pan range, tilt range, pan PWM range, tilt PWM range, centres, speeds/accels.
    vm->setField(Axis::Pan, Field::Range, QStringLiteral("0"));
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("abc"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(),
             QStringLiteral("Invalid number entered: pan range is below the safe minimum."));
    vm->setField(Axis::Pan, Field::Range, QStringLiteral("360"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("Invalid number entered: tilt range must be an integer."));
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("90"));
    vm->setField(Axis::Tilt, Field::Accel, QStringLiteral("-1"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(),
             QStringLiteral("Invalid number entered: tilt acceleration is below the safe minimum."));
    vm->setField(Axis::Tilt, Field::Accel, QStringLiteral("5"));
    QCOMPARE(fixture.factoryCalls, 0); // nothing above touched the port

    // Codec-level validation (a 1 degree tilt range collapses) comes from the service.
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("1"));
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("Invalid tilt range."));
    QCOMPARE(vm->connectText(), QStringLiteral("Connect"));
    QVERIFY(vm->controlsEnabled());

    // Open failure text comes through as MP10 "Error connecting: ...".
    vm->setField(Axis::Tilt, Field::Range, QStringLiteral("90"));
    fixture.configure = [](FakeTrackerTransport *transport) {
        transport->failOpen = true;
        transport->openError = QStringLiteral("Permission denied");
    };
    vm->connectOrDisconnect();
    QCOMPARE(vm->status(), QStringLiteral("Error connecting: Permission denied"));
    QCOMPARE(fixture.factoryCalls, 1);
    QVERIFY(!vm->isRunning());
    QVERIFY(vm->controlsEnabled());
}

void AntennaTrackerUIViewModelTest::loopPointsAtVehicleAndFormatsReadouts()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedInterface(QStringLiteral("DegreeTracker"));
    vm->setLoopIntervalMs(1);

    // Tracker at the origin, vehicle 0.01 degrees east and 100 m higher.
    fixture.telemetry->tracker = AntennaTrackerPosition(0.0, 0.0, 0.0);
    fixture.telemetry->trackerValid = true;
    fixture.telemetry->fix.valid = true;
    fixture.telemetry->fix.latitude = 0.0;
    fixture.telemetry->fix.longitude = 0.01;
    fixture.telemetry->fix.altitudeAmsl = 100.0;
    const AntennaTrackerPosition vehicle(0.0, 0.01, 100.0);
    const double expectedAz = AntennaTrackerGeometry::AZToMAV(fixture.telemetry->tracker, vehicle);
    const double expectedEl = AntennaTrackerGeometry::ELToMAV(fixture.telemetry->tracker, vehicle);
    QCOMPARE(expectedAz, 90.0);
    QVERIFY(expectedEl > 5.0 && expectedEl < 5.3);

    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    QTRY_VERIFY(vm->loopTicks() >= 2);
    QCOMPARE(vm->vehicleAzimuth(), QStringLiteral("90.0"));
    QCOMPARE(vm->vehicleElevation(), AntennaTrackerUIViewModel::FormatAngle(expectedEl));
    QCOMPARE(vm->commandedAzimuth(), QStringLiteral("90.0"));
    QCOMPARE(vm->commandedElevation(), vm->vehicleElevation());
    // The initial centre frame, then the loop's frames.
    const QByteArray centre = degreeLine(0.0, 0.0);
    QVERIFY(fixture.log->wire.startsWith(centre));
    QVERIFY(fixture.log->wire.mid(centre.size()).startsWith(degreeLine(expectedAz, expectedEl)));

    // Without a fix MP10 sends AZToMAV/ELToMAV == 0 and shows "0.0".
    fixture.telemetry->fix.valid = false;
    const int ticksBefore = vm->loopTicks();
    QTRY_VERIFY(vm->loopTicks() >= ticksBefore + 2);
    QCOMPARE(vm->vehicleAzimuth(), QStringLiteral("0.0"));
    QCOMPARE(vm->commandedElevation(), QStringLiteral("0.0"));
    QVERIFY(fixture.log->wire.endsWith(centre));

    vm->connectOrDisconnect();
    const int ticksAfterStop = vm->loopTicks();
    QTest::qWait(20);
    QCOMPARE(vm->loopTicks(), ticksAfterStop); // the loop stopped with the connection
}

void AntennaTrackerUIViewModelTest::manualModeAndHomeCenter()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedInterface(QStringLiteral("DegreeTracker"));
    vm->setLoopIntervalMs(10000); // observe single frames
    fixture.telemetry->tracker = AntennaTrackerPosition(0.0, 0.0, 0.0);
    fixture.telemetry->trackerValid = true;
    fixture.telemetry->fix.valid = true;
    fixture.telemetry->fix.longitude = 0.01;
    fixture.telemetry->fix.altitudeAmsl = 100.0;

    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    const int wireAfterConnect = fixture.log->wire.size();

    // Home / Center while idle: manual angles reset, one centre frame, no manual mode.
    vm->setManualAzimuth(45.0);
    vm->setManualElevation(-10.0);
    vm->homeCenter();
    QCOMPARE(vm->manualAzimuth(), 0.0);
    QCOMPARE(vm->manualElevation(), 0.0);
    QVERIFY(!vm->manualMode());
    QCOMPARE(fixture.log->wire.mid(wireAfterConnect), degreeLine(0.0, 0.0));

    // Manual mode uses the sliders instead of the vehicle on the next tick.
    vm->setManualMode(true);
    vm->setManualAzimuth(45.0);
    vm->setManualElevation(-10.0);
    vm->setLoopIntervalMs(1);
    const int ticks = vm->loopTicks();
    QTRY_VERIFY(vm->loopTicks() > ticks);
    QVERIFY(fixture.log->wire.endsWith(degreeLine(45.0, -10.0)));
    QCOMPARE(vm->commandedAzimuth(), QStringLiteral("45.0"));
    QCOMPARE(vm->commandedElevation(), QStringLiteral("-10.0"));
    QCOMPARE(vm->vehicleAzimuth(), QStringLiteral("90.0")); // still shown in manual mode

    // Home / Center while disconnected only resets the manual angles.
    vm->connectOrDisconnect();
    vm->setManualAzimuth(30.0);
    const int wireAfterDisconnect = fixture.log->wire.size();
    vm->homeCenter();
    QCOMPARE(vm->manualAzimuth(), 0.0);
    QCOMPARE(fixture.log->wire.size(), wireAfterDisconnect);
}

void AntennaTrackerUIViewModelTest::trimAndReverseApplyLive()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedInterface(QStringLiteral("ArduTracker"));
    vm->setLoopIntervalMs(10000);
    vm->setManualMode(true);
    vm->setManualAzimuth(10.0);
    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);

    QSignalSpy trimSpy(vm, &AntennaTrackerUIViewModel::trimChanged);
    vm->setTrim(Axis::Pan, 10.0);
    QCOMPARE(trimSpy.count(), 1);
    QCOMPARE(fixture.service->settings().panTrim, 10.0);
    vm->setReverse(Axis::Tilt, true);
    QVERIFY(fixture.service->settings().tiltReverse);
    vm->setTrim(Axis::Pan, std::nan("")); // ignored
    QCOMPARE(vm->trim(Axis::Pan), 10.0);

    // With trim 10 the manual azimuth 10 lands on the centre pulse.
    const int before = fixture.log->wire.size();
    QVERIFY(fixture.service->setTarget(10.0, 0.0));
    QCOMPARE(fixture.log->wire.mid(before), QByteArrayLiteral("!!!PAN:1500,TLT:1500\n"));
}

void AntennaTrackerUIViewModelTest::serviceFailureResetsControls()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setLoopIntervalMs(1);
    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    QTRY_VERIFY(vm->loopTicks() >= 1);

    fixture.transport->unplug(QStringLiteral("Resource error"));
    QCOMPARE(fixture.service->state(), State::Failed);
    QVERIFY(!vm->isRunning());
    QCOMPARE(vm->connectText(), QStringLiteral("Connect"));
    QVERIFY(vm->controlsEnabled());
    QCOMPARE(vm->status(), QStringLiteral("Tracker serial error: Resource error"));
    const int ticks = vm->loopTicks();
    QTest::qWait(20);
    QCOMPARE(vm->loopTicks(), ticks);

    // Reconnecting works from the Failed state.
    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    QCOMPARE(vm->connectText(), QStringLiteral("Disconnect"));
}

void AntennaTrackerUIViewModelTest::activateRefreshesPortsAndDeactivateSaves()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedPort(QStringLiteral("/dev/ttyTRACKER1"));

    QSignalSpy portsSpy(vm, &AntennaTrackerUIViewModel::portsChanged);
    fixture.ports = QStringList{QStringLiteral("/dev/ttyTRACKER1"), QStringLiteral("/dev/ttyNEW")};
    vm->activate();
    QCOMPARE(portsSpy.count(), 1);
    QCOMPARE(vm->ports(), fixture.ports);
    QCOMPARE(vm->selectedPort(), QStringLiteral("/dev/ttyTRACKER1")); // kept
    fixture.ports = QStringList{QStringLiteral("/dev/ttyOTHER")};
    vm->activate();
    QCOMPARE(vm->selectedPort(), QStringLiteral("/dev/ttyOTHER")); // first when gone
    fixture.ports.clear();
    vm->activate();
    QCOMPARE(vm->selectedPort(), QString());

    vm->setField(Axis::Pan, Field::Speed, QStringLiteral("42"));
    vm->deactivate();
    QSettings raw(fixture.settingsPath(), QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral("AntennaTracker/TXT_panspeed")).toString(),
             QStringLiteral("42"));
}

void AntennaTrackerUIViewModelTest::findTrimPanFollowsMp10Sweep()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedInterface(QStringLiteral("DegreeTracker"));
    vm->setLoopIntervalMs(10000);
    vm->setTrimSearchTiming({0, 0});

    vm->findTrimPan();
    QCOMPARE(vm->status(), QStringLiteral("Connect to the tracker first."));

    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    vm->findTrimPan();
    QCOMPARE(vm->status(), QStringLiteral("No valid SiK radio detected.")); // snr == 0
    QVERIFY(!vm->isSearchingTrim());

    // SNR peaks when the pan trim is 37 degrees; never exactly zero.
    fixture.telemetry->snrProvider = [vm]() {
        return qMax(1.0, 30.0 - std::fabs(vm->trim(Axis::Pan) - 37.0) * 0.5);
    };
    QSignalSpy searching(vm, &AntennaTrackerUIViewModel::searchingTrimChanged);
    QSignalSpy trims(vm, &AntennaTrackerUIViewModel::trimChanged);
    vm->findTrimPan();
    QVERIFY(vm->isSearchingTrim());
    QCOMPARE(vm->status(), QStringLiteral("Searching for best pan trim..."));
    QTRY_VERIFY(!vm->isSearchingTrim());
    QCOMPARE(vm->status(), QStringLiteral("Pan trim search complete."));
    QCOMPARE(vm->trim(Axis::Pan), 37.0);
    QCOMPARE(fixture.service->settings().panTrim, 37.0);
    QCOMPARE(searching.count(), 2);
    // MP10 probes: -90..60 step 30 (6), 0..55 step 5 (12), 30..39 step 1 (10),
    // plus the three phase starts and the final answer.
    QVERIFY(trims.count() >= 6 + 12 + 10);

    // A second click while sweeping cancels; disconnecting stops it silently.
    vm->setTrimSearchTiming({1000, 1000});
    vm->findTrimPan();
    QVERIFY(vm->isSearchingTrim());
    vm->findTrimPan();
    QVERIFY(!vm->isSearchingTrim());
    QCOMPARE(vm->status(), QStringLiteral("Pan trim search cancelled."));
    vm->findTrimPan();
    QVERIFY(vm->isSearchingTrim());
    vm->connectOrDisconnect();
    QVERIFY(!vm->isSearchingTrim());
    QCOMPARE(vm->status(), QStringLiteral("Disconnected."));
}

void AntennaTrackerUIViewModelTest::shutdownDisconnectsAndSaves()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedBaud(QStringLiteral("115200"));
    vm->connectOrDisconnect();
    QCOMPARE(fixture.service->state(), State::Connected);
    QSignalSpy statuses(vm, &AntennaTrackerUIViewModel::statusChanged);

    vm->shutdown();
    QVERIFY(!vm->isRunning());
    QCOMPARE(fixture.log->closes, 1);
    QSettings raw(fixture.settingsPath(), QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral("AntennaTracker/CMB_baudrate")).toString(),
             QStringLiteral("115200"));
    vm->shutdown(); // idempotent
    QCOMPARE(fixture.log->closes, 1);

    // Destroying a connected view model closes the port as well.
    TrackerFixture second;
    AntennaTrackerUIViewModel *other = second.build();
    other->connectOrDisconnect();
    QCOMPARE(second.service->state(), State::Connected);
    second.viewModel.reset();
    QCOMPARE(second.log->closes, 1);
    QVERIFY(second.log->destroyed);
}

QTEST_GUILESS_MAIN(AntennaTrackerUIViewModelTest)
#include "test_antennatrackeruiviewmodel.moc"
