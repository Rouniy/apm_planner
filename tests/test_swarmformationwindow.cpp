#include "comm/SwarmTelemetryRegistry.h"
#include "ui/FormationGridControl.h"
#include "ui/SwarmFormationWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QtTest>

#include <utility>

// The production application supplies these through
// SwarmFormationWindowIntegration.cpp.  The focused widget target uses the
// injected constructor and deliberately has no LinkManager dependency.
SwarmTelemetryRegistry *SwarmFormationApplicationRegistry()
{
    return nullptr;
}

SwarmCommandService *SwarmFormationApplicationCommandService()
{
    return nullptr;
}

namespace
{

constexpr int UseColumn = 0;
constexpr int EndpointColumn = 1;
constexpr int SystemColumn = 2;
constexpr int RoleColumn = 4;
constexpr int XColumn = 5;

VehicleEndpoint endpoint(int linkId, int systemId)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = QStringLiteral("AUTOPILOT1");
    return value;
}

mavlink_message_t heartbeat(int systemId, int vehicleType)
{
    mavlink_heartbeat_t payload{};
    payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    payload.type = static_cast<quint8>(vehicleType);
    payload.base_mode = 0;
    payload.custom_mode = 17;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t globalPosition(int systemId, int coordinateOffset)
{
    mavlink_global_position_int_t payload{};
    payload.time_boot_ms = 1000;
    payload.lat = 351234567 + coordinateOffset;
    payload.lon = 331234567 + coordinateOffset;
    payload.alt = 123450;
    payload.relative_alt = 23450;
    payload.vx = 125;
    payload.vy = -250;
    payload.vz = 75;
    payload.hdg = 1234;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t attitude(int systemId)
{
    mavlink_attitude_t payload{};
    payload.time_boot_ms = 1010;
    payload.roll = 0.1F;
    payload.pitch = -0.2F;
    payload.yaw = 0.5F;
    mavlink_message_t message{};
    mavlink_msg_attitude_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

class FormationRig
{
public:
    FormationRig()
        : registry([this]() { return nowMs; })
    {
    }

    SwarmVehicleInstanceLease addVehicle(
        int linkId, int systemId, int vehicleType,
        const QString &linkName = QString(), bool withTelemetry = true)
    {
        const quint64 session = registry.beginLinkSession(
            linkId, linkName.isEmpty()
                ? QStringLiteral("Link %1").arg(linkId) : linkName);
        sessions.insert(linkId, session);
        if (!registry.observeMessage(
                linkId, session, heartbeat(systemId, vehicleType))) {
            return {};
        }
        if (withTelemetry) {
            registry.observeMessage(
                linkId, session, globalPosition(systemId, linkId * 10));
            registry.observeMessage(linkId, session, attitude(systemId));
        }
        return registry.acquireVehicle(endpoint(linkId, systemId));
    }

    qint64 nowMs = 100;
    QHash<int, quint64> sessions;
    SwarmTelemetryRegistry registry;
};

class FakeSwarmFormationCommands final
    : public SwarmFormationCommandInterface
{
public:
    SwarmCommandService::Result reserve(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) override
    {
        ++reserveCalls;
        lastOwner = owner;
        lastMembers = members;
        lastMaximumBatchHz = maximumBatchHz;
        if (error) {
            error->clear();
        }
        if (token) {
            token->id = nextSessionId++;
        }
        return SwarmCommandService::Result::Reserved;
    }

    SwarmCommandService::Result release(
        const SwarmCommandSessionToken &token) override
    {
        ++releaseCalls;
        releasedSessions.append(token.id);
        return SwarmCommandService::Result::Cancelled;
    }

    SwarmCommandService::BatchReport requestStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) override
    {
        ++streamCalls;
        lastStreamSession = token.id;
        lastStreamSlots = slotIds;
        lastStreamRateHz = rateHz;
        return successfulReport(token.id);
    }

    SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) override
    {
        targetSessions.append(token.id);
        targetBatches.append(targets);
        return successfulReport(token.id);
    }

    void setSessionCancelledHandler(
        SessionCancelledHandler handler) override
    {
        cancelledHandler = std::move(handler);
    }

    void cancelSession(quint64 sessionId, const QString &reason)
    {
        if (cancelledHandler) {
            cancelledHandler(sessionId, reason);
        }
    }

    static SwarmCommandService::BatchReport successfulReport(
        quint64 sessionId)
    {
        SwarmCommandService::BatchReport report;
        report.sessionId = sessionId;
        report.result = SwarmCommandService::Result::SentAll;
        return report;
    }

    int reserveCalls = 0;
    int releaseCalls = 0;
    int streamCalls = 0;
    quint64 nextSessionId = 70;
    QObject *lastOwner = nullptr;
    int lastMaximumBatchHz = 0;
    quint64 lastStreamSession = 0;
    int lastStreamRateHz = 0;
    QVector<SwarmCommandMember> lastMembers;
    QVector<int> lastStreamSlots;
    QVector<quint64> releasedSessions;
    QVector<quint64> targetSessions;
    QVector<QVector<SwarmPositionTarget>> targetBatches;
    SessionCancelledHandler cancelledHandler;
};

SwarmFormationWindow::Dependencies confirmation(bool *called, bool accepted)
{
    SwarmFormationWindow::Dependencies dependencies;
    dependencies.confirmDangerous = [called, accepted](
        QWidget *, const QString &, const QString &, const QString &) {
        if (called) {
            *called = true;
        }
        return accepted;
    };
    return dependencies;
}

QTableWidget *vehicleTable(SwarmFormationWindow *window)
{
    return window->findChild<QTableWidget *>(
        QStringLiteral("FormationVehicleGrid"));
}

void includeFollower(SwarmFormationWindow *window, int row)
{
    QTableWidget *table = vehicleTable(window);
    QVERIFY(table);
    QVERIFY(row >= 0 && row < table->rowCount());
    QVERIFY(table->item(row, UseColumn));
    table->item(row, UseColumn)->setCheckState(Qt::Checked);
}

} // namespace

class SwarmFormationWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void uiInventoryAndUnavailableCommandsAreExplicit();
    void duplicateSystemIdsAcrossLinksStayDistinct();
    void planeCanLeadButCannotBeEnabledAsFollower();
    void cancelledConfirmationDoesNotReserveSender();
    void heartbeatOnlyStartBootstrapsTelemetryBeforeFirstTarget();
    void retiredLeaderClearsOffsetsBeforeAutomaticReplacement();
    void copterRoverStartStopAndRetirementUseExactSession();
};

void SwarmFormationWindowTest::
uiInventoryAndUnavailableCommandsAreExplicit()
{
    FormationRig rig;
    FakeSwarmFormationCommands commands;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));

    QCOMPARE(window.objectName(), QStringLiteral("SwarmFormationWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Swarm Formation (Beta)"));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QCOMPARE(window.size(), QSize(SwarmFormationWindow::WindowWidth,
                                 SwarmFormationWindow::WindowHeight));
    QCOMPARE(window.minimumSize(),
             QSize(SwarmFormationWindow::MinimumWindowWidth,
                   SwarmFormationWindow::MinimumWindowHeight));
    QVERIFY(window.layout());
    QVERIFY(window.findChildren<QWidget *>().size() > 20);

    const QStringList requiredControls = {
        QStringLiteral("formationDangerBanner"),
        QStringLiteral("formationLeader"),
        QStringLiteral("formationRefreshVehicles"),
        QStringLiteral("formationCaptureOffsets"),
        QStringLiteral("formationAlignYaw"),
        QStringLiteral("formationAimGimbals"),
        QStringLiteral("FormationPlaneAttitudeCheckBox"),
        QStringLiteral("formationMainSplitter"),
        QStringLiteral("FormationGrid"),
        QStringLiteral("FormationVehicleGrid"),
        QStringLiteral("FormationRunButton"),
        QStringLiteral("formationTakeoffAltitude"),
        QStringLiteral("formationUnavailableHint"),
        QStringLiteral("formationStatus"),
        QStringLiteral("formationClose")
    };
    for (const QString &name : requiredControls) {
        QVERIFY2(window.findChild<QWidget *>(name), qPrintable(name));
    }

    const QStringList unavailableChecks = {
        QStringLiteral("formationAlignYaw"),
        QStringLiteral("formationAimGimbals"),
        QStringLiteral("FormationPlaneAttitudeCheckBox")
    };
    for (const QString &name : unavailableChecks) {
        const QCheckBox *control = window.findChild<QCheckBox *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(!control->isEnabled(), qPrintable(name));
        QVERIFY2(!control->toolTip().isEmpty(), qPrintable(name));
    }

    const QStringList unavailableButtons = {
        QStringLiteral("formationArmFollowers"),
        QStringLiteral("formationDisarmFollowers"),
        QStringLiteral("formationGuidedFollowers"),
        QStringLiteral("formationAutoFollowers"),
        QStringLiteral("formationTakeoffFollowers"),
        QStringLiteral("formationLandFollowers")
    };
    for (const QString &name : unavailableButtons) {
        const QPushButton *control = window.findChild<QPushButton *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(!control->isEnabled(), qPrintable(name));
        QVERIFY2(!control->toolTip().isEmpty(), qPrintable(name));
    }

    const QDoubleSpinBox *takeoff = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("formationTakeoffAltitude"));
    QVERIFY(takeoff);
    QCOMPARE(takeoff->minimum(), 1.0);
    QCOMPARE(takeoff->maximum(), 10000.0);
    QCOMPARE(takeoff->value(), 5.0);
    QVERIFY(!takeoff->isEnabled());
    QVERIFY(!takeoff->toolTip().isEmpty());
    QVERIFY(window.statusText().contains(
        QStringLiteral("No supported"), Qt::CaseInsensitive));
}

