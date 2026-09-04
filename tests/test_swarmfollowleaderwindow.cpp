#include "comm/SwarmTelemetryRegistry.h"
#include "ui/SwarmFollowLeaderWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QtTest>

#include <cmath>
#include <functional>
#include <utility>

// The production application supplies these through
// SwarmFollowLeaderWindowIntegration.cpp. The focused widget target exercises
// only the injected constructor and deliberately has no LinkManager seam.
SwarmTelemetryRegistry *SwarmFollowLeaderApplicationRegistry()
{
    return nullptr;
}

SwarmCommandService *SwarmFollowLeaderApplicationCommandService()
{
    return nullptr;
}

namespace
{

constexpr int UseColumn = 0;
constexpr int OrderColumn = 1;
constexpr int EndpointColumn = 2;
constexpr int SystemColumn = 3;
constexpr int ComponentColumn = 4;
constexpr int RoleColumn = 5;
constexpr int FirmwareColumn = 6;
constexpr int LiveStatusColumn = 7;
constexpr int TargetColumn = 8;
constexpr quint32 CopterGuidedMode = 4;
constexpr quint32 CopterLoiterMode = 5;

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

mavlink_message_t heartbeat(int systemId, int vehicleType,
                            quint32 customMode)
{
    mavlink_heartbeat_t payload{};
    payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    payload.type = static_cast<quint8>(vehicleType);
    // Keep GUIDED_ENABLED set even for LOITER fixtures. The production gate
    // must use the ArduCopter custom mode rather than this ambiguous base bit.
    payload.base_mode = static_cast<quint8>(
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
    payload.custom_mode = customMode;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t globalPosition(
    int systemId, double eastM, quint32 bootTimeMs = 1000,
    double northMps = 4.0, double eastMps = -2.0,
    double downMps = 1.0)
{
    constexpr double LatitudeDegrees = 35.0;
    constexpr double LongitudeDegrees = 33.0;
    constexpr double EarthRadiusM = 6378137.0;
    constexpr double Pi = 3.14159265358979323846;
    const double longitude = LongitudeDegrees
        + eastM / (EarthRadiusM * std::cos(LatitudeDegrees * Pi / 180.0))
            * 180.0 / Pi;

    mavlink_global_position_int_t payload{};
    payload.time_boot_ms = bootTimeMs;
    payload.lat = static_cast<qint32>(std::llround(
        LatitudeDegrees * 1.0e7));
    payload.lon = static_cast<qint32>(std::llround(longitude * 1.0e7));
    payload.alt = 123450;
    payload.relative_alt = 2000;
    payload.vx = static_cast<qint16>(std::llround(northMps * 100.0));
    payload.vy = static_cast<qint16>(std::llround(eastMps * 100.0));
    payload.vz = static_cast<qint16>(std::llround(downMps * 100.0));
    payload.hdg = 1234;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

class FollowLeaderRig
{
public:
    FollowLeaderRig()
        : registry([this]() { return nowMs; })
    {
    }

    SwarmVehicleInstanceLease addVehicle(
        int linkId, int systemId, int vehicleType,
        quint32 customMode, const QString &linkName = QString(),
        bool withPosition = true, double eastM = 0.0,
        double northMps = 4.0, double eastMps = -2.0,
        double downMps = 1.0)
    {
        const quint64 session = registry.beginLinkSession(
            linkId, linkName.isEmpty()
                ? QStringLiteral("Link %1").arg(linkId) : linkName);
        sessions.insert(linkId, session);
        if (!registry.observeMessage(
                linkId, session,
                heartbeat(systemId, vehicleType, customMode))) {
            return {};
        }
        if (withPosition) {
            registry.observeMessage(
                linkId, session,
                globalPosition(systemId, eastM, 1000,
                               northMps, eastMps, downMps));
        }
        return registry.acquireVehicle(endpoint(linkId, systemId));
    }

    bool updatePosition(
        const SwarmVehicleInstanceLease &lease, double eastM,
        quint32 bootTimeMs, double northMps = 4.0,
        double eastMps = -2.0, double downMps = 1.0)
    {
        return registry.observeMessage(
            lease.endpoint.linkId,
            sessions.value(lease.endpoint.linkId),
            globalPosition(lease.endpoint.systemId, eastM, bootTimeMs,
                           northMps, eastMps, downMps));
    }

    bool updateHeartbeat(const SwarmVehicleInstanceLease &lease,
                         int vehicleType, quint32 customMode)
    {
        return registry.observeMessage(
            lease.endpoint.linkId,
            sessions.value(lease.endpoint.linkId),
            heartbeat(lease.endpoint.systemId, vehicleType, customMode));
    }

    qint64 nowMs = 100;
    QHash<int, quint64> sessions;
    SwarmTelemetryRegistry registry;
};

class FakeSwarmFollowLeaderCommands final
    : public SwarmFollowLeaderCommandInterface
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

    bool routeIsEligible(
        const SwarmVehicleInstanceLease &lease, QString *error) override
    {
        ++routeChecks;
        if (error) {
            error->clear();
        }
        if (blockedRouteLinks.contains(lease.endpoint.linkId)) {
            if (error) {
                *error = QStringLiteral(
                    "Listening UDP is not an exact command route.");
            }
            return false;
        }
        if (onRouteCheck) {
            onRouteCheck(lease);
        }
        return true;
    }

    SwarmCommandService::BatchReport requestPositionStreams(
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
        if (nextSendResult != SwarmCommandService::Result::SentAll) {
            SwarmCommandService::BatchReport report;
            report.sessionId = token.id;
            report.result = nextSendResult;
            report.detail = QStringLiteral("Injected partial send.");
            return report;
        }
        return successfulReport(token.id);
    }

    void setSessionCancelledHandler(
        SessionCancelledHandler handler) override
    {
        cancelledHandler = std::move(handler);
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
    int routeChecks = 0;
    int streamCalls = 0;
    quint64 nextSessionId = 120;
    QObject *lastOwner = nullptr;
    int lastMaximumBatchHz = 0;
    quint64 lastStreamSession = 0;
    int lastStreamRateHz = 0;
    QVector<SwarmCommandMember> lastMembers;
    QVector<int> lastStreamSlots;
    QVector<quint64> releasedSessions;
    QVector<quint64> targetSessions;
    QVector<QVector<SwarmPositionTarget>> targetBatches;
    QSet<int> blockedRouteLinks;
    std::function<void(const SwarmVehicleInstanceLease &)> onRouteCheck;
    SwarmCommandService::Result nextSendResult =
        SwarmCommandService::Result::SentAll;
    SessionCancelledHandler cancelledHandler;
};

SwarmFollowLeaderWindow::Dependencies confirmation(bool *called,
                                                    bool accepted)
{
    SwarmFollowLeaderWindow::Dependencies dependencies;
    dependencies.confirmDangerous = [called, accepted](
        QWidget *, const QString &, const QString &, const QString &) {
        if (called) {
            *called = true;
        }
        return accepted;
    };
    return dependencies;
}

QTableWidget *vehicleTable(SwarmFollowLeaderWindow *window)
{
    return window->findChild<QTableWidget *>(
        QStringLiteral("FollowLeaderVehicleGrid"));
}

QPushButton *runButton(SwarmFollowLeaderWindow *window)
{
    return window->findChild<QPushButton *>(
        QStringLiteral("FollowLeaderRunButton"));
}

int rowForLease(QTableWidget *table,
                const SwarmVehicleInstanceLease &lease)
{
    if (!table) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *item = table->item(row, EndpointColumn);
        if (item && qvariant_cast<SwarmVehicleInstanceLease>(
                        item->data(Qt::UserRole)).sameInstance(lease)) {
            return row;
        }
    }
    return -1;
}

int comboIndexForLease(QComboBox *combo,
                       const SwarmVehicleInstanceLease &lease)
{
    if (!combo) {
        return -1;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (qvariant_cast<SwarmVehicleInstanceLease>(
                combo->itemData(index)).sameInstance(lease)) {
            return index;
        }
    }
    return -1;
}

void includeFollower(SwarmFollowLeaderWindow *window,
                     const SwarmVehicleInstanceLease &lease)
{
    QTableWidget *table = vehicleTable(window);
    QVERIFY(table);
    const int row = rowForLease(table, lease);
    QVERIFY(row >= 0);
    QVERIFY(table->item(row, UseColumn));
    table->item(row, UseColumn)->setCheckState(Qt::Checked);
}

} // namespace

class SwarmFollowLeaderWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void uiInventoryAndUnavailableCommandsAreExplicit();
    void duplicateSystemIdsKeepRolesAndFollowerOrderAcrossRefresh();
    void followerFamilyChangeClearsAnUneditableSelection();
    void airMasterEligibilityLossSelectsAnEligibleReplacement();
    void noFollowersAirOnlyRunIsAccepted();
    void cancelledConfirmationSendsAndReservesNothing();
    void nonGuidedAirRoleIsRejectedDespiteGuidedBaseFlag_data();
    void nonGuidedAirRoleIsRejectedDespiteGuidedBaseFlag();
    void unsafeRouteIsRejectedBeforeConfirmation();
    void reentrantRouteRetirementIsRejectedBeforeConfirmation();
    void bootstrapWaitsThenSendsOnePositionVelocityBatchAtTenHertz();
    void planEditPartialSendRetirementAndCloseReleaseExactSessions();
};

void SwarmFollowLeaderWindowTest::
uiInventoryAndUnavailableCommandsAreExplicit()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground radio")).isValid());
    QVERIFY(rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air radio")).isValid());
    FakeSwarmFollowLeaderCommands commands;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));

    QCOMPARE(window.objectName(), QStringLiteral("SwarmFollowLeaderWindow"));
    QCOMPARE(window.windowTitle(),
             QStringLiteral("Swarm Follow Leader (Beta)"));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QCOMPARE(window.size(), QSize(1240, 720));
    QCOMPARE(window.size(), QSize(SwarmFollowLeaderWindow::WindowWidth,
                                 SwarmFollowLeaderWindow::WindowHeight));
    QCOMPARE(window.minimumSize(), QSize(980, 600));
    QCOMPARE(window.minimumSize(),
             QSize(SwarmFollowLeaderWindow::MinimumWindowWidth,
                   SwarmFollowLeaderWindow::MinimumWindowHeight));
    QVERIFY(window.layout());
    QVERIFY(window.findChildren<QWidget *>().size() > 20);

    const QStringList requiredControls = {
        QStringLiteral("followLeaderDangerBanner"),
        QStringLiteral("followLeaderGroundMaster"),
        QStringLiteral("followLeaderAirMaster"),
        QStringLiteral("followLeaderRefreshVehicles"),
        QStringLiteral("followLeaderRoleHint"),
        QStringLiteral("followLeaderSeparation"),
        QStringLiteral("followLeaderLead"),
        QStringLiteral("followLeaderAltitude"),
        QStringLiteral("followLeaderTakeoffAltitude"),
        QStringLiteral("followLeaderPlacementHint"),
        QStringLiteral("FollowLeaderVehicleGrid"),
        QStringLiteral("FollowLeaderRunButton"),
        QStringLiteral("followLeaderUnavailableHint"),
        QStringLiteral("followLeaderStatus"),
        QStringLiteral("followLeaderClose")
    };
    for (const QString &name : requiredControls) {
        QVERIFY2(window.findChild<QWidget *>(name), qPrintable(name));
    }

    QTableWidget *table = vehicleTable(&window);
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 9);
    QCOMPARE(table->rowCount(), 2);
    const QStringList headers = {
        QStringLiteral("Use"), QStringLiteral("Order"),
        QStringLiteral("Endpoint"), QStringLiteral("Sys"),
        QStringLiteral("Comp"), QStringLiteral("Role"),
        QStringLiteral("Firmware"), QStringLiteral("Live status"),
        QStringLiteral("Current commanded target")
    };
    for (int column = 0; column < headers.size(); ++column) {
        QVERIFY(table->horizontalHeaderItem(column));
        QCOMPARE(table->horizontalHeaderItem(column)->text(),
                 headers.at(column));
    }
    QVERIFY(table->item(0, ComponentColumn));
    QVERIFY(table->item(0, FirmwareColumn));
    QVERIFY(table->item(0, TargetColumn));
    QCOMPARE(table->item(0, UseColumn)->checkState(), Qt::Unchecked);
    QCOMPARE(table->item(1, UseColumn)->checkState(), Qt::Unchecked);
    QVERIFY(!(table->item(0, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));
    QVERIFY(!(table->item(1, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));

    const QStringList unavailableButtons = {
        QStringLiteral("followLeaderArmAirGroup"),
        QStringLiteral("followLeaderDisarmAirGroup"),
        QStringLiteral("followLeaderTakeoffAirGroup"),
        QStringLiteral("followLeaderGuidedAirGroup"),
        QStringLiteral("followLeaderNavGuidedAirGroup"),
        QStringLiteral("followLeaderAutoAllRoles")
    };
    for (const QString &name : unavailableButtons) {
        const QPushButton *control = window.findChild<QPushButton *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(!control->isHidden(), qPrintable(name));
        QVERIFY2(!control->isEnabled(), qPrintable(name));
        QVERIFY2(!control->toolTip().trimmed().isEmpty(), qPrintable(name));
        QCOMPARE(control->accessibleDescription(), control->toolTip());
    }

    const QDoubleSpinBox *takeoff = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("followLeaderTakeoffAltitude"));
    QVERIFY(takeoff);
    QVERIFY(!takeoff->isHidden());
    QVERIFY(!takeoff->isEnabled());
    QCOMPARE(takeoff->minimum(), 1.0);
    QCOMPARE(takeoff->maximum(), 10000.0);
    QCOMPARE(takeoff->value(), 5.0);
    QVERIFY(!takeoff->toolTip().trimmed().isEmpty());
    QCOMPARE(takeoff->accessibleDescription(), takeoff->toolTip());
}

