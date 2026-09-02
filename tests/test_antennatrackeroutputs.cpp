#include <QtTest>

#include "ui/configuration/AntennaTrackerOutputs.h"

#include <climits>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray result;
    for (const int value : values) {
        result.append(static_cast<char>(value));
    }
    return result;
}

AntennaTrackerCommand maestro(int command, int address, int low, int high)
{
    AntennaTrackerCommand result;
    result.payload = bytes({command, address, low, high});
    result.discardInputFirst = true;
    return result;
}

AntennaTrackerCommand text(const char *line)
{
    AntennaTrackerCommand result;
    result.payload = QByteArray(line);
    result.discardInputFirst = false;
    return result;
}

// The MP10 test configuration: pan -180..180, tilt -45..45, PWM 1000 around 1500.
std::unique_ptr<IAntennaTrackerOutput> configured(const QString &interfaceName)
{
    std::unique_ptr<IAntennaTrackerOutput> tracker =
        AntennaTrackerOutputFactory::Create(interfaceName);
    if (!tracker) {
        return nullptr;
    }
    tracker->setPanStartRange(-180);
    tracker->setPanEndRange(180);
    tracker->setTiltStartRange(-45);
    tracker->setTiltEndRange(45);
    tracker->setPanPwmRange(1000);
    tracker->setTiltPwmRange(1000);
    tracker->setPanPwmCenter(1500);
    tracker->setTiltPwmCenter(1500);
    return tracker;
}

struct RecordingWriter
{
    QList<AntennaTrackerCommand> commands;
    int discards = 0;
    int refuseAfter = -1; // refuse the write with this index and every later one

    IAntennaTrackerOutput::Writer writer()
    {
        return [this](const AntennaTrackerCommand &command) {
            if (refuseAfter >= 0 && commands.size() >= refuseAfter) {
                return false;
            }
            if (command.discardInputFirst) {
                ++discards;
            }
            commands.append(command);
            return true;
        };
    }
};

QList<AntennaTrackerCommand> commandsFor(const IAntennaTrackerOutput &tracker, double pan,
                                         double tilt, bool *ok = nullptr)
{
    QList<AntennaTrackerCommand> result;
    const bool produced = tracker.panAndTiltCommands(pan, tilt, &result);
    if (ok) {
        *ok = produced;
    }
    return result;
}

} // namespace

class AntennaTrackerOutputsTest final : public QObject
{
    Q_OBJECT

private slots:
    void factoryInventory();
    void maestroSetupAndTargetBytes();
    void maestroClampTrimAndWrap();
    void maestroReverse();
    void maestroTiltFlip();
    void arduTrackerText();
    void degreeTrackerText();
    void invalidSettingsAreRejectedBeforeOutput();
    void lifecycleAndWriterFailures();
    void arithmeticHelpers();
};

