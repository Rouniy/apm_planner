#include "SwarmSequenceWindow.h"

#include "SequenceLayoutControl.h"
#include "comm/SwarmTelemetryRegistry.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

// Supplied by SwarmSequenceWindowIntegration.cpp in the application and by a
// tiny test seam in the focused widget test.
SwarmTelemetryRegistry *SwarmSequenceApplicationRegistry();

namespace
{
const char kNoVehicles[] =
    "No live ArduCopter autopilots were found across open MAVLink links.";
const char kCommandUnavailable[] =
    "Live swarm commands are unavailable: the exact multi-endpoint command "
    "sender has not been ported yet.";

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

QString number(double value)
{
    return QString::number(value, 'g', 12);
}
}

QPointer<SwarmSequenceWindow> SwarmSequenceWindow::s_current;

SwarmSequenceWindow::SwarmSequenceWindow(QWidget *owner)
    : SwarmSequenceWindow(SwarmSequenceApplicationRegistry(),
                          DefaultDependencies(), owner)
{
}

SwarmSequenceWindow::SwarmSequenceWindow(
    SwarmTelemetryRegistry *registry, Dependencies dependencies,
    QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_registry(registry)
    , m_dependencies(std::move(dependencies))
{
    buildUi(owner);
    connectUi();
    syncAll(true);
    refreshVehicles(true);
}

SwarmSequenceWindow::~SwarmSequenceWindow()
{
    if (s_current == this) {
        s_current = nullptr;
    }
}

SwarmSequenceWindow *SwarmSequenceWindow::OpenWindow(QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    s_current = new SwarmSequenceWindow(resolvedOwner);
    s_current->setAttribute(Qt::WA_DeleteOnClose, true);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

SwarmSequenceWindow::Dependencies SwarmSequenceWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.chooseLoadPath = [](QWidget *owner) {
        return QFileDialog::getOpenFileName(
            owner, SwarmSequenceWindow::tr("Load Mission Planner Sequence"),
            QString(), SwarmSequenceWindow::tr(
                "Mission Planner Sequence (*.txt *.json);;All files (*)"));
    };
    dependencies.chooseSavePath = [](QWidget *owner,
                                     const QString &suggested) {
        return QFileDialog::getSaveFileName(
            owner, SwarmSequenceWindow::tr("Save Mission Planner Sequence"),
            suggested, SwarmSequenceWindow::tr(
                "Mission Planner Sequence (*.txt *.json);;All files (*)"));
    };
    dependencies.chooseBackgroundPath = [](QWidget *owner) {
        return QFileDialog::getOpenFileName(
            owner, SwarmSequenceWindow::tr("Sequence Background Image"),
            QString(), SwarmSequenceWindow::tr(
                "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All files (*)"));
    };
    dependencies.requestLayoutName = [](QWidget *owner,
                                        const QString &suggested) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            owner, SwarmSequenceWindow::tr("New Sequence Layout"),
            SwarmSequenceWindow::tr("Layout name"), QLineEdit::Normal,
            suggested, &accepted);
        return accepted ? name : QString();
    };
    return dependencies;
}

const SwarmSequenceDocument &SwarmSequenceWindow::document() const noexcept
{
    return m_editor.document();
}

QString SwarmSequenceWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

SwarmSequenceIssue SwarmSequenceWindow::setDocument(
    const SwarmSequenceDocument &document)
{
    const SwarmSequenceIssue result = m_editor.replaceDocument(document);
    if (!result) {
        setStatus(tr("Sequence document was rejected: %1").arg(result.message));
        return result;
    }
    m_currentFilePath.clear();
    syncAll(true);
    resetOfflineState(tr("Sequence document replaced."));
    emit documentChanged();
    return result;
}

SwarmSequenceIssue SwarmSequenceWindow::loadPath(const QString &path)
{
    const SwarmSequenceLoadResult loaded = SwarmSequenceFile::load(path);
    if (!loaded.isValid()) {
        setStatus(tr("Sequence load failed: %1").arg(loaded.issue.message));
        return loaded.issue;
    }
    const SwarmSequenceIssue replaced =
        m_editor.replaceDocument(loaded.document);
    if (!replaced) {
        setStatus(tr("Sequence load failed: %1").arg(replaced.message));
        return replaced;
    }
    m_currentFilePath = loaded.absolutePath;
    syncAll(true);
    resetOfflineState(tr("Loaded %1 layout(s) and %2 step(s) from %3.")
        .arg(document().layouts.size()).arg(document().steps.size())
        .arg(m_currentFilePath));
    emit documentChanged();
    return replaced;
}

SwarmSequenceIssue SwarmSequenceWindow::savePath(const QString &path)
{
    const SwarmSequenceIssue result = SwarmSequenceFile::save(path, document());
    if (!result) {
        setStatus(tr("Sequence save failed: %1").arg(result.message));
        return result;
    }
    m_currentFilePath = QFileInfo(path).absoluteFilePath();
    setStatus(tr("Saved %1 layout(s) and %2 step(s) to %3.")
        .arg(document().layouts.size()).arg(document().steps.size())
        .arg(m_currentFilePath));
    return result;
}

void SwarmSequenceWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SwarmSequenceWindow"));
    setWindowTitle(tr("Swarm Sequence Layout Editor (Beta)"));
    setWindowModality(Qt::NonModal);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#SwarmSequenceWindow { background: #303233; color: #dddddd; }"
        "QLabel#sequenceDangerBanner { background: #5a2b20; color: #ffd4b8; "
        "border: 1px solid #ff9b66; border-radius: 4px; padding: 10px; }"
        "QLabel#sequenceStatus, QLabel#sequenceStepDisplay { color: #73c7ff; }"
        "QTableWidget, QListWidget, QComboBox, QSpinBox { background: #242627; "
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
        "Sequence Layout Editor. JSON layouts and ordered steps are editable "
        "and compatible with upstream. Live commands remain locked until the "
        "exact multi-endpoint sender is ported. DelayStart/DelayEnd are "
        "preserved in files."), QStringLiteral("sequenceDangerBanner"), this);
    danger->setWordWrap(true);
    root->addWidget(danger);

    auto *top = new QHBoxLayout;
    top->setSpacing(7);
    QPushButton *load = button(tr("Load"), QStringLiteral("sequenceLoad"), this);
    QPushButton *save = button(tr("Save"), QStringLiteral("sequenceSave"), this);
    top->addWidget(load);
    top->addWidget(save);
    top->addWidget(label(tr("Layout"), QString(), this));
    m_layouts = new QComboBox(this);
    m_layouts->setObjectName(QStringLiteral("sequenceLayoutCombo"));
    m_layouts->setMinimumWidth(220);
    top->addWidget(m_layouts);
    top->addWidget(label(tr("Vehicles"), QString(), this));
    m_vehicleCount = new QSpinBox(this);
    m_vehicleCount->setObjectName(QStringLiteral("sequenceVehicleCount"));
    m_vehicleCount->setRange(1, 255);
    m_vehicleCount->setValue(1);
    top->addWidget(m_vehicleCount);
    QPushButton *newLayout = button(
        tr("New Layout"), QStringLiteral("sequenceNewLayout"), this);
    QPushButton *addStepButton = button(
        tr("Add Step"), QStringLiteral("sequenceAddStep"), this);
    QPushButton *refresh = button(
        tr("Refresh Vehicles"), QStringLiteral("sequenceRefreshVehicles"), this);
    QPushButton *background = button(
        tr("Background Image"), QStringLiteral("sequenceBackgroundImage"), this);
    top->addWidget(newLayout);
    top->addWidget(addStepButton);
    top->addWidget(refresh);
    top->addWidget(background);
    top->addStretch(1);
    m_stepDisplay = label(QStringLiteral("Step 0 / 0"),
                          QStringLiteral("sequenceStepDisplay"), this);
    m_originDisplay = label(tr("Origin not captured"),
                            QStringLiteral("sequenceOriginDisplay"), this);
    top->addWidget(m_stepDisplay);
    top->addWidget(label(QStringLiteral("·"), QString(), this));
    top->addWidget(m_originDisplay);
    root->addLayout(top);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("sequenceMainSplitter"));
    splitter->setChildrenCollapsible(false);

    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(7);
    m_canvas = new SequenceLayoutControl(left);
    m_canvas->setObjectName(QStringLiteral("SequenceLayoutControl"));
    leftLayout->addWidget(m_canvas, 3);

    auto *imageControls = new QHBoxLayout;
    imageControls->setSpacing(5);
    imageControls->addWidget(label(tr("Image:"), QString(), left));
    const struct { const char *text; const char *name; } imageButtons[] = {
        {"←", "sequenceImageLeft"}, {"→", "sequenceImageRight"},
        {"↑", "sequenceImageUp"}, {"↓", "sequenceImageDown"},
        {"W+", "sequenceImageWider"}, {"W−", "sequenceImageNarrower"},
        {"H+", "sequenceImageTaller"}, {"H−", "sequenceImageShorter"},
        {"Step 0.1", "sequenceImageFineStep"},
        {"Step 1", "sequenceImageNormalStep"},
        {"Clear", "sequenceImageClear"}
    };
    for (const auto &definition : imageButtons) {
        imageControls->addWidget(button(QString::fromUtf8(definition.text),
            QString::fromLatin1(definition.name), left));
    }
    imageControls->addStretch(1);
    leftLayout->addLayout(imageControls);

    m_offsets = new QTableWidget(left);
    m_offsets->setObjectName(QStringLiteral("sequenceOffsetTable"));
    m_offsets->setColumnCount(4);
    m_offsets->setHorizontalHeaderLabels({tr("System ID"), tr("East X m"),
        tr("North Y m"), tr("Relative altitude Z m")});
    m_offsets->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < 4; ++column) {
        m_offsets->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::Stretch);
    }
    m_offsets->verticalHeader()->hide();
    m_offsets->setAlternatingRowColors(true);
    m_offsets->setSelectionBehavior(QAbstractItemView::SelectRows);
    leftLayout->addWidget(m_offsets, 2);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(7);

    auto *stepHeader = new QHBoxLayout;
    QLabel *stepTitle = label(tr("Ordered sequence steps"), QString(), right);
    QFont strong = stepTitle->font();
    strong.setBold(true);
    stepTitle->setFont(strong);
    stepHeader->addWidget(stepTitle);
    stepHeader->addStretch(1);
    stepHeader->addWidget(button(QStringLiteral("↑"),
        QStringLiteral("sequenceStepUp"), right));
    stepHeader->addWidget(button(QStringLiteral("↓"),
        QStringLiteral("sequenceStepDown"), right));
    stepHeader->addWidget(button(tr("Remove"),
        QStringLiteral("sequenceStepRemove"), right));
    rightLayout->addLayout(stepHeader);
    m_steps = new QListWidget(right);
    m_steps->setObjectName(QStringLiteral("sequenceSteps"));
    m_steps->setAlternatingRowColors(true);
    rightLayout->addWidget(m_steps, 2);

    auto *assignmentHeader = new QHBoxLayout;
    QLabel *assignmentTitle = label(tr("Exact modem assignments"), QString(), right);
    assignmentTitle->setFont(strong);
    assignmentHeader->addWidget(assignmentTitle);
    assignmentHeader->addStretch(1);
    assignmentHeader->addWidget(label(tr("Anchor"), QString(), right));
    m_anchorCombo = new QComboBox(right);
    m_anchorCombo->setObjectName(QStringLiteral("sequenceAnchor"));
    m_anchorCombo->setMinimumWidth(245);
    assignmentHeader->addWidget(m_anchorCombo);
    rightLayout->addLayout(assignmentHeader);

    m_assignmentTable = new QTableWidget(right);
    m_assignmentTable->setObjectName(QStringLiteral("sequenceAssignmentTable"));
    m_assignmentTable->setColumnCount(3);
    m_assignmentTable->setHorizontalHeaderLabels({tr("Layout Sys"),
        tr("Exact live vehicle"), tr("Last target")});
    m_assignmentTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_assignmentTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_assignmentTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    m_assignmentTable->verticalHeader()->hide();
    m_assignmentTable->setAlternatingRowColors(true);
    m_assignmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rightLayout->addWidget(m_assignmentTable, 3);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    auto *commands = new QHBoxLayout;
    m_runStep = button(tr("Run Step"), QStringLiteral("sequenceRunStep"), this);
    m_runStep->setMinimumWidth(120);
    QPushButton *reset = button(
        tr("Reset Sequence"), QStringLiteral("sequenceReset"), this);
    m_takeoff = button(tr("Takeoff Assigned (2 m)"),
                       QStringLiteral("sequenceTakeoff"), this);
    const QString commandReason = tr(kCommandUnavailable);
    for (QPushButton *command : {m_runStep, m_takeoff}) {
        command->setEnabled(false);
        command->setToolTip(commandReason);
        command->setStatusTip(commandReason);
        command->setAccessibleDescription(commandReason);
    }
    commands->addWidget(m_runStep);
    commands->addWidget(reset);
    commands->addWidget(m_takeoff);
    commands->addStretch(1);
    QLabel *commandHint = label(commandReason,
        QStringLiteral("sequenceCommandUnavailable"), this);
    commandHint->setWordWrap(true);
    commandHint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    commands->addWidget(commandHint);
    root->addLayout(commands);

    auto *footer = new QHBoxLayout;
    m_status = label(tr(
        "Create or load a layout. Sequence files are compatible with the "
        "official JSON format."), QStringLiteral("sequenceStatus"), this);
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    footer->addWidget(button(tr("Close"), QStringLiteral("sequenceClose"), this));
    root->addLayout(footer);

    connect(load, &QPushButton::clicked, this,
            &SwarmSequenceWindow::chooseAndLoad);
    connect(save, &QPushButton::clicked, this,
            &SwarmSequenceWindow::chooseAndSave);
    connect(newLayout, &QPushButton::clicked, this,
            &SwarmSequenceWindow::createLayout);
    connect(addStepButton, &QPushButton::clicked, this,
            &SwarmSequenceWindow::addStep);
    connect(refresh, &QPushButton::clicked, this,
            [this]() { refreshVehicles(); });
    connect(background, &QPushButton::clicked, this,
            &SwarmSequenceWindow::chooseBackground);
    connect(reset, &QPushButton::clicked, this, [this]() {
        resetOfflineState(tr(
            "Sequence reset to the first step; origin will be captured again."));
    });
    connect(findChild<QPushButton *>(QStringLiteral("sequenceClose")),
            &QPushButton::clicked, this, &QWidget::close);
}