void SwarmFollowLeaderWindowTest::
duplicateSystemIdsKeepRolesAndFollowerOrderAcrossRefresh()
{
    FollowLeaderRig rig;
    const SwarmVehicleInstanceLease first = rig.addVehicle(
        3, 42, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Radio A"));
    const SwarmVehicleInstanceLease second = rig.addVehicle(
        7, 42, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Radio B"));
    const SwarmVehicleInstanceLease ground = rig.addVehicle(
        9, 43, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Ground selected"));
    const SwarmVehicleInstanceLease air = rig.addVehicle(
        11, 44, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air selected"));
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(ground.isValid());
    QVERIFY(air.isValid());
    QVERIFY(!first.sameInstance(second));

    FakeSwarmFollowLeaderCommands commands;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));
    QTableWidget *table = vehicleTable(&window);
    QComboBox *groundCombo = window.findChild<QComboBox *>(
        QStringLiteral("followLeaderGroundMaster"));
    QComboBox *airCombo = window.findChild<QComboBox *>(
        QStringLiteral("followLeaderAirMaster"));
    QVERIFY(table);
    QVERIFY(groundCombo);
    QVERIFY(airCombo);
    QCOMPARE(table->rowCount(), 4);
    QCOMPARE(groundCombo->count(), 4);
    QCOMPARE(airCombo->count(), 4);

    const int firstRow = rowForLease(table, first);
    const int secondRow = rowForLease(table, second);
    QVERIFY(firstRow >= 0);
    QVERIFY(secondRow >= 0);
    QCOMPARE(table->item(firstRow, SystemColumn)->text(),
             QStringLiteral("42"));
    QCOMPARE(table->item(secondRow, SystemColumn)->text(),
             QStringLiteral("42"));
    QCOMPARE(table->item(firstRow, EndpointColumn)->text(),
             QStringLiteral("Radio A"));
    QCOMPARE(table->item(secondRow, EndpointColumn)->text(),
             QStringLiteral("Radio B"));

    groundCombo->setCurrentIndex(comboIndexForLease(groundCombo, ground));
    airCombo->setCurrentIndex(comboIndexForLease(airCombo, air));
    table = vehicleTable(&window);
    QCOMPARE(rowForLease(table, ground), 2);
    QCOMPARE(rowForLease(table, air), 3);

    // Swap the two follower orders through a temporary unused value so the
    // final plan remains contiguous and unambiguous.
    table->item(rowForLease(table, second), OrderColumn)->setText(
        QStringLiteral("3"));
    table->item(rowForLease(table, first), OrderColumn)->setText(
        QStringLiteral("2"));
    table->item(rowForLease(table, second), OrderColumn)->setText(
        QStringLiteral("1"));
    includeFollower(&window, first);
    includeFollower(&window, second);

    window.refreshVehicles();
    table = vehicleTable(&window);
    const int refreshedFirst = rowForLease(table, first);
    const int refreshedSecond = rowForLease(table, second);
    const int refreshedGround = rowForLease(table, ground);
    const int refreshedAir = rowForLease(table, air);
    QVERIFY(refreshedFirst >= 0);
    QVERIFY(refreshedSecond >= 0);
    QVERIFY(refreshedGround >= 0);
    QVERIFY(refreshedAir >= 0);
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        groundCombo->currentData()).sameInstance(ground));
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        airCombo->currentData()).sameInstance(air));
    QCOMPARE(table->item(refreshedGround, RoleColumn)->text(),
             QStringLiteral("Ground master (observed)"));
    QCOMPARE(table->item(refreshedAir, RoleColumn)->text(),
             QStringLiteral("Air master"));
    QCOMPARE(table->item(refreshedFirst, UseColumn)->checkState(),
             Qt::Checked);
    QCOMPARE(table->item(refreshedSecond, UseColumn)->checkState(),
             Qt::Checked);
    QCOMPARE(table->item(refreshedFirst, OrderColumn)->text(),
             QStringLiteral("2"));
    QCOMPARE(table->item(refreshedSecond, OrderColumn)->text(),
             QStringLiteral("1"));
}