void AntennaTrackerOutputsTest::factoryInventory()
{
    const QStringList names{QStringLiteral("Maestro"), QStringLiteral("ArduTracker"),
                            QStringLiteral("DegreeTracker")};
    QCOMPARE(AntennaTrackerOutputFactory::InterfaceNames(), names);
    QCOMPARE(AntennaTrackerOutputFactory::Maestro(), names.at(0));
    QCOMPARE(AntennaTrackerOutputFactory::ArduTracker(), names.at(1));
    QCOMPARE(AntennaTrackerOutputFactory::DegreeTracker(), names.at(2));
    for (const QString &name : names) {
        const std::unique_ptr<IAntennaTrackerOutput> tracker =
            AntennaTrackerOutputFactory::Create(name);
        QVERIFY2(tracker != nullptr, qPrintable(name));
        QCOMPARE(tracker->interfaceName(), name);
        QVERIFY(!tracker->isInitialized());
        QVERIFY(!tracker->hasWriter());
        // C# property defaults.
        QCOMPARE(tracker->panStartRange(), 0);
        QCOMPARE(tracker->panPwmCenter(), 0);
        QCOMPARE(tracker->trimPan(), 0.0);
        QVERIFY(!tracker->panReverse());
        QVERIFY(!tracker->tiltReverse());
    }
    QVERIFY(dynamic_cast<MaestroAntennaTrackerOutput *>(
        AntennaTrackerOutputFactory::Create(QStringLiteral("Maestro")).get()));
    QVERIFY(dynamic_cast<ArduAntennaTrackerOutput *>(
        AntennaTrackerOutputFactory::Create(QStringLiteral("ArduTracker")).get()));
    QVERIFY(dynamic_cast<DegreeAntennaTrackerOutput *>(
        AntennaTrackerOutputFactory::Create(QStringLiteral("DegreeTracker")).get()));
    QVERIFY(AntennaTrackerOutputFactory::Create(QStringLiteral("maestro")) == nullptr);
    QVERIFY(AntennaTrackerOutputFactory::Create(QString()) == nullptr);
    QCOMPARE(AntennaTrackerOutputFactory::UnknownInterfaceText(),
             QStringLiteral("Unknown antenna tracker interface."));

    // Reverse flags round-trip on every protocol (MP10 fixed the WinForms getter bug).
    for (const QString &name : names) {
        const std::unique_ptr<IAntennaTrackerOutput> tracker =
            AntennaTrackerOutputFactory::Create(name);
        tracker->setPanReverse(true);
        tracker->setTiltReverse(true);
        QVERIFY(tracker->panReverse());
        QVERIFY(tracker->tiltReverse());
        tracker->setPanReverse(false);
        QVERIFY(!tracker->panReverse());
        QVERIFY(tracker->tiltReverse());
    }
}

