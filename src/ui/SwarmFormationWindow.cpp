#include "SwarmFormationWindow.h"

#include "FormationGridControl.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// Supplied by SwarmFormationWindowIntegration.cpp in the application and by
// null seams in the focused widget test.
SwarmTelemetryRegistry *SwarmFormationApplicationRegistry();
SwarmCommandService *SwarmFormationApplicationCommandService();

namespace
{
constexpr int kLeaderSlot = 1;
constexpr int kFirstFollowerSlot = 2;
constexpr int kFormationRateHz = 10;
constexpr int kTelemetryBootstrapTimeoutMs = 3000;

const char kPositionOnlyReason[] =
    "This Formation slice sends exact Copter/Rover position and velocity "
    "targets only. Follower yaw and gimbal commands are not yet ported.";
const char kPlaneReason[] =
    "ArduPlane attitude/PID commands are not yet ported to the exact swarm "
    "sender. Plane may be inspected or selected as leader, but not commanded "
    "as a follower.";
const char kBulkReason[] =
    "Bulk arm, mode, takeoff and land commands are not yet ported to the "
    "exact multi-endpoint sender.";

enum FormationColumn {
    UseColumn = 0,
    EndpointColumn,
    SystemColumn,
    ComponentColumn,
    RoleColumn,
    XColumn,
    YColumn,
    ZColumn,
    FirmwareColumn,
    LiveStatusColumn,
    FormationColumnCount
};

QPushButton *button(const QString &text, const QString &objectName,
                    QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(objectName);
    return result;
}

QLabel *label(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(objectName);
    return result;
}

QString number(double value)
{
    return QString::number(value, 'f', 1);
}

class ApplicationSwarmFormationCommands final
    : public SwarmFormationCommandInterface
{
public:
    explicit ApplicationSwarmFormationCommands(SwarmCommandService *service)
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

    ~ApplicationSwarmFormationCommands() override
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

    SwarmCommandService::BatchReport requestStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) override
    {
        return m_service
            ? m_service->requestPositionAndAttitudeStreams(
                token, slotIds, rateHz)
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

QPointer<SwarmFormationWindow> SwarmFormationWindow::s_current;

SwarmFormationWindow::SwarmFormationWindow(QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_registry(SwarmFormationApplicationRegistry())
    , m_ownedCommands(new ApplicationSwarmFormationCommands(
          SwarmFormationApplicationCommandService()))
    , m_dependencies(DefaultDependencies())
{
    m_commands = m_ownedCommands.get();
    initialize(owner);
}

SwarmFormationWindow::SwarmFormationWindow(
    SwarmTelemetryRegistry *registry,
    SwarmFormationCommandInterface *commands,
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_registry(registry)
    , m_commands(commands)
    , m_dependencies(std::move(dependencies))
{
    initialize(owner);
}

SwarmFormationWindow::~SwarmFormationWindow()
{
    if (m_commands) {
        m_commands->setSessionCancelledHandler(
            SwarmFormationCommandInterface::SessionCancelledHandler());
    }
    stopFormation(QString());
    if (s_current == this) {
        s_current = nullptr;
    }
}

SwarmFormationWindow *SwarmFormationWindow::OpenWindow(QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    s_current = new SwarmFormationWindow(resolvedOwner);
    s_current->setAttribute(Qt::WA_DeleteOnClose, true);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

SwarmFormationWindow::Dependencies SwarmFormationWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.confirmDangerous = [](
        QWidget *owner, const QString &title, const QString &text,
        const QString &acceptText) {
        QMessageBox box(QMessageBox::Warning, title, text,
                        QMessageBox::NoButton, owner);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        QPushButton *accept = box.addButton(
            acceptText, QMessageBox::AcceptRole);
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.exec();
        return box.clickedButton() == accept;
    };
    return dependencies;
}

void SwarmFormationWindow::initialize(QWidget *owner)
{
    buildUi(owner);
    connectUi();
    if (m_commands) {
        m_commands->setSessionCancelledHandler(
            [this](quint64 sessionId, const QString &reason) {
                handleSessionCancelled(sessionId, reason);
            });
    }
    refreshVehicles(true, false);
    m_statusTimer->start();
}

QString SwarmFormationWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

bool SwarmFormationWindow::sameVehicle(
    const SwarmVehicleInstanceLease &left,
    const SwarmVehicleInstanceLease &right)
{
    return left.isValid() && right.isValid() && left.sameInstance(right);
}

QString SwarmFormationWindow::vehicleKey(
    const SwarmVehicleInstanceLease &vehicle)
{
    return QStringLiteral("%1/%2/%3/%4/%5")
        .arg(vehicle.endpoint.linkId)
        .arg(vehicle.endpoint.systemId)
        .arg(vehicle.endpoint.componentId)
        .arg(vehicle.linkSessionEpoch)
        .arg(vehicle.instanceEpoch);
}

QString SwarmFormationWindow::vehicleLabel(
    const SwarmVehicleInstanceLease &vehicle)
{
    return QStringLiteral("%1 — %2:%3")
        .arg(vehicle.endpoint.linkName.trimmed().isEmpty()
                 ? tr("Link %1").arg(vehicle.endpoint.linkId)
                 : vehicle.endpoint.linkName.trimmed())
        .arg(vehicle.endpoint.systemId)
        .arg(vehicle.endpoint.componentId);
}

QString SwarmFormationWindow::familyLabel(
    const SwarmTelemetrySnapshot &snapshot)
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

void SwarmFormationWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SwarmFormationWindow"));
    setWindowTitle(tr("Swarm Formation (Beta)"));
    setWindowModality(Qt::NonModal);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#SwarmFormationWindow { background: #303233; color: #dddddd; }"
        "QLabel#formationDangerBanner { background: #5a2b20; color: #ffd4b8; "
        "border: 1px solid #ff9b66; border-radius: 4px; padding: 10px; }"
        "QLabel#formationStatus { color: #73c7ff; }"
        "QTableWidget, QComboBox, QDoubleSpinBox { background: #242627; "
        "color: #dddddd; alternate-background-color: #292c2d; }"));
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(width() / 2, height() / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    QLabel *danger = label(tr(
        "BETA / USE AT OWN RISK — port of the official Mission Planner "
        "Formation tool. This slice sends Copter/Rover position and velocity "
        "targets. ArduPlane attitude/PID, follower yaw/gimbal alignment and "
        "bulk flight commands remain locked until their exact-instance senders "
        "are ported. Command streaming is currently restricted to connected "
        "TCP or UDP Client links; Serial/radio and listening UDP remain locked. "
        "Loss of a reserved modem, leader or follower stops the 10 Hz stream "
        "immediately."),
        QStringLiteral("formationDangerBanner"), this);
    danger->setWordWrap(true);
    root->addWidget(danger);

    auto *leaderArea = new QVBoxLayout;
    leaderArea->setSpacing(6);
    auto *leaderRow = new QHBoxLayout;
    leaderRow->setSpacing(8);
    leaderRow->addWidget(label(tr("Leader"), QString(), this));
    m_leaderCombo = new QComboBox(this);
    m_leaderCombo->setObjectName(QStringLiteral("formationLeader"));
    m_leaderCombo->setMinimumWidth(260);
    leaderRow->addWidget(m_leaderCombo);
    leaderRow->addWidget(button(tr("Refresh Vehicles"),
        QStringLiteral("formationRefreshVehicles"), this));
    leaderRow->addWidget(button(tr("Capture Live Offsets"),
        QStringLiteral("formationCaptureOffsets"), this));
    leaderRow->addStretch(1);
    leaderArea->addLayout(leaderRow);

    auto *options = new QHBoxLayout;
    options->setSpacing(18);
    m_alignYaw = new QCheckBox(tr("Align follower yaw"), this);
    m_alignYaw->setObjectName(QStringLiteral("formationAlignYaw"));
    m_alignYaw->setChecked(false);
    m_aimGimbals = new QCheckBox(tr("Aim gimbals instead"), this);
    m_aimGimbals->setObjectName(QStringLiteral("formationAimGimbals"));
    m_planeAttitude = new QCheckBox(
        tr("Enable ArduPlane attitude/PID"), this);
    m_planeAttitude->setObjectName(
        QStringLiteral("FormationPlaneAttitudeCheckBox"));
    for (QCheckBox *control : {m_alignYaw, m_aimGimbals}) {
        control->setEnabled(false);
        control->setToolTip(tr(kPositionOnlyReason));
        control->setAccessibleDescription(tr(kPositionOnlyReason));
    }
    m_planeAttitude->setEnabled(false);
    m_planeAttitude->setToolTip(tr(kPlaneReason));
    m_planeAttitude->setAccessibleDescription(tr(kPlaneReason));
    options->addWidget(m_alignYaw);
    options->addWidget(m_aimGimbals);
    options->addWidget(m_planeAttitude);
    options->addStretch(1);
    leaderArea->addLayout(options);
    root->addLayout(leaderArea);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("formationMainSplitter"));
    splitter->setChildrenCollapsible(false);
    m_grid = new FormationGridControl(splitter);
    m_grid->setObjectName(QStringLiteral("FormationGrid"));
    m_table = new QTableWidget(splitter);
    m_table->setObjectName(QStringLiteral("FormationVehicleGrid"));
    m_table->setColumnCount(FormationColumnCount);
    m_table->setHorizontalHeaderLabels({tr("Use"), tr("Endpoint"),
        tr("Sys"), tr("Comp"), tr("Role"), tr("X m"), tr("Y m"),
        tr("Z m"), tr("Firmware"), tr("Live status")});
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setColumnWidth(UseColumn, 50);
    m_table->setColumnWidth(EndpointColumn, 150);
    m_table->setColumnWidth(SystemColumn, 42);
    m_table->setColumnWidth(ComponentColumn, 48);
    m_table->setColumnWidth(RoleColumn, 130);
    m_table->setColumnWidth(XColumn, 65);
    m_table->setColumnWidth(YColumn, 65);
    m_table->setColumnWidth(ZColumn, 65);
    m_table->setColumnWidth(FirmwareColumn, 95);
    m_table->horizontalHeader()->setSectionResizeMode(
        LiveStatusColumn, QHeaderView::Stretch);
    splitter->addWidget(m_grid);
    splitter->addWidget(m_table);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 7);
    root->addWidget(splitter, 1);

