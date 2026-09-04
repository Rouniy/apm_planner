#include "SwarmFollowLeaderWindow.h"

#include "comm/SwarmFlightMode.h"
#include "tools/SwarmFollowLeaderCore.h"
#include "tools/SwarmFormationCore.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

// Supplied by SwarmFollowLeaderWindowIntegration.cpp in the application and by
// null seams in the focused widget test.
SwarmTelemetryRegistry *SwarmFollowLeaderApplicationRegistry();
SwarmCommandService *SwarmFollowLeaderApplicationCommandService();

namespace
{

constexpr int GroundMasterSlot = 1;
constexpr int AirMasterSlot = 2;
constexpr int FirstFollowerSlot = 3;
constexpr int FollowLeaderRateHz = 10;
constexpr int TelemetryBootstrapTimeoutMs = 3000;

const char BulkUnavailableReason[] =
    "Bulk arm, disarm, mode, takeoff and NAV GUIDED commands require the "
    "shared exact multi-endpoint ACK arbiter and remain locked in this slice.";
const char MissionTurnUnavailableReason[] =
    "Near-waypoint turn anticipation uses the ordinary velocity course until "
    "an exact-instance current/next mission cache is connected.";

enum FollowLeaderColumn {
    UseColumn = 0,
    OrderColumn,
    EndpointColumn,
    SystemColumn,
    ComponentColumn,
    RoleColumn,
    FirmwareColumn,
    LiveStatusColumn,
    TargetColumn,
    FollowLeaderColumnCount
};

QPushButton *button(const QString &text, const QString &objectName,
                    QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(objectName);
    return result;
}

QLabel *label(const QString &text, const QString &objectName,
              QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(objectName);
    return result;
}

bool sameVehicle(const SwarmVehicleInstanceLease &left,
                 const SwarmVehicleInstanceLease &right)
{
    return left.isValid() && right.isValid() && left.sameInstance(right);
}

QString vehicleLabel(const SwarmVehicleInstanceLease &lease)
{
    const QString endpoint = lease.endpoint.linkName.trimmed().isEmpty()
        ? QObject::tr("Link %1").arg(lease.endpoint.linkId)
        : lease.endpoint.linkName.trimmed();
    return QStringLiteral("%1 — %2:%3")
        .arg(endpoint)
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId);
}

QString familyLabel(const SwarmTelemetrySnapshot &snapshot)
{
    switch (SwarmFormationCore::family(snapshot)) {
    case SwarmVehicleFamily::Plane:
        return QStringLiteral("ArduPlane");
    case SwarmVehicleFamily::Copter:
        return QStringLiteral("ArduCopter");
    case SwarmVehicleFamily::Rover:
        return QStringLiteral("ArduRover");
    case SwarmVehicleFamily::Unsupported:
        if (snapshot.autopilot != MAV_AUTOPILOT_INVALID) {
            return QObject::tr("Autopilot %1").arg(snapshot.autopilot);
        }
        return QObject::tr("Unsupported");
    }
    return QObject::tr("Unsupported");
}

bool isArduCopter(const SwarmTelemetrySnapshot &snapshot)
{
    return snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFormationCore::family(snapshot) == SwarmVehicleFamily::Copter;
}

bool isAutopilotCandidate(const SwarmTelemetrySnapshot &snapshot)
{
    return snapshot.lease.endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
        && snapshot.autopilot != MAV_AUTOPILOT_INVALID
        && snapshot.vehicleType != MAV_TYPE_GCS;
}

bool samePlan(const SwarmFollowLeaderPlan &left,
              const SwarmFollowLeaderPlan &right)
{
    if (!sameVehicle(left.groundMaster, right.groundMaster)
        || !sameVehicle(left.airMaster, right.airMaster)
        || left.followers.size() != right.followers.size()
        || left.settings.separationM != right.settings.separationM
        || left.settings.leadM != right.settings.leadM
        || left.settings.altitudeM != right.settings.altitudeM) {
        return false;
    }
    for (int index = 0; index < left.followers.size(); ++index) {
        const SwarmFollowLeaderFollower &a = left.followers.at(index);
        const SwarmFollowLeaderFollower &b = right.followers.at(index);
        if (a.order != b.order || !sameVehicle(a.lease, b.lease)) {
            return false;
        }
    }
    return true;
}

class ApplicationSwarmFollowLeaderCommands final
    : public SwarmFollowLeaderCommandInterface
{
public:
    explicit ApplicationSwarmFollowLeaderCommands(SwarmCommandService *service)
        : m_service(service)
    {
        if (m_service) {
            m_cancelledConnection = QObject::connect(
                m_service.data(), &SwarmCommandService::sessionCancelled,
                [this](qulonglong sessionId, const QString &reason) {
                    if (m_cancelled) {
                        m_cancelled(sessionId, reason);
                    }
                });
        }
    }

    ~ApplicationSwarmFollowLeaderCommands() override
    {
        QObject::disconnect(m_cancelledConnection);
    }

    SwarmCommandService::Result reserve(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) override
    {
        if (!m_service) {
            if (token) {
                *token = SwarmCommandSessionToken();
            }
            if (error) {
                *error = QStringLiteral(
                    "The application swarm command service is unavailable.");
            }
            return SwarmCommandService::Result::InvalidPlan;
        }
        return m_service->reserve(
            owner, members, maximumBatchHz, token, error);
    }

    SwarmCommandService::Result release(
        const SwarmCommandSessionToken &token) override
    {
        return m_service ? m_service->release(token)
                         : SwarmCommandService::Result::InvalidSession;
    }

    bool routeIsEligible(
        const SwarmVehicleInstanceLease &lease, QString *error) override
    {
        if (m_service) {
            return m_service->routeIsEligible(lease, error);
        }
        if (error) {
            *error = QStringLiteral(
                "The application swarm command service is unavailable.");
        }
        return false;
    }

    SwarmCommandService::BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) override
    {
        return m_service
            ? m_service->requestPositionStreams(token, slotIds, rateHz)
            : unavailableReport();
    }

    SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) override
    {
        return m_service ? m_service->sendPositionTargets(token, targets)
                         : unavailableReport();
    }

    void setSessionCancelledHandler(
        SessionCancelledHandler handler) override
    {
        m_cancelled = std::move(handler);
    }

private:
    static SwarmCommandService::BatchReport unavailableReport()
    {
        SwarmCommandService::BatchReport report;
        report.result = SwarmCommandService::Result::InvalidSession;
        report.detail = QStringLiteral(
            "The application swarm command service is unavailable.");
        return report;
    }

    QPointer<SwarmCommandService> m_service;
    QMetaObject::Connection m_cancelledConnection;
    SessionCancelledHandler m_cancelled;
};

} // namespace

class SwarmFollowLeaderWindow::Implementation
{
public:
    struct VehicleRow
    {
        SwarmVehicleInstanceLease lease;
        SwarmTelemetrySnapshot snapshot;
        bool included = false;
        int order = 0;
        QString target = QStringLiteral("—");
    };