void AntennaTrackerOutputsTest::maestroSetupAndTargetBytes()
{
    // MP10 Maestro_emits_official_compact_protocol_setup_and_position_commands.
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("Maestro"));
    QVERIFY(tracker);
    tracker->setPanSpeed(100);
    tracker->setTiltSpeed(80);
    tracker->setPanAccel(5);
    tracker->setTiltAccel(7);
    RecordingWriter recorder;
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->hasWriter());

    QString error = QStringLiteral("stale");
    QVERIFY(tracker->init(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(tracker->isInitialized());
    QVERIFY(tracker->setup());
    QVERIFY(tracker->panAndTilt(90, 30));

    const QList<AntennaTrackerCommand> expected{
        maestro(0x87, 0x00, 0x64, 0x00), maestro(0x87, 0x01, 0x50, 0x00),
        maestro(0x89, 0x00, 0x05, 0x00), maestro(0x89, 0x01, 0x07, 0x00),
        maestro(0x84, 0x01, 0x24, 0x39), maestro(0x84, 0x00, 0x58, 0x36)};
    QCOMPARE(recorder.commands, expected);
    QCOMPARE(recorder.discards, 6); // DiscardInBuffer() before every compact command
    QCOMPARE(tracker->setupCommands(), expected.mid(0, 4));
    QCOMPARE(commandsFor(*tracker, 90, 30), expected.mid(4));

    // Targets are quarter microseconds: 1833 * 4 and 1750 * 4.
    auto *maestroOutput = dynamic_cast<MaestroAntennaTrackerOutput *>(tracker.get());
    QVERIFY(maestroOutput);
    QCOMPARE(int(maestroOutput->tiltTarget(30)), 7332);
    QCOMPARE(int(maestroOutput->panTarget(90)), 7000);

    // Seven-bit packing of larger speeds, and the two's complement low bits of a
    // wrapped negative target.
    QCOMPARE(MaestroAntennaTrackerOutput::CompactCommand(0x87, 0, 300),
             maestro(0x87, 0x00, 0x2C, 0x02));
    QCOMPARE(MaestroAntennaTrackerOutput::CompactCommand(0x84, 1, -1),
             maestro(0x84, 0x01, 0x7F, 0x7F));
    QCOMPARE(MaestroAntennaTrackerOutput::CompactCommand(0x84, 1, 16383),
             maestro(0x84, 0x01, 0x7F, 0x7F));
    QCOMPARE(MaestroAntennaTrackerOutput::CompactCommand(0x84, 1, 16384),
             maestro(0x84, 0x01, 0x00, 0x00));
}

void AntennaTrackerOutputsTest::maestroClampTrimAndWrap()
{
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("Maestro"));
    QVERIFY(tracker);
    // Tilt beyond the 90 degree window clamps to center +/- PWMRange / 2.
    QCOMPARE(commandsFor(*tracker, 0, 80),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x40, 0x3E),
                                           maestro(0x84, 0x00, 0x70, 0x2E)}));
    QCOMPARE(commandsFor(*tracker, 0, -80),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x20, 0x1F),
                                           maestro(0x84, 0x00, 0x70, 0x2E)}));
    // Pan wraps once through +/-180 before the PWM mapping: 270 -> -90 -> 1250.
    QCOMPARE(commandsFor(*tracker, 270, 0),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x70, 0x2E),
                                           maestro(0x84, 0x00, 0x08, 0x27)}));
    // A narrower pan range clamps: -90..90 with pan 170 -> 2444 -> 2000.
    tracker->setPanStartRange(-90);
    tracker->setPanEndRange(90);
    QCOMPARE(commandsFor(*tracker, 170, 0).at(1), maestro(0x84, 0x00, 0x40, 0x3E));
    tracker->setPanStartRange(-180);
    tracker->setPanEndRange(180);

    // Trim shifts both axes: pan 100 - 10 -> 1750, tilt 20 + 5 -> 1777.
    tracker->setTrimPan(10);
    tracker->setTrimTilt(-5);
    QCOMPARE(commandsFor(*tracker, 100, 20),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x44, 0x37),
                                           maestro(0x84, 0x00, 0x58, 0x36)}));
    // Trim wraps the pan too: 350 - 10 = 340 -> -20 -> 1444.
    QCOMPARE(commandsFor(*tracker, 350, 0).at(1), maestro(0x84, 0x00, 0x10, 0x2D));

    // Fractional pulse widths truncate toward zero before the times-four packing.
    tracker->setTrimPan(0);
    tracker->setTrimTilt(0);
    auto *maestroOutput = dynamic_cast<MaestroAntennaTrackerOutput *>(tracker.get());
    QVERIFY(maestroOutput);
    QCOMPARE(int(maestroOutput->panTarget(1)), 1502 * 4);   // 1502.78
    QCOMPARE(int(maestroOutput->panTarget(-1)), 1497 * 4);  // 1497.22
    QCOMPARE(int(maestroOutput->tiltTarget(-1)), 1488 * 4); // 1488.89
}

void AntennaTrackerOutputsTest::maestroReverse()
{
    // MP10 Pwm_tracker_reverse_flags_round_trip_and_change_the_output (Maestro).
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("Maestro"));
    QVERIFY(tracker);
    tracker->setPanReverse(true);
    tracker->setTiltReverse(true);
    RecordingWriter recorder;
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->init());
    QVERIFY(tracker->panAndTilt(90, 30));
    QCOMPARE(recorder.commands,
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x38, 0x24),
                                           maestro(0x84, 0x00, 0x08, 0x27)}));

    // Only the reversed axis mirrors around its center.
    tracker->setTiltReverse(false);
    QCOMPARE(commandsFor(*tracker, 90, 30),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x24, 0x39),
                                           maestro(0x84, 0x00, 0x08, 0x27)}));
    // Reverse clamps on the other side of the window.
    QCOMPARE(commandsFor(*tracker, 170, 0).at(1), maestro(0x84, 0x00, 0x0C, 0x20)); // 1027
    tracker->setPanReverse(false);
    QCOMPARE(commandsFor(*tracker, 170, 0).at(1), maestro(0x84, 0x00, 0x50, 0x3D)); // 1972
}