    auto *commands = new QHBoxLayout;
    commands->setSpacing(8);
    m_runButton = button(tr("Start Formation"),
        QStringLiteral("FormationRunButton"), this);
    m_runButton->setMinimumWidth(130);
    commands->addWidget(m_runButton);
    const struct { const char *text; const char *name; } bulkButtons[] = {
        {"Arm Followers", "formationArmFollowers"},
        {"Disarm Followers", "formationDisarmFollowers"},
        {"GUIDED", "formationGuidedFollowers"},
        {"AUTO", "formationAutoFollowers"},
        {"Take Off", "formationTakeoffFollowers"}
    };
    for (const auto &definition : bulkButtons) {
        QPushButton *control = button(
            tr(definition.text), QString::fromLatin1(definition.name), this);
        setUnavailable(control, tr(kBulkReason));
        commands->addWidget(control);
    }
    m_takeoffAltitude = new QDoubleSpinBox(this);
    m_takeoffAltitude->setObjectName(QStringLiteral("formationTakeoffAltitude"));
    m_takeoffAltitude->setRange(1.0, 10000.0);
    m_takeoffAltitude->setSingleStep(1.0);
    m_takeoffAltitude->setDecimals(1);
    m_takeoffAltitude->setValue(5.0);
    m_takeoffAltitude->setSuffix(tr(" m"));
    m_takeoffAltitude->setToolTip(tr("Takeoff altitude in metres"));
    m_takeoffAltitude->setMinimumWidth(125);
    m_takeoffAltitude->setEnabled(false);
    m_takeoffAltitude->setToolTip(tr(kBulkReason));
    m_takeoffAltitude->setAccessibleDescription(tr(kBulkReason));
    commands->addWidget(m_takeoffAltitude);
    QPushButton *land = button(tr("LAND"),
        QStringLiteral("formationLandFollowers"), this);
    setUnavailable(land, tr(kBulkReason));
    commands->addWidget(land);
    commands->addStretch(1);
    root->addLayout(commands);