void SwarmFollowLeaderWindowTest::
followerFamilyChangeClearsAnUneditableSelection()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    QVERIFY(rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air")).isValid());
    const SwarmVehicleInstanceLease follower = rig.addVehicle(
        6, 3, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Follower"));
    QVERIFY(follower.isValid());
    FakeSwarmFollowLeaderCommands commands;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));

    includeFollower(&window, follower);
    QTableWidget *table = vehicleTable(&window);
    int row = rowForLease(table, follower);
    QVERIFY(row >= 0);
    QCOMPARE(table->item(row, UseColumn)->checkState(), Qt::Checked);

    QVERIFY(rig.updateHeartbeat(
        follower, MAV_TYPE_GROUND_ROVER, CopterGuidedMode));

    table = vehicleTable(&window);
    row = rowForLease(table, follower);
    QVERIFY(row >= 0);
    QCOMPARE(table->item(row, UseColumn)->checkState(), Qt::Unchecked);
    QVERIFY(!(table->item(row, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));
}

void SwarmFollowLeaderWindowTest::
airMasterEligibilityLossSelectsAnEligibleReplacement()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    const SwarmVehicleInstanceLease previousAir = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Previous air"));
    const SwarmVehicleInstanceLease replacementAir = rig.addVehicle(
        6, 3, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Replacement air"));
    QVERIFY(previousAir.isValid());
    QVERIFY(replacementAir.isValid());
    FakeSwarmFollowLeaderCommands commands;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));
    QComboBox *airCombo = window.findChild<QComboBox *>(
        QStringLiteral("followLeaderAirMaster"));
    QVERIFY(airCombo);
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        airCombo->currentData()).sameInstance(previousAir));

    QVERIFY(rig.updateHeartbeat(
        previousAir, MAV_TYPE_GROUND_ROVER, CopterGuidedMode));

    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        airCombo->currentData()).sameInstance(replacementAir));
    QTableWidget *table = vehicleTable(&window);
    const int previousRow = rowForLease(table, previousAir);
    const int replacementRow = rowForLease(table, replacementAir);
    QVERIFY(previousRow >= 0);
    QVERIFY(replacementRow >= 0);
    QCOMPARE(table->item(previousRow, UseColumn)->checkState(),
             Qt::Unchecked);
    QCOMPARE(table->item(replacementRow, RoleColumn)->text(),
             QStringLiteral("Air master"));
}

