#include "ui/SwarmWaypointLeaderWindow.h"
#include "ui/WaypointLeaderProfileControl.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QTest>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

static QPointer<SwarmWaypointLeaderWindowInterface>
    applicationWaypointLeaderInterface;

SwarmWaypointLeaderWindowInterface *
SwarmWaypointLeaderApplicationInterface(QObject *)
{
    return applicationWaypointLeaderInterface.data();
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
constexpr int MissionPositionColumn = 8;
constexpr int CommandedTargetColumn = 9;

SwarmVehicleInstanceLease lease(int linkId, int systemId,
                                int componentId = MAV_COMP_ID_AUTOPILOT1,
                                quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease value;
    value.endpoint.linkId = linkId;
    value.endpoint.systemId = systemId;
    value.endpoint.componentId = componentId;
    value.endpoint.linkName = QStringLiteral("Radio %1").arg(linkId);
    value.endpoint.componentName = QStringLiteral("Component %1")
        .arg(componentId);
    value.linkSessionEpoch = 100 + quint64(linkId);
    value.instanceEpoch = instanceEpoch;
    return value;
}

SwarmWaypointLeaderWindowVehicle vehicle(
    const SwarmVehicleInstanceLease &vehicleLease, const QString &label,
    bool groundEligible, bool flightEligible, double pathDistanceM = 0.0,
    double altitudeM = 10.0)
{
    SwarmWaypointLeaderWindowVehicle result;
    result.lease = vehicleLease;
    result.label = label;
    result.firmware = flightEligible ? QStringLiteral("ArduCopter")
                                     : QStringLiteral("ArduRover");
    result.liveStatus = QStringLiteral("GUIDED; disarmed; GPS 3");
    result.missionPosition = QStringLiteral("%1 m; off 0.5 m")
        .arg(pathDistanceM, 0, 'f', 1);
    result.commandedTarget = QStringLiteral("—");
    result.groundEligible = groundEligible;
    result.flightEligible = flightEligible;
    result.profilePositionValid = true;
    result.pathDistanceM = pathDistanceM;
    result.relativeAltitudeM = altitudeM;
    return result;
}

SwarmWaypointLeaderMissionSnapshot mission(
    const SwarmVehicleInstanceLease &airMaster, quint64 generation = 7)
{
    SwarmWaypointLeaderMissionSnapshot result;
    result.airMaster = airMaster;
    result.missionType = MAV_MISSION_TYPE_MISSION;
    result.contentGeneration = generation;
    result.contentDigest = QByteArray("exact-mission-digest-")
        + QByteArray::number(generation);
    result.items = {
        {0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         350000000, 330000000, 10.0F},
        {1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         350001000, 330000000, 25.0F},
        {2, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         350002000, 330000000, 18.0F}
    };
    return result;
}

class FakeWaypointLeaderInterface final
    : public SwarmWaypointLeaderWindowInterface
{
public:
    QVector<SwarmWaypointLeaderWindowVehicle> vehicles() const override
    {
        return vehicleRows;
    }

    void refreshVehicles() override
    {
        ++vehicleRefreshCalls;
        if (onVehicleRefresh) {
            onVehicleRefresh();
        }
    }

    bool missionForAirMaster(
        const SwarmVehicleInstanceLease &airMaster,
        SwarmWaypointLeaderMissionSnapshot *result,
        QString *error) const override
    {
        ++missionReads;
        if (result) {
            *result = SwarmWaypointLeaderMissionSnapshot();
        }
        if (!missionAvailable
            || !airMaster.sameInstance(missionSnapshot.airMaster)) {
            if (error) {
                *error = QStringLiteral(
                    "No valid exact mission for this air master.");
            }
            return false;
        }
        if (result) {
            *result = missionSnapshot;
        }
        if (error) {
            error->clear();
        }
        return true;
    }

    quint64 missionObservationRevision(
        const SwarmVehicleInstanceLease &airMaster) const noexcept override
    {
        ++missionRevisionReads;
        return airMaster.sameInstance(missionSnapshot.airMaster)
            ? missionObservationRevisionValue : 0;
    }

    bool refreshMission(const SwarmVehicleInstanceLease &airMaster,
                        QString *error) override
    {
        ++missionRefreshCalls;
        lastMissionRefresh = airMaster;
        if (onMissionRefresh) {
            onMissionRefresh();
        }
        if (error) {
            error->clear();
        }
        return missionRefreshAccepted;
    }

    void cancelMissionRefresh(
        const SwarmVehicleInstanceLease &airMaster,
        const QString &reason) override
    {
        ++missionRefreshCancelCalls;
        lastMissionRefreshCancellation = airMaster;
        missionRefreshCancelReasons.append(reason);
        if (onMissionRefreshCancel) {
            onMissionRefreshCancel();
        }
    }

    bool executorReady(QString *error) const override
    {
        ++readinessChecks;
        if (error) {
            *error = ready ? QString()
                           : QStringLiteral(
                               "The injected complete executor is unavailable.");
        }
        return ready;
    }

    bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                      QString *error) const override
    {
        ++validationCalls;
        lastValidatedPlan = plan;
        const bool missionMatches = missionAvailable
            && plan.airMaster.sameInstance(missionSnapshot.airMaster)
            && plan.missionContentGeneration
                == missionSnapshot.contentGeneration
            && plan.missionContentDigest == missionSnapshot.contentDigest;
        if (!ready || !acceptPlans || !missionMatches) {
            if (error) {
                *error = QStringLiteral(
                    "The complete exact plan did not validate.");
            }
            return false;
        }
        if (error) {
            error->clear();
        }
        return true;
    }

    bool start(const SwarmWaypointLeaderPlan &plan,
               QString *error) override
    {
        ++startCalls;
        lastStartedPlan = plan;
        if (!startAccepted || !validatePlan(plan, error)) {
            return false;
        }
        running = true;
        currentMode = SwarmWaypointLeaderMode::Idle;
        currentStatus = QStringLiteral(
            "Executor accepted exact plan; staged takeoff active.");
        notify();
        return true;
    }

    void cancelActiveRun(const QString &reason) override
    {
        ++cancelCalls;
        cancelReasons.append(reason);
        running = false;
        currentMode = SwarmWaypointLeaderMode::Idle;
        currentStatus = reason;
        notify();
    }

    bool requestMode(SwarmWaypointLeaderMode requested,
                     QString *error) override
    {
        ++modeRequestCalls;
        requestedModes.append(requested);
        if (!running || !modeRequestsAccepted) {
            if (error) {
                *error = QStringLiteral("Mode request rejected.");
            }
            return false;
        }
        currentMode = requested;
        currentStatus = QStringLiteral("Mode request accepted.");
        if (error) {
            error->clear();
        }
        notify();
        return true;
    }

    bool isRunning() const noexcept override { return running; }
    SwarmWaypointLeaderMode mode() const noexcept override
    {
        return currentMode;
    }
    QString statusText() const override { return currentStatus; }

    void setChangedHandler(ChangedHandler next) override
    {
        handler = std::move(next);
    }

    void notify()
    {
        const ChangedHandler callback = handler;
        if (callback) {
            callback();
        }
    }

    QVector<SwarmWaypointLeaderWindowVehicle> vehicleRows;
    SwarmWaypointLeaderMissionSnapshot missionSnapshot;
    quint64 missionObservationRevisionValue = 1;
    bool missionAvailable = true;
    bool missionRefreshAccepted = true;
    bool ready = true;
    bool acceptPlans = true;
    bool startAccepted = true;
    bool modeRequestsAccepted = true;
    bool running = false;
    SwarmWaypointLeaderMode currentMode = SwarmWaypointLeaderMode::Idle;
    QString currentStatus;
    int vehicleRefreshCalls = 0;
    mutable int missionReads = 0;
    mutable int missionRevisionReads = 0;
    int missionRefreshCalls = 0;
    int missionRefreshCancelCalls = 0;
    mutable int readinessChecks = 0;
    mutable int validationCalls = 0;
    int startCalls = 0;
    int cancelCalls = 0;
    int modeRequestCalls = 0;
    SwarmVehicleInstanceLease lastMissionRefresh;
    SwarmVehicleInstanceLease lastMissionRefreshCancellation;
    mutable SwarmWaypointLeaderPlan lastValidatedPlan;
    SwarmWaypointLeaderPlan lastStartedPlan;
    QVector<SwarmWaypointLeaderMode> requestedModes;
    QStringList cancelReasons;
    QStringList missionRefreshCancelReasons;
    std::function<void()> onVehicleRefresh;
    std::function<void()> onMissionRefresh;
    std::function<void()> onMissionRefreshCancel;
    ChangedHandler handler;
};