void AntennaTrackerOutputsTest::maestroTiltFlip()
{
    // MP10 Maestro_preserves_official_180_degree_tilt_flip: pan 150 is behind the
    // tracker, so the tilt flips over the top (180 - 10 = 170 -> clamped 2000).
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("Maestro"));
    QVERIFY(tracker);
    tracker->setTiltStartRange(-90);
    tracker->setTiltEndRange(90);
    RecordingWriter recorder;
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->init());
    QVERIFY(tracker->panAndTilt(150, 10));
    QCOMPARE(recorder.commands,
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x40, 0x3E),
                                           maestro(0x84, 0x00, 0x70, 0x3B)}));

    // In front of the tracker nothing flips: tilt 10 -> 1555, pan 45 -> 1625.
    QCOMPARE(commandsFor(*tracker, 45, 10),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x4C, 0x30),
                                           maestro(0x84, 0x00, 0x64, 0x32)}));
    // Exactly 90 degrees does not flip either (strictly greater in MP10).
    QCOMPARE(commandsFor(*tracker, 90, 10).at(1), maestro(0x84, 0x00, 0x58, 0x36));
    // Behind and below: 180 - (-30) = 210 also clamps to 2000; pan -150 -> 1083.
    QCOMPARE(commandsFor(*tracker, -150, -30),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x40, 0x3E),
                                           maestro(0x84, 0x00, 0x6C, 0x21)}));
    // Wide flip without saturation: tilt 180 - 120 = 60 -> 1833, pan 150 -> 1916.
    QCOMPARE(commandsFor(*tracker, 150, 120),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x24, 0x39),
                                           maestro(0x84, 0x00, 0x70, 0x3B)}));

    // MP10 applies the pan trim twice on the flipped side (once for the flip test
    // and again inside Pan): trim 20, pan 150 -> target 130 -> 110 -> 1805.
    tracker->setTrimPan(20);
    QCOMPARE(commandsFor(*tracker, 150, 10).at(1), maestro(0x84, 0x00, 0x34, 0x38));
    tracker->setTrimPan(0);

    // A tilt range of 120 or less never flips, whatever the pan.
    tracker->setTiltStartRange(-60);
    tracker->setTiltEndRange(60);
    QCOMPARE(commandsFor(*tracker, 150, 10),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x3C, 0x31),   // 1583
                                           maestro(0x84, 0x00, 0x70, 0x3B)}));
    tracker->setTiltStartRange(-45);
    tracker->setTiltEndRange(45);
    QCOMPARE(commandsFor(*tracker, 150, 10),
             (QList<AntennaTrackerCommand>{maestro(0x84, 0x01, 0x2C, 0x32),   // 1611
                                           maestro(0x84, 0x00, 0x70, 0x3B)}));
}

