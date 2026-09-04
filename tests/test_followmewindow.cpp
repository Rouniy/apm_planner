#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/FollowMeGpsInput.h"
#include "comm/GuidedTargetService.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/NmeaGgaParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "ui/FollowMeWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVector>

#include <functional>

#include <mavlink.h>

namespace
{
QByteArray ggaSentence(
    const QString &latitude = QStringLiteral("4807.038"),
    const QString &northSouth = QStringLiteral("N"),
    const QString &longitude = QStringLiteral("01131.000"),
    const QString &eastWest = QStringLiteral("E"),
    int quality = 1, double altitudeMsl = 545.4)
{
    const QString body = QStringLiteral(
        "GPGGA,123519,%1,%2,%3,%4,%5,08,0.9,%6,M,46.9,M,,")
        .arg(latitude, northSouth, longitude, eastWest)
        .arg(quality)
        .arg(altitudeMsl, 0, 'f', 1);
    const QString sentence = QLatin1Char('$') + body;
    return (sentence + QLatin1Char('*')
            + NmeaGgaParser::checksum(sentence)
            + QStringLiteral("\r\n")).toLatin1();
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

class FakeGpsByteSource final : public FollowMeGpsByteSource
{
public:
    explicit FakeGpsByteSource(QObject *parent = nullptr)
        : FollowMeGpsByteSource(parent)
    {
    }

    bool open(const QString &portName, int baud, QString *error) override
    {
        lastPort = portName;
        lastBaud = baud;
        if (onOpen) {
            onOpen();
        }
        if (!openSucceeds) {
            if (error) {
                *error = QStringLiteral("synthetic open failure");
            }
            return false;
        }
        opened = true;
        return true;
    }

    void close() override
    {
        opened = false;
        ++closeCount;
    }

    bool isOpen() const override { return opened; }

    void deliver(const QByteArray &bytes) { emit bytesReceived(bytes); }
    void fail(const QString &error) { emit resourceError(error); }

    bool openSucceeds = true;
    bool opened = false;
    int closeCount = 0;
    int lastBaud = 0;
    QString lastPort;
    std::function<void()> onOpen;
};

struct InputHarness
{
    FollowMeWindow::GpsInputFactory factory()
    {
        return [this](QObject *parent) {
            return new FollowMeGpsInput(
                [this](QObject *sourceParent) {
                    auto *created = new FakeGpsByteSource(sourceParent);
                    created->openSucceeds = openSucceeds;
                    created->onOpen = onOpen;
                    source = created;
                    return created;
                },
                []() { return QStringList{QStringLiteral("TEST-NMEA")}; },
                parent);
        };
    }

    QPointer<FakeGpsByteSource> source;
    bool openSucceeds = true;
    std::function<void()> onOpen;
};

struct GuidedRig
{
    QVector<QByteArray> frames;
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    GuidedTargetService guided;
    VehicleEndpoint endpoint;

    GuidedRig()
        : transmitter([this](int, const QByteArray &frame) {
            frames.append(frame);
            return true;
        })
        , commands(&targets, &transmitter)
        , guided(&targets, &commands)
    {
        endpoint.linkId = 7;
        endpoint.systemId = 42;
        endpoint.componentId = 1;
        endpoint.linkName = QStringLiteral("Test modem");
        endpoint.componentName = QStringLiteral("AUTOPILOT1");
        targets.observeEndpoint(endpoint, true);
        targets.observeHeartbeat(
            endpoint, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        guided.setCommandTimeoutForTesting(10000);
    }

    void acknowledge(int result = MAV_RESULT_ACCEPTED)
    {
        mavlink_command_ack_t ack{};
        ack.command = MAV_CMD_DO_REPOSITION;
        ack.result = static_cast<quint8>(result);
        ack.progress = 100;
        ack.target_system = 255;
        ack.target_component = MAV_COMP_ID_MISSIONPLANNER;
        mavlink_message_t message{};
        mavlink_msg_command_ack_encode(
            static_cast<quint8>(endpoint.systemId),
            static_cast<quint8>(endpoint.componentId),
            &message, &ack);
        commands.observeMessage(endpoint.linkId, message);
    }

    mavlink_command_int_t commandAt(int index) const
    {
        mavlink_command_int_t command{};
        const mavlink_message_t message = decodeFrame(frames.at(index));
        if (message.msgid == MAVLINK_MSG_ID_COMMAND_INT) {
            mavlink_msg_command_int_decode(&message, &command);
        }
        return command;
    }
};

FollowMeWindow::Dependencies dependenciesFor(
    GuidedRig *rig, InputHarness *input = nullptr)
{
    FollowMeWindow::Dependencies dependencies;
    dependencies.targetManager = &rig->targets;
    dependencies.guidedService = &rig->guided;
    dependencies.enumeratePorts = []() {
        return QStringList{QStringLiteral("TEST-NMEA")};
    };
    if (input) {
        dependencies.gpsInputFactory = input->factory();
    }
    dependencies.confirmStart = [](
        QWidget *, const QString &, const QString &) { return true; };
    return dependencies;
}

QPushButton *toggle(FollowMeWindow *window)
{
    return window->findChild<QPushButton *>(
        QStringLiteral("ToggleFollowMeButton"));
}
}

class FollowMeWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void rendersMp10ContractAndIndependentWindows();
    void confirmationFailsClosedAndTargetIsRechecked();
    void serialReservesBeforeOpenAndIgnoresGgaAltitude();
    void serialOpenFailureReleasesReservation();
    void nullIslandIsRejectedButIndividualZeroAxesAreAccepted();
    void staleAndNoFixWithholdUpdates();
    void exclusiveOwnerTargetChangeAndInputErrorStopSafely();
};

void FollowMeWindowTest::rendersMp10ContractAndIndependentWindows()
{
    GuidedRig rig;
    InputHarness input;
    const FollowMeWindow::Dependencies dependencies =
        dependenciesFor(&rig, &input);
    auto *first = new FollowMeWindow(dependencies);
    auto *second = new FollowMeWindow(dependencies);
    QPointer<FollowMeWindow> firstGuard(first);
    QPointer<FollowMeWindow> secondGuard(second);

    QCOMPARE(first->objectName(), QStringLiteral("FollowMeWindow"));
    QCOMPARE(first->windowTitle(), QStringLiteral("Follow Me"));
    QCOMPARE(first->windowModality(), Qt::NonModal);
    QVERIFY(first->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(first->size(), QSize(FollowMeWindow::WindowWidth,
                                  FollowMeWindow::WindowHeight));
    QCOMPARE(first->statusText(), QStringLiteral("Stopped."));
    QCOMPARE(first->locationText(),
             QStringLiteral("No target position received."));

    const QStringList requiredControls = {
        QStringLiteral("followMeHeader"),
        QStringLiteral("followMeDescription"),
        QStringLiteral("followMeTargetDescription"),
        QStringLiteral("followMeManualSource"),
        QStringLiteral("followMeManualLat"),
        QStringLiteral("followMeManualLng"),
        QStringLiteral("followMeUse"),
        QStringLiteral("followMeSerialSource"),
        QStringLiteral("followMeSerialPort"),
        QStringLiteral("followMeRefreshPorts"),
        QStringLiteral("followMeBaud"),
        QStringLiteral("followMeRelativeAltitude"),
        QStringLiteral("followMeRate"),
        QStringLiteral("ToggleFollowMeButton"),
        QStringLiteral("followMeStatusPanel"),
        QStringLiteral("followMeStatus"),
        QStringLiteral("followMeLocationLabel")
    };
    for (const QString &name : requiredControls) {
        QVERIFY2(first->findChild<QWidget *>(name), qPrintable(name));
    }
    QVERIFY(first->findChild<QRadioButton *>(
        QStringLiteral("followMeManualSource"))->isChecked());
    QCOMPARE(first->findChild<QComboBox *>(
                 QStringLiteral("followMeBaud"))->currentData().toInt(),
             FollowMeWindow::DefaultBaud);
    QCOMPARE(first->findChild<QComboBox *>(
                 QStringLiteral("followMeRate"))->currentData().toDouble(),
             FollowMeWindow::DefaultRateHz);
    QCOMPARE(first->findChild<QDoubleSpinBox *>(
                 QStringLiteral("followMeRelativeAltitude"))->value(),
             FollowMeWindow::DefaultRelativeAltitudeM);
    QCOMPARE(FollowMeWindow::updateIntervalMs(0.25), 4000);
    QCOMPARE(FollowMeWindow::updateIntervalMs(2.0), 500);
    QCOMPARE(FollowMeWindow::maximumFixAgeMs(0.25), qint64(12000));
    QCOMPARE(FollowMeWindow::maximumFixAgeMs(2.0), qint64(5000));

    first->show();
    second->show();
    QVERIFY(first != second && first->isVisible() && second->isVisible());
    first->close();
    QTRY_VERIFY(firstGuard.isNull());
    QVERIFY(secondGuard && secondGuard->isVisible());
    second->close();
    QTRY_VERIFY(secondGuard.isNull());
}

void FollowMeWindowTest::confirmationFailsClosedAndTargetIsRechecked()
{
    GuidedRig rig;
    FollowMeWindow::Dependencies dependencies = dependenciesFor(&rig);
    dependencies.confirmStart = {};
    FollowMeWindow rejected(dependencies);
    rejected.setAttribute(Qt::WA_DeleteOnClose, false);
    rejected.findChild<QDoubleSpinBox *>(
        QStringLiteral("followMeManualLng"))->setValue(10.0);
    toggle(&rejected)->click();
    QVERIFY(rejected.statusText().contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
    QVERIFY(!rejected.isRunning());
    QVERIFY(!rig.guided.hasActiveSession());
    QCOMPARE(rig.frames.size(), 0);

    VehicleEndpoint replacement = rig.endpoint;
    replacement.linkId = 8;
    replacement.linkName = QStringLiteral("Other modem");
    rig.targets.observeEndpoint(replacement, false);
    dependencies.confirmStart = [&rig, replacement](
        QWidget *, const QString &, const QString &) {
        rig.targets.selectTarget(
            replacement.linkId, replacement.systemId,
            replacement.componentId);
        rig.targets.observeHeartbeat(
            replacement, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        return true;
    };
    FollowMeWindow switched(dependencies);
    switched.setAttribute(Qt::WA_DeleteOnClose, false);
    switched.findChild<QDoubleSpinBox *>(
        QStringLiteral("followMeManualLng"))->setValue(10.0);
    toggle(&switched)->click();
    QVERIFY(!switched.isRunning());
    QVERIFY(switched.statusText().contains(
        QStringLiteral("changed"), Qt::CaseInsensitive));
    QVERIFY(!rig.guided.hasActiveSession());
    QCOMPARE(rig.frames.size(), 0);
}

void FollowMeWindowTest::serialReservesBeforeOpenAndIgnoresGgaAltitude()
{
    GuidedRig rig;
    InputHarness input;
    bool reservedBeforeOpen = false;
    input.onOpen = [&rig, &reservedBeforeOpen]() {
        reservedBeforeOpen = rig.guided.hasActiveSession()
            && rig.guided.state() == GuidedTargetService::State::Reserved;
    };
    FollowMeWindow window(dependenciesFor(&rig, &input));
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.findChild<QRadioButton *>(
        QStringLiteral("followMeSerialSource"))->setChecked(true);

    toggle(&window)->click();

    QVERIFY(window.isRunning());
    QVERIFY(reservedBeforeOpen);
    QVERIFY(input.source && input.source->isOpen());
    QCOMPARE(input.source->lastPort, QStringLiteral("TEST-NMEA"));
    QCOMPARE(input.source->lastBaud, 4800);
    QCOMPARE(rig.frames.size(), 0);
    QCoreApplication::processEvents();
    QVERIFY(window.statusText().contains(
        QStringLiteral("TEST-NMEA"), Qt::CaseInsensitive));
    QVERIFY(window.statusText().contains(
        QStringLiteral("valid GGA fix"), Qt::CaseInsensitive));

    input.source->deliver(ggaSentence());
    QTRY_COMPARE(rig.frames.size(), 1);
    const mavlink_command_int_t command = rig.commandAt(0);
    QCOMPARE(command.command, quint16(MAV_CMD_DO_REPOSITION));
    QCOMPARE(command.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    // The shared guided sender matches MP10's int cast and truncates E7.
    QCOMPARE(command.x, qint32(481172999));
    QCOMPARE(command.y, qint32(115166666));
    QCOMPARE(command.z, 100.0F); // GGA carries 545.4 m AMSL; it is ignored.
    QVERIFY(window.locationText().contains(QStringLiteral("sats 8")));

    toggle(&window)->click();
    QVERIFY(!window.isRunning());
    const int framesAfterStop = rig.frames.size();
    if (input.source) {
        input.source->deliver(ggaSentence(
            QStringLiteral("4900.000"), QStringLiteral("N")));
    }
    QCoreApplication::processEvents();
    QCOMPARE(rig.frames.size(), framesAfterStop);
}

void FollowMeWindowTest::
serialOpenFailureReleasesReservation()
{
    GuidedRig rig;
    InputHarness input;
    input.openSucceeds = false;
    FollowMeWindow window(dependenciesFor(&rig, &input));
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.findChild<QRadioButton *>(
        QStringLiteral("followMeSerialSource"))->setChecked(true);

    toggle(&window)->click();

    QVERIFY(!window.isRunning());
    QVERIFY(!rig.guided.hasActiveSession());
    QCOMPARE(rig.guided.state(), GuidedTargetService::State::Idle);
    QCOMPARE(rig.frames.size(), 0);
    QVERIFY(window.statusText().contains(
        QStringLiteral("synthetic open failure"), Qt::CaseInsensitive));
}

void FollowMeWindowTest::
nullIslandIsRejectedButIndividualZeroAxesAreAccepted()
{
    GuidedRig rig;
    int confirmationCount = 0;
    FollowMeWindow::Dependencies dependencies = dependenciesFor(&rig);
    dependencies.confirmStart = [&confirmationCount](
        QWidget *, const QString &, const QString &) {
        ++confirmationCount;
        return true;
    };
    FollowMeWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *latitude = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("followMeManualLat"));
    auto *longitude = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("followMeManualLng"));

    toggle(&window)->click();
    QVERIFY(!window.isRunning());
    QCOMPARE(rig.frames.size(), 0);
    QVERIFY(window.statusText().contains(
        QStringLiteral("Null Island"), Qt::CaseInsensitive));
    QCOMPARE(confirmationCount, 0);

    longitude->setValue(10.0);
    toggle(&window)->click();
    QVERIFY(window.isRunning());
    QCOMPARE(confirmationCount, 1);
    QCOMPARE(rig.frames.size(), 1);
    mavlink_command_int_t command = rig.commandAt(0);
    QCOMPARE(command.x, qint32(0));
    QCOMPARE(command.y, qint32(100000000));
    rig.acknowledge();
    QCoreApplication::processEvents();
    toggle(&window)->click();
    QVERIFY(!window.isRunning());

    longitude->setValue(0.0);
    latitude->setValue(10.0);
    toggle(&window)->click();
    QVERIFY(window.isRunning());
    QCOMPARE(confirmationCount, 2);
    QCOMPARE(rig.frames.size(), 2);
    command = rig.commandAt(1);
    QCOMPARE(command.x, qint32(100000000));
    QCOMPARE(command.y, qint32(0));
    rig.acknowledge();
    QCoreApplication::processEvents();
    toggle(&window)->click();
}

void FollowMeWindowTest::staleAndNoFixWithholdUpdates()
{
    GuidedRig rig;
    InputHarness input;
    qint64 now = 100;
    FollowMeWindow::Dependencies dependencies = dependenciesFor(&rig, &input);
    dependencies.monotonicMs = [&now]() { return now; };
    FollowMeWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.findChild<QRadioButton *>(
        QStringLiteral("followMeSerialSource"))->setChecked(true);
    auto *rate = window.findChild<QComboBox *>(QStringLiteral("followMeRate"));
    rate->setCurrentIndex(rate->findData(2.0));
    toggle(&window)->click();
    input.source->deliver(ggaSentence());
    QTRY_COMPARE(rig.frames.size(), 1);

    now = 5201;
    QMetaObject::invokeMethod(
        window.findChild<QTimer *>(QStringLiteral("followMeUpdateTimer")),
        "timeout", Qt::DirectConnection);
    QCOMPARE(rig.frames.size(), 1);
    QVERIFY(window.statusText().contains(
        QStringLiteral("stale"), Qt::CaseInsensitive));

    input.source->deliver(ggaSentence(
        QStringLiteral(""), QStringLiteral("N"),
        QStringLiteral(""), QStringLiteral("E"), 0));
    QTRY_VERIFY(window.statusText().contains(
        QStringLiteral("no position fix"), Qt::CaseInsensitive));
    QMetaObject::invokeMethod(
        window.findChild<QTimer *>(QStringLiteral("followMeUpdateTimer")),
        "timeout", Qt::DirectConnection);
    QCOMPARE(rig.frames.size(), 1);
    toggle(&window)->click();
}

void FollowMeWindowTest::
exclusiveOwnerTargetChangeAndInputErrorStopSafely()
{
    GuidedRig rig;
    InputHarness firstInput;
    InputHarness secondInput;
    FollowMeWindow first(dependenciesFor(&rig, &firstInput));
    FollowMeWindow second(dependenciesFor(&rig, &secondInput));
    first.setAttribute(Qt::WA_DeleteOnClose, false);
    second.setAttribute(Qt::WA_DeleteOnClose, false);
    first.findChild<QRadioButton *>(
        QStringLiteral("followMeSerialSource"))->setChecked(true);
    second.findChild<QDoubleSpinBox *>(
        QStringLiteral("followMeManualLng"))->setValue(10.0);

    toggle(&first)->click();
    QVERIFY(first.isRunning());
    QCOMPARE(rig.guided.state(), GuidedTargetService::State::Reserved);
    toggle(&second)->click();
    QVERIFY(!second.isRunning());
    QVERIFY(second.statusText().contains(
        QStringLiteral("another window"), Qt::CaseInsensitive));
    QVERIFY(!secondInput.source);

    firstInput.source->fail(QStringLiteral("synthetic GPS failure"));
    QVERIFY(!first.isRunning());
    QVERIFY(first.statusText().contains(
        QStringLiteral("synthetic GPS failure"), Qt::CaseInsensitive));
    QVERIFY(!rig.guided.hasActiveSession());

    toggle(&first)->click();
    QVERIFY(first.isRunning());
    VehicleEndpoint replacement = rig.endpoint;
    replacement.linkId = 8;
    replacement.linkName = QStringLiteral("Other modem");
    rig.targets.observeEndpoint(replacement, false);
    rig.targets.selectTarget(
        replacement.linkId, replacement.systemId,
        replacement.componentId);
    QVERIFY(!first.isRunning());
    QVERIFY(first.statusText().contains(
        QStringLiteral("changed"), Qt::CaseInsensitive));
    QCOMPARE(rig.frames.size(), 0);
}

QTEST_MAIN(FollowMeWindowTest)

#include "test_followmewindow.moc"