    QLabel *unavailable = label(tr(
        "Available now: exact Copter/Rover position + velocity formation "
        "stream. Not yet ported: Plane attitude/PID, yaw/gimbal alignment, "
        "and follower arm, mode, takeoff or land commands."),
        QStringLiteral("formationUnavailableHint"), this);
    unavailable->setWordWrap(true);
    root->addWidget(unavailable);

    auto *footer = new QHBoxLayout;
    m_status = label(tr(
        "Select a leader, explicitly enable followers, then capture or edit "
        "their offsets."), QStringLiteral("formationStatus"), this);
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    footer->addWidget(button(tr("Close"),
        QStringLiteral("formationClose"), this));
    root->addLayout(footer);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(500);
    m_runTimer = new QTimer(this);
    m_runTimer->setInterval(100);
    m_runTimer->setTimerType(Qt::PreciseTimer);
}

void SwarmFormationWindow::connectUi()
{
    connect(m_leaderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SwarmFormationWindow::leaderChanged);
    connect(findChild<QPushButton *>(QStringLiteral("formationRefreshVehicles")),
            &QPushButton::clicked, this,
            QOverload<>::of(&SwarmFormationWindow::refreshVehicles));
    connect(findChild<QPushButton *>(QStringLiteral("formationCaptureOffsets")),
            &QPushButton::clicked, this, &SwarmFormationWindow::captureOffsets);
    connect(m_table, &QTableWidget::cellChanged,
            this, &SwarmFormationWindow::tableCellChanged);
    connect(m_grid, &FormationGridControl::itemDragged,
            this, &SwarmFormationWindow::canvasItemDragged);
    connect(m_runButton, &QPushButton::clicked,
            this, &SwarmFormationWindow::toggleRun);
    connect(findChild<QPushButton *>(QStringLiteral("formationClose")),
            &QPushButton::clicked, this, &QWidget::close);
    connect(m_statusTimer, &QTimer::timeout,
            this, &SwarmFormationWindow::updateLiveStatus);
    connect(m_runTimer, &QTimer::timeout,
            this, &SwarmFormationWindow::runTick);

    if (m_registry) {
        connect(m_registry, &SwarmTelemetryRegistry::endpointActivated,
                this, [this](const SwarmTelemetrySnapshot &) {
            refreshVehicles(m_rows.isEmpty() && !m_running, false);
        });
        connect(m_registry, &SwarmTelemetryRegistry::endpointUpdated,
                this, [this](const SwarmTelemetrySnapshot &snapshot) {
            const int row = rowForLease(snapshot.lease);
            if (row < 0) {
                refreshVehicles(false, false);
                return;
            }
            const SwarmVehicleFamily oldFamily =
                SwarmFormationCore::family(m_rows.at(row).snapshot);
            m_rows[row].snapshot = snapshot;
            if (oldFamily != SwarmFormationCore::family(snapshot)) {
                refreshVehicles(false, false);
            }
        });
        connect(m_registry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](const SwarmVehicleInstanceLease &lease,
                             SwarmTelemetryRegistry::RetirementReason) {
            if (m_running) {
                if (sameVehicle(lease, m_activePlan.leader)) {
                    stopFormation(tr(
                        "Formation stopped: the leader was retired."));
                } else {
                    for (const SwarmFormationFollower &follower
                         : m_activePlan.followers) {
                        if (sameVehicle(lease, follower.lease)) {
                            stopFormation(tr(
                                "Formation stopped: a follower was retired."));
                            break;
                        }
                    }
                }
            }
            refreshVehicles(false, false);
        });
        connect(m_registry, &QObject::destroyed, this, [this]() {
            stopFormation(tr(
                "Formation stopped: swarm telemetry is unavailable."));
            m_registry = nullptr;
            m_rows.clear();
            m_leader = SwarmVehicleInstanceLease();
            rebuildVehicleWidgets();
        });
    }
}