void SwarmSequenceWindow::connectUi()
{
    connect(m_layouts, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (!m_loading) {
            syncCurrentLayout();
        }
    });
    connect(m_vehicleCount, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) {
        if (!m_loading) {
            resizeSlots(value);
        }
    });
    connect(m_offsets, &QTableWidget::cellChanged, this,
            &SwarmSequenceWindow::offsetCellChanged);
    connect(m_canvas, &SequenceLayoutControl::offsetDragged, this,
            &SwarmSequenceWindow::offsetDragged);
    connect(m_steps, &QListWidget::currentRowChanged, this,
            &SwarmSequenceWindow::stepSelected);
    connect(m_anchorCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading) {
            return;
        }
        const SwarmVehicleInstanceLease selected =
            vehicleForCombo(m_anchorCombo);
        if (sameVehicle(selected, m_anchor)) {
            return;
        }
        m_anchor = selected;
        resetOfflineState(tr(
            "Sequence origin reset because the anchor changed."));
    });
    connect(findChild<QPushButton *>(QStringLiteral("sequenceStepUp")),
            &QPushButton::clicked, this,
            [this]() { moveSelectedStep(-1); });
    connect(findChild<QPushButton *>(QStringLiteral("sequenceStepDown")),
            &QPushButton::clicked, this,
            [this]() { moveSelectedStep(1); });
    connect(findChild<QPushButton *>(QStringLiteral("sequenceStepRemove")),
            &QPushButton::clicked, this,
            &SwarmSequenceWindow::removeSelectedStep);

    struct BackgroundAction { const char *name; std::function<void()> invoke; };
    const BackgroundAction backgroundActions[] = {
        {"sequenceImageLeft", [this]() { m_canvas->moveBackground(-1, 0); }},
        {"sequenceImageRight", [this]() { m_canvas->moveBackground(1, 0); }},
        {"sequenceImageUp", [this]() { m_canvas->moveBackground(0, -1); }},
        {"sequenceImageDown", [this]() { m_canvas->moveBackground(0, 1); }},
        {"sequenceImageWider", [this]() { m_canvas->resizeBackground(1, 0); }},
        {"sequenceImageNarrower", [this]() { m_canvas->resizeBackground(-1, 0); }},
        {"sequenceImageTaller", [this]() { m_canvas->resizeBackground(0, 1); }},
        {"sequenceImageShorter", [this]() { m_canvas->resizeBackground(0, -1); }},
        {"sequenceImageFineStep", [this]() { m_canvas->setBackgroundStep(0.1); }},
        {"sequenceImageNormalStep", [this]() { m_canvas->setBackgroundStep(1.0); }},
        {"sequenceImageClear", [this]() { m_canvas->clearBackground(); }}
    };
    for (const BackgroundAction &action : backgroundActions) {
        connect(findChild<QPushButton *>(QString::fromLatin1(action.name)),
                &QPushButton::clicked, this, action.invoke);
    }

    if (m_registry) {
        connect(m_registry, &SwarmTelemetryRegistry::endpointActivated,
                this, [this](const SwarmTelemetrySnapshot &) {
            refreshVehicles(false);
        });
        connect(m_registry, &SwarmTelemetryRegistry::endpointUpdated,
                this, [this](const SwarmTelemetrySnapshot &snapshot) {
            if (snapshot.heartbeatValid
                && snapshot.lastMessageMs == snapshot.heartbeatObservedMs) {
                refreshVehicles(false);
            }
        });
        connect(m_registry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](const SwarmVehicleInstanceLease &,
                             SwarmTelemetryRegistry::RetirementReason) {
            refreshVehicles(false);
        });
        connect(m_registry, &QObject::destroyed, this, [this]() {
            m_vehicleOptions.clear();
            m_assignments.clear();
            m_anchor = SwarmVehicleInstanceLease();
            rebuildVehicleControls();
            setStatus(tr(kNoVehicles));
        });
    }
}