SwarmWaypointLeaderWindow::Dependencies confirmation(
    bool accepted, int *calls = nullptr,
    std::function<void()> duringConfirmation = {})
{
    SwarmWaypointLeaderWindow::Dependencies dependencies;
    dependencies.confirmDangerous =
        [accepted, calls, duringConfirmation =
             std::move(duringConfirmation)](
            QWidget *, const QString &, const QString &, const QString &) {
            if (calls) {
                ++*calls;
            }
            if (duringConfirmation) {
                duringConfirmation();
            }
            return accepted;
        };
    return dependencies;
}

QTableWidget *vehicleTable(SwarmWaypointLeaderWindow *window)
{
    return window->findChild<QTableWidget *>(
        QStringLiteral("WaypointLeaderVehicleGrid"));
}

QPushButton *startButton(SwarmWaypointLeaderWindow *window)
{
    return window->findChild<QPushButton *>(
        QStringLiteral("WaypointLeaderRunButton"));
}

int comboIndexForLease(QComboBox *combo,
                       const SwarmVehicleInstanceLease &vehicleLease)
{
    if (!combo) {
        return -1;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (qvariant_cast<SwarmVehicleInstanceLease>(
                combo->itemData(index)).sameInstance(vehicleLease)) {
            return index;
        }
    }
    return -1;
}

int rowForLease(QTableWidget *table,
                const SwarmVehicleInstanceLease &vehicleLease)
{
    if (!table) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *endpoint = table->item(row, EndpointColumn);
        if (endpoint && qvariant_cast<SwarmVehicleInstanceLease>(
                            endpoint->data(Qt::UserRole))
                            .sameInstance(vehicleLease)) {
            return row;
        }
    }
    return -1;
}