QVector<SwarmTelemetrySnapshot>
SwarmFormationWindow::discoverVehicles() const
{
    QVector<SwarmTelemetrySnapshot> result;
    if (!m_registry) {
        return result;
    }
    const QList<VehicleEndpoint> endpoints = m_registry->endpoints();
    result.reserve(endpoints.size());
    for (const VehicleEndpoint &endpoint : endpoints) {
        SwarmTelemetrySnapshot snapshot;
        if (m_registry->acquireSnapshot(endpoint, &snapshot)) {
            result.append(snapshot);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const SwarmTelemetrySnapshot &left,
                 const SwarmTelemetrySnapshot &right) {
        return left.lease.endpoint < right.lease.endpoint;
    });
    return result;
}

void SwarmFormationWindow::refreshVehicles()
{
    refreshVehicles(true, true);
}

void SwarmFormationWindow::refreshVehicles(bool explicitlyRequested,
                                           bool stopRunning)
{
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    if (stopRunning && m_running) {
        stopFormation(tr(
            "Formation stopped because the vehicle list was refreshed."));
    }

    const QVector<SwarmTelemetrySnapshot> discovered = discoverVehicles();
    const QVector<VehicleRow> previous = m_rows;
    QVector<VehicleRow> next;
    next.reserve(discovered.size());
    for (const SwarmTelemetrySnapshot &snapshot : discovered) {
        VehicleRow row;
        row.lease = snapshot.lease;
        row.snapshot = snapshot;
        for (const VehicleRow &old : previous) {
            if (sameVehicle(old.lease, row.lease)) {
                row.offset = old.offset;
                row.included = old.included;
                break;
            }
        }
        if (!SwarmFormationCore::supportsFormation(snapshot)
            || (!sameVehicle(row.lease, m_leader)
                && !SwarmFormationCore::supportsPositionFollower(snapshot))) {
            row.included = false;
        }
        next.append(row);
    }
    m_rows = next;

    const bool hadLeader = m_leader.isValid();
    bool replacedLeader = false;
    int leaderRow = rowForLease(m_leader);
    if (leaderRow < 0
        || !SwarmFormationCore::supportsFormation(
            m_rows.at(leaderRow).snapshot)) {
        replacedLeader = hadLeader;
        if (replacedLeader) {
            // Existing offsets are expressed in the retired leader's frame.
            // Do not silently reinterpret them around a replacement leader.
            for (VehicleRow &row : m_rows) {
                row.included = false;
                row.offset = SwarmFormationOffset();
            }
        }
        m_leader = SwarmVehicleInstanceLease();
        for (VehicleRow &row : m_rows) {
            if (SwarmFormationCore::supportsFormation(row.snapshot)) {
                m_leader = row.lease;
                row.included = true;
                break;
            }
        }
    }
    rebuildVehicleWidgets();

    if (explicitlyRequested) {
        int eligible = 0;
        QSet<int> links;
        for (const VehicleRow &row : m_rows) {
            if (SwarmFormationCore::supportsFormation(row.snapshot)) {
                ++eligible;
                links.insert(row.lease.endpoint.linkId);
            }
        }
        setStatus(eligible == 0
            ? tr("No supported Plane/Copter/Rover autopilots were found across open MAVLink links.")
            : tr("Found %1 supported autopilot(s) across %2 link(s). Enable followers explicitly before starting.")
                .arg(eligible).arg(links.size()));
    }
    if (replacedLeader) {
        setStatus(m_leader.isValid()
            ? tr("The previous leader is unavailable. A replacement leader was selected, all follower offsets were cleared, and followers were disabled; capture or edit offsets before starting.")
            : tr("The previous leader is unavailable. Follower offsets were cleared and followers were disabled."));
    }
    m_refreshing = false;
}

int SwarmFormationWindow::rowForLease(
    const SwarmVehicleInstanceLease &lease) const
{
    for (int index = 0; index < m_rows.size(); ++index) {
        if (sameVehicle(m_rows.at(index).lease, lease)) {
            return index;
        }
    }
    return -1;
}

int SwarmFormationWindow::rowForKey(const QString &key) const
{
    for (int index = 0; index < m_rows.size(); ++index) {
        if (vehicleKey(m_rows.at(index).lease) == key) {
            return index;
        }
    }
    return -1;
}

bool SwarmFormationWindow::isLeader(const VehicleRow &row) const
{
    return sameVehicle(row.lease, m_leader);
}

void SwarmFormationWindow::rebuildVehicleWidgets()
{
    m_loading = true;
    rebuildLeaderCombo();
    rebuildTable();
    syncCanvas();
    m_loading = false;
}

void SwarmFormationWindow::rebuildLeaderCombo()
{
    const QSignalBlocker blocker(m_leaderCombo);
    m_leaderCombo->clear();
    int selected = -1;
    for (int index = 0; index < m_rows.size(); ++index) {
        const VehicleRow &row = m_rows.at(index);
        if (!SwarmFormationCore::supportsFormation(row.snapshot)) {
            continue;
        }
        const int comboIndex = m_leaderCombo->count();
        m_leaderCombo->addItem(vehicleLabel(row.lease),
                               QVariant::fromValue(row.lease));
        if (isLeader(row)) {
            selected = comboIndex;
        }
    }
    m_leaderCombo->setCurrentIndex(selected);
}

