#include "comm/SwarmTelemetryRegistry.h"
#include "ui/SwarmFollowPathWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QtTest>

#include <cmath>
#include <utility>

// The production application supplies these through
// SwarmFollowPathWindowIntegration.cpp. The focused widget target uses only
// the injected constructor and therefore has no LinkManager dependency.
SwarmTelemetryRegistry *SwarmFollowPathApplicationRegistry()
{
    return nullptr;
}

SwarmCommandService *SwarmFollowPathApplicationCommandService()
{
    return nullptr;
}

namespace
{

constexpr int UseColumn = 0;
constexpr int OrderColumn = 1;
constexpr int EndpointColumn = 2;
constexpr int SystemColumn = 3;
constexpr int RoleColumn = 5;
constexpr int LiveStatusColumn = 7;
constexpr quint32 CopterGuidedMode = 4;

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
    payload.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    if (customMode != 0) {
        payload.base_mode |= MAV_MODE_FLAG_GUIDED_ENABLED;
    }
    payload.custom_mode = customMode;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t globalPosition(int systemId, double eastM,
                                 quint32 bootTimeMs = 1000)
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
    payload.relative_alt = 23450;
    payload.hdg = 1234;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

class FollowPathRig
{
public:
    FollowPathRig()
        : registry([this]() { return nowMs; })
    {
    }

    SwarmVehicleInstanceLease addVehicle(
        int linkId, int systemId, int vehicleType,
        quint32 customMode, const QString &linkName = QString(),
        bool withPosition = true, double eastM = 0.0)
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
                linkId, session, globalPosition(systemId, eastM));
        }
        return registry.acquireVehicle(endpoint(linkId, systemId));
    }

    bool updatePosition(const SwarmVehicleInstanceLease &lease,
                        double eastM, quint32 bootTimeMs)
    {
        return registry.observeMessage(
            lease.endpoint.linkId,
            sessions.value(lease.endpoint.linkId),
            globalPosition(lease.endpoint.systemId, eastM, bootTimeMs));
    }

    qint64 nowMs = 100;
    QHash<int, quint64> sessions;
    SwarmTelemetryRegistry registry;
};

class FakeSwarmFollowPathCommands final
    : public SwarmFollowPathCommandInterface
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
    quint64 nextSessionId = 90;
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

SwarmFollowPathWindow::Dependencies confirmation(bool *called,
                                                  bool accepted)
{
    SwarmFollowPathWindow::Dependencies dependencies;
    dependencies.confirmDangerous = [called, accepted](
        QWidget *, const QString &, const QString &, const QString &) {
        if (called) {
            *called = true;
        }
        return accepted;
    };
    return dependencies;
}

QTableWidget *vehicleTable(SwarmFollowPathWindow *window)
{
    return window->findChild<QTableWidget *>(
        QStringLiteral("FollowPathVehicleGrid"));
}

void includeFollower(SwarmFollowPathWindow *window, int row)
{
    QTableWidget *table = vehicleTable(window);
    QVERIFY(table);
    QVERIFY(row >= 0 && row < table->rowCount());
    QVERIFY(table->item(row, UseColumn));
    table->item(row, UseColumn)->setCheckState(Qt::Checked);
}

} // namespace

class SwarmFollowPathWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void uiInventoryAndPlaneFollowerLockAreExplicit();
    void exactVehiclesKeepSelectionAndUniqueOrders();
    void cancelledConfirmationDoesNotReserveSender();
    void nonGuidedCustomModeIsRejectedDespiteGuidedBaseFlag();
    void unsafeRouteIsRejectedBeforeConfirmation();
    void reentrantRouteRetirementIsRejectedBeforeConfirmation();
    void bootstrapAndTrailWithholdTargetsUntilEnoughDistance();
    void planEditPartialSendAndRetirementStopExactSession();
};