void includeFollower(SwarmWaypointLeaderWindow *window,
                     const SwarmVehicleInstanceLease &vehicleLease)
{
    QTableWidget *table = vehicleTable(window);
    QVERIFY(table);
    const int row = rowForLease(table, vehicleLease);
    QVERIFY(row >= 0);
    QVERIFY(table->item(row, UseColumn)->flags()
            & Qt::ItemIsUserCheckable);
    table->item(row, UseColumn)->setCheckState(Qt::Checked);
}

void setOrder(SwarmWaypointLeaderWindow *window,
              const SwarmVehicleInstanceLease &vehicleLease, int order)
{
    QTableWidget *table = vehicleTable(window);
    QVERIFY(table);
    const int row = rowForLease(table, vehicleLease);
    QVERIFY(row >= 0);
    table->item(row, OrderColumn)->setText(QString::number(order));
}

void populateUsableFixture(FakeWaypointLeaderInterface *fake,
                           SwarmVehicleInstanceLease *ground,
                           SwarmVehicleInstanceLease *air,
                           SwarmVehicleInstanceLease *follower = nullptr)
{
    *ground = lease(1, 42);
    *air = lease(2, 42);
    fake->vehicleRows = {
        vehicle(*ground, QStringLiteral("Ground Radio — 42:1"),
                true, false, 5.0, 4.0),
        vehicle(*air, QStringLiteral("Air Radio — 42:1"),
                true, true, 30.0, 15.0)
    };
    if (follower) {
        *follower = lease(3, 43);
        fake->vehicleRows.append(vehicle(
            *follower, QStringLiteral("Follower Radio — 43:1"),
            true, true, 24.0, 13.0));
    }
    fake->missionSnapshot = mission(*air);
}
} // namespace

class SwarmWaypointLeaderWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void inventoryDefaultsSplitAndExactIdentityAreStable();
    void defaultWindowUsesApplicationInterfaceFactory();
    void readinessMissionAndEligibilityFailClosed();
    void missionRefreshHidesUntilMatchingObservation();
    void airMasterReplacementCancelsOnlyPreviousRefresh();
    void closeCancelsMissionRefreshWithoutActiveRun();
    void missionRefreshCallbackCanDeleteWindow();
    void duplicateSysidsPreserveExactRolesAndOrdersAcrossRefresh();
    void confirmationDefaultsToCancelAndPlanIsRevalidated();
    void nestedConfirmationCloseNeverStarts();
    void acceptedActionsAndCloseCancelTheExactRun();
    void openWindowIsModelessSingleton();
};

void SwarmWaypointLeaderWindowTest::
defaultWindowUsesApplicationInterfaceFactory()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    applicationWaypointLeaderInterface = &fake;

    QPointer<SwarmWaypointLeaderWindow> window =
        SwarmWaypointLeaderWindow::OpenWindow();
    QVERIFY(window);
    QCOMPARE(window->vehicleCount(), 2);
    QVERIFY(startButton(window)->isEnabled());
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QVERIFY(!window);
    QVERIFY(!fake.handler);
    applicationWaypointLeaderInterface = nullptr;
}