void SwarmFollowLeaderWindowTest::noFollowersAirOnlyRunIsAccepted()
{
    FollowLeaderRig rig;
    const SwarmVehicleInstanceLease ground = rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground"));
    const SwarmVehicleInstanceLease air = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air"));
    QVERIFY(ground.isValid());
    QVERIFY(air.isValid());
    FakeSwarmFollowLeaderCommands commands;
    bool confirmationCalled = false;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(confirmationCalled);
    QVERIFY(window.isRunning());
    QCOMPARE(commands.reserveCalls, 1);
    QCOMPARE(commands.lastOwner, static_cast<QObject *>(&window));
    QCOMPARE(commands.lastMaximumBatchHz, 10);
    QCOMPARE(commands.lastMembers.size(), 2);
    QCOMPARE(commands.lastMembers.at(0).slotId, 1);
    QCOMPARE(commands.lastMembers.at(1).slotId, 2);
    QVERIFY(commands.lastMembers.at(0).lease.sameInstance(ground));
    QVERIFY(commands.lastMembers.at(1).lease.sameInstance(air));
    QVERIFY(commands.lastMembers.at(0).required.fields.testFlag(
        SwarmTelemetryRequirements::Position));
    QVERIFY(commands.lastMembers.at(0).required.fields.testFlag(
        SwarmTelemetryRequirements::Velocity));
    QCOMPARE(int(commands.lastMembers.at(0).flightMode), int(
        SwarmCommandMember::FlightModeRequirement::Any));
    QCOMPARE(int(commands.lastMembers.at(1).flightMode), int(
        SwarmCommandMember::FlightModeRequirement::ArduPilotGuided));
    QCOMPARE(commands.streamCalls, 1);
    QCOMPARE(commands.lastStreamRateHz, 10);
    QCOMPARE(commands.lastStreamSlots, QVector<int>({1, 2}));
    QCOMPARE(commands.targetBatches.size(), 1);
    QCOMPARE(commands.targetBatches.constFirst().size(), 1);
    QCOMPARE(commands.targetBatches.constFirst().constFirst().slotId, 2);
    QVERIFY(commands.targetBatches.constFirst().constFirst().useVelocity);

    const quint64 sessionId = commands.lastStreamSession;
    run->click();
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, 1);
    QCOMPARE(commands.releasedSessions, QVector<quint64>({sessionId}));
}