void SwarmFollowPathWindowTest::
uiInventoryAndPlaneFollowerLockAreExplicit()
{
    FollowPathRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR,
                           0, QStringLiteral("Leader")).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_FIXED_WING,
                           15, QStringLiteral("Plane")).isValid());
    FakeSwarmFollowPathCommands commands;
    SwarmFollowPathWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));

    QCOMPARE(window.objectName(), QStringLiteral("SwarmFollowPathWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Swarm Follow Path (Beta)"));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QCOMPARE(window.size(), QSize(SwarmFollowPathWindow::WindowWidth,
                                 SwarmFollowPathWindow::WindowHeight));
    QCOMPARE(window.minimumSize(),
             QSize(SwarmFollowPathWindow::MinimumWindowWidth,
                   SwarmFollowPathWindow::MinimumWindowHeight));
    QVERIFY(window.layout());
    QVERIFY(window.findChildren<QWidget *>().size() > 15);

    const QStringList requiredControls = {
        QStringLiteral("followPathDangerBanner"),
        QStringLiteral("followPathLeader"),
        QStringLiteral("followPathRefreshVehicles"),
        QStringLiteral("followPathSeparation"),
        QStringLiteral("FollowPathVehicleGrid"),
        QStringLiteral("FollowPathRunButton"),
        QStringLiteral("followPathTakeoffAltitude"),
        QStringLiteral("followPathUnavailableHint"),
        QStringLiteral("followPathStatus"),
        QStringLiteral("followPathClose")
    };
    for (const QString &name : requiredControls) {
        QVERIFY2(window.findChild<QWidget *>(name), qPrintable(name));
    }

    QTableWidget *table = vehicleTable(&window);
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 9);
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(!(table->item(1, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));
    QVERIFY(!(table->item(1, UseColumn)->flags()
              & Qt::ItemIsEnabled));
    QVERIFY(table->item(1, RoleColumn)->text().contains(
        QStringLiteral("Leader only")));

    const QStringList unavailableButtons = {
        QStringLiteral("followPathArmFollowers"),
        QStringLiteral("followPathDisarmFollowers"),
        QStringLiteral("followPathTakeoffFollowers"),
        QStringLiteral("followPathLandFollowers")
    };
    for (const QString &name : unavailableButtons) {
        const QPushButton *control = window.findChild<QPushButton *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(!control->isEnabled(), qPrintable(name));
        QVERIFY2(!control->toolTip().isEmpty(), qPrintable(name));
    }
    const QDoubleSpinBox *takeoff = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("followPathTakeoffAltitude"));
    QVERIFY(takeoff);
    QCOMPARE(takeoff->minimum(), 1.0);
    QCOMPARE(takeoff->maximum(), 10000.0);
    QCOMPARE(takeoff->value(), 5.0);
    QVERIFY(!takeoff->isEnabled());
}

void SwarmFollowPathWindowTest::
exactVehiclesKeepSelectionAndUniqueOrders()
{
    FollowPathRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        3, 42, MAV_TYPE_QUADROTOR, 0, QStringLiteral("Radio A"));
    const SwarmVehicleInstanceLease first = rig.addVehicle(
        7, 42, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Radio B"));
    const SwarmVehicleInstanceLease second = rig.addVehicle(
        9, 43, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Radio C"));
    QVERIFY(leader.isValid());
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    FakeSwarmFollowPathCommands commands;
    SwarmFollowPathWindow window(
        &rig.registry, &commands, confirmation(nullptr, false));
    QTableWidget *table = vehicleTable(&window);
    const QComboBox *leaders = window.findChild<QComboBox *>(
        QStringLiteral("followPathLeader"));
    QVERIFY(table);
    QVERIFY(leaders);
    QCOMPARE(table->rowCount(), 3);
    QCOMPARE(leaders->count(), 3);
    QCOMPARE(table->item(0, SystemColumn)->text(), QStringLiteral("42"));
    QCOMPARE(table->item(1, SystemColumn)->text(), QStringLiteral("42"));
    QCOMPARE(table->item(0, EndpointColumn)->text(), QStringLiteral("Radio A"));
    QCOMPARE(table->item(1, EndpointColumn)->text(), QStringLiteral("Radio B"));
    QCOMPARE(table->item(1, OrderColumn)->text(), QStringLiteral("1"));
    QCOMPARE(table->item(2, OrderColumn)->text(), QStringLiteral("2"));

    includeFollower(&window, 1);
    window.refreshVehicles();
    table = vehicleTable(&window);
    QCOMPARE(table->item(1, UseColumn)->checkState(), Qt::Checked);
    QCOMPARE(table->item(1, OrderColumn)->text(), QStringLiteral("1"));
}

void SwarmFollowPathWindowTest::
cancelledConfirmationDoesNotReserveSender()
{
    FollowPathRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR,
                           0, QStringLiteral("Leader")).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_QUADROTOR,
                           CopterGuidedMode,
                           QStringLiteral("Follower")).isValid());
    FakeSwarmFollowPathCommands commands;
    bool confirmationCalled = false;
    SwarmFollowPathWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, false));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QVERIFY(run);
    run->click();

    QVERIFY(confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
}