void SwarmWaypointLeaderWindowTest::
inventoryDefaultsSplitAndExactIdentityAreStable()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    SwarmVehicleInstanceLease follower;
    populateUsableFixture(&fake, &ground, &air, &follower);
    const SwarmVehicleInstanceLease component = lease(4, 42, 100);
    SwarmWaypointLeaderWindowVehicle camera = vehicle(
        component, QStringLiteral("Camera — 42:100"), false, false);
    camera.profilePositionValid = false;
    fake.vehicleRows.append(camera);

    SwarmWaypointLeaderWindow window(
        &fake, confirmation(false));
    QCOMPARE(window.objectName(), QStringLiteral("SwarmWaypointLeaderWindow"));
    QCOMPARE(window.windowTitle(),
             QStringLiteral("Swarm Waypoint Leader (Beta)"));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QCOMPARE(window.size(), QSize(1320, 790));
    QCOMPARE(window.minimumSize(), QSize(1040, 650));
    QCOMPARE(window.size(), QSize(SwarmWaypointLeaderWindow::WindowWidth,
                                 SwarmWaypointLeaderWindow::WindowHeight));
    QCOMPARE(window.minimumSize(), QSize(
        SwarmWaypointLeaderWindow::MinimumWindowWidth,
        SwarmWaypointLeaderWindow::MinimumWindowHeight));
    QCOMPARE(window.vehicleCount(), 4);
    QVERIFY(window.layout());
    QVERIFY(window.findChildren<QWidget *>().size() > 25);

    const QStringList controls = {
        QStringLiteral("waypointLeaderDangerBanner"),
        QStringLiteral("WaypointLeaderGroundMasterCombo"),
        QStringLiteral("WaypointLeaderAirMasterCombo"),
        QStringLiteral("waypointLeaderRefreshVehicles"),
        QStringLiteral("waypointLeaderRefreshMission"),
        QStringLiteral("waypointLeaderMissionStatus"),
        QStringLiteral("waypointLeaderSeparation"),
        QStringLiteral("waypointLeaderLead"),
        QStringLiteral("waypointLeaderOffPath"),
        QStringLiteral("waypointLeaderAltitudeSeparation"),
        QStringLiteral("waypointLeaderNavigationAcceleration"),
        QStringLiteral("waypointLeaderVFormation"),
        QStringLiteral("waypointLeaderAltitudeInterleave"),
        QStringLiteral("waypointLeaderMode"),
        QStringLiteral("WaypointLeaderVehicleGrid"),
        QStringLiteral("waypointLeaderMainSplitter"),
        QStringLiteral("WaypointLeaderProfile"),
        QStringLiteral("WaypointLeaderRunButton"),
        QStringLiteral("WaypointLeaderStopButton"),
        QStringLiteral("waypointLeaderResetState"),
        QStringLiteral("waypointLeaderReturnAlongMission"),
        QStringLiteral("waypointLeaderAbandonMission"),
        QStringLiteral("waypointLeaderStatus"),
        QStringLiteral("waypointLeaderClose")
    };
    for (const QString &name : controls) {
        QVERIFY2(window.findChild<QWidget *>(name), qPrintable(name));
    }

    QTableWidget *table = vehicleTable(&window);
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 10);
    QCOMPARE(table->rowCount(), 4);
    const QStringList headers = {
        QStringLiteral("Use"), QStringLiteral("Order"),
        QStringLiteral("Endpoint"), QStringLiteral("Sys"),
        QStringLiteral("Comp"), QStringLiteral("Role"),
        QStringLiteral("Firmware"), QStringLiteral("Live"),
        QStringLiteral("Mission position"),
        QStringLiteral("Commanded target")
    };
    for (int column = 0; column < headers.size(); ++column) {
        QVERIFY(table->horizontalHeaderItem(column));
        QCOMPARE(table->horizontalHeaderItem(column)->text(),
                 headers.at(column));
    }
    const int groundRow = rowForLease(table, ground);
    const int airRow = rowForLease(table, air);
    const int followerRow = rowForLease(table, follower);
    const int componentRow = rowForLease(table, component);
    QVERIFY(groundRow >= 0);
    QVERIFY(airRow >= 0);
    QVERIFY(followerRow >= 0);
    QVERIFY(componentRow >= 0);
    QCOMPARE(table->item(groundRow, SystemColumn)->text(),
             QStringLiteral("42"));
    QCOMPARE(table->item(airRow, SystemColumn)->text(),
             QStringLiteral("42"));
    QCOMPARE(table->item(componentRow, ComponentColumn)->text(),
             QStringLiteral("100"));
    QCOMPARE(table->item(groundRow, EndpointColumn)->text(),
             QStringLiteral("Radio 1"));
    QCOMPARE(table->item(airRow, EndpointColumn)->text(),
             QStringLiteral("Radio 2"));
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        table->item(groundRow, EndpointColumn)->data(Qt::UserRole))
        .sameInstance(ground));
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        table->item(airRow, EndpointColumn)->data(Qt::UserRole))
        .sameInstance(air));
    QCOMPARE(table->item(groundRow, RoleColumn)->text(),
             QStringLiteral("Ground master (observed)"));
    QCOMPARE(table->item(airRow, RoleColumn)->text(),
             QStringLiteral("Air master"));
    QVERIFY(table->item(followerRow, UseColumn)->flags()
            & Qt::ItemIsUserCheckable);
    QVERIFY(!(table->item(componentRow, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));
    QVERIFY(table->item(followerRow, OrderColumn)->flags()
            & Qt::ItemIsEditable);
    QVERIFY(!(table->item(componentRow, OrderColumn)->flags()
              & Qt::ItemIsEditable));
    QVERIFY(table->item(componentRow, FirmwareColumn));
    QVERIFY(table->item(componentRow, LiveStatusColumn));
    QVERIFY(table->item(componentRow, MissionPositionColumn));
    QVERIFY(table->item(componentRow, CommandedTargetColumn));

    const auto *separation = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("waypointLeaderSeparation"));
    const auto *lead = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("waypointLeaderLead"));
    const auto *offPath = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("waypointLeaderOffPath"));
    const auto *altitude = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("waypointLeaderAltitudeSeparation"));
    const auto *acceleration = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("waypointLeaderNavigationAcceleration"));
    QVERIFY(separation);
    QVERIFY(lead);
    QVERIFY(offPath);
    QVERIFY(altitude);
    QVERIFY(acceleration);
    QCOMPARE(separation->value(), 5.0);
    QCOMPARE(separation->minimum(), 2.0);
    QCOMPARE(separation->maximum(), 500.0);
    QCOMPARE(lead->value(), 20.0);
    QCOMPARE(lead->minimum(), -500.0);
    QCOMPARE(lead->maximum(), 5000.0);
    QCOMPARE(offPath->value(), 10.0);
    QCOMPARE(altitude->value(), 2.0);
    QCOMPARE(acceleration->value(), 1.0);
    QCOMPARE(acceleration->singleStep(), 0.1);
    QVERIFY(!window.findChild<QCheckBox *>(
        QStringLiteral("waypointLeaderVFormation"))->isChecked());
    QVERIFY(!window.findChild<QCheckBox *>(
        QStringLiteral("waypointLeaderAltitudeInterleave"))->isChecked());

    auto *profile = window.findChild<WaypointLeaderProfileControl *>();
    QVERIFY(profile);
    QCOMPARE(profile->profile().size(), 3);
    QCOMPARE(profile->vehicleMarkers().size(), 3);
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("waypointLeaderMissionStatus"))->text().contains(
            QStringLiteral("signature"), Qt::CaseInsensitive));
    QVERIFY(startButton(&window)->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(
        QStringLiteral("WaypointLeaderStopButton"))->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderResetState"))->isEnabled());

    window.show();
    QCoreApplication::processEvents();
    QSplitter *splitter = window.findChild<QSplitter *>(
        QStringLiteral("waypointLeaderMainSplitter"));
    QVERIFY(splitter);
    const QList<int> sizes = splitter->sizes();
    QCOMPARE(sizes.size(), 2);
    QVERIFY(sizes.at(0) > 0);
    QVERIFY(sizes.at(1) > 0);
    const double ratio = double(sizes.at(0)) / sizes.at(1);
    QVERIFY(ratio > 1.35 && ratio < 1.65);
}