void SwarmFormationWindow::rebuildTable()
{
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(m_rows.size());
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        const VehicleRow &row = m_rows.at(rowIndex);
        auto *use = new QTableWidgetItem;
        Qt::ItemFlags useFlags = Qt::ItemIsSelectable;
        const bool followerEligible =
            SwarmFormationCore::supportsPositionFollower(row.snapshot);
        if (isLeader(row) || followerEligible) {
            useFlags |= Qt::ItemIsEnabled;
        }
        if (followerEligible && !isLeader(row)) {
            useFlags |= Qt::ItemIsUserCheckable;
        }
        use->setFlags(useFlags);
        use->setCheckState(isLeader(row) || row.included
            ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(rowIndex, UseColumn, use);

        const QString endpoint = row.lease.endpoint.linkName.trimmed().isEmpty()
            ? tr("Link %1").arg(row.lease.endpoint.linkId)
            : row.lease.endpoint.linkName.trimmed();
        const QString role = isLeader(row) ? tr("Leader")
            : followerEligible ? tr("Follower")
            : SwarmFormationCore::isPlane(row.snapshot)
                ? tr("Leader only (Plane PID not ported)")
                : tr("Unsupported (disabled)");
        const QStringList values = {endpoint,
            QString::number(row.lease.endpoint.systemId),
            QString::number(row.lease.endpoint.componentId), role,
            number(row.offset.x), number(row.offset.y), number(row.offset.z),
            familyLabel(row.snapshot), liveStatus(row)};
        for (int offset = 0; offset < values.size(); ++offset) {
            const int column = EndpointColumn + offset;
            auto *cell = new QTableWidgetItem(values.at(offset));
            if (column == EndpointColumn) {
                cell->setData(Qt::UserRole, QVariant::fromValue(row.lease));
            }
            const bool editable = followerEligible && !isLeader(row)
                && (column == XColumn || column == YColumn
                    || column == ZColumn);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (editable) {
                flags |= Qt::ItemIsEditable;
            }
            cell->setFlags(flags);
            m_table->setItem(rowIndex, column, cell);
        }
    }
}

void SwarmFormationWindow::syncCanvas()
{
    QVector<FormationGridItem> items;
    items.reserve(m_rows.size());
    for (const VehicleRow &row : m_rows) {
        FormationGridItem item;
        item.instanceKey = vehicleKey(row.lease);
        item.systemId = row.lease.endpoint.systemId;
        item.componentId = row.lease.endpoint.componentId;
        item.x = row.offset.x;
        item.y = row.offset.y;
        item.z = row.offset.z;
        item.included = row.included;
        item.eligible = SwarmFormationCore::supportsFormation(row.snapshot);
        item.leader = isLeader(row);
        items.append(item);
    }
    m_grid->setItems(items);
}

QString SwarmFormationWindow::liveStatus(const VehicleRow &row) const
{
    if (!m_registry || !m_registry->validateLease(row.lease)) {
        return tr("Telemetry stale");
    }
    if (!SwarmFormationCore::supportsFormation(row.snapshot)) {
        return tr("Unsupported formation firmware");
    }
    if (!row.snapshot.positionValid
        || !m_registry->observationIsFresh(
            row.snapshot.positionObservedMs,
            SwarmFormationCore::MaximumTelemetryAgeMs)) {
        return tr("Mode %1; no position").arg(row.snapshot.customMode);
    }
    return tr("%1; Mode %2; %3; GPS n/a")
        .arg(SwarmFormationCore::isPlane(row.snapshot)
                 ? tr("Plane leader only") : tr("Position/velocity"))
        .arg(row.snapshot.customMode)
        .arg(row.snapshot.armed ? tr("armed") : tr("disarmed"));
}

void SwarmFormationWindow::updateLiveStatus()
{
    if (!m_registry) {
        return;
    }
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        SwarmTelemetrySnapshot snapshot;
        if (m_registry->snapshotForLease(m_rows.at(rowIndex).lease,
                                         &snapshot)) {
            m_rows[rowIndex].snapshot = snapshot;
        }
        QTableWidgetItem *cell = m_table->item(rowIndex, LiveStatusColumn);
        if (cell) {
            cell->setText(liveStatus(m_rows.at(rowIndex)));
        }
    }
}

void SwarmFormationWindow::leaderChanged(int index)
{
    if (m_loading || index < 0 || index >= m_leaderCombo->count()) {
        return;
    }
    const SwarmVehicleInstanceLease next = qvariant_cast<
        SwarmVehicleInstanceLease>(m_leaderCombo->itemData(index));
    if (!next.isValid() || sameVehicle(next, m_leader)) {
        return;
    }
    const int nextRow = rowForLease(next);
    if (nextRow < 0 || !SwarmFormationCore::supportsFormation(
            m_rows.at(nextRow).snapshot)) {
        return;
    }
    stopFormation(tr("Formation stopped because the leader changed."));
    const SwarmFormationOffset origin = m_rows.at(nextRow).offset;
    const int oldRow = rowForLease(m_leader);
    for (VehicleRow &row : m_rows) {
        row.offset.x -= origin.x;
        row.offset.y -= origin.y;
        row.offset.z -= origin.z;
    }
    if (oldRow >= 0) {
        m_rows[oldRow].included = false;
    }
    m_leader = next;
    m_rows[nextRow].included = true;
    rebuildVehicleWidgets();
}

