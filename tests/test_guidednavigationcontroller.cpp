#include "ui/GuidedNavigationController.h"
#include "ui/GuidedAltitudeDialog.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"

#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest>
#include <cmath>
#include <utility>

namespace {
VehicleEndpoint endpoint(int link = 7, int system = 42)
{
    VehicleEndpoint value;
    value.linkId = link; value.systemId = system; value.componentId = 1;
    value.linkName = QStringLiteral("Guided fixture %1").arg(link);
    return value;
}
mavlink_message_t decode(const QByteArray &bytes)
{
    MAVLinkFrameParser parser; mavlink_message_t message{};
    unsigned state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) state = parser.parseByte(quint8(byte), &message);
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}
struct Frame { int link; QByteArray bytes; };
struct Fixture {
    QTemporaryDir directory;
    QSettings settings{directory.filePath("preferences.ini"), QSettings::IniFormat};
    VehicleTargetManager targets;
    qint64 now = 1000;
    SwarmTelemetryRegistry telemetry{[this] { return now; }};
    QVector<Frame> frames;
    ExactLinkTransmitter transmitter{[this](int link, const QByteArray &bytes) {
        frames.append({link, bytes}); return true;
    }};
    VehicleCommandService commands{&targets, &transmitter};
    GuidedTargetService guidedLane{&targets, &commands};
    GuidedAltitudeStore altitudes{&targets, &telemetry};
    GuidedNavigationService navigation{&altitudes, &guidedLane, &commands};
    QWidget owner;
    QPointer<GuidedNavigationController> controller;
    bool guidedMode = false;
    bool ready = false;