void SwarmWaypointLeaderWindowTest::
readinessMissionAndEligibilityFailClosed()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    SwarmVehicleInstanceLease follower;
    populateUsableFixture(&fake, &ground, &air, &follower);
    fake.ready = false;
    SwarmWaypointLeaderWindow window(&fake, confirmation(true));
    QPushButton *start = startButton(&window);
    QVERIFY(start);
    QVERIFY(!start->isEnabled());
    QVERIFY(start->toolTip().contains(
        QStringLiteral("unavailable"), Qt::CaseInsensitive));
    start->click();
    QCOMPARE(fake.startCalls, 0);

    fake.ready = true;
    fake.notify();
    QVERIFY(start->isEnabled());
    fake.missionAvailable = false;
    fake.notify();
    QVERIFY(!start->isEnabled());
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("waypointLeaderMissionStatus"))->text().contains(
            QStringLiteral("No valid exact mission")));
    fake.missionAvailable = true;
    fake.notify();
    QVERIFY(start->isEnabled());

    fake.vehicleRows[2].pathDistanceM =
        std::numeric_limits<double>::quiet_NaN();
    fake.notify();
    auto *profile = window.findChild<WaypointLeaderProfileControl *>();
    QVERIFY(profile);
    QCOMPARE(profile->vehicleMarkers().size(), 2);

    fake.vehicleRows[2].flightEligible = false;
    fake.notify();
    QTableWidget *table = vehicleTable(&window);
    const int row = rowForLease(table, follower);
    QVERIFY(row >= 0);
    QVERIFY(!(table->item(row, UseColumn)->flags()
              & Qt::ItemIsUserCheckable));

    window.refreshMission();
    QCOMPARE(fake.missionRefreshCalls, 1);
    QVERIFY(fake.lastMissionRefresh.sameInstance(air));
}

void SwarmWaypointLeaderWindowTest::
missionRefreshHidesUntilMatchingObservation()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    SwarmWaypointLeaderWindow window(&fake, confirmation(false));
    auto *profile = window.findChild<WaypointLeaderProfileControl *>();
    auto *missionStatus = window.findChild<QLabel *>(
        QStringLiteral("waypointLeaderMissionStatus"));
    QVERIFY(profile);
    QVERIFY(missionStatus);
    QVERIFY(startButton(&window)->isEnabled());
    QCOMPARE(profile->profile().size(), 3);
    const int initialMissionReads = fake.missionReads;

    window.refreshMission();
    QCOMPARE(fake.missionRefreshCalls, 1);
    QVERIFY(fake.lastMissionRefresh.sameInstance(air));
    QCOMPARE(profile->profile().size(), 0);
    QCOMPARE(profile->vehicleMarkers().size(), 0);
    QVERIFY(!startButton(&window)->isEnabled());
    QCOMPARE(fake.missionReads, initialMissionReads);
    QVERIFY(missionStatus->text().contains(
        QStringLiteral("hidden"), Qt::CaseInsensitive));

    // A failed/late generic adapter notification with no new successful
    // observation cannot expose the cached pre-refresh snapshot again.
    fake.currentStatus = QStringLiteral("Mission refresh failed.");
    fake.notify();
    QCOMPARE(profile->profile().size(), 0);
    QCOMPARE(profile->vehicleMarkers().size(), 0);
    QVERIFY(!startButton(&window)->isEnabled());
    QCOMPARE(fake.missionReads, initialMissionReads);

    // Identical content is still a valid completion when the adapter's
    // successful-observation revision advances.
    ++fake.missionObservationRevisionValue;
    fake.notify();
    QCOMPARE(profile->profile().size(), 3);
    QVERIFY(!profile->vehicleMarkers().isEmpty());
    QVERIFY(startButton(&window)->isEnabled());
    QCOMPARE(fake.missionReads, initialMissionReads + 1);
    QCOMPARE(fake.missionRefreshCancelCalls, 0);

    FakeWaypointLeaderInterface reentrantFake;
    populateUsableFixture(&reentrantFake, &ground, &air);
    SwarmWaypointLeaderWindow reentrantWindow(
        &reentrantFake, confirmation(false));
    auto *reentrantProfile =
        reentrantWindow.findChild<WaypointLeaderProfileControl *>();
    QVERIFY(reentrantProfile);
    reentrantFake.onMissionRefresh = [&reentrantFake]() {
        ++reentrantFake.missionObservationRevisionValue;
        reentrantFake.notify();
    };
    reentrantWindow.refreshMission();
    QCOMPARE(reentrantFake.missionRefreshCalls, 1);
    QCOMPARE(reentrantProfile->profile().size(), 3);
    QVERIFY(startButton(&reentrantWindow)->isEnabled());
    QCOMPARE(reentrantFake.missionRefreshCancelCalls, 0);
}