void SwarmFollowPathWindowTest::
nonGuidedCustomModeIsRejectedDespiteGuidedBaseFlag()
{
    FollowPathRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR,
                           0, QStringLiteral("Leader")).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_QUADROTOR,
                           5, QStringLiteral("Loiter follower")).isValid());
    FakeSwarmFollowPathCommands commands;
    bool confirmationCalled = false;
    SwarmFollowPathWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));
    includeFollower(&window, 1);

    QTableWidget *table = vehicleTable(&window);
    QVERIFY(table->item(1, LiveStatusColumn)->text().contains(
        QStringLiteral("Loiter")));
    QVERIFY(table->item(1, LiveStatusColumn)->text().contains(
        QStringLiteral("not GUIDED")));

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QVERIFY(run);
    run->click();

    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QVERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(QStringLiteral("Loiter")));
}

void SwarmFollowPathWindowTest::
unsafeRouteIsRejectedBeforeConfirmation()
{
    FollowPathRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR,
                           0, QStringLiteral("Leader")).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_QUADROTOR,
                           CopterGuidedMode,
                           QStringLiteral("Follower")).isValid());
    FakeSwarmFollowPathCommands commands;
    commands.blockedRouteLinks.insert(4);
    bool confirmationCalled = false;
    SwarmFollowPathWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QVERIFY(run);
    run->click();

    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QVERIFY(window.statusText().contains(
        QStringLiteral("route"), Qt::CaseInsensitive));
}

void SwarmFollowPathWindowTest::
reentrantRouteRetirementIsRejectedBeforeConfirmation()
{
    FollowPathRig rig;
    QVERIFY(rig.addVehicle(2, 1, MAV_TYPE_QUADROTOR,
                           0, QStringLiteral("Leader")).isValid());
    QVERIFY(rig.addVehicle(4, 2, MAV_TYPE_QUADROTOR,
                           CopterGuidedMode,
                           QStringLiteral("Follower")).isValid());
    FakeSwarmFollowPathCommands commands;
    bool confirmationCalled = false;
    SwarmFollowPathWindow window(
        &rig.registry, &commands,
        confirmation(&confirmationCalled, true));
    includeFollower(&window, 1);

    bool retired = false;
    commands.onRouteCheck = [&](const SwarmVehicleInstanceLease &lease) {
        if (!retired && lease.endpoint.linkId == 4) {
            retired = true;
            QVERIFY(rig.registry.endLinkSession(4, rig.sessions.value(4)));
        }
    };
    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QVERIFY(run);
    run->click();

    QVERIFY(retired);
    QVERIFY(!confirmationCalled);
    QCOMPARE(commands.reserveCalls, 0);
    QCOMPARE(commands.streamCalls, 0);
    QVERIFY(!window.isRunning());
}