void SwarmFollowLeaderWindowTest::
cancelledConfirmationSendsAndReservesNothing()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    QVERIFY(rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air")).isValid());
    FakeSwarmFollowLeaderCommands commands;
    bool confirmationCalled = false;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, false));

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QCOMPARE(commands.releaseCalls, 0);
    QVERIFY(commands.targetSessions.isEmpty());
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
}

void SwarmFollowLeaderWindowTest::
nonGuidedAirRoleIsRejectedDespiteGuidedBaseFlag_data()
{
    QTest::addColumn<bool>("rejectAirMaster");
    QTest::newRow("air-master-loiter") << true;
    QTest::newRow("follower-loiter") << false;
}

void SwarmFollowLeaderWindowTest::
nonGuidedAirRoleIsRejectedDespiteGuidedBaseFlag()
{
    QFETCH(bool, rejectAirMaster);
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    const SwarmVehicleInstanceLease air = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR,
        rejectAirMaster ? CopterLoiterMode : CopterGuidedMode,
        QStringLiteral("Air"));
    QVERIFY(air.isValid());
    SwarmVehicleInstanceLease rejected = air;
    SwarmVehicleInstanceLease follower;
    if (!rejectAirMaster) {
        follower = rig.addVehicle(
            6, 3, MAV_TYPE_QUADROTOR, CopterLoiterMode,
            QStringLiteral("Loiter follower"));
        QVERIFY(follower.isValid());
        rejected = follower;
    }

    SwarmTelemetrySnapshot rejectedSnapshot;
    QVERIFY(rig.registry.snapshotForLease(rejected, &rejectedSnapshot));
    QVERIFY(rejectedSnapshot.baseMode & MAV_MODE_FLAG_GUIDED_ENABLED);
    QCOMPARE(rejectedSnapshot.customMode, CopterLoiterMode);

    FakeSwarmFollowLeaderCommands commands;
    bool confirmationCalled = false;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));
    if (!rejectAirMaster) {
        includeFollower(&window, follower);
    }

    QTableWidget *table = vehicleTable(&window);
    const int rejectedRow = rowForLease(table, rejected);
    QVERIFY(rejectedRow >= 0);
    QVERIFY(table->item(rejectedRow, LiveStatusColumn)->text().contains(
        QStringLiteral("not GUIDED"), Qt::CaseInsensitive));

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("GUIDED"), Qt::CaseInsensitive));
}