void AntennaTrackerOutputsTest::arduTrackerText()
{
    // MP10 ArduTracker_emits_official_pwm_text_protocol_with_trim.
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("ArduTracker"));
    QVERIFY(tracker);
    tracker->setTrimPan(10);
    tracker->setTrimTilt(-5);
    RecordingWriter recorder;
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->init());
    QVERIFY(tracker->setupCommands().isEmpty());
    QVERIFY(tracker->setup()); // nothing to write for the text protocols
    QVERIFY(tracker->panAndTilt(100, 20));
    QCOMPARE(recorder.commands, (QList<AntennaTrackerCommand>{text("!!!PAN:1750,TLT:1777\n")}));
    QCOMPARE(recorder.discards, 0);

    // MP10 Pwm_tracker_reverse_flags_round_trip_and_change_the_output (ArduTracker).
    tracker->setTrimPan(0);
    tracker->setTrimTilt(0);
    tracker->setPanReverse(true);
    tracker->setTiltReverse(true);
    QCOMPARE(commandsFor(*tracker, 90, 30),
             (QList<AntennaTrackerCommand>{text("!!!PAN:1250,TLT:1166\n")}));
    tracker->setPanReverse(false);
    tracker->setTiltReverse(false);

    // Angles are clamped to the range before the mapping: pan 200 wraps to -160,
    // tilt 80 clamps to 45 -> 2000.
    QCOMPARE(commandsFor(*tracker, 200, 80),
             (QList<AntennaTrackerCommand>{text("!!!PAN:1055,TLT:2000\n")}));
    QCOMPARE(commandsFor(*tracker, 0, -80),
             (QList<AntennaTrackerCommand>{text("!!!PAN:1500,TLT:1000\n")}));
    auto *arduOutput = dynamic_cast<ArduAntennaTrackerOutput *>(tracker.get());
    QVERIFY(arduOutput);
    // The angle truncates to a whole degree (short) before the PWM mapping.
    QCOMPARE(arduOutput->panPwm(0.9), 1500);
    QCOMPARE(arduOutput->panPwm(-0.9), 1500);
    QCOMPARE(arduOutput->tiltPwm(44.9), 1988); // 44 / 90 * 1000
    // MP10 halves the PWM range with integer division: 999 / 2 = 499.
    tracker->setTiltPwmRange(999);
    QCOMPARE(arduOutput->tiltPwm(30), 1832); // 30 / 90 * 2 * 499 = 332.67
    tracker->setTiltPwmRange(1000);
    // The pan center moves with the setting.
    tracker->setPanPwmCenter(1520);
    QCOMPARE(commandsFor(*tracker, 0, 0),
             (QList<AntennaTrackerCommand>{text("!!!PAN:1520,TLT:1500\n")}));
}