void SwarmWaypointLeaderWindowTest::
airMasterReplacementCancelsOnlyPreviousRefresh()
{
    {
        FakeWaypointLeaderInterface fake;
        SwarmVehicleInstanceLease ground;
        SwarmVehicleInstanceLease air;
        SwarmVehicleInstanceLease replacement;
        populateUsableFixture(&fake, &ground, &air, &replacement);
        SwarmWaypointLeaderWindow window(&fake, confirmation(false));
        auto *profile = window.findChild<WaypointLeaderProfileControl *>();
        auto *airCombo = window.findChild<QComboBox *>(
            QStringLiteral("WaypointLeaderAirMasterCombo"));
        QVERIFY(profile);
        QVERIFY(airCombo);

        window.refreshMission();
        const int replacementIndex =
            comboIndexForLease(airCombo, replacement);
        QVERIFY(replacementIndex >= 0);
        airCombo->setCurrentIndex(replacementIndex);
        QCOMPARE(fake.missionRefreshCancelCalls, 1);
        QVERIFY(fake.lastMissionRefreshCancellation.sameInstance(air));
        QVERIFY(!fake.lastMissionRefreshCancellation.sameInstance(
            replacement));
        QCOMPARE(profile->profile().size(), 0);
        QVERIFY(!startButton(&window)->isEnabled());

        fake.missionSnapshot = mission(replacement);
        ++fake.missionObservationRevisionValue;
        fake.notify();
        QCOMPARE(profile->profile().size(), 3);
        QVERIFY(startButton(&window)->isEnabled());

        // Begin a refresh for the replacement.  A late generic callback from
        // the cancelled old request has no matching revision and cannot
        // resurrect the replacement's now-hidden pre-refresh snapshot.
        window.refreshMission();
        QCOMPARE(fake.missionRefreshCalls, 2);
        fake.notify();
        QCOMPARE(profile->profile().size(), 0);
        QVERIFY(!startButton(&window)->isEnabled());
        QCOMPARE(fake.missionRefreshCancelCalls, 1);
    }

    {
        FakeWaypointLeaderInterface fake;
        SwarmVehicleInstanceLease ground;
        SwarmVehicleInstanceLease air;
        SwarmVehicleInstanceLease replacement;
        populateUsableFixture(&fake, &ground, &air, &replacement);
        SwarmWaypointLeaderWindow window(&fake, confirmation(false));
        window.refreshMission();

        fake.vehicleRows = {fake.vehicleRows.at(0),
                            fake.vehicleRows.at(2)};
        fake.missionSnapshot = mission(replacement);
        ++fake.missionObservationRevisionValue;
        fake.notify();
        QCOMPARE(fake.missionRefreshCancelCalls, 1);
        QVERIFY(fake.lastMissionRefreshCancellation.sameInstance(air));
        QVERIFY(!fake.lastMissionRefreshCancellation.sameInstance(
            replacement));
        QVERIFY(startButton(&window)->isEnabled());
    }
}

void SwarmWaypointLeaderWindowTest::
closeCancelsMissionRefreshWithoutActiveRun()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    SwarmWaypointLeaderWindow window(&fake, confirmation(false));
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(!window.isRunning());

    window.refreshMission();
    QCOMPARE(fake.cancelCalls, 0);
    QCOMPARE(fake.missionRefreshCancelCalls, 0);
    window.close();
    QCOMPARE(fake.cancelCalls, 0);
    QCOMPARE(fake.missionRefreshCancelCalls, 1);
    QVERIFY(fake.lastMissionRefreshCancellation.sameInstance(air));
    QVERIFY(fake.missionRefreshCancelReasons.constFirst().contains(
        QStringLiteral("window closed"), Qt::CaseInsensitive));
}

void SwarmWaypointLeaderWindowTest::
missionRefreshCallbackCanDeleteWindow()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    QPointer<SwarmWaypointLeaderWindow> guarded =
        new SwarmWaypointLeaderWindow(&fake, confirmation(false));
    fake.onMissionRefresh = [&guarded]() {
        delete guarded.data();
    };

    guarded->refreshMission();
    QVERIFY(!guarded);
    QCOMPARE(fake.missionRefreshCalls, 1);
    QCOMPARE(fake.missionRefreshCancelCalls, 1);
    QVERIFY(fake.lastMissionRefreshCancellation.sameInstance(air));
    QCOMPARE(fake.startCalls, 0);
    QCOMPARE(fake.cancelCalls, 0);
    QVERIFY(!fake.handler);
}