void SwarmFormationWindowTest::duplicateSystemIdsAcrossLinksStayDistinct()
{
    FormationRig rig;
    const SwarmVehicleInstanceLease first = rig.addVehicle(
        3, 42, MAV_TYPE_QUADROTOR, QStringLiteral("Radio A"));
    const SwarmVehicleInstanceLease second = rig.addVehicle(
        9, 42, MAV_TYPE_GROUND_ROVER, QStringLiteral("Radio B"));
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(!first.sameInstance(second));

    FakeSwarmFormationCommands commands;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));
    QCOMPARE(window.vehicleCount(), 2);

    const QComboBox *leaders = window.findChild<QComboBox *>(
        QStringLiteral("formationLeader"));
    QTableWidget *table = vehicleTable(&window);
    QVERIFY(leaders);
    QVERIFY(table);
    QCOMPARE(leaders->count(), 2);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, SystemColumn)->text(), QStringLiteral("42"));
    QCOMPARE(table->item(1, SystemColumn)->text(), QStringLiteral("42"));
    QCOMPARE(table->item(0, EndpointColumn)->text(), QStringLiteral("Radio A"));
    QCOMPARE(table->item(1, EndpointColumn)->text(), QStringLiteral("Radio B"));
    QVERIFY(leaders->itemText(0).contains(QStringLiteral("Radio A")));
    QVERIFY(leaders->itemText(1).contains(QStringLiteral("Radio B")));

    const SwarmVehicleInstanceLease tableFirst = qvariant_cast<
        SwarmVehicleInstanceLease>(
            table->item(0, EndpointColumn)->data(Qt::UserRole));
    const SwarmVehicleInstanceLease tableSecond = qvariant_cast<
        SwarmVehicleInstanceLease>(
            table->item(1, EndpointColumn)->data(Qt::UserRole));
    QVERIFY(tableFirst.sameInstance(first));
    QVERIFY(tableSecond.sameInstance(second));
    QVERIFY(!tableFirst.sameInstance(tableSecond));
}