void AntennaTrackerOutputsTest::degreeTrackerText()
{
    // MP10 DegreeTracker_emits_official_tenths_of_a_degree_text_protocol.
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("DegreeTracker"));
    QVERIFY(tracker);
    RecordingWriter recorder;
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->init());
    QVERIFY(tracker->setup());
    QVERIFY(tracker->panAndTilt(12.34, -5.67));
    QCOMPARE(recorder.commands, (QList<AntennaTrackerCommand>{text("!!!PAN:0123,TLT:-0056\n")}));
    QCOMPARE(recorder.discards, 0);

    // Negative tenths truncate toward zero, never round away.
    QCOMPARE(commandsFor(*tracker, -0.09, -179.99),
             (QList<AntennaTrackerCommand>{text("!!!PAN:0000,TLT:-1799\n")}));
    QCOMPARE(commandsFor(*tracker, -12.36, -0.5),
             (QList<AntennaTrackerCommand>{text("!!!PAN:-0123,TLT:-0005\n")}));
    // Wider values keep every digit; there is no wrap or clamp.
    QCOMPARE(commandsFor(*tracker, 1234.56, 359.99),
             (QList<AntennaTrackerCommand>{text("!!!PAN:12345,TLT:3599\n")}));
    QCOMPARE(commandsFor(*tracker, 0, 0),
             (QList<AntennaTrackerCommand>{text("!!!PAN:0000,TLT:0000\n")}));

    // PWM, range, trim and reverse settings never touch the degree output.
    tracker->setTrimPan(30);
    tracker->setTrimTilt(-10);
    tracker->setPanReverse(true);
    tracker->setTiltReverse(true);
    tracker->setPanPwmRange(0);
    tracker->setPanStartRange(0);
    tracker->setPanEndRange(0);
    tracker->setTiltStartRange(0);
    tracker->setTiltEndRange(0);
    QCOMPARE(commandsFor(*tracker, 12.34, -5.67),
             (QList<AntennaTrackerCommand>{text("!!!PAN:0123,TLT:-0056\n")}));
    // ... and zero ranges are not an error for it (MP10 has no validation here).
    QString error;
    QVERIFY(tracker->init(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(tracker->validateConfiguration().isEmpty());
}

void AntennaTrackerOutputsTest::invalidSettingsAreRejectedBeforeOutput()
{
    // MP10 Pwm_trackers_reject_zero_ranges_before_opening_the_port.
    const QStringList pwmTrackers{QStringLiteral("Maestro"), QStringLiteral("ArduTracker")};
    for (const QString &name : pwmTrackers) {
        std::unique_ptr<IAntennaTrackerOutput> tracker = configured(name);
        QVERIFY2(tracker, qPrintable(name));
        RecordingWriter recorder;
        tracker->setWriter(recorder.writer());

        tracker->setPanStartRange(0);
        tracker->setPanEndRange(0);
        QString error;
        QVERIFY(!tracker->init(&error));
        QCOMPARE(error, QStringLiteral("Invalid pan range."));
        QCOMPARE(tracker->validateConfiguration(), QStringLiteral("Invalid pan range."));
        QVERIFY(!tracker->isInitialized());
        QVERIFY(!tracker->setup());
        QVERIFY(!tracker->panAndTilt(0, 0));
        QList<AntennaTrackerCommand> untouched{text("keep")};
        QVERIFY(!tracker->panAndTiltCommands(0, 0, &untouched));
        QCOMPARE(untouched, (QList<AntennaTrackerCommand>{text("keep")}));
        QVERIFY(recorder.commands.isEmpty());

        // Any equal pair is a zero range, not only 0..0; the pan check comes first.
        tracker->setPanStartRange(90);
        tracker->setPanEndRange(90);
        tracker->setTiltStartRange(-45);
        tracker->setTiltEndRange(-45);
        QVERIFY(!tracker->init(&error));
        QCOMPARE(error, QStringLiteral("Invalid pan range."));
        tracker->setPanStartRange(-180);
        tracker->setPanEndRange(180);
        QVERIFY(!tracker->init(&error));
        QCOMPARE(error, QStringLiteral("Invalid tilt range."));
        QVERIFY(recorder.commands.isEmpty());

        // MP10 accepts reversed endpoints but its Constrain(min, max) then
        // drives the output to an unexpected extreme. Qt fails closed.
        tracker->setTiltStartRange(45);
        tracker->setTiltEndRange(-45);
        QVERIFY(!tracker->init(&error));
        QCOMPARE(error, QStringLiteral("Invalid tilt range."));
        QVERIFY(recorder.commands.isEmpty());
        tracker->setTiltStartRange(-45);
        tracker->setTiltEndRange(45);
        QVERIFY(tracker->init(&error));
        QVERIFY(error.isEmpty());

        // A range zeroed after init() stops the output instead of dividing by zero.
        tracker->setTiltStartRange(45);
        tracker->setTiltEndRange(45);
        const int written = recorder.commands.size();
        QVERIFY(!tracker->panAndTilt(0, 0));
        QVERIFY(!tracker->panAndTiltCommands(0, 0, nullptr));
        QCOMPARE(recorder.commands.size(), written);
        tracker->setTiltStartRange(-45);
        tracker->setTiltEndRange(45);

        // Non-finite angles never reach the wire.
        QVERIFY(!tracker->panAndTilt(std::nan(""), 0));
        QVERIFY(!tracker->panAndTilt(0, std::numeric_limits<double>::infinity()));
        QVERIFY(!tracker->panAndTilt(-std::numeric_limits<double>::infinity(), 0));
        QCOMPARE(recorder.commands.size(), written);
        QVERIFY(tracker->panAndTilt(0, 0));
        QCOMPARE(recorder.commands.size(), written + (name == QStringLiteral("Maestro") ? 2 : 1));
    }

    std::unique_ptr<IAntennaTrackerOutput> degree = configured(QStringLiteral("DegreeTracker"));
    QVERIFY(degree);
    degree->setWriter([](const AntennaTrackerCommand &) { return true; });
    QVERIFY(degree->init());
    QVERIFY(!degree->panAndTiltCommands(std::nan(""), 0, nullptr));
    QVERIFY(!degree->panAndTiltCommands(0, std::numeric_limits<double>::infinity(), nullptr));
    QVERIFY(degree->panAndTiltCommands(0, 0, nullptr));
}

void AntennaTrackerOutputsTest::lifecycleAndWriterFailures()
{
    std::unique_ptr<IAntennaTrackerOutput> tracker = configured(QStringLiteral("Maestro"));
    QVERIFY(tracker);
    RecordingWriter recorder;

    // Nothing is written before init(), with or without a writer.
    QVERIFY(!tracker->setup());
    QVERIFY(!tracker->panAndTilt(0, 0));
    tracker->setWriter(recorder.writer());
    QVERIFY(!tracker->setup());
    QVERIFY(!tracker->panAndTilt(0, 0));
    QVERIFY(recorder.commands.isEmpty());

    // A validated codec is not considered initialized until an actual writer
    // is installed by the owning serial service.
    tracker->setWriter(nullptr);
    QVERIFY(!tracker->hasWriter());
    QString error;
    QVERIFY(!tracker->init(&error));
    QCOMPARE(error, QStringLiteral("Antenna tracker output writer is unavailable."));
    QVERIFY(!tracker->isInitialized());
    QVERIFY(!tracker->setup());
    QVERIFY(!tracker->panAndTilt(0, 0));
    tracker->setWriter(recorder.writer());
    QVERIFY(tracker->init(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(tracker->setup());
    QCOMPARE(recorder.commands.size(), 4);

    // close() refuses later commands until init() runs again (MP10 Close/Init).
    tracker->close();
    QVERIFY(!tracker->isInitialized());
    QVERIFY(!tracker->panAndTilt(0, 0));
    QCOMPARE(recorder.commands.size(), 4);
    QVERIFY(tracker->init());
    QVERIFY(tracker->panAndTilt(0, 0));
    QCOMPARE(recorder.commands.size(), 6);
    QCOMPARE(recorder.commands.at(4), maestro(0x84, 0x01, 0x70, 0x2E)); // tilt first
    QCOMPARE(recorder.commands.at(5), maestro(0x84, 0x00, 0x70, 0x2E)); // then pan

    // A refused write stops the sequence: the pan command is not attempted.
    recorder.refuseAfter = 7;
    QVERIFY(!tracker->panAndTilt(10, 10));
    QCOMPARE(recorder.commands.size(), 7);
    QCOMPARE(recorder.commands.at(6), maestro(0x84, 0x01, 0x2C, 0x32)); // 1611
    recorder.refuseAfter = -1;
    QVERIFY(tracker->panAndTilt(10, 10));
    QCOMPARE(recorder.commands.size(), 9);

    // A refused setup command stops the setup and reports the failure.
    recorder.refuseAfter = 10;
    QVERIFY(!tracker->setup());
    QCOMPARE(recorder.commands.size(), 10);
    QCOMPARE(recorder.commands.at(9), maestro(0x87, 0x00, 0x00, 0x00));

    // The text protocols behave the same way through the writer.
    std::unique_ptr<IAntennaTrackerOutput> degree = configured(QStringLiteral("DegreeTracker"));
    RecordingWriter degreeRecorder;
    degree->setWriter(degreeRecorder.writer());
    QVERIFY(!degree->panAndTilt(1, 2));
    QVERIFY(degree->init());
    degreeRecorder.refuseAfter = 0;
    QVERIFY(!degree->panAndTilt(1, 2));
    QVERIFY(degreeRecorder.commands.isEmpty());
    degreeRecorder.refuseAfter = -1;
    QVERIFY(degree->panAndTilt(1, 2));
    QCOMPARE(degreeRecorder.commands, (QList<AntennaTrackerCommand>{text("!!!PAN:0010,TLT:0020\n")}));

    // Writer failures must never unwind through a Qt event handler.
    degree->setWriter([](const AntennaTrackerCommand &) -> bool {
        throw std::runtime_error("serial unplugged");
    });
    QVERIFY(!degree->panAndTilt(3, 4));

    // A synchronous close from the first Maestro write stops the pair before
    // the pan command is handed to the writer.
    std::unique_ptr<IAntennaTrackerOutput> reentrant = configured(QStringLiteral("Maestro"));
    int reentrantWrites = 0;
    reentrant->setWriter([&reentrant, &reentrantWrites](const AntennaTrackerCommand &) {
        ++reentrantWrites;
        reentrant->close();
        return true;
    });
    QVERIFY(reentrant->init());
    QVERIFY(!reentrant->panAndTilt(10, 10));
    QCOMPARE(reentrantWrites, 1);
}

void AntennaTrackerOutputsTest::arithmeticHelpers()
{
    QCOMPARE(IAntennaTrackerOutput::Wrap180(181), -179.0);
    QCOMPARE(IAntennaTrackerOutput::Wrap180(-181), 179.0);
    QCOMPARE(IAntennaTrackerOutput::Wrap180(180), 180.0);
    QCOMPARE(IAntennaTrackerOutput::Wrap180(-180), -180.0);
    QCOMPARE(IAntennaTrackerOutput::Wrap180(540), 180.0); // MP10 wraps once only
    QCOMPARE(IAntennaTrackerOutput::Wrap180(-541), -181.0);

    QCOMPARE(int(IAntennaTrackerOutput::Constrain(1833.33, 1000, 2000)), 1833);
    QCOMPARE(int(IAntennaTrackerOutput::Constrain(2444.4, 1000, 2000)), 2000);
    QCOMPARE(int(IAntennaTrackerOutput::Constrain(999.7, 999.5, 2000.5)), 999);
    QCOMPARE(int(IAntennaTrackerOutput::Constrain(-5, 999.5, 2000.5)), 999); // (short)999.5
    QCOMPARE(int(IAntennaTrackerOutput::Constrain(-0.9, -180, 180)), 0);

    QCOMPARE(IAntennaTrackerOutput::ToInt32(-56.7), -56);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(123.9), 123);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(-0.999), 0);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(std::nan("")), 0);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(1e12), INT_MAX);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(-1e12), INT_MIN);
    QCOMPARE(IAntennaTrackerOutput::ToInt32(2147483647.5), INT_MAX);
    QCOMPARE(int(IAntennaTrackerOutput::ToInt16(40000)), int(SHRT_MAX));
    QCOMPARE(int(IAntennaTrackerOutput::ToInt16(-40000)), int(SHRT_MIN));
    QCOMPARE(int(IAntennaTrackerOutput::ToInt16(-32768.9)), int(SHRT_MIN));
    QCOMPARE(int(IAntennaTrackerOutput::ToInt16(std::nan(""))), 0);

    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(0), QStringLiteral("0000"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(7), QStringLiteral("0007"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(-56), QStringLiteral("-0056"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(1750), QStringLiteral("1750"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(12345), QStringLiteral("12345"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(-12345), QStringLiteral("-12345"));
    QCOMPARE(IAntennaTrackerOutput::FormatFourDigits(INT_MIN), QStringLiteral("-2147483648"));
    QCOMPARE(IAntennaTrackerOutput::FormatPanTiltLine(1750, 1777),
             QByteArrayLiteral("!!!PAN:1750,TLT:1777\n"));
    QCOMPARE(IAntennaTrackerOutput::FormatPanTiltLine(123, -56),
             QByteArrayLiteral("!!!PAN:0123,TLT:-0056\n"));

    QCOMPARE(IAntennaTrackerOutput::InvalidPanRangeText(), QStringLiteral("Invalid pan range."));
    QCOMPARE(IAntennaTrackerOutput::InvalidTiltRangeText(), QStringLiteral("Invalid tilt range."));
    QCOMPARE(IAntennaTrackerOutput::WriterUnavailableText(),
             QStringLiteral("Antenna tracker output writer is unavailable."));
}

QTEST_APPLESS_MAIN(AntennaTrackerOutputsTest)
#include "test_antennatrackeroutputs.moc"