void SwarmWaypointLeaderWindowTest::
duplicateSysidsPreserveExactRolesAndOrdersAcrossRefresh()
{
    FakeWaypointLeaderInterface fake;
    const SwarmVehicleInstanceLease ground = lease(1, 10);
    const SwarmVehicleInstanceLease air = lease(2, 11);
    const SwarmVehicleInstanceLease first = lease(3, 77);
    const SwarmVehicleInstanceLease second = lease(4, 77);
    fake.vehicleRows = {
        vehicle(ground, QStringLiteral("Ground"), true, false),
        vehicle(air, QStringLiteral("Air"), true, true),
        vehicle(first, QStringLiteral("Duplicate A"), true, true),
        vehicle(second, QStringLiteral("Duplicate B"), true, true)
    };
    fake.missionSnapshot = mission(air);
    SwarmWaypointLeaderWindow window(&fake, confirmation(false));

    setOrder(&window, first, 2);
    setOrder(&window, second, 1);
    includeFollower(&window, first);
    includeFollower(&window, second);
    std::reverse(fake.vehicleRows.begin(), fake.vehicleRows.end());
    window.refreshVehicles();
    QCOMPARE(fake.vehicleRefreshCalls, 1);

    QTableWidget *table = vehicleTable(&window);
    const int firstRow = rowForLease(table, first);
    const int secondRow = rowForLease(table, second);
    QVERIFY(firstRow >= 0);
    QVERIFY(secondRow >= 0);
    QCOMPARE(table->item(firstRow, SystemColumn)->text(),
             QStringLiteral("77"));
    QCOMPARE(table->item(secondRow, SystemColumn)->text(),
             QStringLiteral("77"));
    QCOMPARE(table->item(firstRow, EndpointColumn)->text(),
             QStringLiteral("Radio 3"));
    QCOMPARE(table->item(secondRow, EndpointColumn)->text(),
             QStringLiteral("Radio 4"));
    QCOMPARE(table->item(firstRow, UseColumn)->checkState(), Qt::Checked);
    QCOMPARE(table->item(secondRow, UseColumn)->checkState(), Qt::Checked);
    QCOMPARE(table->item(firstRow, OrderColumn)->text(), QStringLiteral("2"));
    QCOMPARE(table->item(secondRow, OrderColumn)->text(), QStringLiteral("1"));
    QCOMPARE(table->item(rowForLease(table, ground), RoleColumn)->text(),
             QStringLiteral("Ground master (observed)"));
    QCOMPARE(table->item(rowForLease(table, air), RoleColumn)->text(),
             QStringLiteral("Air master"));

    QComboBox *groundCombo = window.findChild<QComboBox *>(
        QStringLiteral("WaypointLeaderGroundMasterCombo"));
    QComboBox *airCombo = window.findChild<QComboBox *>(
        QStringLiteral("WaypointLeaderAirMasterCombo"));
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        groundCombo->currentData()).sameInstance(ground));
    QVERIFY(qvariant_cast<SwarmVehicleInstanceLease>(
        airCombo->currentData()).sameInstance(air));
}

void SwarmWaypointLeaderWindowTest::
confirmationDefaultsToCancelAndPlanIsRevalidated()
{
    {
        FakeWaypointLeaderInterface fake;
        SwarmVehicleInstanceLease ground;
        SwarmVehicleInstanceLease air;
        populateUsableFixture(&fake, &ground, &air);
        SwarmWaypointLeaderWindow window(
            &fake, SwarmWaypointLeaderWindow::Dependencies());

        bool sawDialog = false;
        bool cancelWasDefault = false;
        bool cancelWasEscape = false;
        QTimer::singleShot(0, [&]() {
            auto *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box) {
                return;
            }
            QAbstractButton *cancel = box->button(QMessageBox::Cancel);
            sawDialog = cancel != nullptr;
            cancelWasDefault = box->defaultButton() == cancel;
            cancelWasEscape = box->escapeButton() == cancel;
            if (cancel) {
                cancel->click();
            }
        });
        startButton(&window)->click();
        QVERIFY(sawDialog);
        QVERIFY(cancelWasDefault);
        QVERIFY(cancelWasEscape);
        QCOMPARE(fake.startCalls, 0);
        QVERIFY(window.statusText().contains(
            QStringLiteral("cancelled"), Qt::CaseInsensitive));
    }

    {
        FakeWaypointLeaderInterface fake;
        SwarmVehicleInstanceLease ground;
        SwarmVehicleInstanceLease air;
        populateUsableFixture(&fake, &ground, &air);
        int confirmations = 0;
        SwarmWaypointLeaderWindow window(
            &fake, confirmation(true, &confirmations, [&fake]() {
                fake.missionSnapshot = mission(
                    fake.missionSnapshot.airMaster,
                    fake.missionSnapshot.contentGeneration + 1);
            }));
        startButton(&window)->click();
        QCOMPARE(confirmations, 1);
        QCOMPARE(fake.startCalls, 0);
        QVERIFY(fake.validationCalls >= 2);
        QVERIFY(window.statusText().contains(
            QStringLiteral("changed while confirmation"),
            Qt::CaseInsensitive));
    }
}

