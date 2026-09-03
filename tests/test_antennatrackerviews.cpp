#include <QtTest>

#include "AntennaTrackerTestFakes.h"
#include "ui/configuration/AntennaTrackerAxisPanel.h"
#include "ui/configuration/AntennaTrackerUIView.h"
#include "ui/configuration/ConfigAntennaTrackerView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>

using Axis = AntennaTrackerUIViewModel::Axis;
using Field = AntennaTrackerUIViewModel::Field;
using State = AntennaTrackerSerialService::State;

class AntennaTrackerViewsTest final : public QObject
{
    Q_OBJECT

private slots:
    void serialAndLivePagesExposeMp10Controls();
    void editsFlowBothWaysThroughTheSharedViewModel();
    void connectionGatesControlsOnBothPages();
    void manualSlewAndTelemetryReadouts();
};

void AntennaTrackerViewsTest::serialAndLivePagesExposeMp10Controls()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    ConfigAntennaTrackerView serial(vm);
    AntennaTrackerUIView live(vm);
    QCOMPARE(serial.pageLayout(), AntennaTrackerUIView::PageLayout::Serial);
    QCOMPARE(live.pageLayout(), AntennaTrackerUIView::PageLayout::Live);
    QCOMPARE(serial.viewModel(), vm);
    QCOMPARE(live.viewModel(), vm);

    QCOMPARE(serial.findChild<QLabel *>(QStringLiteral("trkSerialTitle"))->text(),
             QStringLiteral("Antenna Tracker"));
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveTitle"))->text(),
             QStringLiteral("Antenna Tracker"));
    QCOMPARE(serial.findTrimButton()->text(), QStringLiteral("Find Trim Pan (Sik Radio)"));
    QCOMPARE(live.findTrimButton()->text(), QStringLiteral("Find Trim Pan (SiK Radio)"));
    QCOMPARE(serial.connectButton()->text(), QStringLiteral("Connect"));
    QCOMPARE(serial.findChild<QLabel *>(QStringLiteral("trkSerialWarning"))->text(),
             QStringLiteral("Misusing this interface can cause servo damage, use with caution!!!"));
    QCOMPARE(serial.panPanel()->reverseCheck()->text(), QStringLiteral("Rev"));
    QCOMPARE(live.tiltPanel()->reverseCheck()->text(), QStringLiteral("Reverse"));
    QVERIFY(serial.panPanel()->centerLabel());
    QCOMPARE(serial.panPanel()->centerLabel()->text(), QStringLiteral("0"));
    QVERIFY(!live.panPanel()->centerLabel());
    QVERIFY(!serial.manualModeCheck());
    QVERIFY(!serial.findChild<QFrame *>(QStringLiteral("trkSerialTelemetryBox")));
    QVERIFY(live.manualModeCheck());
    QCOMPARE(live.manualModeCheck()->text(), QStringLiteral("Manual Slew (override point-at-vehicle)"));
    QCOMPARE(live.homeCenterButton()->text(), QStringLiteral("Home / Center"));
    QCOMPARE(live.manualAzimuthSlider()->minimum(), -180);
    QCOMPARE(live.manualAzimuthSlider()->maximum(), 180);
    QCOMPARE(live.manualElevationSlider()->minimum(), -90);
    QCOMPARE(live.manualElevationSlider()->maximum(), 90);
    QVERIFY(!live.manualAzimuthSlider()->isEnabled()); // enabled only in manual mode
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveVehicleAzimuth"))->text(),
             QStringLiteral("--"));

    // Combo contents and selections mirror the view model.
    QCOMPARE(serial.interfaceCombo()->count(), 3);
    QCOMPARE(serial.interfaceCombo()->currentText(), QStringLiteral("Maestro"));
    QCOMPARE(serial.baudCombo()->count(), 8);
    QCOMPARE(serial.baudCombo()->currentText(), QStringLiteral("9600"));
    QCOMPARE(serial.portCombo()->count(), 2);
    QCOMPARE(serial.portCombo()->currentText(), QStringLiteral("/dev/ttyTRACKER0"));

    // Axis panels: labels, trim ranges and the "Trim: N" text.
    QCOMPARE(serial.findChild<QLabel *>(QStringLiteral("trkSerialPanRangeLabel"))->text(),
             QStringLiteral("Range / Angle"));
    QCOMPARE(serial.panPanel()->fieldEdit(Field::Center)->text(), QStringLiteral("1500"));
    QCOMPARE(serial.panPanel()->trimSlider()->minimum(), -180);
    QCOMPARE(serial.panPanel()->trimSlider()->maximum(), 180);
    QCOMPARE(serial.tiltPanel()->trimSlider()->minimum(), -45);
    QCOMPARE(serial.tiltPanel()->trimSlider()->maximum(), 45);
    QCOMPARE(serial.tiltPanel()->trimSlider()->tickInterval(), 5);
    QCOMPARE(serial.panPanel()->trimLabel()->text(), QStringLiteral("Trim: 0"));
    QCOMPARE(serial.statusLabel()->text(), QStringLiteral("Disconnected."));
}