bool SwarmSequenceWindow::isArduCopter(
    const SwarmTelemetrySnapshot &snapshot)
{
    if (!snapshot.heartbeatValid
        || snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return false;
    }
    switch (snapshot.vehicleType) {
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return true;
    default:
        return false;
    }
}

bool SwarmSequenceWindow::sameVehicle(
    const SwarmVehicleInstanceLease &left,
    const SwarmVehicleInstanceLease &right)
{
    return left.isValid() && right.isValid() && left.sameInstance(right);
}

QString SwarmSequenceWindow::vehicleLabel(
    const SwarmVehicleInstanceLease &vehicle)
{
    const VehicleEndpoint &endpoint = vehicle.endpoint;
    return tr("%1 [link %2, sys %3, comp %4]")
        .arg(endpoint.displayName()).arg(endpoint.linkId)
        .arg(endpoint.systemId).arg(endpoint.componentId);
}

QVector<SwarmVehicleInstanceLease>
SwarmSequenceWindow::discoverVehicles() const
{
    QVector<SwarmVehicleInstanceLease> result;
    if (!m_registry) {
        return result;
    }
    const QList<VehicleEndpoint> endpoints = m_registry->endpoints();
    result.reserve(endpoints.size());
    for (const VehicleEndpoint &endpoint : endpoints) {
        SwarmTelemetrySnapshot snapshot;
        if (m_registry->acquireSnapshot(endpoint, &snapshot)
            && isArduCopter(snapshot)) {
            result.append(snapshot.lease);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const SwarmVehicleInstanceLease &left,
                 const SwarmVehicleInstanceLease &right) {
        return left.endpoint < right.endpoint;
    });
    return result;
}