void SwarmFormationWindowTest::planeCanLeadButCannotBeEnabledAsFollower()
{
    FormationRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_FIXED_WING).isValid());
    FakeSwarmFormationCommands commands;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));

    const QComboBox *leaders = window.findChild<QComboBox *>(
        QStringLiteral("formationLeader"));
    QTableWidget *table = vehicleTable(&window);
    QVERIFY(leaders);
    QVERIFY(table);
    QCOMPARE(leaders->count(), 2);
    QVERIFY(!(table->item(1, UseColumn)->flags() & Qt::ItemIsUserCheckable));
    QVERIFY(!(table->item(1, UseColumn)->flags() & Qt::ItemIsEnabled));
    QVERIFY(!(table->item(1, XColumn)->flags() & Qt::ItemIsEditable));
    QVERIFY(table->item(1, RoleColumn)->text().contains(
        QStringLiteral("Leader only")));
}

void SwarmFormationWindowTest::cancelledConfirmationDoesNotReserveSender()
{
    FormationRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_GROUND_ROVER).isValid());
    FakeSwarmFormationCommands commands;
    bool confirmationCalled = false;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(&confirmationCalled, false));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FormationRunButton"));
    QVERIFY(run && run->isEnabled());
    run->click();

    QVERIFY(confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QCOMPARE(commands.targetBatches.size(), 0);
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
}

void SwarmFormationWindowTest::
heartbeatOnlyStartBootstrapsTelemetryBeforeFirstTarget()
{
    FormationRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        2, 1, MAV_TYPE_QUADROTOR, QStringLiteral("Leader"), false);
    QVERIFY(leader.isValid());
    QVERIFY(rig.addVehicle(
        4, 2, MAV_TYPE_GROUND_ROVER,
        QStringLiteral("Follower"), false).isValid());
    FakeSwarmFormationCommands commands;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(nullptr, true));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FormationRunButton"));
    QVERIFY(run);
    run->click();
    QVERIFY(window.isRunning());
    QCOMPARE(commands.reserveCalls, 1);
    QCOMPARE(commands.streamCalls, 1);
    QCOMPARE(commands.lastStreamSlots, QVector<int>({1}));
    QCOMPARE(commands.targetBatches.size(), 0);
    QVERIFY(window.statusText().contains(
        QStringLiteral("Waiting for fresh")));

    QVERIFY(rig.registry.observeMessage(
        leader.endpoint.linkId,
        rig.sessions.value(leader.endpoint.linkId),
        globalPosition(leader.endpoint.systemId, 0)));
    QVERIFY(rig.registry.observeMessage(
        leader.endpoint.linkId,
        rig.sessions.value(leader.endpoint.linkId),
        attitude(leader.endpoint.systemId)));
    QTRY_COMPARE_WITH_TIMEOUT(commands.targetBatches.size(), 1, 1000);
    QVERIFY(window.isRunning());
    QVERIFY(window.statusText().contains(QStringLiteral("active")));
}

void SwarmFormationWindowTest::
retiredLeaderClearsOffsetsBeforeAutomaticReplacement()
{
    FormationRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        2, 11, MAV_TYPE_QUADROTOR);
    QVERIFY(leader.isValid());
    QVERIFY(rig.addVehicle(7, 12, MAV_TYPE_GROUND_ROVER).isValid());
    FakeSwarmFormationCommands commands;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));
    QTableWidget *table = vehicleTable(&window);
    QVERIFY(table);
    table->item(1, XColumn)->setText(QStringLiteral("9.94"));
    QCOMPARE(table->item(1, XColumn)->text(), QStringLiteral("9.9"));

    QVERIFY(rig.registry.endLinkSession(
        leader.endpoint.linkId,
        rig.sessions.value(leader.endpoint.linkId)));
    QCOMPARE(window.vehicleCount(), 1);
    table = vehicleTable(&window);
    QCOMPARE(table->item(0, XColumn)->text(), QStringLiteral("0.0"));
    QCOMPARE(table->item(0, UseColumn)->checkState(), Qt::Checked);
    QVERIFY(!(table->item(0, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));
    QVERIFY(window.statusText().contains(QStringLiteral("offsets were cleared")));
}