void SwarmFollowLeaderWindowTest::
unsafeRouteIsRejectedBeforeConfirmation()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    QVERIFY(rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air")).isValid());
    FakeSwarmFollowLeaderCommands commands;
    commands.blockedRouteLinks.insert(4);
    bool confirmationCalled = false;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("route"), Qt::CaseInsensitive));
}

void SwarmFollowLeaderWindowTest::
reentrantRouteRetirementIsRejectedBeforeConfirmation()
{
    FollowLeaderRig rig;
    QVERIFY(rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground")).isValid());
    const SwarmVehicleInstanceLease air = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air"));
    QVERIFY(air.isValid());
    FakeSwarmFollowLeaderCommands commands;
    bool confirmationCalled = false;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));

    bool retired = false;
    commands.onRouteCheck = [&](const SwarmVehicleInstanceLease &lease) {
        if (!retired && lease.sameInstance(air)) {
            retired = true;
            QVERIFY(rig.registry.endLinkSession(
                air.endpoint.linkId,
                rig.sessions.value(air.endpoint.linkId)));
        }
    };

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(retired);
    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QCOMPARE(commands.releaseCalls, 0);
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(!window.isRunning());
}

void SwarmFollowLeaderWindowTest::
bootstrapWaitsThenSendsOnePositionVelocityBatchAtTenHertz()
{
    FollowLeaderRig rig;
    const SwarmVehicleInstanceLease ground = rig.addVehicle(
        2, 1, MAV_TYPE_GROUND_ROVER, 0,
        QStringLiteral("Ground"), false);
    const SwarmVehicleInstanceLease air = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Air"), false);
    const SwarmVehicleInstanceLease follower = rig.addVehicle(
        6, 3, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Follower"), false);
    QVERIFY(ground.isValid());
    QVERIFY(air.isValid());
    QVERIFY(follower.isValid());
    FakeSwarmFollowLeaderCommands commands;
    SwarmFollowLeaderWindow window(
        &rig.registry, &commands, confirmation(nullptr, true));
    includeFollower(&window, follower);

    QPushButton *run = runButton(&window);
    QVERIFY(run);
    run->click();

    QVERIFY(window.isRunning());
    QCOMPARE(commands.reserveCalls, 1);
    QCOMPARE(commands.lastMaximumBatchHz, 10);
    QCOMPARE(commands.streamCalls, 1);
    QCOMPARE(commands.lastStreamRateHz, 10);
    QCOMPARE(commands.lastStreamSlots, QVector<int>({1, 2, 3}));
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(window.statusText().contains(
        QStringLiteral("Waiting for fresh"), Qt::CaseInsensitive));

    QVERIFY(rig.updatePosition(
        ground, 0.0, 1000, 4.0, -2.0, 1.0));
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(rig.updatePosition(
        air, 5.0, 1000, 0.0, 0.0, 0.0));
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(rig.updatePosition(
        follower, -5.0, 1000, 0.0, 0.0, 0.0));
    QTRY_COMPARE_WITH_TIMEOUT(commands.targetBatches.size(), 1, 1000);

    const QVector<SwarmPositionTarget> batch =
        commands.targetBatches.constFirst();
    QCOMPARE(batch.size(), 2);
    const SwarmPositionTarget airTarget = batch.at(0);
    QCOMPARE(airTarget.slotId, 2);
    QVERIFY(airTarget.useVelocity);
    QCOMPARE(airTarget.relativeAltitudeM, 10.0F);
    QVERIFY(std::abs(airTarget.velocityNorthMps - 2.4F) < 0.001F);
    QVERIFY(std::abs(airTarget.velocityEastMps + 1.2F) < 0.001F);
    QVERIFY(std::abs(airTarget.velocityDownMps - 0.6F) < 0.001F);
    QVERIFY(airTarget.latitudeDegrees >= -90.0
            && airTarget.latitudeDegrees <= 90.0);
    QVERIFY(airTarget.longitudeDegrees >= -180.0
            && airTarget.longitudeDegrees <= 180.0);
    const SwarmPositionTarget followerTarget = batch.at(1);
    QCOMPARE(followerTarget.slotId, 3);
    QVERIFY(followerTarget.useVelocity);
    QCOMPARE(followerTarget.relativeAltitudeM, 12.0F);
    QVERIFY(std::abs(followerTarget.velocityNorthMps - 2.0F) < 0.001F);
    QVERIFY(std::abs(followerTarget.velocityEastMps + 1.0F) < 0.001F);
    QVERIFY(std::abs(followerTarget.velocityDownMps - 0.5F) < 0.001F);
    QCOMPARE(commands.targetSessions.constFirst(),
             commands.lastStreamSession);
    QVERIFY(window.statusText().contains(
        QStringLiteral("active"), Qt::CaseInsensitive));

    run->click();
    QCOMPARE(commands.targetBatches.size(), 1);
    QCOMPARE(commands.releasedSessions,
             QVector<quint64>({commands.lastStreamSession}));
}

void SwarmFollowLeaderWindowTest::
planEditPartialSendRetirementAndCloseReleaseExactSessions()
{
    {
        FollowLeaderRig rig;
        QVERIFY(rig.addVehicle(
            2, 1, MAV_TYPE_GROUND_ROVER, 0,
            QStringLiteral("Ground")).isValid());
        QVERIFY(rig.addVehicle(
            4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
            QStringLiteral("Air")).isValid());
        FakeSwarmFollowLeaderCommands commands;
        SwarmFollowLeaderWindow window(
            &rig.registry, &commands, confirmation(nullptr, true));
        QPushButton *run = runButton(&window);
        QDoubleSpinBox *separation = window.findChild<QDoubleSpinBox *>(
            QStringLiteral("followLeaderSeparation"));
        QVERIFY(run);
        QVERIFY(separation);
        run->click();
        QVERIFY(window.isRunning());
        const quint64 sessionId = commands.lastStreamSession;

        separation->setValue(separation->value() + 1.0);

        QVERIFY(!window.isRunning());
        QCOMPARE(commands.releaseCalls, 1);
        QCOMPARE(commands.releasedSessions,
                 QVector<quint64>({sessionId}));
        QVERIFY(window.statusText().contains(
            QStringLiteral("setting changed"), Qt::CaseInsensitive));
    }

    {
        FollowLeaderRig rig;
        QVERIFY(rig.addVehicle(
            2, 1, MAV_TYPE_GROUND_ROVER, 0,
            QStringLiteral("Ground")).isValid());
        QVERIFY(rig.addVehicle(
            4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
            QStringLiteral("Air")).isValid());
        FakeSwarmFollowLeaderCommands commands;
        commands.nextSendResult = SwarmCommandService::Result::PartialSend;
        SwarmFollowLeaderWindow window(
            &rig.registry, &commands, confirmation(nullptr, true));
        QPushButton *run = runButton(&window);
        QVERIFY(run);

        run->click();

        QVERIFY(!window.isRunning());
        QCOMPARE(commands.reserveCalls, 1);
        QCOMPARE(commands.targetSessions.size(), 1);
        QCOMPARE(commands.releaseCalls, 1);
        QCOMPARE(commands.releasedSessions,
                 QVector<quint64>({commands.targetSessions.constFirst()}));
        QVERIFY(window.statusText().contains(
            QStringLiteral("partial"), Qt::CaseInsensitive));
    }

    {
        FollowLeaderRig rig;
        QVERIFY(rig.addVehicle(
            2, 1, MAV_TYPE_GROUND_ROVER, 0,
            QStringLiteral("Ground")).isValid());
        const SwarmVehicleInstanceLease air = rig.addVehicle(
            4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
            QStringLiteral("Air"));
        QVERIFY(air.isValid());
        FakeSwarmFollowLeaderCommands commands;
        SwarmFollowLeaderWindow window(
            &rig.registry, &commands, confirmation(nullptr, true));
        QPushButton *run = runButton(&window);
        QVERIFY(run);
        run->click();
        QVERIFY(window.isRunning());
        const quint64 sessionId = commands.lastStreamSession;

        QVERIFY(rig.registry.endLinkSession(
            air.endpoint.linkId,
            rig.sessions.value(air.endpoint.linkId)));

        QVERIFY(!window.isRunning());
        QCOMPARE(commands.releaseCalls, 1);
        QCOMPARE(commands.releasedSessions,
                 QVector<quint64>({sessionId}));
    }

    {
        FollowLeaderRig rig;
        QVERIFY(rig.addVehicle(
            2, 1, MAV_TYPE_GROUND_ROVER, 0,
            QStringLiteral("Ground")).isValid());
        QVERIFY(rig.addVehicle(
            4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
            QStringLiteral("Air")).isValid());
        FakeSwarmFollowLeaderCommands commands;
        SwarmFollowLeaderWindow window(
            &rig.registry, &commands, confirmation(nullptr, true));
        QPushButton *run = runButton(&window);
        QVERIFY(run);
        run->click();
        QVERIFY(window.isRunning());
        const quint64 sessionId = commands.lastStreamSession;

        QVERIFY(window.close());

        QVERIFY(!window.isRunning());
        QCOMPARE(commands.releaseCalls, 1);
        QCOMPARE(commands.releasedSessions,
                 QVector<quint64>({sessionId}));
        QVERIFY(window.statusText().contains(
            QStringLiteral("window closed"), Qt::CaseInsensitive));
    }
}

QTEST_MAIN(SwarmFollowLeaderWindowTest)

#include "test_swarmfollowleaderwindow.moc"