void SwarmFormationWindow::tableCellChanged(int rowIndex, int column)
{
    if (m_loading || rowIndex < 0 || rowIndex >= m_rows.size()) {
        return;
    }
    VehicleRow &row = m_rows[rowIndex];
    if (column == UseColumn) {
        const bool included = m_table->item(rowIndex, column)->checkState()
            == Qt::Checked;
        if (row.included != included) {
            stopFormation(tr(
                "Formation stopped because a follower selection changed."));
            row.included = included;
            rebuildTable();
            syncCanvas();
        }
        return;
    }
    if (column < XColumn || column > ZColumn) {
        return;
    }
    bool valid = false;
    const double value = m_table->item(rowIndex, column)->text().toDouble(&valid);
    double roundedValue = std::nearbyint(value * 10.0) / 10.0;
    if (qFuzzyIsNull(roundedValue)) {
        roundedValue = 0.0;
    }
    SwarmFormationOffset candidate = row.offset;
    if (column == XColumn) {
        candidate.x = roundedValue;
    } else if (column == YColumn) {
        candidate.y = roundedValue;
    } else {
        candidate.z = roundedValue;
    }
    if (!valid || !SwarmFormationCore::isSafeOffset(candidate)) {
        setStatus(tr("The formation offset is non-numeric or exceeds the safety limit."));
        rebuildTable();
        return;
    }
    if (candidate.x == row.offset.x && candidate.y == row.offset.y
        && candidate.z == row.offset.z) {
        return;
    }
    stopFormation(tr("Formation stopped because an offset changed."));
    row.offset = candidate;
    rebuildTable();
    syncCanvas();
}

void SwarmFormationWindow::canvasItemDragged(
    const QString &key, double x, double y)
{
    const int rowIndex = rowForKey(key);
    if (rowIndex < 0) {
        return;
    }
    SwarmFormationOffset candidate = m_rows.at(rowIndex).offset;
    candidate.x = x;
    candidate.y = y;
    if (!SwarmFormationCore::isSafeOffset(candidate)) {
        syncCanvas();
        return;
    }
    stopFormation(tr("Formation stopped because an offset changed."));
    m_rows[rowIndex].offset = candidate;
    rebuildTable();
    syncCanvas();
}

void SwarmFormationWindow::captureOffsets()
{
    stopFormation(tr("Formation stopped before capturing live offsets."));
    if (!m_registry || !m_leader.isValid()) {
        setStatus(tr("Choose a leader before capturing offsets."));
        return;
    }
    SwarmTelemetrySnapshot leader;
    if (!m_registry->snapshotForLease(m_leader, &leader)
        || !m_registry->validateLease(m_leader)
        || !m_registry->observationIsFresh(
            leader.positionObservedMs,
            SwarmFormationCore::MaximumTelemetryAgeMs)
        || !m_registry->observationIsFresh(
            leader.attitudeObservedMs,
            SwarmFormationCore::MaximumTelemetryAgeMs)) {
        setStatus(tr(
            "Cannot capture offsets: leader position or attitude is unavailable."));
        return;
    }

    int updated = 0;
    int skipped = 0;
    for (VehicleRow &row : m_rows) {
        if (isLeader(row) || !row.included
            || !SwarmFormationCore::supportsFormation(row.snapshot)) {
            continue;
        }
        SwarmTelemetrySnapshot follower;
        if (!m_registry->snapshotForLease(row.lease, &follower)
            || !m_registry->validateLease(row.lease)
            || !m_registry->observationIsFresh(
                follower.positionObservedMs,
                SwarmFormationCore::MaximumTelemetryAgeMs)) {
            ++skipped;
            continue;
        }
        SwarmFormationOffset offset;
        QString error;
        if (!SwarmFormationCore::tryOffsetFromLeader(
                leader, follower, registryTimestamp(), &offset, &error)
            || std::abs(offset.x) > 200.0 || std::abs(offset.y) > 200.0) {
            ++skipped;
            continue;
        }
        offset.x = std::nearbyint(offset.x * 10.0) / 10.0;
        offset.y = std::nearbyint(offset.y * 10.0) / 10.0;
        offset.z = std::nearbyint(offset.z * 10.0) / 10.0;
        row.offset = offset;
        row.snapshot = follower;
        ++updated;
    }
    rebuildTable();
    syncCanvas();
    setStatus(tr("Captured %1 follower offset(s) from live positions%2")
        .arg(updated)
        .arg(skipped == 0 ? QStringLiteral(".")
                          : tr("; skipped %1 missing or >200 m target(s).")
                              .arg(skipped)));
}