void AntennaTrackerViewsTest::editsFlowBothWaysThroughTheSharedViewModel()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    ConfigAntennaTrackerView serial(vm);
    AntennaTrackerUIView live(vm);

    // Serial edit -> view model -> live page.
    serial.panPanel()->fieldEdit(Field::Range)->setText(QStringLiteral("270"));
    QCOMPARE(vm->field(Axis::Pan, Field::Range), QStringLiteral("270"));
    QCOMPARE(live.panPanel()->fieldEdit(Field::Range)->text(), QStringLiteral("270"));

    // Tilt range changes the tilt trim slider range on both pages.
    live.tiltPanel()->fieldEdit(Field::Range)->setText(QStringLiteral("180"));
    QCOMPARE(vm->trimMax(Axis::Tilt), 90.0);
    QCOMPARE(serial.tiltPanel()->trimSlider()->maximum(), 90);
    QCOMPARE(live.tiltPanel()->trimSlider()->minimum(), -90);

    // Trim slider -> view model (live push into the service) -> other page + label.
    serial.panPanel()->trimSlider()->setValue(25);
    QCOMPARE(vm->trim(Axis::Pan), 25.0);
    QCOMPARE(fixture.service->settings().panTrim, 25.0);
    QCOMPARE(live.panPanel()->trimSlider()->value(), 25);
    QCOMPARE(live.panPanel()->trimLabel()->text(), QStringLiteral("Trim: 25"));

    // Reverse checkbox and combos.
    live.tiltPanel()->reverseCheck()->setChecked(true);
    QVERIFY(vm->reverse(Axis::Tilt));
    QVERIFY(serial.tiltPanel()->reverseCheck()->isChecked());
    serial.interfaceCombo()->setCurrentText(QStringLiteral("ArduTracker"));
    QCOMPARE(vm->selectedInterface(), QStringLiteral("ArduTracker"));
    QCOMPARE(live.interfaceCombo()->currentText(), QStringLiteral("ArduTracker"));
    QVERIFY(!live.panPanel()->fieldEdit(Field::Speed)->isEnabled()); // not Maestro
    QVERIFY(live.panPanel()->fieldEdit(Field::Range)->isEnabled());
    live.baudCombo()->setCurrentText(QStringLiteral("57600"));
    QCOMPARE(vm->selectedBaud(), QStringLiteral("57600"));
    QCOMPARE(serial.baudCombo()->currentText(), QStringLiteral("57600"));
    serial.portCombo()->setCurrentIndex(1);
    QCOMPARE(vm->selectedPort(), QStringLiteral("/dev/ttyTRACKER1"));
    QCOMPARE(live.portCombo()->currentText(), QStringLiteral("/dev/ttyTRACKER1"));

    // View model -> pages (settings reload path): a port refresh repopulates both combos.
    fixture.ports = QStringList{QStringLiteral("/dev/ttyTRACKER1"), QStringLiteral("/dev/ttyNEW")};
    serial.activate();
    QCOMPARE(live.portCombo()->count(), 2);
    QCOMPARE(live.portCombo()->currentText(), QStringLiteral("/dev/ttyTRACKER1"));
}

