#include "SwarmFollowPathWindow.h"

#include "comm/SwarmFlightMode.h"
#include "tools/SwarmFollowPathCore.h"
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

// Supplied by SwarmFollowPathWindowIntegration.cpp in the application and by
// null seams in the focused widget test.
SwarmTelemetryRegistry *SwarmFollowPathApplicationRegistry();
SwarmCommandService *SwarmFollowPathApplicationCommandService();

namespace
{

constexpr int LeaderSlot = 1;
constexpr int FirstFollowerSlot = 2;
constexpr int FollowPathRateHz = 5;
constexpr int TelemetryBootstrapTimeoutMs = 3000;

const char BulkUnavailableReason[] =
    "Bulk arm, disarm, takeoff and land commands are not yet ported to the "
    "exact multi-endpoint sender.";
const char PlaneUnavailableReason[] =
    "ArduPlane may lead Follow Path, but Plane followers require the exact "
    "guided MISSION_ITEM/ACK workflow and remain locked in this slice.";

enum FollowPathColumn {
    UseColumn = 0,
    OrderColumn,
    EndpointColumn,
    SystemColumn,
    ComponentColumn,
    RoleColumn,
    FirmwareColumn,
    LiveStatusColumn,
    TargetColumn,
    FollowPathColumnCount
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
        return QStringLiteral("Unsupported");
    }
    return QStringLiteral("Unsupported");
}