void SwarmSequenceWindow::refreshVehicles()
{
    refreshVehicles(true);
}

void SwarmSequenceWindow::refreshVehicles(bool explicitlyRequested)
{
    if (m_refreshingVehicles) {
        return;
    }
    m_refreshingVehicles = true;
    const QVector<SwarmVehicleInstanceLease> discovered = discoverVehicles();
    bool changed = discovered.size() != m_vehicleOptions.size();
    if (!changed) {
        for (int index = 0; index < discovered.size(); ++index) {
            if (!sameVehicle(discovered.at(index), m_vehicleOptions.at(index))) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        m_vehicleOptions = discovered;
        rebuildVehicleControls();
    }
    if (changed || explicitlyRequested) {
        setStatus(discovered.isEmpty() ? tr(kNoVehicles)
            : tr("Found %1 Sequence vehicle(s). Assign every layout system "
                 "id to an exact modem vehicle; duplicate sysids are never guessed.")
                .arg(discovered.size()));
    }
    m_refreshingVehicles = false;
}

int SwarmSequenceWindow::optionIndex(
    const SwarmVehicleInstanceLease &vehicle) const
{
    for (int index = 0; index < m_vehicleOptions.size(); ++index) {
        if (sameVehicle(m_vehicleOptions.at(index), vehicle)) {
            return index;
        }
    }
    return -1;
}

SwarmVehicleInstanceLease SwarmSequenceWindow::vehicleForCombo(
    const QComboBox *combo) const
{
    if (!combo) {
        return SwarmVehicleInstanceLease();
    }
    const int option = combo->currentData(Qt::UserRole).toInt() - 1;
    return option >= 0 && option < m_vehicleOptions.size()
        ? m_vehicleOptions.at(option) : SwarmVehicleInstanceLease();
}

void SwarmSequenceWindow::rebuildVehicleControls()
{
    const SwarmVehicleInstanceLease previousAnchor = m_anchor;
    QMap<int, SwarmVehicleInstanceLease> previousAssignments = m_assignments;
    m_loading = true;
    m_anchorCombo->clear();
    for (int index = 0; index < m_vehicleOptions.size(); ++index) {
        m_anchorCombo->addItem(vehicleLabel(m_vehicleOptions.at(index)), index + 1);
        m_anchorCombo->setItemData(
            index, QVariant::fromValue<qulonglong>(
                       m_vehicleOptions.at(index).linkSessionEpoch),
            LinkSessionEpochRole);
        m_anchorCombo->setItemData(
            index, QVariant::fromValue<qulonglong>(
                       m_vehicleOptions.at(index).instanceEpoch),
            InstanceEpochRole);
    }
    int anchorIndex = optionIndex(previousAnchor);
    if (anchorIndex < 0 && !m_vehicleOptions.isEmpty()) {
        anchorIndex = 0;
    }
    m_anchorCombo->setCurrentIndex(anchorIndex);
    m_anchor = anchorIndex >= 0 ? m_vehicleOptions.at(anchorIndex)
                                : SwarmVehicleInstanceLease();
    m_loading = false;

    m_assignments.clear();
    for (auto iterator = previousAssignments.constBegin();
         iterator != previousAssignments.constEnd(); ++iterator) {
        const int option = optionIndex(iterator.value());
        if (option >= 0) {
            m_assignments.insert(iterator.key(), m_vehicleOptions.at(option));
        }
    }
    rebuildAssignments();
}

void SwarmSequenceWindow::rebuildAssignments()
{
    QMap<int, SwarmVehicleInstanceLease> preserved = m_assignments;
    m_assignments.clear();
    QList<int> ids;
    if (!document().layouts.isEmpty()) {
        ids = document().layouts.first().offsets.keys();
    }

    // Preserve only live one-to-one exact assignments.
    QVector<SwarmVehicleInstanceLease> used;
    for (int systemId : ids) {
        const SwarmVehicleInstanceLease old = preserved.value(systemId);
        if (optionIndex(old) < 0) {
            continue;
        }
        bool duplicate = false;
        for (const SwarmVehicleInstanceLease &alreadyUsed : used) {
            duplicate = duplicate || sameVehicle(alreadyUsed, old);
        }
        if (!duplicate) {
            m_assignments.insert(systemId, old);
            used.append(old);
        }
    }

    // Match a sysid automatically only when it names exactly one live endpoint.
    for (int systemId : ids) {
        if (m_assignments.contains(systemId)) {
            continue;
        }
        QVector<SwarmVehicleInstanceLease> matches;
        for (const SwarmVehicleInstanceLease &vehicle : m_vehicleOptions) {
            if (vehicle.endpoint.systemId == systemId) {
                matches.append(vehicle);
            }
        }
        if (matches.size() != 1) {
            continue;
        }
        bool duplicate = false;
        for (const SwarmVehicleInstanceLease &alreadyUsed : used) {
            duplicate = duplicate || sameVehicle(alreadyUsed, matches.first());
        }
        if (!duplicate) {
            m_assignments.insert(systemId, matches.first());
            used.append(matches.first());
        }
    }

    m_loading = true;
    m_assignmentTable->clearContents();
    m_assignmentTable->setRowCount(ids.size());
    for (int row = 0; row < ids.size(); ++row) {
        const int systemId = ids.at(row);
        auto *systemItem = new QTableWidgetItem(QString::number(systemId));
        systemItem->setTextAlignment(Qt::AlignCenter);
        m_assignmentTable->setItem(row, 0, systemItem);
        auto *combo = new QComboBox(m_assignmentTable);
        combo->setObjectName(QStringLiteral("sequenceAssignment_%1").arg(systemId));
        combo->addItem(tr("— Unassigned —"), 0);
        for (int index = 0; index < m_vehicleOptions.size(); ++index) {
            combo->addItem(vehicleLabel(m_vehicleOptions.at(index)), index + 1);
            combo->setItemData(
                index + 1, QVariant::fromValue<qulonglong>(
                               m_vehicleOptions.at(index).linkSessionEpoch),
                LinkSessionEpochRole);
            combo->setItemData(
                index + 1, QVariant::fromValue<qulonglong>(
                               m_vehicleOptions.at(index).instanceEpoch),
                InstanceEpochRole);
        }
        const int assigned = optionIndex(m_assignments.value(systemId));
        combo->setCurrentIndex(assigned < 0 ? 0 : assigned + 1);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, systemId, combo](int) {
            assignmentChanged(systemId, combo);
        });
        m_assignmentTable->setCellWidget(row, 1, combo);
        auto *target = new QTableWidgetItem(QStringLiteral("—"));
        target->setTextAlignment(Qt::AlignCenter);
        m_assignmentTable->setItem(row, 2, target);
    }
    m_loading = false;
}