    Implementation(
        SwarmFollowLeaderWindow *window,
        SwarmTelemetryRegistry *telemetryRegistry,
        SwarmFollowLeaderCommandInterface *commandInterface,
        std::unique_ptr<SwarmFollowLeaderCommandInterface> ownedInterface,
        Dependencies deps, QWidget *owner)
        : q(window)
        , registry(telemetryRegistry)
        , commands(commandInterface)
        , ownedCommands(std::move(ownedInterface))
        , dependencies(std::move(deps))
    {
        buildUi(owner);
        connectUi();
        if (commands) {
            commands->setSessionCancelledHandler(
                [this](quint64 id, const QString &reason) {
                    handleSessionCancelled(id, reason);
                });
        }
        refreshVehicles(true, false);
        statusTimer->start();
    }

    ~Implementation()
    {
        if (commands) {
            commands->setSessionCancelledHandler(
                SwarmFollowLeaderCommandInterface::SessionCancelledHandler());
        }
        stop(QString());
    }

    void buildUi(QWidget *owner)
    {
        q->setObjectName(QStringLiteral("SwarmFollowLeaderWindow"));
        q->setWindowTitle(q->tr("Swarm Follow Leader (Beta)"));
        q->setWindowModality(Qt::NonModal);
        q->resize(WindowWidth, WindowHeight);
        q->setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
        q->setStyleSheet(QStringLiteral(
            "QWidget#SwarmFollowLeaderWindow { background: #303233; color: #dddddd; }"
            "QLabel#followLeaderDangerBanner { background: #5a2b20; color: #ffd4b8; "
            "border: 1px solid #ff9b66; border-radius: 4px; padding: 10px; }"
            "QLabel#followLeaderStatus { color: #73c7ff; }"
            "QTableWidget, QComboBox, QDoubleSpinBox { background: #242627; "
            "color: #dddddd; alternate-background-color: #292c2d; }"));
        if (owner) {
            q->move(owner->frameGeometry().center()
                    - QPoint(q->width() / 2, q->height() / 2));
        }

        auto *root = new QVBoxLayout(q);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(10);

        QLabel *danger = label(q->tr(
            "BETA / USE AT OWN RISK — the ground master is observed only. "
            "The selected exact-GUIDED ArduCopter air master and followers "
            "receive position/velocity targets at 10 Hz. A stale, missing or "
            "replacement vehicle, unsafe route, GPS jump, role edit or partial "
            "batch stops the complete command stream."),
            QStringLiteral("followLeaderDangerBanner"), q);
        danger->setWordWrap(true);
        root->addWidget(danger);

        auto *masterRow = new QHBoxLayout;
        masterRow->setSpacing(8);
        masterRow->addWidget(label(q->tr("Ground master"), QString(), q));
        groundCombo = new QComboBox(q);
        groundCombo->setObjectName(QStringLiteral("followLeaderGroundMaster"));
        groundCombo->setMinimumWidth(275);
        masterRow->addWidget(groundCombo);
        masterRow->addWidget(label(q->tr("Air master"), QString(), q));
        airCombo = new QComboBox(q);
        airCombo->setObjectName(QStringLiteral("followLeaderAirMaster"));
        airCombo->setMinimumWidth(275);
        masterRow->addWidget(airCombo);
        masterRow->addWidget(button(q->tr("Refresh Vehicles"),
            QStringLiteral("followLeaderRefreshVehicles"), q));
        QLabel *roleHint = label(q->tr(
            "Ground master may be any live autopilot on an eligible exact route; commanded air roles require ArduCopter."),
            QStringLiteral("followLeaderRoleHint"), q);
        roleHint->setWordWrap(true);
        masterRow->addWidget(roleHint, 1);
        root->addLayout(masterRow);

        auto *settingsRow = new QHBoxLayout;
        settingsRow->setSpacing(8);
        settingsRow->addWidget(label(q->tr("Separation m"), QString(), q));
        separation = numeric(QStringLiteral("followLeaderSeparation"),
                             SwarmFollowLeaderCore::MinimumSeparationM,
                             SwarmFollowLeaderCore::MaximumSeparationM,
                             5.0);
        settingsRow->addWidget(separation);
        QLabel *leadLabel = label(q->tr("Lead m"), QString(), q);
        leadLabel->setToolTip(q->tr(
            "Retained for parity: official FollowLeader exposes this value but does not use it."));
        settingsRow->addWidget(leadLabel);
        lead = numeric(QStringLiteral("followLeaderLead"), -100000.0,
                       100000.0, 20.0);
        lead->setToolTip(q->tr(
            "Upstream-reserved setting; it currently has no flight effect."));
        settingsRow->addWidget(lead);
        settingsRow->addWidget(label(q->tr("Flight altitude m"), QString(), q));
        altitude = numeric(QStringLiteral("followLeaderAltitude"),
                           SwarmFollowLeaderCore::MinimumAltitudeM,
                           SwarmFollowLeaderCore::MaximumAltitudeM, 10.0);
        settingsRow->addWidget(altitude);
        settingsRow->addWidget(label(q->tr("Takeoff m"), QString(), q));
        takeoffAltitude = numeric(QStringLiteral("followLeaderTakeoffAltitude"),
                                  1.0, 10000.0, 5.0);
        takeoffAltitude->setEnabled(false);
        takeoffAltitude->setToolTip(q->tr(BulkUnavailableReason));
        takeoffAltitude->setAccessibleDescription(q->tr(BulkUnavailableReason));
        settingsRow->addWidget(takeoffAltitude);
        QLabel *placement = label(q->tr(
            "Follower #1 follows the current ground position; later followers occupy Separation-spaced trail points."),
            QStringLiteral("followLeaderPlacementHint"), q);
        placement->setWordWrap(true);
        settingsRow->addWidget(placement, 1);
        root->addLayout(settingsRow);

        table = new QTableWidget(q);
        table->setObjectName(QStringLiteral("FollowLeaderVehicleGrid"));
        table->setColumnCount(FollowLeaderColumnCount);
        table->setHorizontalHeaderLabels({q->tr("Use"), q->tr("Order"),
            q->tr("Endpoint"), q->tr("Sys"), q->tr("Comp"),
            q->tr("Role"), q->tr("Firmware"), q->tr("Live status"),
            q->tr("Current commanded target")});
        table->verticalHeader()->hide();
        table->setAlternatingRowColors(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setColumnWidth(UseColumn, 50);
        table->setColumnWidth(OrderColumn, 62);
        table->setColumnWidth(EndpointColumn, 170);
        table->setColumnWidth(SystemColumn, 44);
        table->setColumnWidth(ComponentColumn, 48);
        table->setColumnWidth(RoleColumn, 150);
        table->setColumnWidth(FirmwareColumn, 105);
        table->setColumnWidth(LiveStatusColumn, 205);
        table->horizontalHeader()->setSectionResizeMode(
            TargetColumn, QHeaderView::Stretch);
        root->addWidget(table, 1);

        auto *commandRow = new QHBoxLayout;
        commandRow->setSpacing(8);
        runButton = button(q->tr("Start Follow Leader"),
            QStringLiteral("FollowLeaderRunButton"), q);
        runButton->setMinimumWidth(165);
        commandRow->addWidget(runButton);
        const struct { const char *text; const char *name; } bulkButtons[] = {
            {"Arm Air Group", "followLeaderArmAirGroup"},
            {"Disarm Air Group", "followLeaderDisarmAirGroup"},
            {"Take Off", "followLeaderTakeoffAirGroup"},
            {"GUIDED", "followLeaderGuidedAirGroup"},
            {"NAV GUIDED", "followLeaderNavGuidedAirGroup"},
            {"AUTO All Roles", "followLeaderAutoAllRoles"}
        };
        for (const auto &definition : bulkButtons) {
            QPushButton *control = button(q->tr(definition.text),
                QString::fromLatin1(definition.name), q);
            setUnavailable(control, q->tr(BulkUnavailableReason));
            commandRow->addWidget(control);
        }
        commandRow->addStretch(1);
        root->addLayout(commandRow);

        QLabel *unavailable = label(q->tr(
            "Available now: exact 10 Hz Follow Leader position/velocity stream. "
            "Not yet ported: exact ACK-arbitrated bulk flight commands. %1")
            .arg(q->tr(MissionTurnUnavailableReason)),
            QStringLiteral("followLeaderUnavailableHint"), q);
        unavailable->setWordWrap(true);
        root->addWidget(unavailable);

        auto *footer = new QHBoxLayout;
        status = label(q->tr(
            "Select distinct ground and air masters, then explicitly enable ordered Copter followers."),
            QStringLiteral("followLeaderStatus"), q);
        status->setWordWrap(true);
        footer->addWidget(status, 1);
        footer->addWidget(button(q->tr("Close"),
            QStringLiteral("followLeaderClose"), q));
        root->addLayout(footer);

        statusTimer = new QTimer(q);
        statusTimer->setInterval(500);
        runTimer = new QTimer(q);
        runTimer->setInterval(1000 / FollowLeaderRateHz);
        runTimer->setTimerType(Qt::PreciseTimer);
    }

    QDoubleSpinBox *numeric(const QString &objectName, double minimum,
                            double maximum, double value)
    {
        auto *control = new QDoubleSpinBox(q);
        control->setObjectName(objectName);
        control->setRange(minimum, maximum);
        control->setSingleStep(1.0);
        control->setDecimals(1);
        control->setValue(value);
        control->setMinimumWidth(95);
        return control;
    }

    void connectUi()
    {
        QObject::connect(groundCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), q,
            [this](int index) { masterChanged(index, true); });
        QObject::connect(airCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), q,
            [this](int index) { masterChanged(index, false); });
        QObject::connect(q->findChild<QPushButton *>(
                QStringLiteral("followLeaderRefreshVehicles")),
            &QPushButton::clicked, q,
            [this]() { refreshVehicles(true, true); });
        QObject::connect(table, &QTableWidget::cellChanged, q,
            [this](int row, int column) { tableCellChanged(row, column); });
        for (QDoubleSpinBox *control : {separation, lead, altitude}) {
            QObject::connect(control,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged), q,
                [this](double) {
                    stop(q->tr(
                        "Follow Leader stopped because a flight setting changed."));
                    clearTargets();
                    trail.clear();
                });
        }
        QObject::connect(runButton, &QPushButton::clicked, q,
                         [this]() { toggleRun(); });
        QObject::connect(q->findChild<QPushButton *>(
                QStringLiteral("followLeaderClose")),
            &QPushButton::clicked, q, &QWidget::close);
        QObject::connect(statusTimer, &QTimer::timeout, q,
                         [this]() { updateLiveStatus(); });
        QObject::connect(runTimer, &QTimer::timeout, q,
                         [this]() { runTick(); });

        if (registry) {
            QObject::connect(registry.data(),
                &SwarmTelemetryRegistry::linkSessionBegan, q,
                [this](int, qulonglong) {
                    if (running) {
                        stop(q->tr(
                            "Follow Leader stopped because the MAVLink connection topology changed."));
                    }
                    refreshVehicles(false, false);
                });
            QObject::connect(registry.data(),
                &SwarmTelemetryRegistry::linkSessionEnded, q,
                [this](int, qulonglong) {
                    if (running) {
                        stop(q->tr(
                            "Follow Leader stopped because the MAVLink connection topology changed."));
                    }
                    refreshVehicles(false, false);
                });
            QObject::connect(registry.data(),
                &SwarmTelemetryRegistry::endpointActivated, q,
                [this](const SwarmTelemetrySnapshot &) {
                    refreshVehicles(rows.isEmpty() && !running, false);
                });
            QObject::connect(registry.data(),
                &SwarmTelemetryRegistry::endpointUpdated, q,
                [this](const SwarmTelemetrySnapshot &snapshot) {
                    const int row = rowForLease(snapshot.lease);
                    if (row < 0) {
                        refreshVehicles(false, false);
                        return;
                    }
                    const bool oldCopter = isArduCopter(rows.at(row).snapshot);
                    rows[row].snapshot = snapshot;
                    if (oldCopter != isArduCopter(snapshot)) {
                        stop(q->tr(
                            "Follow Leader stopped because a vehicle family changed."));
                        refreshVehicles(false, false);
                    }
                });
            QObject::connect(registry.data(),
                &SwarmTelemetryRegistry::endpointRetired, q,
                [this](const SwarmVehicleInstanceLease &lease,
                       SwarmTelemetryRegistry::RetirementReason) {
                    if (running && activePlanContains(lease)) {
                        stop(q->tr(
                            "Follow Leader stopped: a reserved vehicle was retired."));
                    }
                    refreshVehicles(false, false);
                });
            QObject::connect(registry.data(), &QObject::destroyed, q,
                [this]() {
                    stop(q->tr(
                        "Follow Leader stopped: swarm telemetry is unavailable."));
                    registry = nullptr;
                    rows.clear();
                    groundMaster = SwarmVehicleInstanceLease();
                    airMaster = SwarmVehicleInstanceLease();
                    rebuildVehicleWidgets();
                });
        }
    }

    QVector<SwarmTelemetrySnapshot> discoverVehicles() const
    {
        QVector<SwarmTelemetrySnapshot> result;
        if (!registry) {
            return result;
        }
        const QList<VehicleEndpoint> endpoints = registry->endpoints();
        result.reserve(endpoints.size());
        for (const VehicleEndpoint &endpoint : endpoints) {
            SwarmTelemetrySnapshot snapshot;
            if (registry->acquireSnapshot(endpoint, &snapshot)) {
                result.append(snapshot);
            }
        }
        return result;
    }

    int rowForLease(const SwarmVehicleInstanceLease &lease) const
    {
        for (int index = 0; index < rows.size(); ++index) {
            if (sameVehicle(rows.at(index).lease, lease)) {
                return index;
            }
        }
        return -1;
    }

    bool isGroundMaster(const VehicleRow &row) const
    {
        return sameVehicle(row.lease, groundMaster);
    }

    bool isAirMaster(const VehicleRow &row) const
    {
        return sameVehicle(row.lease, airMaster);
    }

    bool activePlanContains(const SwarmVehicleInstanceLease &lease) const
    {
        if (sameVehicle(lease, activePlan.groundMaster)
            || sameVehicle(lease, activePlan.airMaster)) {
            return true;
        }
        for (const SwarmFollowLeaderFollower &follower
             : activePlan.followers) {
            if (sameVehicle(lease, follower.lease)) {
                return true;
            }
        }
        return false;
    }

    void refreshVehicles(bool explicitlyRequested, bool stopRunning)
    {
        if (stopRunning) {
            stop(q->tr(
                "Follow Leader stopped because the vehicle list was refreshed."));
        }
        const QVector<SwarmTelemetrySnapshot> discovered = discoverVehicles();
        const SwarmVehicleInstanceLease previousGround = groundMaster;
        const SwarmVehicleInstanceLease previousAir = airMaster;
        QVector<VehicleRow> next;
        next.reserve(discovered.size());
        for (const SwarmTelemetrySnapshot &snapshot : discovered) {
            VehicleRow value;
            value.lease = snapshot.lease;
            value.snapshot = snapshot;
            const int previous = rowForLease(snapshot.lease);
            if (previous >= 0 && isArduCopter(snapshot)) {
                value.included = rows.at(previous).included;
                value.order = rows.at(previous).order;
            }
            next.append(value);
        }
        rows = next;

        const int previousGroundRow = rowForLease(previousGround);
        const int previousAirRow = rowForLease(previousAir);
        const bool groundReplaced = previousGround.isValid()
            && (previousGroundRow < 0
                || !isAutopilotCandidate(
                    rows.at(previousGroundRow).snapshot));
        const bool airReplaced = previousAir.isValid()
            && (previousAirRow < 0
                || !isArduCopter(rows.at(previousAirRow).snapshot));
        groundMaster = groundReplaced ? SwarmVehicleInstanceLease()
                                      : previousGround;
        airMaster = airReplaced ? SwarmVehicleInstanceLease() : previousAir;
        if (!groundMaster.isValid()) {
            for (const VehicleRow &row : rows) {
                if (isAutopilotCandidate(row.snapshot)) {
                    groundMaster = row.lease;
                    break;
                }
            }
        }
        if (!airMaster.isValid()) {
            for (const VehicleRow &row : rows) {
                if (isArduCopter(row.snapshot)
                    && !sameVehicle(row.lease, groundMaster)) {
                    airMaster = row.lease;
                    break;
                }
            }
        }
        if (sameVehicle(groundMaster, airMaster)) {
            airMaster = SwarmVehicleInstanceLease();
        }

        if (groundReplaced || airReplaced) {
            for (VehicleRow &row : rows) {
                row.included = false;
                row.order = 0;
            }
            trail.clear();
        }
        clearMasterSelections();
        assignMissingOrders();
        clearTargets(false);
        rebuildVehicleWidgets();
        updateLiveStatus();

        if (groundReplaced || airReplaced) {
            setStatus(q->tr(
                "A previous master is unavailable. Replacement roles were selected where possible and followers were disabled; review the complete group."));
        } else if (explicitlyRequested) {
            int copters = 0;
            QSet<int> links;
            for (const VehicleRow &row : rows) {
                links.insert(row.lease.endpoint.linkId);
                if (isArduCopter(row.snapshot)) {
                    ++copters;
                }
            }
            setStatus(copters == 0
                ? q->tr(
                    "No live ArduCopter autopilots were found across open MAVLink links.")
                : q->tr(
                    "Found %1 Follow Leader Copter(s) across %2 link(s). Select distinct masters and explicitly enable ordered followers.")
                    .arg(copters).arg(links.size()));
        }
    }

    void clearMasterSelections()
    {
        for (VehicleRow &row : rows) {
            if (isGroundMaster(row) || isAirMaster(row)) {
                row.included = false;
                row.order = 0;
            }
        }
    }

    void assignMissingOrders()
    {
        QSet<int> used;
        for (const VehicleRow &row : rows) {
            if (!isGroundMaster(row) && !isAirMaster(row)
                && isArduCopter(row.snapshot) && row.order > 0) {
                used.insert(row.order);
            }
        }
        int next = 1;
        for (VehicleRow &row : rows) {
            if (isGroundMaster(row) || isAirMaster(row)
                || !isArduCopter(row.snapshot) || row.order > 0) {
                continue;
            }
            while (used.contains(next)) {
                ++next;
            }
            row.order = next;
            used.insert(next);
        }
    }

    void rebuildVehicleWidgets()
    {
        loading = true;
        rebuildMasterCombos();
        rebuildTable();
        loading = false;
    }

    void rebuildMasterCombos()
    {
        const QSignalBlocker groundBlocker(groundCombo);
        const QSignalBlocker airBlocker(airCombo);
        groundCombo->clear();
        airCombo->clear();
        int selectedGround = -1;
        int selectedAir = -1;
        for (const VehicleRow &row : rows) {
            int index = -1;
            if (isAutopilotCandidate(row.snapshot)) {
                index = groundCombo->count();
                groundCombo->addItem(
                    vehicleLabel(row.lease), QVariant::fromValue(row.lease));
                if (isGroundMaster(row)) {
                    selectedGround = index;
                }
            }
            if (isArduCopter(row.snapshot)) {
                index = airCombo->count();
                airCombo->addItem(
                    vehicleLabel(row.lease), QVariant::fromValue(row.lease));
                if (isAirMaster(row)) {
                    selectedAir = index;
                }
            }
        }
        groundCombo->setCurrentIndex(selectedGround);
        airCombo->setCurrentIndex(selectedAir);
    }

    void rebuildTable()
    {
        const QSignalBlocker blocker(table);
        table->setRowCount(rows.size());
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const VehicleRow row = rows.at(rowIndex);
            const bool ground = isGroundMaster(row);
            const bool air = isAirMaster(row);
            const bool copter = isArduCopter(row.snapshot);
            const bool follower = copter && !ground && !air;

            auto *use = new QTableWidgetItem;
            Qt::ItemFlags useFlags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (follower) {
                useFlags |= Qt::ItemIsUserCheckable;
            }
            use->setFlags(useFlags);
            use->setCheckState(row.included ? Qt::Checked : Qt::Unchecked);
            if (!follower) {
                use->setToolTip(ground || air
                    ? q->tr("Master roles are selected above and cannot also be followers.")
                    : q->tr("Only ArduCopter can be an air role in Follow Leader."));
            }
            table->setItem(rowIndex, UseColumn, use);

            auto *order = new QTableWidgetItem(
                ground || air ? QStringLiteral("—")
                              : copter ? QString::number(row.order)
                                       : QStringLiteral("—"));
            Qt::ItemFlags orderFlags = Qt::ItemIsSelectable
                | Qt::ItemIsEnabled;
            if (follower) {
                orderFlags |= Qt::ItemIsEditable;
            }
            order->setFlags(orderFlags);
            table->setItem(rowIndex, OrderColumn, order);

            const QString endpoint = row.lease.endpoint.linkName.trimmed().isEmpty()
                ? q->tr("Link %1").arg(row.lease.endpoint.linkId)
                : row.lease.endpoint.linkName.trimmed();
            const QString role = ground ? q->tr("Ground master (observed)")
                : air ? q->tr("Air master")
                : copter ? q->tr("Follower")
                : q->tr("Ground master only");
            const QStringList values = {endpoint,
                QString::number(row.lease.endpoint.systemId),
                QString::number(row.lease.endpoint.componentId), role,
                familyLabel(row.snapshot), q->tr("Checking…"), row.target};
            for (int offset = 0; offset < values.size(); ++offset) {
                const int column = EndpointColumn + offset;
                auto *cell = new QTableWidgetItem(values.at(offset));
                if (column == EndpointColumn) {
                    cell->setData(Qt::UserRole,
                                  QVariant::fromValue(row.lease));
                }
                cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                table->setItem(rowIndex, column, cell);
            }
        }
    }

    QString liveStatus(VehicleRow row) const
    {
        if (!registry || !registry->validateLease(row.lease)) {
            return q->tr("Telemetry stale");
        }
        QString routeError;
        if (!commands || !commands->routeIsEligible(row.lease, &routeError)) {
            return routeError.trimmed().isEmpty()
                ? q->tr("Command route unavailable")
                : q->tr("Route locked: %1").arg(routeError);
        }
        SwarmTelemetrySnapshot current;
        if (!registry->snapshotForLease(row.lease, &current)
            || !registry->validateLease(row.lease)) {
            return q->tr("Telemetry changed during route check");
        }
        row.snapshot = current;
        if (!row.snapshot.positionValid
            || !registry->observationIsFresh(
                row.snapshot.positionObservedMs,
                SwarmFollowLeaderCore::MaximumTelemetryAgeMs)) {
            return q->tr("Position unavailable");
        }
        if (isGroundMaster(row)) {
            if (!row.snapshot.velocityValid
                || !registry->observationIsFresh(
                    row.snapshot.positionObservedMs,
                    SwarmFollowLeaderCore::MaximumTelemetryAgeMs)) {
                return q->tr("Ground velocity unavailable");
            }
            return q->tr("Observed; %1")
                .arg(row.snapshot.armed ? q->tr("armed")
                                       : q->tr("disarmed"));
        }
        if (isAirMaster(row) || row.included) {
            if (!isArduCopter(row.snapshot)) {
                return q->tr("Air role requires ArduCopter");
            }
            const QString mode = SwarmFlightMode::displayName(row.snapshot);
            return q->tr("%1; %2").arg(
                SwarmFlightMode::isExactGuided(row.snapshot)
                    ? mode : q->tr("%1 (not GUIDED)").arg(mode),
                row.snapshot.armed ? q->tr("armed") : q->tr("disarmed"));
        }
        return q->tr("Available as %1").arg(familyLabel(row.snapshot));
    }

    void updateLiveStatus()
    {
        if (!registry) {
            return;
        }
        const QVector<VehicleRow> statusRows = rows;
        for (const VehicleRow &statusRow : statusRows) {
            VehicleRow currentRow = statusRow;
            SwarmTelemetrySnapshot snapshot;
            if (registry->snapshotForLease(currentRow.lease, &snapshot)) {
                currentRow.snapshot = snapshot;
            }
            const QString text = liveStatus(currentRow);
            const int index = rowForLease(currentRow.lease);
            if (index < 0 || index >= table->rowCount()) {
                continue;
            }
            rows[index].snapshot = currentRow.snapshot;
            QTableWidgetItem *cell = table->item(index, LiveStatusColumn);
            if (cell) {
                cell->setText(text);
            }
        }
    }

    void masterChanged(int index, bool groundRole)
    {
        QComboBox *combo = groundRole ? groundCombo : airCombo;
        if (loading || index < 0 || index >= combo->count()) {
            return;
        }
        const SwarmVehicleInstanceLease next =
            qvariant_cast<SwarmVehicleInstanceLease>(combo->itemData(index));
        SwarmVehicleInstanceLease &role = groundRole ? groundMaster : airMaster;
        if (!next.isValid() || sameVehicle(next, role)) {
            return;
        }
        const int nextRow = rowForLease(next);
        if (nextRow < 0
            || (!groundRole && !isArduCopter(rows.at(nextRow).snapshot))) {
            rebuildMasterCombos();
            return;
        }
        if ((groundRole && sameVehicle(next, airMaster))
            || (!groundRole && sameVehicle(next, groundMaster))) {
            setStatus(q->tr("Ground master and air master must be different vehicles."));
            rebuildMasterCombos();
            return;
        }
        stop(q->tr("Follow Leader stopped because a master role changed."));
        role = next;
        clearMasterSelections();
        assignMissingOrders();
        clearTargets(false);
        trail.clear();
        rebuildVehicleWidgets();
        updateLiveStatus();
    }

    void tableCellChanged(int rowIndex, int column)
    {
        if (loading || rowIndex < 0 || rowIndex >= rows.size()) {
            return;
        }
        VehicleRow &row = rows[rowIndex];
        const bool master = isGroundMaster(row) || isAirMaster(row);
        const bool follower = isArduCopter(row.snapshot) && !master;
        if (column == UseColumn) {
            if (!follower) {
                rebuildTable();
                updateLiveStatus();
                return;
            }
            const bool included = table->item(rowIndex, UseColumn)->checkState()
                == Qt::Checked;
            if (row.included != included) {
                stop(q->tr(
                    "Follow Leader stopped because follower selection changed."));
                row.included = included;
                clearTargets(false);
                trail.clear();
                rebuildTable();
                updateLiveStatus();
            }
            return;
        }
        if (column != OrderColumn || !follower) {
            return;
        }
        bool valid = false;
        const int value = table->item(rowIndex, OrderColumn)->text().toInt(&valid);
        if (!valid || value < 1
            || value > SwarmFollowLeaderCore::MaximumFollowers) {
            setStatus(q->tr("Follower order must be an integer from 1 to 21."));
            rebuildTable();
            updateLiveStatus();
            return;
        }
        if (row.order == value) {
            return;
        }
        stop(q->tr("Follow Leader stopped because follower order changed."));
        row.order = value;
        clearTargets(false);
        trail.clear();
        rebuildTable();
        updateLiveStatus();
    }

    bool tryBuildPlan(SwarmFollowLeaderPlan *plan,
                      QList<SwarmTelemetrySnapshot> *snapshots,
                      QVector<SwarmCommandMember> *members,
                      QString *error) const
    {
        if (plan) {
            *plan = SwarmFollowLeaderPlan();
        }
        if (snapshots) {
            snapshots->clear();
        }
        if (members) {
            members->clear();
        }
        if (!plan || !snapshots || !members || !error || !registry
            || !groundMaster.isValid() || !airMaster.isValid()) {
            if (error) {
                *error = q->tr(
                    "Select distinct live ground and ArduCopter air masters.");
            }
            return false;
        }
        if (sameVehicle(groundMaster, airMaster)) {
            *error = q->tr(
                "Ground master and air master must be different vehicles.");
            return false;
        }
        const int groundRowIndex = rowForLease(groundMaster);
        const int airRowIndex = rowForLease(airMaster);
        if (groundRowIndex < 0 || airRowIndex < 0
            || !isArduCopter(rows.at(airRowIndex).snapshot)) {
            *error = q->tr(
                "The selected ground or ArduCopter air master is unavailable.");
            return false;
        }

        plan->groundMaster = groundMaster;
        plan->airMaster = airMaster;
        plan->settings.separationM = separation->value();
        plan->settings.leadM = lead->value();
        plan->settings.altitudeM = altitude->value();
        if (!std::isfinite(plan->settings.separationM)
            || plan->settings.separationM
                < SwarmFollowLeaderCore::MinimumSeparationM
            || plan->settings.separationM
                > SwarmFollowLeaderCore::MaximumSeparationM) {
            *error = q->tr("Separation must be between 1 and 500 m.");
            return false;
        }
        if (!std::isfinite(plan->settings.leadM)
            || std::abs(plan->settings.leadM) > 100000.0) {
            *error = q->tr("Lead must be finite and within +/-100000 m.");
            return false;
        }
        if (!std::isfinite(plan->settings.altitudeM)
            || plan->settings.altitudeM
                < SwarmFollowLeaderCore::MinimumAltitudeM
            || plan->settings.altitudeM
                > SwarmFollowLeaderCore::MaximumAltitudeM) {
            *error = q->tr("Flight altitude must be between 1 and 10000 m.");
            return false;
        }

        QSet<int> orders;
        for (const VehicleRow &row : rows) {
            if (!row.included || isGroundMaster(row) || isAirMaster(row)) {
                continue;
            }
            if (!isArduCopter(row.snapshot)) {
                *error = q->tr("Follower %1 is not ArduCopter.")
                    .arg(vehicleLabel(row.lease));
                return false;
            }
            if (row.order < 1
                || row.order > SwarmFollowLeaderCore::MaximumFollowers
                || orders.contains(row.order)) {
                *error = q->tr(
                    "Follower order must be unique and between 1 and 21.");
                return false;
            }
            orders.insert(row.order);
            plan->followers.append({row.lease, row.order});
        }
        std::sort(plan->followers.begin(), plan->followers.end(),
            [](const SwarmFollowLeaderFollower &left,
               const SwarmFollowLeaderFollower &right) {
                return left.order < right.order;
            });
        for (int expected = 1; expected <= plan->followers.size(); ++expected) {
            if (plan->followers.at(expected - 1).order != expected) {
                *error = q->tr(
                    "Selected follower order must be contiguous from 1 through %1.")
                    .arg(plan->followers.size());
                return false;
            }
        }

        QList<SwarmVehicleInstanceLease> groupLeases;
        groupLeases.append(plan->groundMaster);
        groupLeases.append(plan->airMaster);
        for (const SwarmFollowLeaderFollower &follower : plan->followers) {
            groupLeases.append(follower.lease);
        }
        QString routeError;
        for (const SwarmVehicleInstanceLease &lease : groupLeases) {
            if (!commands || !commands->routeIsEligible(lease, &routeError)) {
                *error = routeError.trimmed().isEmpty()
                    ? q->tr("The command route for %1 is unavailable.")
                        .arg(vehicleLabel(lease))
                    : q->tr("Route for %1 is unavailable: %2")
                        .arg(vehicleLabel(lease), routeError);
                return false;
            }
        }

        SwarmVehicleGroupLease group;
        group.members = groupLeases;
        if (!registry->validateGroup(
                group, snapshots,
                SwarmFollowLeaderCore::MaximumTelemetryAgeMs)) {
            *error = q->tr(
                "A Follow Leader vehicle disappeared, expired, or was reconnected.");
            return false;
        }
        if (snapshots->size() != groupLeases.size()) {
            *error = q->tr(
                "The exact Follow Leader group could not be reconstructed.");
            return false;
        }
        if (!isArduCopter(snapshots->at(1))) {
            *error = q->tr("The air master is no longer ArduCopter.");
            return false;
        }
        for (int index = 1; index < snapshots->size(); ++index) {
            if (!isArduCopter(snapshots->at(index))) {
                *error = q->tr(
                    "A commanded air role is no longer ArduCopter.");
                return false;
            }
            if (!SwarmFlightMode::isExactGuided(snapshots->at(index))) {
                *error = q->tr(
                    "%1 is in %2 and must already be in exact GUIDED mode before starting.")
                    .arg(vehicleLabel(snapshots->at(index).lease),
                         SwarmFlightMode::displayName(snapshots->at(index)));
                return false;
            }
        }

        SwarmCommandMember groundMember;
        groundMember.slotId = GroundMasterSlot;
        groundMember.lease = plan->groundMaster;
        groundMember.required.fields = SwarmTelemetryRequirements::Position
            | SwarmTelemetryRequirements::Velocity;
        groundMember.required.heartbeatMaximumAgeMs =
            SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
        groundMember.required.positionMaximumAgeMs =
            SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
        groundMember.required.velocityMaximumAgeMs =
            SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
        members->append(groundMember);

        SwarmCommandMember airMember;
        airMember.slotId = AirMasterSlot;
        airMember.lease = plan->airMaster;
        airMember.required.fields = SwarmTelemetryRequirements::Position;
        airMember.required.heartbeatMaximumAgeMs =
            SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
        airMember.required.positionMaximumAgeMs =
            SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
        airMember.flightMode =
            SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
        members->append(airMember);

        for (int index = 0; index < plan->followers.size(); ++index) {
            SwarmCommandMember followerMember;
            followerMember.slotId = FirstFollowerSlot + index;
            followerMember.lease = plan->followers.at(index).lease;
            followerMember.required.fields = SwarmTelemetryRequirements::Position;
            followerMember.required.heartbeatMaximumAgeMs =
                SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
            followerMember.required.positionMaximumAgeMs =
                SwarmFollowLeaderCore::MaximumTelemetryAgeMs;
            followerMember.flightMode =
                SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
            members->append(followerMember);
        }
        error->clear();
        return true;
    }

    QString confirmationTargets(const SwarmFollowLeaderPlan &plan) const
    {
        QStringList lines;
        lines.append(q->tr("• Air master %1 — %2 m ahead")
            .arg(vehicleLabel(plan.airMaster))
            .arg(plan.settings.separationM, 0, 'f', 1));
        for (const SwarmFollowLeaderFollower &follower : plan.followers) {
            lines.append(q->tr("• Follower #%1 %2 — %3 m behind")
                .arg(follower.order)
                .arg(vehicleLabel(follower.lease))
                .arg((follower.order - 1) * plan.settings.separationM,
                     0, 'f', 1));
        }
        return lines.join(QLatin1Char('\n'));
    }

    void toggleRun()
    {
        if (running) {
            stop(q->tr("Follow Leader stopped by operator."));
            return;
        }
        if (!commands) {
            setStatus(q->tr("The exact swarm command service is unavailable."));
            return;
        }
        SwarmFollowLeaderPlan plan;
        QList<SwarmTelemetrySnapshot> snapshots;
        QVector<SwarmCommandMember> members;
        QString error;
        if (!tryBuildPlan(&plan, &snapshots, &members, &error)) {
            setStatus(error);
            return;
        }
        const SwarmFollowLeaderPlan confirmedPlan = plan;
        const bool accepted = dependencies.confirmDangerous
            && dependencies.confirmDangerous(
                q, q->tr("Start Swarm Follow Leader"),
                q->tr(
                    "BETA / USE AT OWN RISK. APM Planner 3.0 will send exact position/velocity targets at 10 Hz to these exact-GUIDED ArduCopter roles:\n\n%1\n\nThe ground master %2 is observed only. Verify relative altitude, ordering, the ground mission and clear airspace. Cancel is the default action.")
                    .arg(confirmationTargets(plan),
                         vehicleLabel(plan.groundMaster)),
                q->tr("START FOLLOW LEADER"));
        if (!accepted) {
            setStatus(q->tr("Follow Leader start cancelled."));
            return;
        }
        if (!tryBuildPlan(&plan, &snapshots, &members, &error)
            || !samePlan(confirmedPlan, plan)) {
            setStatus(q->tr(
                "Follow Leader changed while confirmation was open. %1")
                .arg(error.isEmpty()
                    ? q->tr("Roles, order or settings changed.") : error));
            return;
        }

        SwarmCommandSessionToken token;
        const SwarmCommandService::Result reserved = commands->reserve(
            q, members, FollowLeaderRateHz, &token, &error);
        if (reserved != SwarmCommandService::Result::Reserved
            || !token.isValid()) {
            setStatus(error.isEmpty()
                ? q->tr(
                    "The exact Follow Leader command session could not be reserved.")
                : error);
            return;
        }
        session = token;
        activePlan = plan;
        trail.clear();
        clearTargets();

        QVector<int> streamSlots;
        streamSlots.reserve(members.size());
        for (const SwarmCommandMember &member : members) {
            streamSlots.append(member.slotId);
        }
        const SwarmCommandService::BatchReport streams =
            commands->requestPositionStreams(
                session, streamSlots, FollowLeaderRateHz);
        if (!streams.allSent()) {
            stop(q->tr("Follow Leader could not start: %1")
                .arg(streams.detail));
            return;
        }

        bootstrapping = true;
        bootstrapTimer.restart();
        setRunning(true);
        setStatus(q->tr(
            "Waiting for fresh position and ground-master velocity telemetry…"));
        runTick();
        if (running) {
            runTimer->start();
        }
    }

    QVector<SwarmPositionTarget> positionTargets(
        const SwarmFollowLeaderTick &tick) const
    {
        QVector<SwarmPositionTarget> targets;
        targets.reserve(tick.commands.size());
        for (const SwarmFollowLeaderCommand &command : tick.commands) {
            int slot = -1;
            if (sameVehicle(command.lease, activePlan.airMaster)) {
                slot = AirMasterSlot;
            } else {
                for (int index = 0; index < activePlan.followers.size(); ++index) {
                    if (sameVehicle(command.lease,
                                    activePlan.followers.at(index).lease)) {
                        slot = FirstFollowerSlot + index;
                        break;
                    }
                }
            }
            if (slot < 0) {
                return {};
            }
            SwarmPositionTarget target;
            target.slotId = slot;
            target.latitudeDegrees = command.target.latitudeDegrees;
            target.longitudeDegrees = command.target.longitudeDegrees;
            target.relativeAltitudeM = static_cast<float>(
                command.target.relativeAltitudeM);
            target.velocityNorthMps = static_cast<float>(command.velocity.northMps);
            target.velocityEastMps = static_cast<float>(command.velocity.eastMps);
            target.velocityDownMps = static_cast<float>(command.velocity.downMps);
            target.useVelocity = true;
            targets.append(target);
        }
        return targets;
    }

    bool bootstrapCanWait(const QString &detail) const
    {
        return (detail.contains(QStringLiteral("position"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("velocity"), Qt::CaseInsensitive))
            && (detail.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("stale"), Qt::CaseInsensitive));
    }

    void runTick()
    {
        if (!running || !registry || !commands || !session.isValid()) {
            return;
        }
        SwarmVehicleGroupLease group;
        group.members.append(activePlan.groundMaster);
        group.members.append(activePlan.airMaster);
        for (const SwarmFollowLeaderFollower &follower
             : activePlan.followers) {
            group.members.append(follower.lease);
        }
        QList<SwarmTelemetrySnapshot> snapshots;
        if (!registry->validateGroup(
                group, &snapshots,
                SwarmFollowLeaderCore::MaximumTelemetryAgeMs)) {
            stop(q->tr(
                "Follow Leader stopped: a reserved vehicle disappeared, expired, or was reconnected."));
            return;
        }
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            activePlan, snapshots, registry->observationClockNowMs(), &trail);
        if (tick.state == SwarmFollowLeaderTick::State::Stopped) {
            if (bootstrapping && bootstrapCanWait(tick.detail)
                && bootstrapTimer.isValid()
                && bootstrapTimer.elapsed() < TelemetryBootstrapTimeoutMs) {
                setStatus(q->tr(
                    "Waiting for fresh position and ground-master velocity telemetry…"));
                return;
            }
            stop(tick.detail);
            return;
        }
        if (bootstrapping) {
            bootstrapping = false;
            bootstrapTimer.invalidate();
        }
        if (tick.state == SwarmFollowLeaderTick::State::WaitingForTrail) {
            clearTargets();
            setStatus(tick.detail);
            return;
        }

        const QVector<SwarmPositionTarget> targets = positionTargets(tick);
        if (targets.size() != activePlan.followers.size() + 1) {
            stop(q->tr(
                "Follow Leader stopped: generated targets no longer match the reserved air group."));
            return;
        }
        const SwarmCommandService::BatchReport sent =
            commands->sendPositionTargets(session, targets);
        if (!running) {
            return;
        }
        if (sent.result == SwarmCommandService::Result::RateLimited) {
            return;
        }
        if (!sent.allSent()) {
            stop(q->tr("Follow Leader stopped: %1").arg(sent.detail));
            return;
        }

        clearTargets(false);
        for (const SwarmFollowLeaderCommand &command : tick.commands) {
            const int row = rowForLease(command.lease);
            if (row < 0) {
                continue;
            }
            rows[row].target = q->tr("%1: %2, %3, %4 m; N/E/D %5/%6/%7")
                .arg(command.role)
                .arg(command.target.latitudeDegrees, 0, 'f', 6)
                .arg(command.target.longitudeDegrees, 0, 'f', 6)
                .arg(command.target.relativeAltitudeM, 0, 'f', 1)
                .arg(command.velocity.northMps, 0, 'f', 1)
                .arg(command.velocity.eastMps, 0, 'f', 1)
                .arg(command.velocity.downMps, 0, 'f', 1);
            QTableWidgetItem *cell = table->item(row, TargetColumn);
            if (cell) {
                cell->setText(rows.at(row).target);
            }
        }
        setStatus(q->tr(
            "Follow Leader active: position/velocity targets queued for %1 air vehicle(s); trail %2 m.")
            .arg(targets.size()).arg(trail.lengthM(), 0, 'f', 1));
    }

    void clearTargets(bool updateCells = true)
    {
        for (int index = 0; index < rows.size(); ++index) {
            rows[index].target = QStringLiteral("—");
            if (updateCells && table) {
                QTableWidgetItem *cell = table->item(index, TargetColumn);
                if (cell) {
                    cell->setText(rows.at(index).target);
                }
            }
        }
    }

    void stop(const QString &reason)
    {
        runTimer->stop();
        const SwarmCommandSessionToken token = session;
        session = SwarmCommandSessionToken();
        activePlan = SwarmFollowLeaderPlan();
        bootstrapping = false;
        bootstrapTimer.invalidate();
        trail.clear();
        clearTargets();
        if (token.isValid() && commands) {
            commands->release(token);
        }
        setRunning(false);
        if (!reason.isEmpty()) {
            setStatus(reason);
        }
    }

    void handleSessionCancelled(quint64 id, const QString &reason)
    {
        if (!session.isValid() || session.id != id) {
            return;
        }
        runTimer->stop();
        session = SwarmCommandSessionToken();
        activePlan = SwarmFollowLeaderPlan();
        bootstrapping = false;
        bootstrapTimer.invalidate();
        trail.clear();
        clearTargets();
        setRunning(false);
        setStatus(reason.trimmed().isEmpty()
            ? q->tr(
                "Follow Leader stopped because the exact command session ended.")
            : reason);
    }

    void setRunning(bool value)
    {
        if (running == value) {
            runButton->setText(value ? q->tr("Stop Follow Leader")
                                     : q->tr("Start Follow Leader"));
            return;
        }
        running = value;
        runButton->setText(value ? q->tr("Stop Follow Leader")
                                 : q->tr("Start Follow Leader"));
        Q_EMIT q->runningChanged(value);
    }

    void setStatus(const QString &text)
    {
        if (status) {
            status->setText(text);
        }
    }

    void setUnavailable(QPushButton *control, const QString &reason)
    {
        if (!control) {
            return;
        }
        control->setEnabled(false);
        control->setToolTip(reason);
        control->setAccessibleDescription(reason);
    }

    SwarmFollowLeaderWindow *q = nullptr;
    QPointer<SwarmTelemetryRegistry> registry;
    SwarmFollowLeaderCommandInterface *commands = nullptr;
    std::unique_ptr<SwarmFollowLeaderCommandInterface> ownedCommands;
    Dependencies dependencies;
    QVector<VehicleRow> rows;
    SwarmVehicleInstanceLease groundMaster;
    SwarmVehicleInstanceLease airMaster;
    SwarmFollowLeaderPlan activePlan;
    SwarmCommandSessionToken session;
    SwarmFollowPathTrail trail;
    bool loading = false;
    bool running = false;
    bool bootstrapping = false;
    QElapsedTimer bootstrapTimer;

    QComboBox *groundCombo = nullptr;
    QComboBox *airCombo = nullptr;
    QDoubleSpinBox *separation = nullptr;
    QDoubleSpinBox *lead = nullptr;
    QDoubleSpinBox *altitude = nullptr;
    QDoubleSpinBox *takeoffAltitude = nullptr;
    QTableWidget *table = nullptr;
    QPushButton *runButton = nullptr;
    QLabel *status = nullptr;
    QTimer *statusTimer = nullptr;
    QTimer *runTimer = nullptr;
};