void SwarmWaypointLeaderWindowTest::nestedConfirmationCloseNeverStarts()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    QPointer<SwarmWaypointLeaderWindow> guarded;

    SwarmWaypointLeaderWindow::Dependencies dependencies;
    dependencies.confirmDangerous =
        [&guarded](QWidget *, const QString &, const QString &,
                   const QString &) {
            QEventLoop nestedLoop;
            QTimer::singleShot(0, [&guarded, &nestedLoop]() {
                if (guarded) {
                    guarded->close();
                }
                nestedLoop.quit();
            });
            nestedLoop.exec();
            return true;
        };

    SwarmWaypointLeaderWindow *window =
        SwarmWaypointLeaderWindow::OpenWindow(
            &fake, std::move(dependencies));
    guarded = window;
    QVERIFY(startButton(window)->isEnabled());
    startButton(window)->click();

    QCOMPARE(fake.startCalls, 0);
    QTRY_VERIFY(guarded.isNull());
}

void SwarmWaypointLeaderWindowTest::
acceptedActionsAndCloseCancelTheExactRun()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    SwarmVehicleInstanceLease follower;
    populateUsableFixture(&fake, &ground, &air, &follower);
    int confirmations = 0;
    SwarmWaypointLeaderWindow window(
        &fake, confirmation(true, &confirmations));
    window.show();
    QCoreApplication::processEvents();
    QSignalSpy runningChanged(
        &window, &SwarmWaypointLeaderWindow::runningChanged);
    includeFollower(&window, follower);
    setOrder(&window, follower, 1);

    startButton(&window)->click();
    QCOMPARE(fake.startCalls, 1);
    QVERIFY(window.isRunning());
    QVERIFY(fake.lastStartedPlan.groundMaster.sameInstance(ground));
    QVERIFY(fake.lastStartedPlan.airMaster.sameInstance(air));
    QCOMPARE(fake.lastStartedPlan.followers.size(), 1);
    QVERIFY(fake.lastStartedPlan.followers.constFirst().lease
        .sameInstance(follower));
    QCOMPARE(fake.lastStartedPlan.followers.constFirst().order, 1);
    QCOMPARE(fake.lastStartedPlan.missionContentGeneration,
             fake.missionSnapshot.contentGeneration);
    QCOMPARE(fake.lastStartedPlan.missionContentDigest,
             fake.missionSnapshot.contentDigest);
    QVERIFY(!startButton(&window)->isEnabled());
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("WaypointLeaderStopButton"))->isEnabled());
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderResetState"))->isEnabled());
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderReturnAlongMission"))->isEnabled());
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderAbandonMission"))->isEnabled());

    window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderResetState"))->click();
    window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderReturnAlongMission"))->click();
    window.findChild<QPushButton *>(
        QStringLiteral("waypointLeaderAbandonMission"))->click();
    QCOMPARE(fake.modeRequestCalls, 3);
    QCOMPARE(int(fake.requestedModes.at(0)),
             int(SwarmWaypointLeaderMode::Idle));
    QCOMPARE(int(fake.requestedModes.at(1)),
             int(SwarmWaypointLeaderMode::ReturnAlongMission));
    QCOMPARE(int(fake.requestedModes.at(2)),
             int(SwarmWaypointLeaderMode::LandAltitude));
    QCOMPARE(confirmations, 4);

    window.close();
    QCOMPARE(fake.cancelCalls, 1);
    QVERIFY(!fake.running);
    QVERIFY(!window.isRunning());
    QVERIFY(fake.cancelReasons.constFirst().contains(
        QStringLiteral("window closed"), Qt::CaseInsensitive));
    QCOMPARE(runningChanged.count(), 2);
}

void SwarmWaypointLeaderWindowTest::openWindowIsModelessSingleton()
{
    FakeWaypointLeaderInterface fake;
    SwarmVehicleInstanceLease ground;
    SwarmVehicleInstanceLease air;
    populateUsableFixture(&fake, &ground, &air);
    QWidget owner;
    owner.resize(800, 600);
    owner.show();

    SwarmWaypointLeaderWindow *first =
        SwarmWaypointLeaderWindow::OpenWindow(
            &fake, confirmation(false), &owner);
    QVERIFY(first);
    QPointer<SwarmWaypointLeaderWindow> guarded(first);
    QCOMPARE(first->windowModality(), Qt::NonModal);
    QVERIFY(first->isWindow());
    QVERIFY(first->testAttribute(Qt::WA_DeleteOnClose));
    SwarmWaypointLeaderWindow *second =
        SwarmWaypointLeaderWindow::OpenWindow(
            &fake, confirmation(true), &owner);
    QCOMPARE(second, first);

    first->close();
    QTRY_VERIFY(guarded.isNull());

    SwarmWaypointLeaderWindow *replacement =
        SwarmWaypointLeaderWindow::OpenWindow(
            &fake, confirmation(false), &owner);
    QVERIFY(replacement);
    QPointer<SwarmWaypointLeaderWindow> replacementGuard(replacement);
    replacement->close();
    QTRY_VERIFY(replacementGuard.isNull());
}

QTEST_MAIN(SwarmWaypointLeaderWindowTest)
#include "test_swarmwaypointleaderwindow.moc"