void SwarmSequenceWindow::syncAll(bool selectFirst)
{
    m_loading = true;
    syncLayouts(selectFirst ? 0 : m_layouts->currentIndex());
    syncSteps(selectFirst ? (document().steps.isEmpty() ? -1 : 0)
                          : m_steps->currentRow());
    m_loading = false;
    syncCurrentLayout();
    rebuildAssignments();
    m_stepIndex = 0;
    syncStepDisplay();
    m_originDisplay->setText(tr("Origin not captured"));
}

void SwarmSequenceWindow::syncLayouts(int selectedIndex)
{
    const QSignalBlocker blocker(m_layouts);
    m_layouts->clear();
    for (const SwarmSequenceLayout &layout : document().layouts) {
        m_layouts->addItem(layout.id);
    }
    if (!document().layouts.isEmpty()) {
        m_layouts->setCurrentIndex(qBound(0, selectedIndex,
                                          document().layouts.size() - 1));
    }
}

void SwarmSequenceWindow::syncCurrentLayout()
{
    const SwarmSequenceLayout *layout = currentLayout();
    {
        const QSignalBlocker blocker(m_vehicleCount);
        m_vehicleCount->setValue(layout ? qMax(1, layout->offsets.size()) : 1);
    }
    syncOffsets();
}

void SwarmSequenceWindow::syncOffsets()
{
    const SwarmSequenceLayout *layout = currentLayout();
    const QSignalBlocker blocker(m_offsets);
    m_offsets->clearContents();
    m_offsets->setRowCount(layout ? layout->offsets.size() : 0);
    QVector<SequenceLayoutOffset> canvasOffsets;
    if (layout) {
        canvasOffsets.reserve(layout->offsets.size());
        int row = 0;
        for (auto iterator = layout->offsets.constBegin();
             iterator != layout->offsets.constEnd(); ++iterator, ++row) {
            auto *system = new QTableWidgetItem(QString::number(iterator.key()));
            system->setFlags(system->flags() & ~Qt::ItemIsEditable);
            system->setTextAlignment(Qt::AlignCenter);
            m_offsets->setItem(row, 0, system);
            m_offsets->setItem(row, 1, new QTableWidgetItem(number(iterator->x)));
            m_offsets->setItem(row, 2, new QTableWidgetItem(number(iterator->y)));
            m_offsets->setItem(row, 3, new QTableWidgetItem(number(iterator->z)));
            SequenceLayoutOffset canvasOffset;
            canvasOffset.systemId = iterator.key();
            canvasOffset.x = iterator->x;
            canvasOffset.y = iterator->y;
            canvasOffset.z = iterator->z;
            canvasOffsets.append(canvasOffset);
        }
    }
    m_canvas->setOffsets(canvasOffsets);
}

void SwarmSequenceWindow::syncSteps(int selectedRow)
{
    const QSignalBlocker blocker(m_steps);
    m_steps->clear();
    m_steps->addItems(document().steps);
    if (!document().steps.isEmpty() && selectedRow >= 0) {
        m_steps->setCurrentRow(qBound(0, selectedRow,
                                      document().steps.size() - 1));
    }
}

