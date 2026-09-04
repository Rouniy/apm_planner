#include "SwarmWaypointLeaderWindow.h"

#include "WaypointLeaderProfileControl.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace
{
enum WaypointLeaderColumn {
    UseColumn = 0,
    OrderColumn,
    EndpointColumn,
    SystemColumn,
    ComponentColumn,
    RoleColumn,
    FirmwareColumn,
    LiveStatusColumn,
    MissionPositionColumn,
    CommandedTargetColumn,
    WaypointLeaderColumnCount
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

QString endpointLabel(const SwarmVehicleInstanceLease &lease)
{
    return lease.endpoint.linkName.trimmed().isEmpty()
        ? QObject::tr("Link %1").arg(lease.endpoint.linkId)
        : lease.endpoint.linkName.trimmed();
}

QString fallbackVehicleLabel(const SwarmVehicleInstanceLease &lease)
{
    return QStringLiteral("%1 — %2:%3")
        .arg(endpointLabel(lease))
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId);
}

bool sameSettings(const SwarmWaypointLeaderSettings &left,
                  const SwarmWaypointLeaderSettings &right)
{
    return left.separationM == right.separationM
        && left.leadM == right.leadM
        && left.offPathTriggerM == right.offPathTriggerM
        && left.takeoffLandAltitudeSeparationM
            == right.takeoffLandAltitudeSeparationM
        && left.navigationAccelerationMps2
            == right.navigationAccelerationMps2
        && left.vFormation == right.vFormation
        && left.altitudeInterleave == right.altitudeInterleave;
}

bool samePlan(const SwarmWaypointLeaderPlan &left,
              const SwarmWaypointLeaderPlan &right)
{
    if (!sameVehicle(left.groundMaster, right.groundMaster)
        || !sameVehicle(left.airMaster, right.airMaster)
        || !sameSettings(left.settings, right.settings)
        || left.missionSignature != right.missionSignature
        || left.missionContentGeneration != right.missionContentGeneration
        || left.missionContentDigest != right.missionContentDigest
        || left.followers.size() != right.followers.size()) {
        return false;
    }
    for (int index = 0; index < left.followers.size(); ++index) {
        const SwarmWaypointLeaderFollower &a = left.followers.at(index);
        const SwarmWaypointLeaderFollower &b = right.followers.at(index);
        if (a.order != b.order || !sameVehicle(a.lease, b.lease)) {
            return false;
        }
    }
    return true;
}

bool defaultConfirmation(QWidget *owner, const QString &title,
                         const QString &text, const QString &acceptText)
{
    QMessageBox box(QMessageBox::Warning, title, text,
                    QMessageBox::NoButton, owner);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    QPushButton *accept = box.addButton(acceptText, QMessageBox::AcceptRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == accept;
}
} // namespace

class SwarmWaypointLeaderWindow::Implementation
{
public:
    struct VehicleRow
    {
        SwarmWaypointLeaderWindowVehicle vehicle;
        bool included = false;
        int order = 0;
    };

    struct MissionRefreshState
    {
        quint64 localGeneration = 0;
        SwarmVehicleInstanceLease airMaster;
        quint64 baselineObservationRevision = 0;
        bool completionArmed = false;
        bool requestOutstanding = false;

        bool isActive() const noexcept
        {
            return localGeneration != 0 && airMaster.isValid();
        }
    };

    Implementation(SwarmWaypointLeaderWindow *window,
                   SwarmWaypointLeaderWindowInterface *windowInterface,
                   Dependencies deps, QWidget *owner)
        : q(window)
        , interface(windowInterface)
        , dependencies(std::move(deps))
    {
        if (!dependencies.confirmDangerous) {
            dependencies.confirmDangerous = defaultConfirmation;
        }
        buildUi(owner);
        connectUi();
        if (interface) {
            interface->setChangedHandler([guard = QPointer<
                                              SwarmWaypointLeaderWindow>(q)]() {
                if (guard && guard->m_impl) {
                    guard->m_impl->interfaceChanged();
                }
            });
            interfaceDestroyedConnection = QObject::connect(
                interface.data(), &QObject::destroyed, q,
                [this]() { interfaceDestroyed(); });
        }
        refreshFromInterface(true);
    }

    ~Implementation()
    {
        QObject::disconnect(interfaceDestroyedConnection);
        if (interface) {
            interface->setChangedHandler(
                SwarmWaypointLeaderWindowInterface::ChangedHandler());
        }
        prepareClose();
    }

    void buildUi(QWidget *owner)
    {
        q->setObjectName(QStringLiteral("SwarmWaypointLeaderWindow"));
        q->setWindowTitle(q->tr("Swarm Waypoint Leader (Beta)"));
        q->setWindowModality(Qt::NonModal);
        q->resize(WindowWidth, WindowHeight);
        q->setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
        q->setStyleSheet(QStringLiteral(
            "QWidget#SwarmWaypointLeaderWindow { background: #303233; color: #dddddd; }"
            "QLabel#waypointLeaderDangerBanner { background: #5a2b20; color: #ffd4b8; "
            "border: 1px solid #ff9b66; border-radius: 4px; padding: 10px; }"
            "QLabel#waypointLeaderStatus, QLabel#waypointLeaderMode { color: #73c7ff; }"
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
            "BETA / USE AT OWN RISK — port of official Mission Planner "
            "WaypointLeader. The ground master is observed only. The air "
            "master and explicitly enabled ArduCopter followers can be "
            "switched to GUIDED, armed, taken off and commanded at 10 Hz; "
            "RTL_ALT and WPNAV_ACCEL/WP_ACC are changed when available. A "
            "stale/replaced member, changed mission, invalid role or incomplete "
            "group validation stops the complete batch."),
            QStringLiteral("waypointLeaderDangerBanner"), q);
        danger->setWordWrap(true);
        root->addWidget(danger);

        auto *masterRow = new QHBoxLayout;
        masterRow->setSpacing(8);
        masterRow->addWidget(label(q->tr("Ground master"), QString(), q));
        groundCombo = new QComboBox(q);
        groundCombo->setObjectName(
            QStringLiteral("WaypointLeaderGroundMasterCombo"));
        groundCombo->setMinimumWidth(300);
        masterRow->addWidget(groundCombo);
        masterRow->addWidget(label(q->tr("Air master"), QString(), q));
        airCombo = new QComboBox(q);
        airCombo->setObjectName(
            QStringLiteral("WaypointLeaderAirMasterCombo"));
        airCombo->setMinimumWidth(300);
        masterRow->addWidget(airCombo);
        masterRow->addWidget(button(q->tr("Refresh Vehicles"),
            QStringLiteral("waypointLeaderRefreshVehicles"), q));
        masterRow->addWidget(button(q->tr("Refresh Mission"),
            QStringLiteral("waypointLeaderRefreshMission"), q));
        missionStatus = label(q->tr("No air-master mission selected."),
                              QStringLiteral("waypointLeaderMissionStatus"), q);
        missionStatus->setWordWrap(true);
        masterRow->addWidget(missionStatus, 1);
        root->addLayout(masterRow);

        auto *settingsRow = new QHBoxLayout;
        settingsRow->setSpacing(6);
        settingsRow->addWidget(label(q->tr("Spacing m"), QString(), q));
        separation = numeric(
            QStringLiteral("waypointLeaderSeparation"),
            SwarmWaypointLeaderCore::MinimumSeparationM,
            SwarmWaypointLeaderCore::MaximumSeparationM, 5.0, 1.0);
        settingsRow->addWidget(separation);
        settingsRow->addWidget(label(q->tr("Lead m"), QString(), q));
        lead = numeric(QStringLiteral("waypointLeaderLead"),
                       SwarmWaypointLeaderCore::MinimumLeadM,
                       SwarmWaypointLeaderCore::MaximumLeadM, 20.0, 1.0);
        settingsRow->addWidget(lead);
        settingsRow->addWidget(label(q->tr("Off path m"), QString(), q));
        offPath = numeric(QStringLiteral("waypointLeaderOffPath"),
                          SwarmWaypointLeaderCore::MinimumOffPathTriggerM,
                          SwarmWaypointLeaderCore::MaximumOffPathTriggerM,
                          10.0, 1.0);
        settingsRow->addWidget(offPath);
        settingsRow->addWidget(label(q->tr("Alt sep m"), QString(), q));
        altitudeSeparation = numeric(
            QStringLiteral("waypointLeaderAltitudeSeparation"),
            SwarmWaypointLeaderCore::MinimumAltitudeSeparationM,
            SwarmWaypointLeaderCore::MaximumAltitudeSeparationM,
            2.0, 1.0);
        settingsRow->addWidget(altitudeSeparation);
        settingsRow->addWidget(label(q->tr("Nav accel m/s²"), QString(), q));
        navigationAcceleration = numeric(
            QStringLiteral("waypointLeaderNavigationAcceleration"),
            SwarmWaypointLeaderCore::MinimumNavigationAccelerationMps2,
            SwarmWaypointLeaderCore::MaximumNavigationAccelerationMps2,
            1.0, 0.1);
        settingsRow->addWidget(navigationAcceleration);
        vFormation = new QCheckBox(q->tr("V formation"), q);
        vFormation->setObjectName(QStringLiteral("waypointLeaderVFormation"));
        settingsRow->addWidget(vFormation);
        altitudeInterleave = new QCheckBox(q->tr("Altitude interleave"), q);
        altitudeInterleave->setObjectName(
            QStringLiteral("waypointLeaderAltitudeInterleave"));
        settingsRow->addWidget(altitudeInterleave);
        settingsRow->addStretch(1);
        settingsRow->addWidget(label(q->tr("Mode:"), QString(), q));
        mode = label(SwarmWaypointLeaderCore::modeName(
                         SwarmWaypointLeaderMode::Idle),
                     QStringLiteral("waypointLeaderMode"), q);
        settingsRow->addWidget(mode);
        root->addLayout(settingsRow);

        splitter = new QSplitter(Qt::Horizontal, q);
        splitter->setObjectName(QStringLiteral("waypointLeaderMainSplitter"));
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(6);

        table = new QTableWidget;
        table->setObjectName(QStringLiteral("WaypointLeaderVehicleGrid"));
        table->setColumnCount(WaypointLeaderColumnCount);
        table->setHorizontalHeaderLabels({q->tr("Use"), q->tr("Order"),
            q->tr("Endpoint"), q->tr("Sys"), q->tr("Comp"),
            q->tr("Role"), q->tr("Firmware"), q->tr("Live"),
            q->tr("Mission position"), q->tr("Commanded target")});
        table->verticalHeader()->hide();
        table->setAlternatingRowColors(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setColumnWidth(UseColumn, 48);
        table->setColumnWidth(OrderColumn, 58);
        table->setColumnWidth(EndpointColumn, 140);
        table->setColumnWidth(SystemColumn, 42);
        table->setColumnWidth(ComponentColumn, 48);
        table->setColumnWidth(RoleColumn, 125);
        table->setColumnWidth(FirmwareColumn, 90);
        table->setColumnWidth(LiveStatusColumn, 155);
        table->setColumnWidth(MissionPositionColumn, 130);
        table->horizontalHeader()->setSectionResizeMode(
            CommandedTargetColumn, QHeaderView::Stretch);

        auto *profileFrame = new QFrame;
        profileFrame->setObjectName(QStringLiteral("waypointLeaderProfileFrame"));
        profileFrame->setFrameShape(QFrame::StyledPanel);
        auto *profileLayout = new QVBoxLayout(profileFrame);
        profileLayout->setContentsMargins(1, 1, 1, 1);
        profile = new WaypointLeaderProfileControl(profileFrame);
        profile->setObjectName(QStringLiteral("WaypointLeaderProfile"));
        profileLayout->addWidget(profile);

        splitter->addWidget(table);
        splitter->addWidget(profileFrame);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        splitter->setSizes({750, 500});
        root->addWidget(splitter, 1);

        auto *commandRow = new QHBoxLayout;
        commandRow->setSpacing(8);
        startButton = button(q->tr("Start Waypoint Leader"),
            QStringLiteral("WaypointLeaderRunButton"), q);
        startButton->setMinimumWidth(180);
        commandRow->addWidget(startButton);
        stopButton = button(q->tr("Stop Waypoint Leader"),
            QStringLiteral("WaypointLeaderStopButton"), q);
        commandRow->addWidget(stopButton);
        resetButton = button(q->tr("Reset State"),
            QStringLiteral("waypointLeaderResetState"), q);
        commandRow->addWidget(resetButton);
        returnButton = button(q->tr("Return Along Mission"),
            QStringLiteral("waypointLeaderReturnAlongMission"), q);
        commandRow->addWidget(returnButton);
        abandonButton = button(q->tr("Abandon → Altitudes + RTL"),
            QStringLiteral("waypointLeaderAbandonMission"), q);
        commandRow->addWidget(abandonButton);
        QLabel *stopHint = label(q->tr(
            "Stop only stops this GCS command stream; use the explicit "
            "return/RTL actions when required."),
            QStringLiteral("waypointLeaderStopHint"), q);
        stopHint->setWordWrap(true);
        commandRow->addWidget(stopHint, 1);
        root->addLayout(commandRow);

        auto *footer = new QHBoxLayout;
        status = label(q->tr(
            "Select distinct ground and air masters, then explicitly enable "
            "Copter followers."),
            QStringLiteral("waypointLeaderStatus"), q);
        status->setWordWrap(true);
        footer->addWidget(status, 1);
        QPushButton *closeButton = button(q->tr("Close"),
            QStringLiteral("waypointLeaderClose"), q);
        closeButton->setMinimumWidth(90);
        footer->addWidget(closeButton);
        root->addLayout(footer);
    }

    QDoubleSpinBox *numeric(const QString &objectName, double minimum,
                            double maximum, double value, double step)
    {
        auto *control = new QDoubleSpinBox(q);
        control->setObjectName(objectName);
        control->setRange(minimum, maximum);
        control->setSingleStep(step);
        control->setDecimals(1);
        control->setValue(value);
        control->setMinimumWidth(76);
        return control;
    }

    void connectUi()
    {
        QObject::connect(groundCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), q,
            [this](int) { masterChanged(true); });
        QObject::connect(airCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), q,
            [this](int) { masterChanged(false); });
        QObject::connect(q->findChild<QPushButton *>(
                QStringLiteral("waypointLeaderRefreshVehicles")),
            &QPushButton::clicked, q,
            &SwarmWaypointLeaderWindow::refreshVehicles);
        QObject::connect(q->findChild<QPushButton *>(
                QStringLiteral("waypointLeaderRefreshMission")),
            &QPushButton::clicked, q,
            &SwarmWaypointLeaderWindow::refreshMission);
        QObject::connect(table, &QTableWidget::cellChanged, q,
            [this](int row, int column) { tableCellChanged(row, column); });

        for (QDoubleSpinBox *control : {separation, lead, offPath,
                                       altitudeSeparation,
                                       navigationAcceleration}) {
            QObject::connect(control,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged), q,
                [this](double) { settingsChanged(); });
        }
        QObject::connect(vFormation, &QCheckBox::toggled, q,
                         [this](bool) { settingsChanged(); });
        QObject::connect(altitudeInterleave, &QCheckBox::toggled, q,
                         [this](bool) { settingsChanged(); });
        QObject::connect(startButton, &QPushButton::clicked, q,
                         [this]() { start(); });
        QObject::connect(stopButton, &QPushButton::clicked, q,
                         [this]() {
            cancelActive(q->tr(
                "Waypoint Leader stopped by operator; no further targets are sent."));
        });
        QObject::connect(resetButton, &QPushButton::clicked, q,
                         [this]() {
            requestMode(SwarmWaypointLeaderMode::Idle,
                q->tr("Reset Waypoint Leader State"),
                q->tr("RESET AND RESTART"),
                q->tr("This resets the official state machine. On the next "
                      "10 Hz tick it can write navigation parameters again, "
                      "enter GUIDED, arm and start staged takeoff for every "
                      "named air vehicle."));
        });
        QObject::connect(returnButton, &QPushButton::clicked, q,
                         [this]() {
            requestMode(SwarmWaypointLeaderMode::ReturnAlongMission,
                q->tr("Return Waypoint Leader Formation"),
                q->tr("RETURN ALONG MISSION"),
                q->tr("This stops following the ground master and sends every "
                      "named air vehicle back along the air-master mission, "
                      "followed by separated-altitude RTL."));
        });
        QObject::connect(abandonButton, &QPushButton::clicked, q,
                         [this]() {
            requestMode(SwarmWaypointLeaderMode::LandAltitude,
                q->tr("Abandon Waypoint Leader Mission"),
                q->tr("ESTABLISH ALTITUDES AND RTL"),
                q->tr("This abandons path following, commands separated "
                      "landing altitudes, then switches every named air "
                      "vehicle to RTL."));
        });
        QObject::connect(q->findChild<QPushButton *>(
                QStringLiteral("waypointLeaderClose")),
            &QPushButton::clicked, q, &QWidget::close);
    }

    int rowForLease(const SwarmVehicleInstanceLease &lease) const
    {
        for (int index = 0; index < rows.size(); ++index) {
            if (sameVehicle(rows.at(index).vehicle.lease, lease)) {
                return index;
            }
        }
        return -1;
    }

    bool groundSelected(const VehicleRow &row) const
    {
        return sameVehicle(row.vehicle.lease, groundMaster);
    }

    bool airSelected(const VehicleRow &row) const
    {
        return sameVehicle(row.vehicle.lease, airMaster);
    }

    bool followerCandidate(const VehicleRow &row) const
    {
        return row.vehicle.flightEligible
            && !groundSelected(row) && !airSelected(row);
    }

    void refreshFromInterface(bool explicitStatus)
    {
        if (refreshing) {
            return;
        }
        refreshing = true;
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const SwarmVehicleInstanceLease previousGround = groundMaster;
        const SwarmVehicleInstanceLease previousAir = airMaster;
        const QVector<VehicleRow> previousRows = rows;
        rows.clear();

        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        if (currentInterface) {
            const QVector<SwarmWaypointLeaderWindowVehicle> discovered =
                currentInterface->vehicles();
            if (!windowGuard) {
                return;
            }
            if (!interface || interface.data() != currentInterface.data()) {
                refreshing = false;
                return;
            }
            for (const SwarmWaypointLeaderWindowVehicle &vehicle : discovered) {
                if (!vehicle.lease.isValid() || rows.size() >= 24
                    || rowForLease(vehicle.lease) >= 0) {
                    continue;
                }
                VehicleRow next;
                next.vehicle = vehicle;
                if (next.vehicle.label.trimmed().isEmpty()) {
                    next.vehicle.label = fallbackVehicleLabel(vehicle.lease);
                }
                for (const VehicleRow &old : previousRows) {
                    if (sameVehicle(old.vehicle.lease, vehicle.lease)) {
                        next.included = old.included
                            && vehicle.flightEligible;
                        next.order = old.order;
                        break;
                    }
                }
                rows.append(next);
            }
        }

        groundMaster = SwarmVehicleInstanceLease();
        airMaster = SwarmVehicleInstanceLease();
        const int oldGround = rowForLease(previousGround);
        if (oldGround >= 0 && rows.at(oldGround).vehicle.groundEligible) {
            groundMaster = previousGround;
        }
        const int oldAir = rowForLease(previousAir);
        if (oldAir >= 0 && rows.at(oldAir).vehicle.flightEligible) {
            airMaster = previousAir;
        }
        if (!groundMaster.isValid()) {
            for (const VehicleRow &row : rows) {
                if (row.vehicle.groundEligible) {
                    groundMaster = row.vehicle.lease;
                    break;
                }
            }
        }
        if (!airMaster.isValid()) {
            for (const VehicleRow &row : rows) {
                if (row.vehicle.flightEligible
                    && !sameVehicle(row.vehicle.lease, groundMaster)) {
                    airMaster = row.vehicle.lease;
                    break;
                }
            }
        }
        if (!airMaster.isValid()) {
            for (const VehicleRow &row : rows) {
                if (row.vehicle.flightEligible) {
                    airMaster = row.vehicle.lease;
                    break;
                }
            }
        }
        const bool masterWasReplaced =
            (previousGround.isValid()
             && !sameVehicle(previousGround, groundMaster))
            || (previousAir.isValid()
                && !sameVehicle(previousAir, airMaster));
        const bool airWasReplaced = previousAir.isValid()
            && !sameVehicle(previousAir, airMaster);
        if (airWasReplaced) {
            cancelMissionRefreshFor(previousAir, q->tr(
                "The exact air master changed before its mission refresh completed."));
            if (!windowGuard) {
                return;
            }
            observedMissionRevision = 0;
        }
        if (masterWasReplaced) {
            for (VehicleRow &row : rows) {
                row.included = false;
                row.order = 0;
            }
        }
        clearMasterFollowerState();
        assignMissingOrders();
        rebuildVehicleWidgets();
        loadMission();
        if (!windowGuard) {
            return;
        }
        updateProfileMarkers();
        refreshing = false;

        if (explicitStatus) {
            if (!interface) {
                setStatus(q->tr(
                    "The exact Waypoint Leader executor is unavailable; all flight actions are locked."));
            } else if (masterWasReplaced) {
                setStatus(q->tr(
                    "A previous master exact instance is unavailable. Replacement roles were selected where possible and every follower was disabled; review the complete group."));
            } else {
                int flightCount = 0;
                QSet<int> links;
                for (const VehicleRow &row : rows) {
                    links.insert(row.vehicle.lease.endpoint.linkId);
                    if (row.vehicle.flightEligible) {
                        ++flightCount;
                    }
                }
                setStatus(flightCount == 0
                    ? q->tr(
                        "No live Waypoint Leader ArduCopter autopilots were found across open MAVLink links.")
                    : q->tr(
                        "Found %1 Waypoint Leader Copter(s) across %2 link(s). Select distinct masters and explicitly enable ordered followers.")
                        .arg(flightCount).arg(links.size()));
            }
        }
        syncRuntime(false);
        updateActions();
    }

    void clearMasterFollowerState()
    {
        for (VehicleRow &row : rows) {
            if (groundSelected(row) || airSelected(row)
                || !row.vehicle.flightEligible) {
                row.included = false;
            }
            if (groundSelected(row) || airSelected(row)) {
                row.order = 0;
            }
        }
    }

    void assignMissingOrders()
    {
        QSet<int> used;
        for (const VehicleRow &row : rows) {
            if (followerCandidate(row) && row.order > 0
                && row.order <= SwarmWaypointLeaderCore::MaximumOrder) {
                used.insert(row.order);
            }
        }
        int next = 1;
        for (VehicleRow &row : rows) {
            if (!followerCandidate(row) || row.order > 0) {
                continue;
            }
            while (used.contains(next)
                   && next <= SwarmWaypointLeaderCore::MaximumOrder) {
                ++next;
            }
            row.order = next <= SwarmWaypointLeaderCore::MaximumOrder
                ? next : 0;
            used.insert(row.order);
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
            if (row.vehicle.groundEligible) {
                const int index = groundCombo->count();
                groundCombo->addItem(row.vehicle.label,
                    QVariant::fromValue(row.vehicle.lease));
                if (groundSelected(row)) {
                    selectedGround = index;
                }
            }
            if (row.vehicle.flightEligible) {
                const int index = airCombo->count();
                airCombo->addItem(row.vehicle.label,
                    QVariant::fromValue(row.vehicle.lease));
                if (airSelected(row)) {
                    selectedAir = index;
                }
            }
        }
        groundCombo->setCurrentIndex(selectedGround);
        airCombo->setCurrentIndex(selectedAir);
    }

    QString roleText(const VehicleRow &row) const
    {
        if (groundSelected(row) && airSelected(row)) {
            return q->tr("Invalid: both masters");
        }
        if (groundSelected(row)) {
            return q->tr("Ground master (observed)");
        }
        if (airSelected(row)) {
            return q->tr("Air master");
        }
        if (row.vehicle.flightEligible) {
            return q->tr("Follower candidate");
        }
        return row.vehicle.groundEligible
            ? q->tr("Ground master only") : q->tr("Component");
    }

    void rebuildTable()
    {
        const QSignalBlocker blocker(table);
        table->setRowCount(rows.size());
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const VehicleRow &row = rows.at(rowIndex);
            const bool follower = followerCandidate(row);

            auto *use = new QTableWidgetItem;
            Qt::ItemFlags useFlags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (follower) {
                useFlags |= Qt::ItemIsUserCheckable;
            }
            use->setFlags(useFlags);
            use->setCheckState(row.included ? Qt::Checked : Qt::Unchecked);
            if (!follower) {
                use->setToolTip(groundSelected(row) || airSelected(row)
                    ? q->tr(
                        "Master roles are selected above and cannot also be followers.")
                    : q->tr(
                        "Only an executor-eligible ArduCopter exact instance can be a follower."));
            }
            table->setItem(rowIndex, UseColumn, use);

            auto *order = new QTableWidgetItem(
                follower ? QString::number(row.order) : QStringLiteral("—"));
            Qt::ItemFlags orderFlags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (follower) {
                orderFlags |= Qt::ItemIsEditable;
            }
            order->setFlags(orderFlags);
            table->setItem(rowIndex, OrderColumn, order);

            const QStringList values = {
                endpointLabel(row.vehicle.lease),
                QString::number(row.vehicle.lease.endpoint.systemId),
                QString::number(row.vehicle.lease.endpoint.componentId),
                roleText(row), row.vehicle.firmware,
                row.vehicle.liveStatus.trimmed().isEmpty()
                    ? q->tr("Waiting for telemetry") : row.vehicle.liveStatus,
                row.vehicle.missionPosition.trimmed().isEmpty()
                    ? QStringLiteral("—") : row.vehicle.missionPosition,
                row.vehicle.commandedTarget.trimmed().isEmpty()
                    ? QStringLiteral("—") : row.vehicle.commandedTarget
            };
            for (int offset = 0; offset < values.size(); ++offset) {
                const int column = EndpointColumn + offset;
                auto *cell = new QTableWidgetItem(values.at(offset));
                cell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                if (column == EndpointColumn) {
                    cell->setData(Qt::UserRole,
                                  QVariant::fromValue(row.vehicle.lease));
                    cell->setToolTip(row.vehicle.label);
                }
                table->setItem(rowIndex, column, cell);
            }
        }
    }

    bool missionRefreshMatches(
        quint64 generation,
        const SwarmVehicleInstanceLease &vehicle) const noexcept
    {
        return missionRefresh.isActive()
            && missionRefresh.localGeneration == generation
            && sameVehicle(missionRefresh.airMaster, vehicle);
    }

    void clearMissionPresentation(const QString &text)
    {
        missionValid = false;
        mission = SwarmWaypointLeaderMissionSnapshot();
        missionPath = SwarmWaypointLeaderMissionPath();
        profile->setProfile({});
        profile->setVehicleMarkers({});
        missionStatus->setText(text);
    }

    void rejectMissionObservation(quint64 generation, quint64 revision)
    {
        if (!missionRefreshMatches(generation, airMaster)) {
            return;
        }
        missionRefresh.baselineObservationRevision = std::max(
            missionRefresh.baselineObservationRevision, revision);
        missionRefresh.requestOutstanding = false;
    }

    void loadMission()
    {
        if (loadingMission) {
            return;
        }
        loadingMission = true;
        clearMissionPresentation(QString());
        if (!interface) {
            observedMissionRevision = 0;
            missionStatus->setText(q->tr(
                "The exact mission service is unavailable."));
            loadingMission = false;
            return;
        }
        if (!airMaster.isValid()) {
            observedMissionRevision = 0;
            missionStatus->setText(q->tr(
                "No air-master mission selected."));
            loadingMission = false;
            return;
        }

        const SwarmVehicleInstanceLease selectedAir = airMaster;
        const MissionRefreshState expectedRefresh = missionRefresh;
        const bool refreshGated = expectedRefresh.isActive();
        if (refreshGated
            && !sameVehicle(expectedRefresh.airMaster, selectedAir)) {
            missionStatus->setText(q->tr(
                "The selected exact air master changed while its mission refresh was pending."));
            loadingMission = false;
            return;
        }
        if (refreshGated && !expectedRefresh.completionArmed) {
            missionStatus->setText(q->tr(
                "Preparing an exact air-master mission refresh; the previous snapshot is hidden."));
            loadingMission = false;
            return;
        }

        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        const quint64 observationRevision =
            currentInterface->missionObservationRevision(selectedAir);
        if (!windowGuard) {
            return;
        }
        if (!interface || interface.data() != currentInterface.data()
            || !sameVehicle(airMaster, selectedAir)
            || (refreshGated
                && !missionRefreshMatches(
                    expectedRefresh.localGeneration, selectedAir))) {
            loadingMission = false;
            return;
        }
        observedMissionRevision = observationRevision;
        if (refreshGated
            && observationRevision
                <= expectedRefresh.baselineObservationRevision) {
            missionStatus->setText(q->tr(
                "Waiting for a new complete observation of the exact air-master mission; the previous snapshot remains hidden."));
            loadingMission = false;
            return;
        }
        if (observationRevision == 0) {
            missionStatus->setText(q->tr(
                "No successful exact air-master mission observation is available."));
            loadingMission = false;
            return;
        }

        QString error;
        SwarmWaypointLeaderMissionSnapshot snapshot;
        const bool available = currentInterface->missionForAirMaster(
            selectedAir, &snapshot, &error);
        if (!windowGuard) {
            return;
        }
        if (!interface || interface.data() != currentInterface.data()
            || !sameVehicle(airMaster, selectedAir)
            || (refreshGated
                && !missionRefreshMatches(
                    expectedRefresh.localGeneration, selectedAir))) {
            loadingMission = false;
            return;
        }
        if (!available) {
            if (refreshGated) {
                rejectMissionObservation(
                    expectedRefresh.localGeneration, observationRevision);
            }
            missionStatus->setText(error.trimmed().isEmpty()
                ? q->tr("No exact air-master mission is available.") : error);
            loadingMission = false;
            return;
        }
        if (!sameVehicle(snapshot.airMaster, selectedAir)) {
            if (refreshGated) {
                rejectMissionObservation(
                    expectedRefresh.localGeneration, observationRevision);
            }
            missionStatus->setText(q->tr(
                "The mission snapshot does not belong to the selected exact air master."));
            loadingMission = false;
            return;
        }
        if (snapshot.missionType != MAV_MISSION_TYPE_MISSION
            || snapshot.contentGeneration == 0
            || snapshot.contentDigest.isEmpty()) {
            if (refreshGated) {
                rejectMissionObservation(
                    expectedRefresh.localGeneration, observationRevision);
            }
            missionStatus->setText(q->tr(
                "The air-master mission lacks exact type, generation or digest identity."));
            loadingMission = false;
            return;
        }
        SwarmWaypointLeaderMissionPath path;
        if (!SwarmWaypointLeaderMissionPath::build(snapshot, &path, &error)) {
            if (refreshGated) {
                rejectMissionObservation(
                    expectedRefresh.localGeneration, observationRevision);
            }
            missionStatus->setText(error);
            loadingMission = false;
            return;
        }

        if (refreshGated) {
            missionRefresh = MissionRefreshState();
        }
        mission = snapshot;
        missionPath = path;
        missionValid = true;
        profile->setProfile(path.profile());
        missionStatus->setText(q->tr(
            "Air-master mission: %1 vertex/vertices, %2 m, signature %3.")
            .arg(path.profile().size())
            .arg(path.lengthM(), 0, 'f', 1)
            .arg(path.signature().left(8)));
        loadingMission = false;
    }

    void updateProfileMarkers()
    {
        if (!missionValid) {
            profile->setVehicleMarkers({});
            return;
        }
        QVector<WaypointLeaderVehicleMarker> markers;
        markers.reserve(rows.size());
        for (const VehicleRow &row : rows) {
            if (!row.vehicle.profilePositionValid
                || !std::isfinite(row.vehicle.pathDistanceM)
                || !std::isfinite(row.vehicle.relativeAltitudeM)) {
                continue;
            }
            WaypointLeaderVehicleRole role =
                WaypointLeaderVehicleRole::Follower;
            if (groundSelected(row)) {
                role = WaypointLeaderVehicleRole::GroundMaster;
            } else if (airSelected(row)) {
                role = WaypointLeaderVehicleRole::AirMaster;
            }
            markers.append({role, row.vehicle.label,
                            row.vehicle.pathDistanceM,
                            row.vehicle.relativeAltitudeM});
        }
        profile->setVehicleMarkers(markers);
    }

    SwarmWaypointLeaderSettings settings() const
    {
        SwarmWaypointLeaderSettings result;
        result.separationM = separation->value();
        result.leadM = lead->value();
        result.offPathTriggerM = offPath->value();
        result.takeoffLandAltitudeSeparationM =
            altitudeSeparation->value();
        result.navigationAccelerationMps2 =
            navigationAcceleration->value();
        result.vFormation = vFormation->isChecked();
        result.altitudeInterleave = altitudeInterleave->isChecked();
        return result;
    }

    bool tryBuildPlan(SwarmWaypointLeaderPlan *plan, QString *error) const
    {
        if (plan) {
            *plan = SwarmWaypointLeaderPlan();
        }
        if (error) {
            error->clear();
        }
        auto fail = [error](const QString &detail) {
            if (error) {
                *error = detail;
            }
            return false;
        };
        if (!plan) {
            return fail(q->tr("The Waypoint Leader plan output is unavailable."));
        }
        if (closing) {
            return fail(q->tr("The Waypoint Leader window is closing."));
        }
        if (!interface) {
            return fail(q->tr(
                "The exact Waypoint Leader executor is unavailable."));
        }
        const int groundRow = rowForLease(groundMaster);
        if (groundRow < 0 || !rows.at(groundRow).vehicle.groundEligible) {
            return fail(q->tr("Select a live autopilot as ground master."));
        }
        const int airRow = rowForLease(airMaster);
        if (airRow < 0 || !rows.at(airRow).vehicle.flightEligible) {
            return fail(q->tr(
                "Select an executor-eligible ArduCopter as air master."));
        }
        if (sameVehicle(groundMaster, airMaster)) {
            return fail(q->tr(
                "Ground master and air master must be different exact vehicles."));
        }
        QString detail;
        const SwarmWaypointLeaderSettings currentSettings = settings();
        if (!SwarmWaypointLeaderCore::validateSettings(
                currentSettings, &detail)) {
            return fail(detail);
        }
        if (!missionValid || !sameVehicle(mission.airMaster, airMaster)) {
            return fail(q->tr(
                "A valid exact mission is required for the selected air master."));
        }

        QVector<SwarmWaypointLeaderFollower> followers;
        QSet<int> orders;
        for (const VehicleRow &row : rows) {
            if (!row.included) {
                continue;
            }
            if (!followerCandidate(row)) {
                return fail(q->tr(
                    "A selected follower is no longer an eligible exact ArduCopter instance."));
            }
            if (row.order < 1
                || row.order > SwarmWaypointLeaderCore::MaximumOrder
                || orders.contains(row.order)) {
                return fail(q->tr(
                    "Follower order must be unique and between 1 and %1.")
                    .arg(SwarmWaypointLeaderCore::MaximumOrder));
            }
            orders.insert(row.order);
            followers.append({row.vehicle.lease, row.order});
        }
        std::sort(followers.begin(), followers.end(),
                  [](const SwarmWaypointLeaderFollower &left,
                     const SwarmWaypointLeaderFollower &right) {
            return left.order < right.order;
        });

        plan->groundMaster = groundMaster;
        plan->airMaster = airMaster;
        plan->followers = followers;
        plan->settings = currentSettings;
        plan->missionSignature = missionPath.signature();
        plan->missionContentGeneration = mission.contentGeneration;
        plan->missionContentDigest = mission.contentDigest;
        if (!interface->executorReady(&detail)) {
            return fail(detail.trimmed().isEmpty()
                ? q->tr("The complete Waypoint Leader executor is not ready.")
                : detail);
        }
        if (!interface->validatePlan(*plan, &detail)) {
            return fail(detail.trimmed().isEmpty()
                ? q->tr("The exact Waypoint Leader plan is not currently valid.")
                : detail);
        }
        return true;
    }

    QString confirmationTargets(const SwarmWaypointLeaderPlan &plan) const
    {
        QStringList lines;
        const int airRow = rowForLease(plan.airMaster);
        lines.append(q->tr("• Air master %1")
            .arg(airRow >= 0 ? rows.at(airRow).vehicle.label
                             : fallbackVehicleLabel(plan.airMaster)));
        for (const SwarmWaypointLeaderFollower &follower : plan.followers) {
            const int row = rowForLease(follower.lease);
            lines.append(q->tr("• Follower #%1 %2")
                .arg(follower.order)
                .arg(row >= 0 ? rows.at(row).vehicle.label
                              : fallbackVehicleLabel(follower.lease)));
        }
        return lines.join(QLatin1Char('\n'));
    }

    void start()
    {
        if (closing) {
            return;
        }
        if (runtimeRunning()) {
            setStatus(q->tr("Waypoint Leader is already running."));
            return;
        }
        SwarmWaypointLeaderPlan plan;
        QString error;
        if (!tryBuildPlan(&plan, &error)) {
            setStatus(error);
            updateActions();
            return;
        }
        const SwarmWaypointLeaderPlan confirmedPlan = plan;
        const auto confirmDangerous = dependencies.confirmDangerous;
        const QString confirmationTitle =
            q->tr("Start Swarm Waypoint Leader");
        const QString confirmationText = q->tr(
                "BETA / USE AT OWN RISK. This official Mission Planner "
                "WaypointLeader workflow can persistently write RTL_ALT=0 "
                "(or RTL_ALT_M=0) and WPNAV_ACCEL/WP_ACC, switch the named "
                "air vehicles to GUIDED, arm, take off and command position "
                "targets at 10 Hz before issuing RTL. The ground master is "
                "observed only and is never commanded.\n\nCommanded exact "
                "air vehicles:\n%1\n\nVerify the downloaded air-master mission, "
                "relative-altitude reference, order, spacing and clear "
                "airspace. Cancel is the default action.")
                .arg(confirmationTargets(plan));
        const QString confirmationAccept =
            q->tr("START WAYPOINT LEADER");
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const bool accepted = confirmDangerous(
            windowGuard.data(), confirmationTitle, confirmationText,
            confirmationAccept);
        if (!windowGuard) {
            return;
        }
        if (closing) {
            return;
        }
        if (!accepted) {
            setStatus(q->tr("Waypoint Leader start cancelled."));
            return;
        }

        // Confirmation may run a nested event loop. Re-read discovery and the
        // exact mission, then validate through the executor a second time.
        refreshFromInterface(false);
        if (!windowGuard) {
            return;
        }
        if (!tryBuildPlan(&plan, &error)
            || !samePlan(confirmedPlan, plan)) {
            setStatus(q->tr(
                "Waypoint Leader plan changed while confirmation was open. %1")
                .arg(error.trimmed().isEmpty()
                    ? q->tr("Roles, order, settings or exact mission changed.")
                    : error));
            return;
        }

        activePlan = plan;
        cancellationIssued = false;
        if (!interface || !interface->start(plan, &error)) {
            activePlan.reset();
            setStatus(error.trimmed().isEmpty()
                ? q->tr("The exact Waypoint Leader executor rejected Start.")
                : error);
            syncRuntime(false);
            updateActions();
            return;
        }
        if (!interface || !interface->isRunning()) {
            activePlan.reset();
            setStatus(q->tr(
                "The executor returned without entering a running Waypoint Leader session."));
            syncRuntime(false);
            updateActions();
            return;
        }
        const QString executorStatus = interface->statusText().trimmed();
        setStatus(executorStatus.isEmpty()
            ? q->tr("Starting Waypoint Leader validation and staged takeoff…")
            : executorStatus);
        syncRuntime(false);
        updateActions();
    }

    void requestMode(SwarmWaypointLeaderMode requested,
                     const QString &title, const QString &acceptText,
                     const QString &warning)
    {
        if (closing) {
            return;
        }
        if (!interface || !runtimeRunning() || !activePlan) {
            setStatus(q->tr("Waypoint Leader is not running."));
            return;
        }
        const SwarmWaypointLeaderPlan confirmedPlan = *activePlan;
        const auto confirmDangerous = dependencies.confirmDangerous;
        const QString confirmationTitle = title;
        const QString confirmationText =
            warning + q->tr("\n\nAffected exact air vehicles:\n")
                + confirmationTargets(confirmedPlan)
                + q->tr(
                    "\n\nThe ground master is not commanded. Cancel is the default action.");
        const QString confirmationAccept = acceptText;
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const bool accepted = confirmDangerous(
            windowGuard.data(), confirmationTitle, confirmationText,
            confirmationAccept);
        if (!windowGuard) {
            return;
        }
        if (closing) {
            return;
        }
        if (!accepted) {
            setStatus(title + q->tr(" cancelled."));
            return;
        }
        refreshFromInterface(false);
        if (!windowGuard) {
            return;
        }
        SwarmWaypointLeaderPlan current;
        QString error;
        if (!runtimeRunning() || !tryBuildPlan(&current, &error)
            || !samePlan(confirmedPlan, current)) {
            setStatus(q->tr(
                "Waypoint Leader run changed while confirmation was open. %1")
                .arg(error));
            return;
        }
        if (!interface->requestMode(requested, &error)) {
            setStatus(error.trimmed().isEmpty()
                ? q->tr("The executor rejected the requested mode.") : error);
            return;
        }
        setStatus(title + q->tr(" requested."));
        syncRuntime(false);
    }

    void masterChanged(bool ground)
    {
        if (loading || refreshing) {
            return;
        }
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const SwarmVehicleInstanceLease previousAir = airMaster;
        const SwarmVehicleInstanceLease nextGround =
            qvariant_cast<SwarmVehicleInstanceLease>(
                groundCombo->currentData());
        const SwarmVehicleInstanceLease nextAir =
            qvariant_cast<SwarmVehicleInstanceLease>(
                airCombo->currentData());
        const bool airWasReplaced = previousAir.isValid()
            && !sameVehicle(previousAir, nextAir);
        if (airWasReplaced) {
            cancelMissionRefreshFor(previousAir, q->tr(
                "The exact air master was changed before its mission refresh completed."));
            if (!windowGuard) {
                return;
            }
            observedMissionRevision = 0;
        }
        cancelActive(q->tr("Waypoint Leader stopped because the %1 changed.")
            .arg(ground ? q->tr("ground master") : q->tr("air master")));
        if (!windowGuard) {
            return;
        }
        groundMaster = nextGround;
        airMaster = nextAir;
        clearMasterFollowerState();
        assignMissingOrders();
        rebuildTable();
        if (!ground) {
            loadMission();
            if (!windowGuard) {
                return;
            }
        }
        updateProfileMarkers();
        updateActions();
    }

    void tableCellChanged(int rowIndex, int column)
    {
        if (loading || refreshing || rowIndex < 0
            || rowIndex >= rows.size()
            || (column != UseColumn && column != OrderColumn)) {
            return;
        }
        VehicleRow &row = rows[rowIndex];
        if (!followerCandidate(row)) {
            rebuildTable();
            return;
        }
        cancelActive(q->tr(
            "Waypoint Leader stopped because a follower or order changed."));
        if (column == UseColumn) {
            row.included = table->item(rowIndex, UseColumn)->checkState()
                == Qt::Checked;
        } else {
            bool ok = false;
            const int order = table->item(rowIndex, OrderColumn)
                ->text().toInt(&ok);
            row.order = ok ? order : 0;
        }
        rebuildTable();
        updateProfileMarkers();
        updateActions();
    }

    void settingsChanged()
    {
        if (loading || refreshing) {
            return;
        }
        cancelActive(q->tr(
            "Waypoint Leader stopped because a flight setting changed."));
        updateActions();
    }

    void cancelMissionRefreshFor(
        const SwarmVehicleInstanceLease &vehicle,
        const QString &reason)
    {
        if (!missionRefresh.isActive()
            || !sameVehicle(missionRefresh.airMaster, vehicle)) {
            return;
        }
        const bool cancelAdapterRequest =
            missionRefresh.requestOutstanding;
        missionRefresh = MissionRefreshState();
        if (!cancelAdapterRequest || !interface) {
            return;
        }

        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        cancellingMissionRefresh = true;
        currentInterface->cancelMissionRefresh(vehicle, reason);
        if (!windowGuard) {
            return;
        }
        cancellingMissionRefresh = false;
    }

    void explicitVehicleRefresh()
    {
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        cancelActive(q->tr(
            "Waypoint Leader stopped because the vehicle list was refreshed."));
        if (!windowGuard) {
            return;
        }
        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        if (currentInterface) {
            currentInterface->refreshVehicles();
            if (!windowGuard) {
                return;
            }
        }
        refreshFromInterface(true);
    }

    void explicitMissionRefresh()
    {
        if (closing || !interface || !airMaster.isValid()) {
            setStatus(q->tr("Select an exact air master before refreshing its mission."));
            updateActions();
            return;
        }

        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        const SwarmVehicleInstanceLease requestedAir = airMaster;
        cancelMissionRefreshFor(requestedAir, q->tr(
            "A newer mission refresh superseded the previous request for this exact air master."));
        if (!windowGuard) {
            return;
        }
        if (!interface || interface.data() != currentInterface.data()
            || !sameVehicle(airMaster, requestedAir)) {
            return;
        }

        ++nextMissionRefreshGeneration;
        if (nextMissionRefreshGeneration == 0) {
            ++nextMissionRefreshGeneration;
        }
        missionRefresh.localGeneration = nextMissionRefreshGeneration;
        missionRefresh.airMaster = requestedAir;
        missionRefresh.baselineObservationRevision =
            observedMissionRevision;
        missionRefresh.completionArmed = false;
        missionRefresh.requestOutstanding = false;
        const quint64 localGeneration =
            missionRefresh.localGeneration;
        clearMissionPresentation(q->tr(
            "Preparing an exact air-master mission refresh; the previous snapshot is hidden."));
        updateActions();

        cancelActive(q->tr(
            "Waypoint Leader stopped because the air-master mission was refreshed."));
        if (!windowGuard) {
            return;
        }
        if (!interface || !sameVehicle(airMaster, requestedAir)
            || !missionRefreshMatches(localGeneration, requestedAir)) {
            return;
        }

        const quint64 adapterRevision =
            currentInterface->missionObservationRevision(requestedAir);
        if (!windowGuard) {
            return;
        }
        if (!interface || interface.data() != currentInterface.data()
            || !sameVehicle(airMaster, requestedAir)
            || !missionRefreshMatches(localGeneration, requestedAir)) {
            return;
        }
        missionRefresh.baselineObservationRevision = std::max(
            missionRefresh.baselineObservationRevision, adapterRevision);
        missionRefresh.completionArmed = true;
        missionRefresh.requestOutstanding = true;

        QString error;
        const bool requested =
            currentInterface->refreshMission(requestedAir, &error);
        if (!windowGuard) {
            return;
        }
        if (!interface || interface.data() != currentInterface.data()
            || !sameVehicle(airMaster, requestedAir)
            || !missionRefreshMatches(localGeneration, requestedAir)) {
            return;
        }
        if (!requested) {
            missionRefresh.requestOutstanding = false;
        }
        setStatus(requested
            ? q->tr("Air-master mission refresh requested for %1.")
                .arg(fallbackVehicleLabel(requestedAir))
            : (error.trimmed().isEmpty()
                ? q->tr("The exact mission refresh request was rejected.")
                : error));
        updateActions();
    }

    void interfaceChanged()
    {
        if (refreshing || loadingMission || cancelling
            || cancellingMissionRefresh || closing || !interface) {
            return;
        }
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const bool wasRunning = runtimeRunning();
        const std::optional<SwarmWaypointLeaderPlan> expected = activePlan;
        refreshFromInterface(false);
        if (!windowGuard) {
            return;
        }
        if (!interface) {
            activePlan.reset();
            syncRuntime(false);
            updateActions();
            return;
        }
        if (wasRunning && expected && runtimeRunning()) {
            SwarmWaypointLeaderPlan current;
            QString error;
            if (!tryBuildPlan(&current, &error)
                || !samePlan(*expected, current)) {
                cancelActive(q->tr(
                    "Waypoint Leader stopped because an exact vehicle, role or mission changed. %1")
                    .arg(error));
                return;
            }
        }
        if (!runtimeRunning()) {
            activePlan.reset();
        }
        const QString executorStatus = interface->statusText().trimmed();
        if (!executorStatus.isEmpty()) {
            setStatus(executorStatus);
        }
        syncRuntime(false);
        updateActions();
    }

    void interfaceDestroyed()
    {
        interface = nullptr;
        activePlan.reset();
        missionRefresh = MissionRefreshState();
        observedMissionRevision = 0;
        loadingMission = false;
        cancellingMissionRefresh = false;
        rows.clear();
        groundMaster = SwarmVehicleInstanceLease();
        airMaster = SwarmVehicleInstanceLease();
        rebuildVehicleWidgets();
        clearMissionPresentation(q->tr(
            "The exact mission service is unavailable."));
        setStatus(q->tr(
            "Waypoint Leader stopped: the exact executor was destroyed."));
        syncRuntime(false);
        updateActions();
    }

    void cancelActive(const QString &reason)
    {
        if (!interface || (!runtimeRunning() && !activePlan)) {
            return;
        }
        if (cancelling || cancellationIssued) {
            return;
        }
        cancelling = true;
        cancellationIssued = true;
        activePlan.reset();
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        const QPointer<SwarmWaypointLeaderWindowInterface> currentInterface =
            interface;
        currentInterface->cancelActiveRun(reason);
        if (!windowGuard) {
            return;
        }
        cancelling = false;
        setStatus(reason);
        syncRuntime(false);
        updateActions();
    }

    void prepareClose()
    {
        if (closing) {
            return;
        }
        closing = true;
        const QPointer<SwarmWaypointLeaderWindow> windowGuard(q);
        cancelActive(q->tr(
            "Waypoint Leader stopped because the window closed; no further targets are sent."));
        if (!windowGuard) {
            return;
        }
        if (missionRefresh.isActive()
            && sameVehicle(missionRefresh.airMaster, airMaster)) {
            cancelMissionRefreshFor(airMaster, q->tr(
                "The mission refresh was cancelled because the Waypoint Leader window closed."));
            if (!windowGuard) {
                return;
            }
        }
        updateActions();
    }

    bool runtimeRunning() const noexcept
    {
        return interface && interface->isRunning();
    }

    void syncRuntime(bool useExecutorStatus)
    {
        const bool nextRunning = runtimeRunning();
        if (displayedRunning != nextRunning) {
            displayedRunning = nextRunning;
            emit q->runningChanged(displayedRunning);
        }
        mode->setText(interface
            ? SwarmWaypointLeaderCore::modeName(interface->mode())
            : SwarmWaypointLeaderCore::modeName(
                SwarmWaypointLeaderMode::Idle));
        if (useExecutorStatus && interface) {
            const QString executorStatus = interface->statusText().trimmed();
            if (!executorStatus.isEmpty()) {
                setStatus(executorStatus);
            }
        }
    }

    void updateActions()
    {
        const bool running = runtimeRunning();
        SwarmWaypointLeaderPlan candidate;
        QString reason;
        const bool canStart = !closing && !running
            && tryBuildPlan(&candidate, &reason);
        startButton->setEnabled(canStart);
        startButton->setToolTip(canStart ? QString() : reason);
        startButton->setAccessibleDescription(startButton->toolTip());
        stopButton->setEnabled(!closing && running);
        bool canRequestMode = false;
        if (!closing && running && interface && activePlan && missionValid
            && interface->executorReady(&reason)
            && interface->validatePlan(*activePlan, &reason)) {
            canRequestMode = true;
        }
        for (QPushButton *control : {resetButton, returnButton,
                                     abandonButton}) {
            control->setEnabled(canRequestMode);
            control->setToolTip(canRequestMode ? QString() : reason);
            control->setAccessibleDescription(control->toolTip());
        }
        const bool canRefreshMission = !closing && interface
            && airMaster.isValid();
        q->findChild<QPushButton *>(
            QStringLiteral("waypointLeaderRefreshVehicles"))
            ->setEnabled(!closing && !interface.isNull());
        q->findChild<QPushButton *>(
            QStringLiteral("waypointLeaderRefreshMission"))
            ->setEnabled(canRefreshMission);
    }

    void setStatus(const QString &text)
    {
        status->setText(text);
    }

    SwarmWaypointLeaderWindow *q = nullptr;
    QPointer<SwarmWaypointLeaderWindowInterface> interface;
    Dependencies dependencies;
    QVector<VehicleRow> rows;
    SwarmVehicleInstanceLease groundMaster;
    SwarmVehicleInstanceLease airMaster;
    SwarmWaypointLeaderMissionSnapshot mission;
    SwarmWaypointLeaderMissionPath missionPath;
    MissionRefreshState missionRefresh;
    std::optional<SwarmWaypointLeaderPlan> activePlan;
    quint64 nextMissionRefreshGeneration = 0;
    quint64 observedMissionRevision = 0;
    bool missionValid = false;
    bool loading = false;
    bool loadingMission = false;
    bool refreshing = false;
    bool cancelling = false;
    bool cancellingMissionRefresh = false;
    bool cancellationIssued = false;
    bool displayedRunning = false;
    bool closing = false;
    QMetaObject::Connection interfaceDestroyedConnection;

    QComboBox *groundCombo = nullptr;
    QComboBox *airCombo = nullptr;
    QLabel *missionStatus = nullptr;
    QDoubleSpinBox *separation = nullptr;
    QDoubleSpinBox *lead = nullptr;
    QDoubleSpinBox *offPath = nullptr;
    QDoubleSpinBox *altitudeSeparation = nullptr;
    QDoubleSpinBox *navigationAcceleration = nullptr;
    QCheckBox *vFormation = nullptr;
    QCheckBox *altitudeInterleave = nullptr;
    QLabel *mode = nullptr;
    QSplitter *splitter = nullptr;
    QTableWidget *table = nullptr;
    WaypointLeaderProfileControl *profile = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *resetButton = nullptr;
    QPushButton *returnButton = nullptr;
    QPushButton *abandonButton = nullptr;
    QLabel *status = nullptr;
};