bool SwarmFormationWindow::tryBuildPlan(
    SwarmFormationPlan *plan, QList<SwarmTelemetrySnapshot> *snapshots,
    QVector<SwarmCommandMember> *members, QString *error) const
{
    if (plan) {
        *plan = SwarmFormationPlan();
    }
    if (snapshots) {
        snapshots->clear();
    }
    if (members) {
        members->clear();
    }
    if (!plan || !snapshots || !members || !error || !m_registry
        || !m_leader.isValid()) {
        if (error) {
            *error = tr("Select a live autopilot as formation leader.");
        }
        return false;
    }

    const int leaderRow = rowForLease(m_leader);
    if (leaderRow < 0
        || !SwarmFormationCore::supportsFormation(
            m_rows.at(leaderRow).snapshot)) {
        *error = tr("Select a supported live autopilot as formation leader.");
        return false;
    }
    plan->leader = m_leader;
    // The controls remain visible for Mission Planner recognisability, but the
    // exact yaw/gimbal sender is deliberately unavailable in this slice.
    plan->alignYaw = false;
    plan->aimGimbals = false;
    for (const VehicleRow &row : m_rows) {
        if (!row.included || isLeader(row)
            || !SwarmFormationCore::supportsFormation(row.snapshot)) {
            continue;
        }
        if (SwarmFormationCore::isPlane(row.snapshot)) {
            *error = tr(
                "Follower %1 is ArduPlane. Attitude/PID commands are not yet ported.")
                .arg(vehicleLabel(row.lease));
            return false;
        }
        if (!SwarmFormationCore::isSafeOffset(row.offset)) {
            *error = tr("Follower %1 has an invalid or excessive offset.")
                .arg(vehicleLabel(row.lease));
            return false;
        }
        plan->followers.append({row.lease, row.offset});
    }
    if (plan->followers.isEmpty()) {
        *error = tr("Explicitly enable at least one Copter or Rover follower.");
        return false;
    }

    SwarmVehicleGroupLease group;
    group.members.append(plan->leader);
    for (const SwarmFormationFollower &follower : plan->followers) {
        group.members.append(follower.lease);
    }
    if (!m_registry->validateGroup(
            group, snapshots, SwarmFormationCore::MaximumTelemetryAgeMs)) {
        *error = tr(
            "A formation vehicle disappeared, expired, or was reconnected.");
        return false;
    }

    SwarmCommandMember leader;
    leader.slotId = kLeaderSlot;
    leader.lease = plan->leader;
    leader.required.fields = SwarmTelemetryRequirements::Position
        | SwarmTelemetryRequirements::Velocity
        | SwarmTelemetryRequirements::Attitude;
    leader.required.heartbeatMaximumAgeMs =
        SwarmFormationCore::MaximumTelemetryAgeMs;
    leader.required.positionMaximumAgeMs =
        SwarmFormationCore::MaximumTelemetryAgeMs;
    leader.required.velocityMaximumAgeMs =
        SwarmFormationCore::MaximumTelemetryAgeMs;
    leader.required.attitudeMaximumAgeMs =
        SwarmFormationCore::MaximumTelemetryAgeMs;
    members->append(leader);
    for (int index = 0; index < plan->followers.size(); ++index) {
        SwarmCommandMember follower;
        follower.slotId = kFirstFollowerSlot + index;
        follower.lease = plan->followers.at(index).lease;
        follower.required.fields = plan->alignYaw
            ? SwarmTelemetryRequirements::Fields(
                SwarmTelemetryRequirements::Attitude)
            : SwarmTelemetryRequirements::Fields(
                SwarmTelemetryRequirements::NoFields);
        follower.required.heartbeatMaximumAgeMs =
            SwarmFormationCore::MaximumTelemetryAgeMs;
        follower.required.attitudeMaximumAgeMs =
            SwarmFormationCore::MaximumTelemetryAgeMs;
        members->append(follower);
    }
    error->clear();
    return true;
}

qint64 SwarmFormationWindow::registryTimestamp() const
{
    return m_registry ? m_registry->observationClockNowMs() : -1;
}

QString SwarmFormationWindow::confirmationTargets(
    const SwarmFormationPlan &plan) const
{
    QStringList lines;
    for (const SwarmFormationFollower &follower : plan.followers) {
        lines.append(tr("• %1 — X %2 m, Y %3 m, Z %4 m; position/velocity")
            .arg(vehicleLabel(follower.lease))
            .arg(number(follower.offset.x))
            .arg(number(follower.offset.y))
            .arg(number(follower.offset.z)));
    }
    return lines.join(QLatin1Char('\n'));
}

void SwarmFormationWindow::toggleRun()
{
    if (m_running) {
        stopFormation(tr("Formation stopped by operator."));
        return;
    }
    if (!m_commands) {
        setStatus(tr("The exact swarm command service is unavailable."));
        return;
    }

    SwarmFormationPlan plan;
    QList<SwarmTelemetrySnapshot> snapshots;
    QVector<SwarmCommandMember> members;
    QString error;
    if (!tryBuildPlan(&plan, &snapshots, &members, &error)) {
        setStatus(error);
        return;
    }
    const bool accepted = m_dependencies.confirmDangerous
        && m_dependencies.confirmDangerous(
            this, tr("Start Formation Flight"),
            tr("BETA / USE AT OWN RISK. APM Planner 3.0 will continuously "
               "send exact position and velocity targets at 10 Hz to these "
               "followers:\n\n%1\n\nFollower yaw/gimbal and Plane attitude "
               "commands are not included in this build. Verify flight "
               "modes, coordinate frame, altitude reference and clear "
               "airspace. Cancel is the default action.")
                .arg(confirmationTargets(plan)),
            tr("START FORMATION"));
    if (!accepted) {
        setStatus(tr("Formation start cancelled."));
        return;
    }
    if (!tryBuildPlan(&plan, &snapshots, &members, &error)) {
        setStatus(tr("Formation changed while confirmation was open. %1")
            .arg(error));
        return;
    }

    SwarmCommandSessionToken token;
    const SwarmCommandService::Result reserved = m_commands->reserve(
        this, members, kFormationRateHz, &token, &error);
    if (reserved != SwarmCommandService::Result::Reserved
        || !token.isValid()) {
        setStatus(error.isEmpty()
            ? tr("The exact swarm command session could not be reserved.")
            : error);
        return;
    }
    m_session = token;
    m_activePlan = plan;

    QVector<int> streamSlots = {kLeaderSlot};
    if (plan.alignYaw) {
        for (int index = 0; index < plan.followers.size(); ++index) {
            streamSlots.append(kFirstFollowerSlot + index);
        }
    }
    const SwarmCommandService::BatchReport streams =
        m_commands->requestStreams(m_session, streamSlots, kFormationRateHz);
    if (!streams.allSent()) {
        stopFormation(tr("Formation could not start: %1")
            .arg(streams.detail));
        return;
    }

    m_bootstrapping = true;
    m_bootstrapTimer.restart();
    setRunning(true);
    setStatus(tr(
        "Waiting for fresh leader position, velocity and attitude telemetry…"));
    runTick();
    if (m_running) {
        m_runTimer->start();
    }
}