class ApplicationSwarmFollowPathCommands final
    : public SwarmFollowPathCommandInterface
{
public:
    explicit ApplicationSwarmFollowPathCommands(SwarmCommandService *service)
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

    ~ApplicationSwarmFollowPathCommands() override
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
        if (m_service) {
            return m_service->requestPositionStreams(token, slotIds, rateHz);
        }
        return unavailableReport();
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

class SwarmFollowPathWindow::Implementation
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
        SwarmFollowPathWindow *window,
        SwarmTelemetryRegistry *telemetryRegistry,
        SwarmFollowPathCommandInterface *commandInterface,
        std::unique_ptr<SwarmFollowPathCommandInterface> ownedInterface,
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
                SwarmFollowPathCommandInterface::SessionCancelledHandler());
        }
        stop(QString());
    }

    void buildUi(QWidget *owner)
    {
        q->setObjectName(QStringLiteral("SwarmFollowPathWindow"));
        q->setWindowTitle(q->tr("Swarm Follow Path (Beta)"));
        q->setWindowModality(Qt::NonModal);
        q->resize(WindowWidth, WindowHeight);
        q->setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
        q->setStyleSheet(QStringLiteral(
            "QWidget#SwarmFollowPathWindow { background: #303233; color: #dddddd; }"
            "QLabel#followPathDangerBanner { background: #5a2b20; color: #ffd4b8; "
            "border: 1px solid #ff9b66; border-radius: 4px; padding: 10px; }"
            "QLabel#followPathStatus { color: #73c7ff; }"
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
            "BETA / USE AT OWN RISK — exact Copter/Rover followers receive "
            "5 Hz position-only GUIDED targets at Order × Separation behind "
            "the recorded leader trail. Followers must already have the "
            "family-specific ArduPilot GUIDED custom mode. "
            "ArduPlane followers and bulk flight commands remain locked. "
            "Command streaming is restricted to connected TCP or UDP Client "
            "links; loss or replacement of any reserved vehicle stops the stream."),
            QStringLiteral("followPathDangerBanner"), q);
        danger->setWordWrap(true);
        root->addWidget(danger);

        auto *leaderRow = new QHBoxLayout;
        leaderRow->setSpacing(8);
        leaderRow->addWidget(label(q->tr("Leader"), QString(), q));
        leaderCombo = new QComboBox(q);
        leaderCombo->setObjectName(QStringLiteral("followPathLeader"));
        leaderCombo->setMinimumWidth(280);
        leaderRow->addWidget(leaderCombo);
        leaderRow->addWidget(button(
            q->tr("Refresh Vehicles"),
            QStringLiteral("followPathRefreshVehicles"), q));
        leaderRow->addSpacing(12);
        leaderRow->addWidget(label(q->tr("Separation m"), QString(), q));
        separation = new QDoubleSpinBox(q);
        separation->setObjectName(QStringLiteral("followPathSeparation"));
        separation->setRange(
            SwarmFollowPathCore::MinimumSeparationM,
            SwarmFollowPathCore::MaximumSeparationM);
        separation->setSingleStep(1.0);
        separation->setDecimals(1);
        separation->setValue(2.0);
        separation->setMinimumWidth(110);
        leaderRow->addWidget(separation);
        leaderRow->addWidget(label(q->tr(
            "Target distance = Order × Separation; order values must be unique."),
            QString(), q), 1);
        root->addLayout(leaderRow);

        table = new QTableWidget(q);
        table->setObjectName(QStringLiteral("FollowPathVehicleGrid"));
        table->setColumnCount(FollowPathColumnCount);
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
        table->setColumnWidth(RoleColumn, 145);
        table->setColumnWidth(FirmwareColumn, 100);
        table->setColumnWidth(LiveStatusColumn, 190);
        table->horizontalHeader()->setSectionResizeMode(
            TargetColumn, QHeaderView::Stretch);
        root->addWidget(table, 1);

        auto *commandRow = new QHBoxLayout;
        commandRow->setSpacing(8);
        runButton = button(q->tr("Start Follow Path"),
            QStringLiteral("FollowPathRunButton"), q);
        runButton->setMinimumWidth(150);
        commandRow->addWidget(runButton);
        const struct { const char *text; const char *name; } bulkButtons[] = {
            {"Arm Followers", "followPathArmFollowers"},
            {"Disarm Followers", "followPathDisarmFollowers"},
            {"Take Off", "followPathTakeoffFollowers"}
        };
        for (const auto &definition : bulkButtons) {
            QPushButton *control = button(
                q->tr(definition.text),
                QString::fromLatin1(definition.name), q);
            setUnavailable(control, q->tr(BulkUnavailableReason));
            commandRow->addWidget(control);
        }
        takeoffAltitude = new QDoubleSpinBox(q);
        takeoffAltitude->setObjectName(
            QStringLiteral("followPathTakeoffAltitude"));
        takeoffAltitude->setRange(1.0, 10000.0);
        takeoffAltitude->setSingleStep(1.0);
        takeoffAltitude->setDecimals(1);
        takeoffAltitude->setValue(5.0);
        takeoffAltitude->setSuffix(q->tr(" m"));
        takeoffAltitude->setMinimumWidth(125);
        takeoffAltitude->setEnabled(false);
        takeoffAltitude->setToolTip(q->tr(BulkUnavailableReason));
        takeoffAltitude->setAccessibleDescription(
            q->tr(BulkUnavailableReason));
        commandRow->addWidget(takeoffAltitude);
        QPushButton *land = button(q->tr("LAND Followers"),
            QStringLiteral("followPathLandFollowers"), q);
        setUnavailable(land, q->tr(BulkUnavailableReason));
        commandRow->addWidget(land);
        commandRow->addStretch(1);
        root->addLayout(commandRow);

        QLabel *unavailable = label(q->tr(
            "Available now: exact position-only Follow Path for exact-GUIDED "
            "Copter/Rover followers. Not yet ported: automatic GUIDED switching, "
            "Plane guided-waypoint/ACK and follower arm/takeoff/land commands."),
            QStringLiteral("followPathUnavailableHint"), q);
        unavailable->setWordWrap(true);
        root->addWidget(unavailable);

        auto *footer = new QHBoxLayout;
        status = label(q->tr(
            "Choose a leader and explicitly enable exact-GUIDED followers in "
            "their trail order."), QStringLiteral("followPathStatus"), q);
        status->setWordWrap(true);
        footer->addWidget(status, 1);
        footer->addWidget(button(q->tr("Close"),
            QStringLiteral("followPathClose"), q));
        root->addLayout(footer);

        statusTimer = new QTimer(q);
        statusTimer->setInterval(500);
        runTimer = new QTimer(q);
        runTimer->setInterval(1000 / FollowPathRateHz);
        runTimer->setTimerType(Qt::PreciseTimer);
    }

    void connectUi()
    {
        QObject::connect(
            leaderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            q, [this](int index) { leaderChanged(index); });
        QObject::connect(
            q->findChild<QPushButton *>(
                QStringLiteral("followPathRefreshVehicles")),
            &QPushButton::clicked, q,
            [this]() { refreshVehicles(true, true); });
        QObject::connect(table, &QTableWidget::cellChanged, q,
                         [this](int row, int column) {
            tableCellChanged(row, column);
        });
        QObject::connect(separation,
                         QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                         q, [this](double) {
            stop(q->tr("Follow Path stopped because separation changed."));
            clearTargets();
            trail.clear();
        });
        QObject::connect(runButton, &QPushButton::clicked, q,
                         [this]() { toggleRun(); });
        QObject::connect(
            q->findChild<QPushButton *>(QStringLiteral("followPathClose")),
            &QPushButton::clicked, q, &QWidget::close);
        QObject::connect(statusTimer, &QTimer::timeout, q,
                         [this]() { updateLiveStatus(); });
        QObject::connect(runTimer, &QTimer::timeout, q,
                         [this]() { runTick(); });

        if (registry) {
            QObject::connect(
                registry.data(), &SwarmTelemetryRegistry::endpointActivated,
                q, [this](const SwarmTelemetrySnapshot &) {
                    refreshVehicles(rows.isEmpty() && !running, false);
                });
            QObject::connect(
                registry.data(), &SwarmTelemetryRegistry::endpointUpdated,
                q, [this](const SwarmTelemetrySnapshot &snapshot) {
                    const int row = rowForLease(snapshot.lease);
                    if (row < 0) {
                        refreshVehicles(false, false);
                        return;
                    }
                    const SwarmVehicleFamily oldFamily =
                        SwarmFormationCore::family(rows.at(row).snapshot);
                    rows[row].snapshot = snapshot;
                    if (oldFamily != SwarmFormationCore::family(snapshot)) {
                        refreshVehicles(false, false);
                    }
                });
            QObject::connect(
                registry.data(), &SwarmTelemetryRegistry::endpointRetired,
                q, [this](const SwarmVehicleInstanceLease &lease,
                          SwarmTelemetryRegistry::RetirementReason) {
                    QString reason;
                    if (running && sameVehicle(lease, activePlan.leader)) {
                        reason = q->tr(
                            "Follow Path stopped: the leader was retired.");
                    } else if (running) {
                        for (const SwarmFollowPathFollower &follower
                             : activePlan.followers) {
                            if (sameVehicle(lease, follower.lease)) {
                                reason = q->tr(
                                    "Follow Path stopped: a follower was retired.");
                                break;
                            }
                        }
                    }
                    if (!reason.isEmpty()) {
                        stop(reason);
                    }
                    refreshVehicles(false, false);
                });
            QObject::connect(registry.data(), &QObject::destroyed, q,
                             [this]() {
                stop(q->tr(
                    "Follow Path stopped: swarm telemetry is unavailable."));
                registry = nullptr;
                rows.clear();
                leader = SwarmVehicleInstanceLease();
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

    bool isLeader(const VehicleRow &row) const
    {
        return sameVehicle(row.lease, leader);
    }

    void refreshVehicles(bool explicitlyRequested, bool stopRunning)
    {
        if (stopRunning) {
            stop(q->tr(
                "Follow Path stopped because the vehicle list was refreshed."));
        }
        const QVector<SwarmTelemetrySnapshot> discovered = discoverVehicles();
        const SwarmVehicleInstanceLease previousLeader = leader;
        QVector<VehicleRow> next;
        next.reserve(discovered.size());
        for (const SwarmTelemetrySnapshot &snapshot : discovered) {
            VehicleRow value;
            value.lease = snapshot.lease;
            value.snapshot = snapshot;
            const int previous = rowForLease(snapshot.lease);
            if (previous >= 0) {
                value.included = rows.at(previous).included;
                value.order = rows.at(previous).order;
            }
            next.append(value);
        }
        rows = next;

        bool replacedLeader = previousLeader.isValid()
            && rowForLease(previousLeader) < 0;
        if (!previousLeader.isValid() || replacedLeader) {
            leader = SwarmVehicleInstanceLease();
            for (const VehicleRow &row : rows) {
                if (SwarmFormationCore::supportsFormation(row.snapshot)) {
                    leader = row.lease;
                    break;
                }
            }
        } else {
            leader = previousLeader;
        }

        if (replacedLeader) {
            for (VehicleRow &row : rows) {
                row.included = false;
                row.order = 0;
            }
            trail.clear();
        }
        const int leaderRow = rowForLease(leader);
        if (leaderRow >= 0) {
            rows[leaderRow].included = true;
            rows[leaderRow].order = 0;
        }
        assignMissingOrders();
        clearTargets(false);
        rebuildVehicleWidgets();
        updateLiveStatus();

        if (replacedLeader) {
            setStatus(leader.isValid()
                ? q->tr("The previous leader is unavailable. A replacement was selected and followers were disabled; review their order before starting.")
                : q->tr("The previous leader is unavailable and no supported replacement was found."));
        } else if (explicitlyRequested) {
            int eligible = 0;
            QSet<int> links;
            for (const VehicleRow &row : rows) {
                if (SwarmFormationCore::supportsFormation(row.snapshot)) {
                    ++eligible;
                    links.insert(row.lease.endpoint.linkId);
                }
            }
            setStatus(eligible == 0
                ? q->tr("No supported Plane/Copter/Rover autopilots were found across open MAVLink links.")
                : q->tr("Found %1 supported autopilot(s) across %2 link(s). Explicitly enable exact-GUIDED Copter/Rover followers.")
                    .arg(eligible).arg(links.size()));
        }
    }

    void assignMissingOrders()
    {
        QSet<int> used;
        for (const VehicleRow &row : rows) {
            if (!isLeader(row) && row.order > 0) {
                used.insert(row.order);
            }
        }
        int next = 1;
        for (VehicleRow &row : rows) {
            if (isLeader(row)
                || !SwarmFormationCore::supportsPositionFollower(row.snapshot)
                || row.order > 0) {
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
        rebuildLeaderCombo();
        rebuildTable();
        loading = false;
    }

    void rebuildLeaderCombo()
    {
        const QSignalBlocker blocker(leaderCombo);
        leaderCombo->clear();
        int selected = -1;
        for (const VehicleRow &row : rows) {
            if (!SwarmFormationCore::supportsFormation(row.snapshot)) {
                continue;
            }
            const int index = leaderCombo->count();
            leaderCombo->addItem(
                vehicleLabel(row.lease), QVariant::fromValue(row.lease));
            if (isLeader(row)) {
                selected = index;
            }
        }
        leaderCombo->setCurrentIndex(selected);
    }

    void rebuildTable()
    {
        const QSignalBlocker blocker(table);
        table->setRowCount(rows.size());
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const VehicleRow row = rows.at(rowIndex);
            const bool leaderRow = isLeader(row);
            const bool followerEligible =
                SwarmFormationCore::supportsPositionFollower(row.snapshot);
            const bool plane = SwarmFormationCore::isPlane(row.snapshot);

            auto *use = new QTableWidgetItem;
            Qt::ItemFlags useFlags = Qt::ItemIsSelectable;
            if (leaderRow || followerEligible) {
                useFlags |= Qt::ItemIsEnabled;
            }
            if (followerEligible && !leaderRow) {
                useFlags |= Qt::ItemIsUserCheckable;
            }
            use->setFlags(useFlags);
            use->setCheckState(leaderRow || row.included
                ? Qt::Checked : Qt::Unchecked);
            if (plane && !leaderRow) {
                use->setToolTip(q->tr(PlaneUnavailableReason));
            }
            table->setItem(rowIndex, UseColumn, use);

            auto *order = new QTableWidgetItem(
                leaderRow ? QStringLiteral("—")
                          : QString::number(row.order));
            Qt::ItemFlags orderFlags = Qt::ItemIsSelectable;
            if (leaderRow || followerEligible) {
                orderFlags |= Qt::ItemIsEnabled;
            }
            if (followerEligible && !leaderRow) {
                orderFlags |= Qt::ItemIsEditable;
            }
            order->setFlags(orderFlags);
            table->setItem(rowIndex, OrderColumn, order);

            const QString endpoint = row.lease.endpoint.linkName.trimmed().isEmpty()
                ? q->tr("Link %1").arg(row.lease.endpoint.linkId)
                : row.lease.endpoint.linkName.trimmed();
            const QString role = leaderRow ? q->tr("Leader")
                : followerEligible ? q->tr("Follower")
                : plane ? q->tr("Leader only (Plane locked)")
                : q->tr("Unsupported (disabled)");
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
        if (!commands || !commands->routeIsEligible(
                row.lease, &routeError)) {
            return routeError.trimmed().isEmpty()
                ? q->tr("Command route unavailable")
                : q->tr("Route locked: %1").arg(routeError);
        }
        SwarmTelemetrySnapshot current;
        if (!registry || !registry->snapshotForLease(row.lease, &current)
            || !registry->validateLease(row.lease)) {
            return q->tr("Telemetry changed during route check");
        }
        row.snapshot = current;
        if (!SwarmFormationCore::supportsFormation(row.snapshot)) {
            return q->tr("Unsupported Follow Path firmware");
        }
        if (!row.snapshot.positionValid
            || !registry->observationIsFresh(
                row.snapshot.positionObservedMs,
                SwarmFollowPathCore::MaximumTelemetryAgeMs)) {
            return q->tr("Position unavailable");
        }
        const QString actualMode = SwarmFlightMode::displayName(row.snapshot);
        const QString mode = SwarmFlightMode::isExactGuided(row.snapshot)
            ? actualMode : q->tr("%1 (not GUIDED)").arg(actualMode);
        return q->tr("%1; %2").arg(
            mode, row.snapshot.armed ? q->tr("armed") : q->tr("disarmed"));
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

    void leaderChanged(int index)
    {
        if (loading || index < 0 || index >= leaderCombo->count()) {
            return;
        }
        const SwarmVehicleInstanceLease next = qvariant_cast<
            SwarmVehicleInstanceLease>(leaderCombo->itemData(index));
        if (!next.isValid() || sameVehicle(next, leader)) {
            return;
        }
        const int nextRow = rowForLease(next);
        if (nextRow < 0 || !SwarmFormationCore::supportsFormation(
                rows.at(nextRow).snapshot)) {
            return;
        }
        stop(q->tr("Follow Path stopped because the leader changed."));
        const int oldLeader = rowForLease(leader);
        if (oldLeader >= 0) {
            rows[oldLeader].included = false;
            rows[oldLeader].order = 0;
        }
        leader = next;
        rows[nextRow].included = true;
        rows[nextRow].order = 0;
        assignMissingOrders();
        trail.clear();
        clearTargets(false);
        rebuildVehicleWidgets();
        updateLiveStatus();
    }

    void tableCellChanged(int rowIndex, int column)
    {
        if (loading || rowIndex < 0 || rowIndex >= rows.size()) {
            return;
        }
        VehicleRow &row = rows[rowIndex];
        if (column == UseColumn) {
            if (isLeader(row)
                || !SwarmFormationCore::supportsPositionFollower(row.snapshot)) {
                rebuildTable();
                updateLiveStatus();
                return;
            }
            const bool included = table->item(rowIndex, UseColumn)->checkState()
                == Qt::Checked;
            if (row.included != included) {
                stop(q->tr(
                    "Follow Path stopped because follower selection changed."));
                row.included = included;
                clearTargets(false);
                trail.clear();
                rebuildTable();
                updateLiveStatus();
            }
            return;
        }
        if (column != OrderColumn || isLeader(row)
            || !SwarmFormationCore::supportsPositionFollower(row.snapshot)) {
            return;
        }
        bool valid = false;
        const int value = table->item(rowIndex, OrderColumn)->text().toInt(&valid);
        if (!valid || value < 1 || value > SwarmFollowPathCore::MaximumOrder) {
            setStatus(q->tr("Follower order must be an integer from 1 to 100."));
            rebuildTable();
            updateLiveStatus();
            return;
        }
        if (row.order == value) {
            return;
        }
        stop(q->tr("Follow Path stopped because follower order changed."));
        row.order = value;
        clearTargets(false);
        trail.clear();
        rebuildTable();
        updateLiveStatus();
    }

    bool tryBuildPlan(SwarmFollowPathPlan *plan,
                      QList<SwarmTelemetrySnapshot> *snapshots,
                      QVector<SwarmCommandMember> *members,
                      QString *error) const
    {
        if (plan) {
            *plan = SwarmFollowPathPlan();
        }
        if (snapshots) {
            snapshots->clear();
        }
        if (members) {
            members->clear();
        }
        if (!plan || !snapshots || !members || !error || !registry
            || !leader.isValid()) {
            if (error) {
                *error = q->tr(
                    "Select a live autopilot as Follow Path leader.");
            }
            return false;
        }
        const SwarmVehicleInstanceLease selectedLeader = leader;
        const int leaderRow = rowForLease(selectedLeader);
        if (leaderRow < 0) {
            *error = q->tr(
                "Select a supported Plane/Copter/Rover leader.");
            return false;
        }
        const VehicleRow selectedLeaderRow = rows.at(leaderRow);
        if (!SwarmFormationCore::supportsFormation(
                selectedLeaderRow.snapshot)) {
            *error = q->tr(
                "Select a supported Plane/Copter/Rover leader.");
            return false;
        }
        if (!std::isfinite(separation->value())
            || separation->value() < SwarmFollowPathCore::MinimumSeparationM
            || separation->value() > SwarmFollowPathCore::MaximumSeparationM) {
            *error = q->tr("Separation must be between 1 and 500 m.");
            return false;
        }

        QVector<VehicleRow> selectedFollowers;
        for (const VehicleRow &row : rows) {
            if (row.included && !sameVehicle(row.lease, selectedLeader)) {
                selectedFollowers.append(row);
            }
        }

        plan->leader = selectedLeader;
        plan->separationM = separation->value();
        QSet<int> orders;
        for (const VehicleRow &row : selectedFollowers) {
            if (SwarmFormationCore::isPlane(row.snapshot)) {
                *error = q->tr(
                    "Follower %1 is ArduPlane; guided-waypoint/ACK control is not yet ported.")
                    .arg(vehicleLabel(row.lease));
                return false;
            }
            if (!SwarmFormationCore::supportsPositionFollower(row.snapshot)) {
                *error = q->tr("Follower %1 is unsupported.")
                    .arg(vehicleLabel(row.lease));
                return false;
            }
            if (row.order < 1 || row.order > SwarmFollowPathCore::MaximumOrder
                || orders.contains(row.order)) {
                *error = q->tr(
                    "Follower order must be unique and between 1 and 100.");
                return false;
            }
            if (!SwarmFlightMode::isExactGuided(row.snapshot)) {
                *error = q->tr(
                    "Follower %1 is in %2 and must already be in exact GUIDED mode before starting.")
                    .arg(vehicleLabel(row.lease),
                         SwarmFlightMode::displayName(row.snapshot));
                return false;
            }
            orders.insert(row.order);
            plan->followers.append({row.lease, row.order});
        }
        if (plan->followers.isEmpty()) {
            *error = q->tr(
                "Explicitly enable at least one exact-GUIDED Copter or Rover follower.");
            return false;
        }
        std::sort(plan->followers.begin(), plan->followers.end(),
                  [](const SwarmFollowPathFollower &left,
                     const SwarmFollowPathFollower &right) {
            return left.order < right.order;
        });

        QString routeError;
        if (!commands || !commands->routeIsEligible(
                plan->leader, &routeError)) {
            *error = routeError.trimmed().isEmpty()
                ? q->tr("The leader command route is unavailable.")
                : q->tr("Leader route is unavailable: %1").arg(routeError);
            return false;
        }
        for (const SwarmFollowPathFollower &follower : plan->followers) {
            if (!commands->routeIsEligible(follower.lease, &routeError)) {
                *error = routeError.trimmed().isEmpty()
                    ? q->tr("Follower %1 command route is unavailable.")
                        .arg(vehicleLabel(follower.lease))
                    : q->tr("Follower %1 route is unavailable: %2")
                        .arg(vehicleLabel(follower.lease), routeError);
                return false;
            }
        }

        SwarmVehicleGroupLease group;
        group.members.append(plan->leader);
        for (const SwarmFollowPathFollower &follower : plan->followers) {
            group.members.append(follower.lease);
        }
        if (!registry->validateGroup(
                group, snapshots,
                SwarmFollowPathCore::MaximumTelemetryAgeMs)) {
            *error = q->tr(
                "A Follow Path vehicle disappeared, expired, or was reconnected.");
            return false;
        }

        for (int index = 0; index < snapshots->size(); ++index) {
            const SwarmTelemetrySnapshot &snapshot = snapshots->at(index);
            if (index > 0 && !SwarmFlightMode::isExactGuided(snapshot)) {
                *error = q->tr(
                    "A selected follower is in %1, not exact GUIDED mode.")
                    .arg(SwarmFlightMode::displayName(snapshot));
                return false;
            }
        }

        SwarmCommandMember leaderMember;
        leaderMember.slotId = LeaderSlot;
        leaderMember.lease = plan->leader;
        leaderMember.required.fields = SwarmTelemetryRequirements::Position;
        leaderMember.required.heartbeatMaximumAgeMs =
            SwarmFollowPathCore::MaximumTelemetryAgeMs;
        leaderMember.required.positionMaximumAgeMs =
            SwarmFollowPathCore::MaximumTelemetryAgeMs;
        members->append(leaderMember);
        for (int index = 0; index < plan->followers.size(); ++index) {
            SwarmCommandMember followerMember;
            followerMember.slotId = FirstFollowerSlot + index;
            followerMember.lease = plan->followers.at(index).lease;
            followerMember.required.fields = SwarmTelemetryRequirements::Position;
            followerMember.required.heartbeatMaximumAgeMs =
                SwarmFollowPathCore::MaximumTelemetryAgeMs;
            followerMember.required.positionMaximumAgeMs =
                SwarmFollowPathCore::MaximumTelemetryAgeMs;
            followerMember.flightMode =
                SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
            members->append(followerMember);
        }
        error->clear();
        return true;
    }

    QString confirmationTargets(const SwarmFollowPathPlan &plan) const
    {
        QVector<SwarmFollowPathFollower> ordered = plan.followers;
        std::sort(ordered.begin(), ordered.end(),
                  [](const SwarmFollowPathFollower &left,
                     const SwarmFollowPathFollower &right) {
            return left.order < right.order;
        });
        QStringList lines;
        for (const SwarmFollowPathFollower &follower : ordered) {
            lines.append(q->tr("• #%1 %2 — %3 m behind leader")
                .arg(follower.order)
                .arg(vehicleLabel(follower.lease))
                .arg(follower.order * plan.separationM, 0, 'f', 1));
        }
        return lines.join(QLatin1Char('\n'));
    }

    void toggleRun()
    {
        if (running) {
            stop(q->tr("Follow Path stopped by operator."));
            return;
        }
        if (!commands) {
            setStatus(q->tr("The exact swarm command service is unavailable."));
            return;
        }
        SwarmFollowPathPlan plan;
        QList<SwarmTelemetrySnapshot> snapshots;
        QVector<SwarmCommandMember> members;
        QString error;
        if (!tryBuildPlan(&plan, &snapshots, &members, &error)) {
            setStatus(error);
            return;
        }
        const bool accepted = dependencies.confirmDangerous
            && dependencies.confirmDangerous(
                q, q->tr("Start Swarm Follow Path"),
                q->tr("BETA / USE AT OWN RISK. APM Planner 3.0 will queue "
                      "position-only trail targets at 5 Hz to these exact, "
                      "exact-GUIDED followers:\n\n%1\n\nTargets begin only "
                      "after fresh position telemetry and enough leader trail "
                      "are available. Verify ordering, separation, relative "
                      "altitude and clear airspace. Cancel is the default action.")
                    .arg(confirmationTargets(plan)),
                q->tr("START FOLLOW PATH"));
        if (!accepted) {
            setStatus(q->tr("Follow Path start cancelled."));
            return;
        }
        if (!tryBuildPlan(&plan, &snapshots, &members, &error)) {
            setStatus(q->tr(
                "Follow Path changed while confirmation was open. %1")
                .arg(error));
            return;
        }

        SwarmCommandSessionToken token;
        const SwarmCommandService::Result reserved = commands->reserve(
            q, members, FollowPathRateHz, &token, &error);
        if (reserved != SwarmCommandService::Result::Reserved
            || !token.isValid()) {
            setStatus(error.isEmpty()
                ? q->tr(
                    "The exact Follow Path command session could not be reserved.")
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
                session, streamSlots, FollowPathRateHz);
        if (!streams.allSent()) {
            stop(q->tr("Follow Path could not start: %1")
                .arg(streams.detail));
            return;
        }

        bootstrapping = true;
        bootstrapTimer.restart();
        setRunning(true);
        setStatus(q->tr(
            "Waiting for fresh leader and follower position telemetry…"));
        runTick();
        if (running) {
            runTimer->start();
        }
    }

    QVector<SwarmPositionTarget> positionTargets(
        const SwarmFollowPathTick &tick) const
    {
        QVector<SwarmPositionTarget> targets;
        targets.reserve(tick.commands.size());
        for (const SwarmFollowPathCommand &command : tick.commands) {
            int followerIndex = -1;
            for (int index = 0; index < activePlan.followers.size(); ++index) {
                if (sameVehicle(command.follower,
                                activePlan.followers.at(index).lease)) {
                    followerIndex = index;
                    break;
                }
            }
            if (followerIndex < 0) {
                return {};
            }
            SwarmPositionTarget target;
            target.slotId = FirstFollowerSlot + followerIndex;
            target.latitudeDegrees = command.target.latitudeDegrees;
            target.longitudeDegrees = command.target.longitudeDegrees;
            target.relativeAltitudeM = static_cast<float>(
                command.target.relativeAltitudeM);
            target.useVelocity = false;
            targets.append(target);
        }
        return targets;
    }

    bool bootstrapCanWait(const QString &detail) const
    {
        return detail.contains(QStringLiteral("position"), Qt::CaseInsensitive)
            && (detail.contains(QStringLiteral("unavailable"),
                                Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("stale"),
                                   Qt::CaseInsensitive));
    }

    void runTick()
    {
        if (!running || !registry || !commands || !session.isValid()) {
            return;
        }
        SwarmVehicleGroupLease group;
        group.members.append(activePlan.leader);
        for (const SwarmFollowPathFollower &follower : activePlan.followers) {
            group.members.append(follower.lease);
        }
        QList<SwarmTelemetrySnapshot> snapshots;
        if (!registry->validateGroup(
                group, &snapshots,
                SwarmFollowPathCore::MaximumTelemetryAgeMs)) {
            stop(q->tr(
                "Follow Path stopped: a reserved vehicle disappeared, expired, or was reconnected."));
            return;
        }
        const SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
            activePlan, snapshots, registry->observationClockNowMs(), &trail);
        if (tick.state == SwarmFollowPathTick::State::Stopped) {
            if (bootstrapping && bootstrapCanWait(tick.detail)
                && bootstrapTimer.isValid()
                && bootstrapTimer.elapsed() < TelemetryBootstrapTimeoutMs) {
                setStatus(q->tr(
                    "Waiting for fresh leader and follower position telemetry…"));
                return;
            }
            stop(tick.detail);
            return;
        }
        if (bootstrapping) {
            bootstrapping = false;
            bootstrapTimer.invalidate();
        }
        if (tick.state == SwarmFollowPathTick::State::WaitingForTrail) {
            clearTargets();
            setStatus(tick.detail);
            return;
        }

        const QVector<SwarmPositionTarget> targets = positionTargets(tick);
        if (targets.size() != activePlan.followers.size()) {
            stop(q->tr(
                "Follow Path stopped: generated targets no longer match the reserved followers."));
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
            stop(q->tr("Follow Path stopped: %1").arg(sent.detail));
            return;
        }
        clearTargets(false);
        for (const SwarmFollowPathCommand &command : tick.commands) {
            const int row = rowForLease(command.follower);
            if (row >= 0) {
                rows[row].target = q->tr(
                    "%1 m: %2, %3, %4 m")
                    .arg(command.distanceBehindM, 0, 'f', 1)
                    .arg(command.target.latitudeDegrees, 0, 'f', 6)
                    .arg(command.target.longitudeDegrees, 0, 'f', 6)
                    .arg(command.target.relativeAltitudeM, 0, 'f', 1);
                QTableWidgetItem *cell = table->item(row, TargetColumn);
                if (cell) {
                    cell->setText(rows.at(row).target);
                }
            }
        }
        setStatus(q->tr(
            "Follow Path active: position-only targets queued for %1 follower(s); trail %2 m.")
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
        activePlan = SwarmFollowPathPlan();
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
        activePlan = SwarmFollowPathPlan();
        bootstrapping = false;
        bootstrapTimer.invalidate();
        trail.clear();
        clearTargets();
        setRunning(false);
        setStatus(reason.trimmed().isEmpty()
            ? q->tr(
                "Follow Path stopped because the exact command session ended.")
            : reason);
    }

    void setRunning(bool value)
    {
        if (running == value) {
            runButton->setText(value ? q->tr("Stop Follow Path")
                                     : q->tr("Start Follow Path"));
            return;
        }
        running = value;
        runButton->setText(value ? q->tr("Stop Follow Path")
                                 : q->tr("Start Follow Path"));
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

    SwarmFollowPathWindow *q = nullptr;
    QPointer<SwarmTelemetryRegistry> registry;
    SwarmFollowPathCommandInterface *commands = nullptr;
    std::unique_ptr<SwarmFollowPathCommandInterface> ownedCommands;
    Dependencies dependencies;
    QVector<VehicleRow> rows;
    SwarmVehicleInstanceLease leader;
    SwarmFollowPathPlan activePlan;
    SwarmCommandSessionToken session;
    SwarmFollowPathTrail trail;
    bool loading = false;
    bool running = false;
    bool bootstrapping = false;
    QElapsedTimer bootstrapTimer;

    QComboBox *leaderCombo = nullptr;
    QDoubleSpinBox *separation = nullptr;
    QTableWidget *table = nullptr;
    QPushButton *runButton = nullptr;
    QDoubleSpinBox *takeoffAltitude = nullptr;
    QLabel *status = nullptr;
    QTimer *statusTimer = nullptr;
    QTimer *runTimer = nullptr;
};

QPointer<SwarmFollowPathWindow> SwarmFollowPathWindow::s_current;

SwarmFollowPathWindow::SwarmFollowPathWindow(QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    std::unique_ptr<SwarmFollowPathCommandInterface> owned(
        new ApplicationSwarmFollowPathCommands(
            SwarmFollowPathApplicationCommandService()));
    SwarmFollowPathCommandInterface *commandInterface = owned.get();
    m_impl.reset(new Implementation(
        this, SwarmFollowPathApplicationRegistry(), commandInterface,
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

SwarmFollowPathWindow::SwarmFollowPathWindow(
    SwarmTelemetryRegistry *registry,
    SwarmFollowPathCommandInterface *commands,
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    m_impl.reset(new Implementation(
        this, registry, commands, {}, std::move(dependencies), owner));
}

SwarmFollowPathWindow::~SwarmFollowPathWindow()
{
    m_impl.reset();
    if (s_current == this) {
        s_current = nullptr;
    }
}

SwarmFollowPathWindow *SwarmFollowPathWindow::OpenWindow(QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    s_current = new SwarmFollowPathWindow(resolvedOwner);
    s_current->setAttribute(Qt::WA_DeleteOnClose, true);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

QString SwarmFollowPathWindow::statusText() const
{
    return m_impl && m_impl->status ? m_impl->status->text() : QString();
}

bool SwarmFollowPathWindow::isRunning() const noexcept
{
    return m_impl && m_impl->running;
}

int SwarmFollowPathWindow::vehicleCount() const noexcept
{
    return m_impl ? m_impl->rows.size() : 0;
}

void SwarmFollowPathWindow::refreshVehicles()
{
    if (m_impl) {
        m_impl->refreshVehicles(true, true);
    }
}

void SwarmFollowPathWindow::closeEvent(QCloseEvent *event)
{
    if (m_impl) {
        m_impl->stop(tr("Follow Path stopped because the window closed."));
    }
    QWidget::closeEvent(event);
}