void SwarmSequenceWindow::syncStepDisplay()
{
    const int count = document().steps.size();
    const int shown = count == 0 ? 0 : qMin(m_stepIndex + 1, count);
    m_stepDisplay->setText(tr("Step %1 / %2").arg(shown).arg(count));
}

void SwarmSequenceWindow::resetOfflineState(const QString &status)
{
    m_stepIndex = 0;
    if (!document().steps.isEmpty()) {
        m_steps->setCurrentRow(0);
    }
    m_originDisplay->setText(tr("Origin not captured"));
    for (int row = 0; row < m_assignmentTable->rowCount(); ++row) {
        QTableWidgetItem *target = m_assignmentTable->item(row, 2);
        if (target) {
            target->setText(QStringLiteral("—"));
        }
    }
    syncStepDisplay();
    setStatus(status);
}

void SwarmSequenceWindow::setStatus(const QString &status)
{
    if (m_status) {
        m_status->setText(status);
    }
}

QString SwarmSequenceWindow::currentLayoutId() const
{
    const SwarmSequenceLayout *layout = currentLayout();
    return layout ? layout->id : QString();
}

const SwarmSequenceLayout *SwarmSequenceWindow::currentLayout() const
{
    const int index = m_layouts ? m_layouts->currentIndex() : -1;
    return index >= 0 && index < document().layouts.size()
        ? &document().layouts.at(index) : nullptr;
}

int SwarmSequenceWindow::nextLayoutNumber() const
{
    int numberValue = 1;
    while (true) {
        const QString candidate = tr("Layout %1").arg(numberValue);
        bool used = false;
        for (const SwarmSequenceLayout &layout : document().layouts) {
            used = used || layout.id == candidate;
        }
        if (!used) {
            return numberValue;
        }
        ++numberValue;
    }
}

void SwarmSequenceWindow::chooseAndLoad()
{
    if (!m_dependencies.chooseLoadPath) {
        return;
    }
    const QString path = m_dependencies.chooseLoadPath(this);
    if (!path.isEmpty()) {
        loadPath(path);
    }
}

void SwarmSequenceWindow::chooseAndSave()
{
    if (!m_dependencies.chooseSavePath) {
        return;
    }
    const QString suggested = m_currentFilePath.isEmpty()
        ? QStringLiteral("swarm-sequence.txt") : m_currentFilePath;
    const QString path = m_dependencies.chooseSavePath(this, suggested);
    if (!path.isEmpty()) {
        savePath(path);
    }
}

void SwarmSequenceWindow::createLayout()
{
    const QString proposed = tr("Layout %1").arg(nextLayoutNumber());
    if (!m_dependencies.requestLayoutName) {
        setStatus(tr("New layout cancelled."));
        return;
    }
    const QString name = m_dependencies.requestLayoutName(this, proposed).trimmed();
    if (name.isEmpty()) {
        setStatus(tr("New layout cancelled."));
        return;
    }
    const QString source = currentLayoutId();
    const SwarmSequenceIssue result = source.isEmpty()
        ? m_editor.createLayout(name) : m_editor.cloneLayout(source, name);
    if (!result) {
        setStatus(tr("Cannot create layout: %1").arg(result.message));
        return;
    }
    syncLayouts(document().layouts.size() - 1);
    syncCurrentLayout();
    rebuildAssignments();
    setStatus(tr("Created layout '%1'.").arg(name));
    emit documentChanged();
}

void SwarmSequenceWindow::addStep()
{
    const QString layoutId = currentLayoutId();
    if (layoutId.isEmpty()) {
        setStatus(tr("Select or create a layout before adding a step."));
        return;
    }
    const SwarmSequenceIssue result = m_editor.addStep(layoutId);
    if (!result) {
        setStatus(tr("Cannot add Sequence step: %1").arg(result.message));
        return;
    }
    syncSteps(document().steps.size() - 1);
    syncStepDisplay();
    setStatus(tr("Added step '%1'.").arg(layoutId));
    emit documentChanged();
}

void SwarmSequenceWindow::resizeSlots(int count)
{
    const SwarmSequenceIssue result = m_editor.resizeSlots(count);
    if (!result) {
        setStatus(tr("Cannot resize layouts: %1").arg(result.message));
        syncCurrentLayout();
        return;
    }
    syncOffsets();
    rebuildAssignments();
    resetOfflineState(tr("Resized every layout to %1 vehicle slot(s).").arg(count));
    emit documentChanged();
}