QVector<SwarmPositionTarget> SwarmFormationWindow::positionTargets(
    const SwarmFormationTick &tick) const
{
    QVector<SwarmPositionTarget> targets;
    targets.reserve(tick.commands.size());
    for (const SwarmFormationCommand &command : tick.commands) {
        int followerIndex = -1;
        for (int index = 0; index < m_activePlan.followers.size(); ++index) {
            if (sameVehicle(command.follower,
                            m_activePlan.followers.at(index).lease)) {
                followerIndex = index;
                break;
            }
        }
        if (followerIndex < 0) {
            return {};
        }
        SwarmPositionTarget target;
        target.slotId = kFirstFollowerSlot + followerIndex;
        target.latitudeDegrees = command.target.latitudeDegrees;
        target.longitudeDegrees = command.target.longitudeDegrees;
        target.relativeAltitudeM = static_cast<float>(
            command.target.relativeAltitudeM);
        target.velocityNorthMps = static_cast<float>(
            command.target.velocityNorthMps);
        target.velocityEastMps = static_cast<float>(
            command.target.velocityEastMps);
        target.velocityDownMps = static_cast<float>(
            command.target.velocityDownMps);
        target.useVelocity = true;
        targets.append(target);
    }
    return targets;
}

void SwarmFormationWindow::runTick()
{
    if (!m_running || !m_registry || !m_commands
        || !m_session.isValid()) {
        return;
    }
    SwarmVehicleGroupLease group;
    group.members.append(m_activePlan.leader);
    for (const SwarmFormationFollower &follower : m_activePlan.followers) {
        group.members.append(follower.lease);
    }
    QList<SwarmTelemetrySnapshot> snapshots;
    if (!m_registry->validateGroup(
            group, &snapshots,
            SwarmFormationCore::MaximumTelemetryAgeMs)) {
        stopFormation(tr(
            "Formation stopped: a reserved vehicle disappeared, expired, or was reconnected."));
        return;
    }
    const SwarmFormationTick tick = SwarmFormationCore::buildTick(
        m_activePlan, snapshots, registryTimestamp());
    if (!tick.isValid()) {
        if (m_bootstrapping && m_bootstrapTimer.isValid()
            && m_bootstrapTimer.elapsed() < kTelemetryBootstrapTimeoutMs) {
            setStatus(tr(
                "Waiting for fresh leader position, velocity and attitude telemetry…"));
            return;
        }
        stopFormation(tick.error);
        return;
    }
    const QVector<SwarmPositionTarget> targets = positionTargets(tick);
    if (targets.size() != m_activePlan.followers.size()) {
        stopFormation(tr(
            "Formation stopped: generated targets no longer match the reserved followers."));
        return;
    }
    const SwarmCommandService::BatchReport sent =
        m_commands->sendPositionTargets(m_session, targets);
    if (!m_running) {
        return;
    }
    if (sent.result == SwarmCommandService::Result::RateLimited) {
        return;
    }
    if (!sent.allSent()) {
        stopFormation(tr("Formation stopped: %1").arg(sent.detail));
        return;
    }
    m_bootstrapping = false;
    m_bootstrapTimer.invalidate();
    setStatus(tr(
        "Formation active: position/velocity targets queued for %1 follower(s). "
        "Yaw/gimbal alignment remains unavailable.")
        .arg(targets.size()));
}

void SwarmFormationWindow::stopFormation(const QString &reason)
{
    m_runTimer->stop();
    const SwarmCommandSessionToken token = m_session;
    m_session = SwarmCommandSessionToken();
    m_activePlan = SwarmFormationPlan();
    m_bootstrapping = false;
    m_bootstrapTimer.invalidate();
    if (token.isValid() && m_commands) {
        m_commands->release(token);
    }
    setRunning(false);
    if (!reason.isEmpty()) {
        setStatus(reason);
    }
}

void SwarmFormationWindow::handleSessionCancelled(
    quint64 sessionId, const QString &reason)
{
    if (!m_session.isValid() || m_session.id != sessionId) {
        return;
    }
    m_runTimer->stop();
    m_session = SwarmCommandSessionToken();
    m_activePlan = SwarmFormationPlan();
    m_bootstrapping = false;
    m_bootstrapTimer.invalidate();
    setRunning(false);
    setStatus(reason.trimmed().isEmpty()
        ? tr("Formation stopped because the exact command session ended.")
        : reason);
}

void SwarmFormationWindow::setRunning(bool running)
{
    if (m_running == running) {
        m_runButton->setText(running ? tr("Stop Formation")
                                     : tr("Start Formation"));
        return;
    }
    m_running = running;
    m_runButton->setText(running ? tr("Stop Formation")
                                 : tr("Start Formation"));
    emit runningChanged(running);
}

void SwarmFormationWindow::setStatus(const QString &status)
{
    if (m_status) {
        m_status->setText(status);
    }
}

void SwarmFormationWindow::setUnavailable(
    QPushButton *control, const QString &reason)
{
    if (!control) {
        return;
    }
    control->setEnabled(false);
    control->setToolTip(reason);
    control->setStatusTip(reason);
    control->setAccessibleDescription(reason);
}

void SwarmFormationWindow::closeEvent(QCloseEvent *event)
{
    stopFormation(tr("Formation stopped because the window closed."));
    if (m_commands) {
        m_commands->setSessionCancelledHandler(
            SwarmFormationCommandInterface::SessionCancelledHandler());
    }
    QWidget::closeEvent(event);
}