QPointer<SwarmFollowLeaderWindow> SwarmFollowLeaderWindow::s_current;

SwarmFollowLeaderWindow::SwarmFollowLeaderWindow(QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    std::unique_ptr<SwarmFollowLeaderCommandInterface> owned(
        new ApplicationSwarmFollowLeaderCommands(
            SwarmFollowLeaderApplicationCommandService()));
    SwarmFollowLeaderCommandInterface *commandInterface = owned.get();
    m_impl.reset(new Implementation(
        this, SwarmFollowLeaderApplicationRegistry(), commandInterface,
        std::move(owned), Dependencies{
            [](QWidget *dialogOwner, const QString &title,
               const QString &text, const QString &acceptText) {
                QMessageBox box(QMessageBox::Warning, title, text,
                                QMessageBox::NoButton, dialogOwner);
                QPushButton *cancel = box.addButton(QMessageBox::Cancel);
                QPushButton *accept = box.addButton(
                    acceptText, QMessageBox::AcceptRole);
                box.setDefaultButton(cancel);
                box.setEscapeButton(cancel);
                box.exec();
                return box.clickedButton() == accept;
            }}, owner));
}

SwarmFollowLeaderWindow::SwarmFollowLeaderWindow(
    SwarmTelemetryRegistry *registry,
    SwarmFollowLeaderCommandInterface *commands,
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    m_impl.reset(new Implementation(
        this, registry, commands, {}, std::move(dependencies), owner));
}

SwarmFollowLeaderWindow::~SwarmFollowLeaderWindow()
{
    m_impl.reset();
    if (s_current == this) {
        s_current = nullptr;
    }
}

SwarmFollowLeaderWindow *SwarmFollowLeaderWindow::OpenWindow(QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    s_current = new SwarmFollowLeaderWindow(resolvedOwner);
    s_current->setAttribute(Qt::WA_DeleteOnClose, true);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

QString SwarmFollowLeaderWindow::statusText() const
{
    return m_impl && m_impl->status ? m_impl->status->text() : QString();
}

bool SwarmFollowLeaderWindow::isRunning() const noexcept
{
    return m_impl && m_impl->running;
}

int SwarmFollowLeaderWindow::vehicleCount() const noexcept
{
    return m_impl ? m_impl->rows.size() : 0;
}

void SwarmFollowLeaderWindow::refreshVehicles()
{
    if (m_impl) {
        m_impl->refreshVehicles(true, true);
    }
}

void SwarmFollowLeaderWindow::closeEvent(QCloseEvent *event)
{
    if (m_impl) {
        m_impl->stop(tr(
            "Follow Leader stopped because the window closed."));
    }
    QWidget::closeEvent(event);
}