void SwarmSequenceWindow::offsetCellChanged(int row, int column)
{
    if (m_loading || column < 1 || column > 3 || row < 0
        || row >= m_offsets->rowCount()) {
        return;
    }
    const SwarmSequenceLayout *layout = currentLayout();
    QTableWidgetItem *systemItem = m_offsets->item(row, 0);
    QTableWidgetItem *valueItem = m_offsets->item(row, column);
    if (!layout || !systemItem || !valueItem) {
        return;
    }
    bool parsed = false;
    const double value = valueItem->text().trimmed().toDouble(&parsed);
    const int systemId = systemItem->text().toInt();
    if (!parsed || !std::isfinite(value)
        || !layout->offsets.contains(systemId)) {
        setStatus(tr("Offset values must be finite decimal numbers."));
        syncOffsets();
        return;
    }
    SwarmSequenceOffset offset = layout->offsets.value(systemId);
    if (column == 1) offset.x = value;
    if (column == 2) offset.y = value;
    if (column == 3) offset.z = value;
    const QString layoutId = layout->id;
    const SwarmSequenceIssue result =
        m_editor.setOffset(layoutId, systemId, offset);
    if (!result) {
        setStatus(tr("Cannot change offset: %1").arg(result.message));
        syncOffsets();
        return;
    }
    syncOffsets();
    resetOfflineState(tr("Layout '%1' changed.").arg(layoutId));
    emit documentChanged();
}

void SwarmSequenceWindow::offsetDragged(
    int systemId, double x, double y)
{
    const SwarmSequenceLayout *layout = currentLayout();
    if (!layout || !layout->offsets.contains(systemId)) {
        return;
    }
    SwarmSequenceOffset offset = layout->offsets.value(systemId);
    offset.x = x;
    offset.y = y;
    const QString layoutId = layout->id;
    const SwarmSequenceIssue result =
        m_editor.setOffset(layoutId, systemId, offset);
    if (!result) {
        setStatus(tr("Cannot drag offset: %1").arg(result.message));
        syncOffsets();
        return;
    }
    syncOffsets();
    resetOfflineState(tr("Layout '%1' changed.").arg(layoutId));
    emit documentChanged();
}

void SwarmSequenceWindow::stepSelected(int row)
{
    if (m_loading || row < 0 || row >= document().steps.size()) {
        return;
    }
    const QString layoutId = document().steps.at(row);
    for (int index = 0; index < document().layouts.size(); ++index) {
        if (document().layouts.at(index).id == layoutId) {
            m_layouts->setCurrentIndex(index);
            break;
        }
    }
}

void SwarmSequenceWindow::moveSelectedStep(int delta)
{
    const int index = m_steps->currentRow();
    const int destination = index + delta;
    if (index < 0 || destination < 0
        || destination >= document().steps.size()) {
        return;
    }
    const SwarmSequenceIssue result = m_editor.moveStep(index, destination);
    if (!result) {
        setStatus(tr("Cannot reorder step: %1").arg(result.message));
        return;
    }
    if (m_stepIndex == index) {
        m_stepIndex = destination;
    } else if (m_stepIndex == destination) {
        m_stepIndex = index;
    }
    syncSteps(destination);
    syncStepDisplay();
    setStatus(tr("Reordered sequence steps."));
    emit documentChanged();
}

void SwarmSequenceWindow::removeSelectedStep()
{
    const int index = m_steps->currentRow();
    if (index < 0) {
        setStatus(tr("Select a sequence step to remove."));
        return;
    }
    const SwarmSequenceIssue result = m_editor.removeStep(index);
    if (!result) {
        setStatus(tr("Cannot remove step: %1").arg(result.message));
        return;
    }
    m_stepIndex = qMin(m_stepIndex, document().steps.size());
    syncSteps(document().steps.isEmpty() ? -1
        : qMin(index, document().steps.size() - 1));
    syncStepDisplay();
    setStatus(tr("Removed sequence step."));
    emit documentChanged();
}

void SwarmSequenceWindow::assignmentChanged(
    int systemId, QComboBox *combo)
{
    if (m_loading || !combo) {
        return;
    }
    const SwarmVehicleInstanceLease selected = vehicleForCombo(combo);
    if (!selected.isValid()) {
        m_assignments.remove(systemId);
        resetOfflineState(tr("System %1 assignment cleared.").arg(systemId));
        return;
    }
    for (auto iterator = m_assignments.constBegin();
         iterator != m_assignments.constEnd(); ++iterator) {
        if (iterator.key() != systemId
            && sameVehicle(iterator.value(), selected)) {
            const QSignalBlocker blocker(combo);
            const int previous = optionIndex(m_assignments.value(systemId));
            combo->setCurrentIndex(previous < 0 ? 0 : previous + 1);
            setStatus(tr("That exact vehicle is already assigned to layout "
                         "system id %1.").arg(iterator.key()));
            return;
        }
    }
    m_assignments.insert(systemId, selected);
    resetOfflineState(tr("Assigned layout system %1 to %2.")
        .arg(systemId).arg(vehicleLabel(selected)));
}

void SwarmSequenceWindow::chooseBackground()
{
    if (!m_dependencies.chooseBackgroundPath) {
        return;
    }
    const QString path = m_dependencies.chooseBackgroundPath(this);
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_canvas->loadBackground(path, &error)) {
        setStatus(tr("Sequence background failed: %1").arg(error));
        return;
    }
    setStatus(tr("Loaded Sequence background image from %1.")
              .arg(QFileInfo(path).absoluteFilePath()));
}