QPointer<SwarmWaypointLeaderWindow> SwarmWaypointLeaderWindow::s_current;

SwarmWaypointLeaderWindow::SwarmWaypointLeaderWindow(QWidget *owner)
    : SwarmWaypointLeaderWindow(nullptr, Dependencies(), owner)
{
}

SwarmWaypointLeaderWindow::SwarmWaypointLeaderWindow(
    SwarmWaypointLeaderWindowInterface *windowInterface,
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    m_impl.reset(new Implementation(
        this, windowInterface, std::move(dependencies), owner));
}

SwarmWaypointLeaderWindow::~SwarmWaypointLeaderWindow()
{
    m_impl.reset();
    if (s_current == this) {
        s_current = nullptr;
    }
}

SwarmWaypointLeaderWindow *SwarmWaypointLeaderWindow::OpenWindow(
    QWidget *owner)
{
    return OpenWindow(nullptr, Dependencies(), owner);
}

SwarmWaypointLeaderWindow *SwarmWaypointLeaderWindow::OpenWindow(
    SwarmWaypointLeaderWindowInterface *windowInterface,
    Dependencies dependencies, QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    s_current = new SwarmWaypointLeaderWindow(
        windowInterface, std::move(dependencies), resolvedOwner);
    s_current->setAttribute(Qt::WA_DeleteOnClose, true);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

QString SwarmWaypointLeaderWindow::statusText() const
{
    return m_impl && m_impl->status ? m_impl->status->text() : QString();
}

bool SwarmWaypointLeaderWindow::isRunning() const noexcept
{
    return m_impl && m_impl->runtimeRunning();
}

int SwarmWaypointLeaderWindow::vehicleCount() const noexcept
{
    return m_impl ? m_impl->rows.size() : 0;
}

void SwarmWaypointLeaderWindow::refreshVehicles()
{
    if (m_impl) {
        m_impl->explicitVehicleRefresh();
    }
}

void SwarmWaypointLeaderWindow::refreshMission()
{
    if (m_impl) {
        m_impl->explicitMissionRefresh();
    }
}

void SwarmWaypointLeaderWindow::closeEvent(QCloseEvent *event)
{
    if (m_impl) {
        m_impl->prepareClose();
    }
    QWidget::closeEvent(event);
}