void SwarmFormationWindowTest::
copterRoverStartStopAndRetirementUseExactSession()
{
    FormationRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        2, 11, MAV_TYPE_QUADROTOR, QStringLiteral("Leader radio"));
    const SwarmVehicleInstanceLease follower = rig.addVehicle(
        7, 12, MAV_TYPE_GROUND_ROVER, QStringLiteral("Follower radio"));
    QVERIFY(leader.isValid());
    QVERIFY(follower.isValid());
    FakeSwarmFormationCommands commands;
    bool confirmationCalled = false;
    SwarmFormationWindow window(
        &rig.registry, &commands, confirmation(&confirmationCalled, true));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FormationRunButton"));
    QVERIFY(run);
    run->click();

    QVERIFY(confirmationCalled);
    QVERIFY(window.isRunning());
    QCOMPARE(commands.reserveCalls, 1);
    QCOMPARE(commands.streamCalls, 1);
    QCOMPARE(commands.lastOwner, static_cast<QObject *>(&window));
    QCOMPARE(commands.lastMaximumBatchHz, 10);
    QCOMPARE(commands.lastStreamRateHz, 10);
    QVERIFY(commands.lastStreamSlots.contains(1));
    QCOMPARE(commands.lastMembers.size(), 2);
    QCOMPARE(commands.lastMembers.at(0).slotId, 1);
    QCOMPARE(commands.lastMembers.at(1).slotId, 2);
    QVERIFY(commands.lastMembers.at(0).lease.sameInstance(leader));
    QVERIFY(commands.lastMembers.at(1).lease.sameInstance(follower));
    QCOMPARE(commands.targetBatches.size(), 1);
    QCOMPARE(commands.targetBatches.constFirst().size(), 1);
    const SwarmPositionTarget firstTarget =
        commands.targetBatches.constFirst().constFirst();
    QCOMPARE(firstTarget.slotId, 2);
    QVERIFY(firstTarget.useVelocity);
    QVERIFY(firstTarget.latitudeDegrees >= -90.0
            && firstTarget.latitudeDegrees <= 90.0);
    QVERIFY(firstTarget.longitudeDegrees >= -180.0
            && firstTarget.longitudeDegrees <= 180.0);
    QCOMPARE(run->text(), QStringLiteral("Stop Formation"));

    const quint64 firstSession = commands.lastStreamSession;
    run->click();
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, 1);
    QCOMPARE(commands.releasedSessions.constLast(), firstSession);

    run->click();
    QVERIFY(window.isRunning());
    const quint64 cancelledSession = commands.lastStreamSession;
    commands.cancelSession(
        cancelledSession, QStringLiteral("Sender cancelled exact session."));
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, 1);
    QVERIFY(window.statusText().contains(QStringLiteral("Sender cancelled")));

    run->click();
    QVERIFY(window.isRunning());
    const int releasesBeforeRetirement = commands.releaseCalls;
    QVERIFY(rig.registry.endLinkSession(
        follower.endpoint.linkId,
        rig.sessions.value(follower.endpoint.linkId)));
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, releasesBeforeRetirement + 1);
    QCOMPARE(window.vehicleCount(), 1);
    QCOMPARE(run->text(), QStringLiteral("Start Formation"));
}

QTEST_MAIN(SwarmFormationWindowTest)

#include "test_swarmformationwindow.moc"