    explicit Fixture(int vehicleType = MAV_TYPE_QUADROTOR)
    {
        commands.setLocalIdentity(250, 190); guidedLane.setLocalIdentity(250, 190);
        commands.setExactQuarantineForTesting(50);
        ready = directory.isValid() && commands.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &lease) { return telemetry.validateLease(lease); },
            [](const SwarmVehicleInstanceLease &, QString *) { return true; })
            && commands.configureSingleVehicleExactRoute(
                [](const SwarmVehicleInstanceLease &, QString *) { return true; });
        QObject::connect(&telemetry, &SwarmTelemetryRegistry::endpointRetired, &commands,
            [this](const SwarmVehicleInstanceLease &lease) { commands.retireExactVehicle(lease); });
        admit(endpoint(), vehicleType);
        GuidedNavigationController::Dependencies dependencies;
        dependencies.altitudes = &altitudes; dependencies.navigation = &navigation;
        dependencies.telemetry = &telemetry; dependencies.settings = &settings;
        dependencies.isGuided = [this](const GuidedAltitudeStore::Context &) { return guidedMode; };
        controller = new GuidedNavigationController(dependencies, &owner);
        owner.resize(640, 480); owner.show();
    }
    ~Fixture()
    {
        delete controller.data();
        navigation.shutdown();
    }
    quint64 admit(const VehicleEndpoint &target, int vehicleType = MAV_TYPE_QUADROTOR)
    {
        auto epoch = telemetry.currentLinkSessionEpoch(target.linkId);
        if (!epoch) epoch = telemetry.beginLinkSession(target.linkId);
        transmitter.setLinkSessionEpoch(target.linkId, epoch);
        transmitter.setOutboundVersion(target.linkId, 2);
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(quint8(target.systemId), 1, &message,
            quint8(vehicleType), MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
        telemetry.observeMessage(target.linkId, epoch, message);
        targets.observeEndpoint(target, false);
        targets.selectTarget(target.linkId, target.systemId, target.componentId);
        targets.observeHeartbeat(target, false, MAV_AUTOPILOT_ARDUPILOTMEGA, vehicleType);
        return epoch;
    }
    bool setAltitude(double metres = 25, MAV_FRAME frame = MAV_FRAME_GLOBAL_RELATIVE_ALT)
    {
        return altitudes.commitAltitude(altitudes.current(), metres, frame);
    }
    template<class T> T *dialog(const char *name) const
    {
        for (auto *candidate : owner.findChildren<T *>())
            if (candidate->objectName() == QLatin1String(name) && candidate->isVisible()) return candidate;
        return nullptr;
    }
    GuidedAltitudeDialog *altitudeDialog() const { return dialog<GuidedAltitudeDialog>("GuidedAltitudeDialog"); }
    QMessageBox *confirmation() const { return dialog<QMessageBox>("GuidedNavigationConfirmation"); }
    QInputDialog *coordinatesDialog() const { return dialog<QInputDialog>("GuidedCoordinatesDialog"); }
    Terrain3DCore::Snapshot rendered() const
    {
        const auto context = altitudes.current();
        Terrain3DCore::Snapshot snapshot;
        snapshot.vehicle = {35, 33, 200};
        snapshot.linkId = context.target.endpoint.linkId;
        snapshot.systemId = quint8(context.target.endpoint.systemId);
        snapshot.componentId = quint8(context.target.endpoint.componentId);
        snapshot.targetGeneration = context.target.generation;
        snapshot.linkSessionEpoch = context.vehicle.linkSessionEpoch;
        snapshot.vehicleInstanceEpoch = context.vehicle.instanceEpoch;
        snapshot.capturedMonotonicMs = now;
        return snapshot;
    }
    mavlink_command_int_t command(int index = 0) const
    {
        mavlink_command_int_t value{};
        if (index >= frames.size()) return value;
        const auto message = decode(frames.at(index).bytes);
        if (message.msgid == MAVLINK_MSG_ID_COMMAND_INT) mavlink_msg_command_int_decode(&message, &value);
        return value;
    }
    void acknowledge(int result = MAV_RESULT_ACCEPTED)
    {
        const auto target = targets.acquireTarget();
        mavlink_message_t ack{};
        mavlink_msg_command_ack_pack(quint8(target.endpoint.systemId), quint8(target.endpoint.componentId),
            &ack, MAV_CMD_DO_REPOSITION, quint8(result), 255, 0, 250, 190);
        commands.observePhysicalMessage(target.endpoint.linkId,
            telemetry.currentLinkSessionEpoch(target.endpoint.linkId), ack);
    }
};
void enterAltitude(GuidedAltitudeDialog *dialog, const QString &value, MAV_FRAME frame)
{
    auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("GuidedAltitudeValue"));
    auto *combo = dialog->findChild<QComboBox *>(QStringLiteral("GuidedAltitudeFrame"));
    QVERIFY(edit); QVERIFY(combo);
    edit->setText(value); combo->setCurrentIndex(combo->findData(int(frame)));
    dialog->accept();
}
void acceptConfirmation(QMessageBox *dialog)
{
    QVERIFY(dialog); QVERIFY(dialog->button(QMessageBox::Yes));
    dialog->button(QMessageBox::Yes)->click();
}
}

class GuidedNavigationControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void unavailableAndCancelledAltitudeNeverAdmitIntent()
    {
        Fixture f; QVERIFY(f.ready);
        f.targets.clearTarget(); f.controller->editAltitude();
        QVERIFY(!f.controller->busy()); QVERIFY(!f.altitudeDialog()); QVERIFY(f.frames.isEmpty());
        f.admit(endpoint()); f.controller->editAltitude();
        QTRY_VERIFY(f.altitudeDialog()); QVERIFY(f.controller->busy());
        auto *dialog = f.altitudeDialog();
        QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("GuidedAltitudeCancelButton"))->isDefault());
        dialog->reject(); QTRY_VERIFY(!f.controller->busy());
        QVERIFY(!f.altitudes.current().altitudeSet); QVERIFY(f.frames.isEmpty());
        QVERIFY(!f.settings.contains(QStringLiteral("guided_alt")));
    }

    void defaultAltitudeAndDisplayPreferences_data()
    {
        QTest::addColumn<int>("vehicleType"); QTest::addColumn<double>("defaultMetres");
        QTest::newRow("copter") << int(MAV_TYPE_QUADROTOR) << 10.0;
        QTest::newRow("plane") << int(MAV_TYPE_FIXED_WING) << 100.0;
    }
    void defaultAltitudeAndDisplayPreferences()
    {
        QFETCH(int, vehicleType); QFETCH(double, defaultMetres);
        Fixture f(vehicleType); QVERIFY(f.ready);
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        auto *edit = f.altitudeDialog()->findChild<QLineEdit *>(QStringLiteral("GuidedAltitudeValue"));
        QVERIFY(edit); QCOMPARE(edit->text().toDouble(), defaultMetres);
        f.altitudeDialog()->reject(); QTRY_VERIFY(!f.controller->busy());

        f.settings.setValue(QStringLiteral("altunits"), QStringLiteral("Feet"));
        f.settings.setValue(QStringLiteral("guided_alt"), QStringLiteral("328.084"));
        f.settings.setValue(QStringLiteral("guided_alt_frame"), int(MAV_FRAME_GLOBAL));
        QVERIFY(!f.altitudes.current().altitudeSet); // Preferences are not vehicle intent.
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        edit = f.altitudeDialog()->findChild<QLineEdit *>(QStringLiteral("GuidedAltitudeValue"));
        QVERIFY(std::abs(edit->text().toDouble() - 328.084) < 1e-5);
        QCOMPARE(f.altitudeDialog()->findChild<QComboBox *>(QStringLiteral("GuidedAltitudeFrame"))->currentData().toInt(), int(MAV_FRAME_GLOBAL));
        enterAltitude(f.altitudeDialog(), QStringLiteral("-164.042"), MAV_FRAME_GLOBAL_TERRAIN_ALT);
        QTRY_VERIFY(!f.controller->busy());
        QVERIFY(std::abs(f.altitudes.current().altitudeM + 50.0f) < 0.001f);
        QCOMPARE(f.altitudes.current().frame, MAV_FRAME_GLOBAL_TERRAIN_ALT);
        QVERIFY(std::abs(f.settings.value(QStringLiteral("guided_alt")).toDouble() + 164.042) < 1e-5);
        QCOMPARE(f.settings.value(QStringLiteral("guided_alt_frame")).toInt(), int(MAV_FRAME_GLOBAL_TERRAIN_ALT));
        QVERIFY(f.frames.isEmpty());
    }

    void flyToHereAltitudeThenDefaultCancel()
    {
        Fixture f; QVERIFY(f.ready);
        f.controller->flyToHere(35.1, 33.2); QTRY_VERIFY(f.altitudeDialog());
        enterAltitude(f.altitudeDialog(), QStringLiteral("25"), MAV_FRAME_GLOBAL_RELATIVE_ALT);
        QTRY_VERIFY(f.confirmation()); QVERIFY(f.frames.isEmpty());
        QCOMPARE(f.confirmation()->defaultButton(), qobject_cast<QPushButton *>(f.confirmation()->button(QMessageBox::Cancel)));
        QVERIFY(f.confirmation()->text().contains(QStringLiteral("42")));
        QVERIFY(f.confirmation()->text().contains(QStringLiteral("7")));
        f.confirmation()->button(QMessageBox::Cancel)->click();
        QTRY_VERIFY(!f.controller->busy()); QVERIFY(f.frames.isEmpty());
        QVERIFY(f.altitudes.current().altitudeSet); QVERIFY(!f.altitudes.current().pointSet);
        QVERIFY(f.setAltitude(0));
        f.controller->flyToHere(35.1, 33.2); QTRY_VERIFY(f.altitudeDialog());
        f.altitudeDialog()->reject(); QTRY_VERIFY(!f.controller->busy()); QVERIFY(f.frames.isEmpty());
    }

    void coordinatesFramesUnitsAndExactWire_data()
    {
        QTest::addColumn<int>("frame"); QTest::addColumn<bool>("feet");
        QTest::newRow("relative-metres") << int(MAV_FRAME_GLOBAL_RELATIVE_ALT) << false;
        QTest::newRow("absolute-feet") << int(MAV_FRAME_GLOBAL) << true;
        QTest::newRow("terrain-metres") << int(MAV_FRAME_GLOBAL_TERRAIN_ALT) << false;
    }
    void coordinatesFramesUnitsAndExactWire()
    {
        QFETCH(int, frame); QFETCH(bool, feet);
        Fixture f; QVERIFY(f.ready);
        f.settings.setValue(QStringLiteral("altunits"), feet ? QStringLiteral("Feet") : QStringLiteral("Meters"));
        f.settings.setValue(QStringLiteral("guided_alt_frame"), frame);
        f.controller->flyToCoordinates(); QTRY_VERIFY(f.coordinatesDialog());
        f.coordinatesDialog()->setTextValue(feet ? QStringLiteral("12.34567899;-45.67891239;164.042")
            : QStringLiteral("12.34567899;-45.67891239;50"));
        f.coordinatesDialog()->accept(); QTRY_VERIFY(f.confirmation());
        QVERIFY(f.frames.isEmpty()); acceptConfirmation(f.confirmation());
        QTRY_COMPARE(f.frames.size(), 1);
        QCOMPARE(f.frames.first().link, 7);
        const auto wire = decode(f.frames.first().bytes);
        QCOMPARE(wire.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
        QCOMPARE(wire.sysid, quint8(250)); QCOMPARE(wire.compid, quint8(190));
        const auto command = f.command();
        QCOMPARE(command.command, quint16(MAV_CMD_DO_REPOSITION));
        QCOMPARE(command.target_system, quint8(42)); QCOMPARE(command.target_component, quint8(1));
        QCOMPARE(command.frame, quint8(frame)); QCOMPARE(command.x, qint32(123456789));
        QCOMPARE(command.y, qint32(-456789123)); QVERIFY(std::abs(command.z - 50.0f) < 0.001f);
        QCOMPARE(command.param1, -1.0f); QCOMPARE(command.param2, float(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));
        QCOMPARE(command.param3, 0.0f); QVERIFY(std::isnan(command.param4));
        QCOMPARE(command.current, quint8(0)); QCOMPARE(command.autocontinue, quint8(0));
        QVERIFY(!f.altitudes.current().pointSet);
        f.acknowledge(); QTRY_VERIFY(!f.navigation.busy()); QTRY_VERIFY(!f.controller->busy());
        QVERIFY(f.altitudes.current().pointSet); QCOMPARE(f.altitudes.current().frame, MAV_FRAME(frame));
    }

    void malformedCoordinatesAndDialogCancelSendNothing()
    {
        Fixture f; QVERIFY(f.ready);
        f.controller->flyToCoordinates(); QTRY_VERIFY(f.coordinatesDialog());
        f.coordinatesDialog()->reject(); QTRY_VERIFY(!f.controller->busy());
        for (const auto &text : {QStringLiteral("91;20;30"), QStringLiteral("35;181;50"),
            QStringLiteral("nan;33;50"), QStringLiteral("35;33;inf"), QStringLiteral("35;33;50;60")}) {
            f.controller->flyToCoordinates(); QTRY_VERIFY(f.coordinatesDialog());
            f.coordinatesDialog()->setTextValue(text); f.coordinatesDialog()->accept();
            QTRY_VERIFY(!f.controller->busy()); QVERIFY(!f.confirmation()); QVERIFY(f.frames.isEmpty());
        }
        QVERIFY(!f.altitudes.current().altitudeSet);
    }

    void consentCannotSurviveTargetOrIntentChanges_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("target-aba") << 0;
        QTest::newRow("intent-revision") << 1;
        QTest::newRow("physical-instance") << 2;
    }
    void consentCannotSurviveTargetOrIntentChanges()
    {
        QFETCH(int, change);
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        f.controller->flyToHere(35, 33); QTRY_VERIFY(f.confirmation());
        QPointer<QMessageBox> old = f.confirmation();
        if (change == 0) { f.admit(endpoint(8)); f.admit(endpoint(7)); }
        else if (change == 1) QVERIFY(f.setAltitude(99));
        else {
            const auto epoch = f.telemetry.currentLinkSessionEpoch(7);
            QVERIFY(f.telemetry.endLinkSession(7, epoch)); f.admit(endpoint(7));
        }
        if (old) old->button(QMessageBox::Yes)->click();
        QTRY_VERIFY(!f.controller->busy()); QVERIFY(f.frames.isEmpty());
        QVERIFY(!f.navigation.busy());
    }

    void activeTargetAltitudeUpdateUsesStoredPointNotGps()
    {
        Fixture f; QVERIFY(f.ready); f.guidedMode = true;
        auto context = f.altitudes.current();
        QVERIFY(f.altitudes.recordTarget(context, 12.5, -45.25, 25, MAV_FRAME_GLOBAL, &context));
        mavlink_message_t gps{};
        mavlink_msg_global_position_int_pack(42, 1, &gps, 1000, 350000000, 330000000,
            200000, 50000, 0, 0, 0, 0);
        QVERIFY(f.telemetry.observeMessage(7, f.telemetry.currentLinkSessionEpoch(7), gps));
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        enterAltitude(f.altitudeDialog(), QStringLiteral("-10"), MAV_FRAME_GLOBAL);
        QTRY_VERIFY(f.confirmation()); QVERIFY(f.frames.isEmpty());
        QCOMPARE(f.altitudes.current().altitudeM, -10.0f);
        acceptConfirmation(f.confirmation()); QTRY_COMPARE(f.frames.size(), 1);
        QCOMPARE(f.command().x, qint32(125000000)); QCOMPARE(f.command().y, qint32(-452500000));
        QCOMPARE(f.command().z, -10.0f); QCOMPARE(f.command().param2, float(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));
        f.acknowledge(); QTRY_VERIFY(!f.controller->busy());
    }

    void guidedWithoutExplicitPointDoesNotUseCurrentLocation()
    {
        Fixture f; QVERIFY(f.ready); f.guidedMode = true;
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        enterAltitude(f.altitudeDialog(), QStringLiteral("40"), MAV_FRAME_GLOBAL_RELATIVE_ALT);
        QTRY_VERIFY(!f.controller->busy()); QVERIFY(!f.confirmation()); QVERIFY(f.frames.isEmpty());
        QVERIFY(!f.altitudes.current().pointSet); QCOMPARE(f.altitudes.current().altitudeM, 40.0f);
    }

    void altitudeConsentCannotSurviveSelectionChange()
    {
        Fixture f; QVERIFY(f.ready);
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        QPointer<GuidedAltitudeDialog> old = f.altitudeDialog();
        f.admit(endpoint(8)); f.admit(endpoint(7));
        if (old) enterAltitude(old, QStringLiteral("75"), MAV_FRAME_GLOBAL);
        QTRY_VERIFY(!f.controller->busy()); QVERIFY(!f.altitudes.current().altitudeSet);
        QVERIFY(!f.settings.contains(QStringLiteral("guided_alt"))); QVERIFY(f.frames.isEmpty());
    }

    void heartbeatExpiryBeforeConsentSendsNothing()
    {
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        f.controller->flyToHere(35, 33); QTRY_VERIFY(f.confirmation());
        f.now += f.telemetry.heartbeatMaximumAgeMs() + 1;
        acceptConfirmation(f.confirmation()); QTRY_VERIFY(!f.controller->busy());
        QVERIFY(f.frames.isEmpty()); QVERIFY(!f.navigation.busy());
        f.controller->editAltitude(); QVERIFY(!f.altitudeDialog()); QVERIFY(!f.controller->busy());
    }

    void terrainRenderedIdentityIsPinned_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("generation") << 0;
        QTest::newRow("physical-epoch") << 1;
        QTest::newRow("vehicle-instance") << 2;
        QTest::newRow("link") << 3;
        QTest::newRow("system") << 4;
        QTest::newRow("component") << 5;
    }
    void terrainRenderedIdentityIsPinned()
    {
        QFETCH(int, change);
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        auto rendered = f.rendered();
        switch (change) {
        case 0: ++rendered.targetGeneration; break;
        case 1: ++rendered.linkSessionEpoch; break;
        case 2: ++rendered.vehicleInstanceEpoch; break;
        case 3: ++rendered.linkId; break;
        case 4: ++rendered.systemId; break;
        case 5: ++rendered.componentId; break;
        }
        f.controller->terrainClick({35.25, 33.75, 999}, rendered);
        QVERIFY(!f.controller->busy()); QVERIFY(!f.confirmation()); QVERIFY(f.frames.isEmpty());
        QVERIFY(f.controller->statusText().contains(QStringLiteral("old"), Qt::CaseInsensitive));
    }

    void terrainRequiresExplicitNonzeroAltitude()
    {
        Fixture f; QVERIFY(f.ready);
        const auto rendered = f.rendered();
        f.controller->terrainClick({35.25, 33.75, 999}, rendered);
        QVERIFY(!f.controller->busy()); QVERIFY(!f.altitudeDialog()); QVERIFY(!f.confirmation());
        QVERIFY(f.setAltitude(0)); f.controller->terrainClick({35.25, 33.75, 999}, rendered);
        QVERIFY(!f.controller->busy()); QVERIFY(!f.confirmation());
        QVERIFY(f.setAltitude(-0.001)); f.controller->terrainClick({35.25, 33.75, 999}, rendered);
        QVERIFY(!f.controller->busy()); QVERIFY(!f.confirmation()); QVERIFY(f.frames.isEmpty());
    }

    void terrainExpiredRenderingIsRejectedDespiteFreshVehicle()
    {
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        const auto rendered = f.rendered();
        f.now += 3001;
        f.admit(endpoint()); // Same target/instance, newly fresh heartbeat.
        QVERIFY(f.altitudes.current().target.isValid());
        QCOMPARE(f.altitudes.current().target.generation, rendered.targetGeneration);
        QCOMPARE(f.altitudes.current().vehicle.instanceEpoch, rendered.vehicleInstanceEpoch);
        f.controller->terrainClick({35.25, 33.75, 999}, rendered);
        QVERIFY(!f.controller->busy()); QVERIFY(!f.confirmation()); QVERIFY(f.frames.isEmpty());
        QVERIFY(!f.controller->statusText().isEmpty());
    }

    void terrainReinterpretationIsVisibleAndDoesNotChangeMode_data()
    {
        QTest::addColumn<int>("savedFrame");
        QTest::newRow("absolute") << int(MAV_FRAME_GLOBAL);
        QTest::newRow("terrain") << int(MAV_FRAME_GLOBAL_TERRAIN_ALT);
    }
    void terrainReinterpretationIsVisibleAndDoesNotChangeMode()
    {
        QFETCH(int, savedFrame);
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude(-25, MAV_FRAME(savedFrame)));
        f.controller->terrainClick({35.25, 33.75, 999}, f.rendered()); QTRY_VERIFY(f.confirmation());
        const auto text = f.confirmation()->text();
        QVERIFY(text.contains(QStringLiteral("RELATIVE"), Qt::CaseInsensitive));
        QVERIFY(text.contains(QStringLiteral("DATA")));
        QVERIFY(text.contains(QStringLiteral("inspection only"), Qt::CaseInsensitive));
        QVERIFY(text.contains(QStringLiteral("does NOT change flight mode"), Qt::CaseInsensitive));
        QCOMPARE(f.confirmation()->defaultButton(), qobject_cast<QPushButton *>(f.confirmation()->button(QMessageBox::Cancel)));
        acceptConfirmation(f.confirmation()); QTRY_COMPARE(f.frames.size(), 1);
        const auto command = f.command(); QCOMPARE(command.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
        QCOMPARE(command.param2, 0.0f); QCOMPARE(command.x, qint32(352500000));
        QCOMPARE(command.y, qint32(337500000)); QCOMPARE(command.z, -25.0f);
        f.acknowledge(); QTRY_VERIFY(!f.navigation.busy()); QTRY_VERIFY(!f.controller->busy());
        QCOMPARE(f.altitudes.current().frame, MAV_FRAME_GLOBAL_RELATIVE_ALT);
    }

    void ownerCloseCancelsConsentAndKeepsApplicationServices()
    {
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        f.controller->flyToHere(35, 33); QTRY_VERIFY(f.confirmation());
        QPointer<QMessageBox> old = f.confirmation();
        QVERIFY(f.owner.close());
        if (old) old->button(QMessageBox::Yes)->click();
        QVERIFY(f.frames.isEmpty()); QVERIFY(!f.navigation.busy());
        if (f.controller) QVERIFY(!f.controller->busy());
        QVERIFY(f.altitudes.current().altitudeSet);
    }

    void ownerCloseAfterSubmissionCancelsRetriesButKeepsDrainOwned()
    {
        Fixture f; QVERIFY(f.ready); QVERIFY(f.setAltitude());
        f.controller->flyToHere(35, 33); QTRY_VERIFY(f.confirmation());
        acceptConfirmation(f.confirmation()); QTRY_COMPARE(f.frames.size(), 1);
        QVERIFY(f.navigation.busy());
        const auto operation = f.navigation.currentOperationId(); QVERIFY(operation != 0);
        QVERIFY(f.owner.close());
        // The coordinator remains application-owned and owns any ambiguous
        // outcome/drain, not the dialog or its closing page.
        QTRY_VERIFY(!f.navigation.busy());
        QCOMPARE(f.frames.size(), 1);
        QVERIFY(f.navigation.lastReport().isValid());
        QCOMPARE(f.navigation.lastReport().operationId, operation);
        QVERIFY(f.navigation.lastReport().cancellationRequested);
        QVERIFY(f.navigation.lastReport().frameAttempted);
    }

    void injectedModeCallbackCanDeleteController()
    {
        Fixture f; QVERIFY(f.ready);
        delete f.controller.data();
        GuidedNavigationController::Dependencies dependencies;
        dependencies.altitudes = &f.altitudes; dependencies.navigation = &f.navigation;
        dependencies.telemetry = &f.telemetry; dependencies.settings = &f.settings;
        bool called = false;
        dependencies.isGuided = [&f, &called, marker = QStringLiteral("mode callback remains alive")] (
            const GuidedAltitudeStore::Context &) {
            delete f.controller.data();
            called = marker == QStringLiteral("mode callback remains alive");
            return true;
        };
        f.controller = new GuidedNavigationController(std::move(dependencies), &f.owner);
        f.controller->editAltitude(); QTRY_VERIFY(f.altitudeDialog());
        enterAltitude(f.altitudeDialog(), QStringLiteral("25"), MAV_FRAME_GLOBAL);
        QVERIFY(called); QVERIFY(!f.controller); QVERIFY(f.frames.isEmpty());
        QCOMPARE(f.altitudes.current().altitudeM, 25.0f);
    }
};

QTEST_MAIN(GuidedNavigationControllerTest)
#include "test_guidednavigationcontroller.moc"