void AntennaTrackerViewsTest::connectionGatesControlsOnBothPages()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    ConfigAntennaTrackerView serial(vm);
    AntennaTrackerUIView live(vm);

    serial.connectButton()->click();
    QCOMPARE(fixture.service->state(), State::Connected);
    for (AntennaTrackerUIView *page : {static_cast<AntennaTrackerUIView *>(&serial), &live}) {
        QCOMPARE(page->connectButton()->text(), QStringLiteral("Disconnect"));
        QVERIFY(!page->interfaceCombo()->isEnabled());
        QVERIFY(!page->portCombo()->isEnabled());
        QVERIFY(!page->baudCombo()->isEnabled());
        QVERIFY(!page->panPanel()->fieldEdit(Field::Range)->isEnabled());
        QVERIFY(!page->tiltPanel()->fieldEdit(Field::Center)->isEnabled());
        QVERIFY(!page->panPanel()->fieldEdit(Field::Speed)->isEnabled());
        QVERIFY(page->panPanel()->trimSlider()->isEnabled());   // trim stays live
        QVERIFY(page->tiltPanel()->reverseCheck()->isEnabled()); // reverse stays live
        QCOMPARE(page->statusLabel()->text(), QStringLiteral("Connected (Maestro)."));
    }

    live.connectButton()->click();
    QCOMPARE(fixture.service->state(), State::Disconnected);
    QCOMPARE(serial.connectButton()->text(), QStringLiteral("Connect"));
    QVERIFY(serial.interfaceCombo()->isEnabled());
    QVERIFY(serial.panPanel()->fieldEdit(Field::Speed)->isEnabled());
    QCOMPARE(live.statusLabel()->text(), QStringLiteral("Disconnected."));

    // A validation failure shows on both status lines and leaves the controls alone.
    // The baud combo is not editable (as in MP10), so an odd stored value can only
    // arrive through the view model; the pages then display it as an extra item.
    vm->setSelectedBaud(QStringLiteral("nope"));
    QCOMPARE(serial.baudCombo()->currentText(), QStringLiteral("nope"));
    QCOMPARE(live.baudCombo()->currentText(), QStringLiteral("nope"));
    serial.connectButton()->click();
    QCOMPARE(live.statusLabel()->text(),
             QStringLiteral("Error connecting: baud rate must be an integer."));
    QVERIFY(live.interfaceCombo()->isEnabled());
}

void AntennaTrackerViewsTest::manualSlewAndTelemetryReadouts()
{
    TrackerFixture fixture;
    AntennaTrackerUIViewModel *vm = fixture.build();
    vm->setSelectedInterface(QStringLiteral("DegreeTracker"));
    vm->setLoopIntervalMs(1);
    fixture.telemetry->tracker = AntennaTrackerPosition(0.0, 0.0, 0.0);
    fixture.telemetry->trackerValid = true;
    fixture.telemetry->fix.valid = true;
    fixture.telemetry->fix.longitude = 0.01;
    fixture.telemetry->fix.altitudeAmsl = 100.0;
    AntennaTrackerUIView live(vm);

    live.manualModeCheck()->setChecked(true);
    QVERIFY(vm->manualMode());
    QVERIFY(live.manualAzimuthSlider()->isEnabled());
    live.manualAzimuthSlider()->setValue(45);
    live.manualElevationSlider()->setValue(-10);
    QCOMPARE(vm->manualAzimuth(), 45.0);
    QCOMPARE(vm->manualElevation(), -10.0);
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveManualAzimuthLabel"))->text(),
             QStringLiteral("45°"));
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveManualElevationLabel"))->text(),
             QStringLiteral("-10°"));

    live.connectButton()->click();
    QCOMPARE(fixture.service->state(), State::Connected);
    QTRY_VERIFY(vm->loopTicks() >= 1);
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveVehicleAzimuth"))->text(),
             QStringLiteral("90.0"));
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveCommandedAzimuth"))->text(),
             QStringLiteral("45.0"));
    QCOMPARE(live.findChild<QLabel *>(QStringLiteral("trkLiveCommandedElevation"))->text(),
             QStringLiteral("-10.0"));

    live.homeCenterButton()->click();
    QCOMPARE(live.manualAzimuthSlider()->value(), 0);
    QCOMPARE(live.manualElevationSlider()->value(), 0);
    QVERIFY(live.manualModeCheck()->isChecked()); // MP10 leaves manual mode alone
    live.manualModeCheck()->setChecked(false);
    QVERIFY(!live.manualAzimuthSlider()->isEnabled());
}

QTEST_MAIN(AntennaTrackerViewsTest)
#include "test_antennatrackerviews.moc"