void SwarmFollowPathWindowTest::
bootstrapAndTrailWithholdTargetsUntilEnoughDistance()
{
    FollowPathRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        2, 1, MAV_TYPE_QUADROTOR, 0,
        QStringLiteral("Leader"), false);
    const SwarmVehicleInstanceLease follower = rig.addVehicle(
        4, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Follower"), false);
    QVERIFY(leader.isValid());
    QVERIFY(follower.isValid());
    FakeSwarmFollowPathCommands commands;
    SwarmFollowPathWindow window(
        &rig.registry, &commands, confirmation(nullptr, true));
    includeFollower(&window, 1);

    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QVERIFY(run);
    run->click();
    QVERIFY(window.isRunning());
    QCOMPARE(commands.reserveCalls, 1);
    QCOMPARE(commands.lastMaximumBatchHz, 5);
    QCOMPARE(commands.streamCalls, 1);
    QCOMPARE(commands.lastStreamRateHz, 5);
    QCOMPARE(commands.lastStreamSlots, QVector<int>({1, 2}));
    QVERIFY(commands.targetBatches.isEmpty());

    QVERIFY(rig.updatePosition(leader, 0.0, 1000));
    QVERIFY(rig.updatePosition(follower, -5.0, 1000));
    QTest::qWait(250);
    QVERIFY(commands.targetBatches.isEmpty());
    QVERIFY(window.statusText().contains(
        QStringLiteral("trail"), Qt::CaseInsensitive));

    QVERIFY(rig.updatePosition(leader, 10.0, 2000));
    QTRY_VERIFY_WITH_TIMEOUT(!commands.targetBatches.isEmpty(), 1000);
    const SwarmPositionTarget target =
        commands.targetBatches.constFirst().constFirst();
    QCOMPARE(target.slotId, 2);
    QVERIFY(!target.useVelocity);
    QVERIFY(target.latitudeDegrees >= -90.0
            && target.latitudeDegrees <= 90.0);
    QVERIFY(target.longitudeDegrees >= -180.0
            && target.longitudeDegrees <= 180.0);
    QVERIFY(window.statusText().contains(
        QStringLiteral("active"), Qt::CaseInsensitive));
}

void SwarmFollowPathWindowTest::
planEditPartialSendAndRetirementStopExactSession()
{
    FollowPathRig rig;
    const SwarmVehicleInstanceLease leader = rig.addVehicle(
        2, 1, MAV_TYPE_QUADROTOR, 0,
        QStringLiteral("Leader"), true, 0.0);
    const SwarmVehicleInstanceLease follower = rig.addVehicle(
        7, 2, MAV_TYPE_QUADROTOR, CopterGuidedMode,
        QStringLiteral("Follower"), true, -5.0);
    QVERIFY(leader.isValid());
    QVERIFY(follower.isValid());
    FakeSwarmFollowPathCommands commands;
    SwarmFollowPathWindow window(
        &rig.registry, &commands, confirmation(nullptr, true));
    includeFollower(&window, 1);
    QPushButton *run = window.findChild<QPushButton *>(
        QStringLiteral("FollowPathRunButton"));
    QTableWidget *table = vehicleTable(&window);
    QVERIFY(run);
    QVERIFY(table);

    run->click();
    QVERIFY(window.isRunning());
    table->item(1, OrderColumn)->setText(QStringLiteral("2"));
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, 1);

    table->item(1, OrderColumn)->setText(QStringLiteral("1"));
    run->click();
    QVERIFY(window.isRunning());
    commands.nextSendResult = SwarmCommandService::Result::PartialSend;
    QVERIFY(rig.updatePosition(leader, 10.0, 2000));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isRunning(), 1000);
    QCOMPARE(commands.releaseCalls, 2);
    QVERIFY(window.statusText().contains(
        QStringLiteral("partial"), Qt::CaseInsensitive));

    commands.nextSendResult = SwarmCommandService::Result::SentAll;
    run->click();
    QVERIFY(window.isRunning());
    const int releasesBeforeRetirement = commands.releaseCalls;
    QVERIFY(rig.registry.endLinkSession(
        follower.endpoint.linkId,
        rig.sessions.value(follower.endpoint.linkId)));
    QVERIFY(!window.isRunning());
    QCOMPARE(commands.releaseCalls, releasesBeforeRetirement + 1);
    QCOMPARE(window.vehicleCount(), 1);
}

QTEST_MAIN(SwarmFollowPathWindowTest)

#include "test_swarmfollowpathwindow.moc"
