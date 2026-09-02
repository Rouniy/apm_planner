#include "FlightPlannerWaypointPanel.h"

#include "FlightPlannerMissionModel.h"
#include "FlightPlannerViewModel.h"
#include "QGCMAVLink.h"
#include "WpRow.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QModelIndex>
#include <QFrame>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetItem>

namespace {
class PlannerWrapLayout final : public QLayout
{
public:
    explicit PlannerWrapLayout(QWidget *parent = nullptr,
                               int margin = 0, int spacing = 4)
        : QLayout(parent)
    {
        setContentsMargins(margin, margin, margin, margin);
        setSpacing(spacing);
    }

    ~PlannerWrapLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) delete item;
    }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_items.size()
            ? m_items.at(index) : nullptr;
    }
    QLayoutItem *takeAt(int index) override
    {
        return index >= 0 && index < m_items.size()
            ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override
    {
        return {};
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return layoutItems(QRect(0, 0, width, 0), true);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize result;
        for (QLayoutItem *item : m_items)
            result = result.expandedTo(item->minimumSize());
        const QMargins margins = contentsMargins();
        return result + QSize(margins.left() + margins.right(),
                              margins.top() + margins.bottom());
    }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        layoutItems(rect, false);
    }

private:
    int layoutItems(const QRect &rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect available = rect.adjusted(
            margins.left(), margins.top(),
            -margins.right(), -margins.bottom());
        int x = available.x();
        int y = available.y();
        int lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            if (!item || item->isEmpty()) continue;
            const QSize hint = item->sizeHint();
            const int nextX = x + hint.width();
            if (lineHeight > 0 && nextX > available.right() + 1) {
                x = available.x();
                y += lineHeight + spacing();
                lineHeight = 0;
            }
            if (!testOnly)
                item->setGeometry(QRect(QPoint(x, y), hint));
            x += hint.width() + spacing();
            lineHeight = qMax(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }

    QList<QLayoutItem *> m_items;
};

class WaypointComboDelegate final : public QStyledItemDelegate
{
public:
    explicit WaypointComboDelegate(const QStringList &items,
                                   QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_items(items)
    {
    }

    QWidget *createEditor(QWidget *parent,
                          const QStyleOptionViewItem &,
                          const QModelIndex &) const override
    {
        auto *editor = new QComboBox(parent);
        editor->setEditable(false);
        editor->addItems(m_items);
        return editor;
    }

    void setEditorData(QWidget *editor,
                       const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo) return;

        const QString current = index.data(Qt::DisplayRole).toString();
        const int item = combo->findText(current, Qt::MatchFixedString);
        combo->setCurrentIndex(item >= 0 ? item : 0);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo || !model) return;
        model->setData(index, combo->currentText(), Qt::EditRole);
    }

private:
    QStringList m_items;
};

class MissionCommandDelegate final : public QStyledItemDelegate
{
public:
    explicit MissionCommandDelegate(FlightPlannerViewModel *viewModel,
                                    QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_viewModel(viewModel)
    {
    }

    QWidget *createEditor(QWidget *parent,
                          const QStyleOptionViewItem &,
                          const QModelIndex &) const override
    {
        auto *editor = new QComboBox(parent);
        editor->setEditable(false);
        QStringList commands;
        const QString missionType = m_viewModel
            ? m_viewModel->MissionType() : QStringLiteral("Mission");
        if (missionType == QStringLiteral("Fence")) {
            const quint16 fenceCommands[] = {
                MAV_CMD_NAV_FENCE_RETURN_POINT,
                MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION,
                MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION,
                MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION,
                MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION,
            };
            for (quint16 command : fenceCommands) {
                commands.append(WpRow::CommandNameFor(command));
            }
        } else if (missionType == QStringLiteral("Rally")) {
            commands.append(WpRow::CommandNameFor(MAV_CMD_NAV_RALLY_POINT));
        } else {
            commands = WpRow::CommandList();
        }
        editor->addItems(commands);
        return editor;
    }

    void setEditorData(QWidget *editor,
                       const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo) return;
        const QString current = index.data(Qt::DisplayRole).toString();
        const int item = combo->findText(current, Qt::MatchFixedString);
        if (item >= 0) {
            combo->setCurrentIndex(item);
        } else {
            combo->addItem(current);
            combo->setCurrentIndex(combo->count() - 1);
        }
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (combo && model) {
            model->setData(index, combo->currentText(), Qt::EditRole);
        }
    }

private:
    QPointer<FlightPlannerViewModel> m_viewModel;
};
}

FlightPlannerWaypointPanel::FlightPlannerWaypointPanel(
    FlightPlannerViewModel *viewModel, QWidget *parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
{
    setObjectName(QStringLiteral("WaypointPanel"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *toolBar = new QWidget(this);
    toolBar->setObjectName(QStringLiteral("WaypointToolBar"));
    auto *wrap = new PlannerWrapLayout(toolBar, 4, 6);
    layout->addWidget(toolBar);

    auto *wpRadiusLabel = new QLabel(tr("WP Radius"), toolBar);
    wpRadiusLabel->setObjectName(QStringLiteral("LBL_WPRad"));
    wrap->addWidget(wpRadiusLabel);
    m_wpRadiusEditor = new QDoubleSpinBox(toolBar);
    m_wpRadiusEditor->setObjectName(QStringLiteral("TXT_WPRad"));
    m_wpRadiusEditor->setRange(0.0, 100000.0);
    m_wpRadiusEditor->setDecimals(2);
    m_wpRadiusEditor->setSuffix(tr(" m"));
    m_wpRadiusEditor->setKeyboardTracking(false);
    m_wpRadiusEditor->setToolTip(tr(
        "Waypoint acceptance radius. WP_RADIUS_M uses meters; "
        "WPNAV_RADIUS is converted to centimeters."));
    wrap->addWidget(m_wpRadiusEditor);

    auto *loiterRadiusLabel = new QLabel(tr("Loiter Radius"), toolBar);
    loiterRadiusLabel->setObjectName(QStringLiteral("label5"));
    wrap->addWidget(loiterRadiusLabel);
    m_loiterRadiusEditor = new QDoubleSpinBox(toolBar);
    m_loiterRadiusEditor->setObjectName(QStringLiteral("TXT_loiterrad"));
    m_loiterRadiusEditor->setRange(-100000.0, 100000.0);
    m_loiterRadiusEditor->setDecimals(2);
    m_loiterRadiusEditor->setSuffix(tr(" m"));
    m_loiterRadiusEditor->setKeyboardTracking(false);
    wrap->addWidget(m_loiterRadiusEditor);

    auto *defaultAltitudeLabel = new QLabel(tr("Default Alt"), toolBar);
    defaultAltitudeLabel->setObjectName(QStringLiteral("LBL_defalutalt"));
    wrap->addWidget(defaultAltitudeLabel);
    m_defaultAltitudeEditor = new QDoubleSpinBox(toolBar);
    m_defaultAltitudeEditor->setObjectName(QStringLiteral("TXT_DefaultAlt"));
    m_defaultAltitudeEditor->setRange(-10000.0, 100000.0);
    m_defaultAltitudeEditor->setDecimals(2);
    m_defaultAltitudeEditor->setSuffix(tr(" m"));
    m_defaultAltitudeEditor->setKeyboardTracking(false);
    wrap->addWidget(m_defaultAltitudeEditor);

    auto *altModeLabel = new QLabel(tr("Alt Mode"), toolBar);
    altModeLabel->setObjectName(QStringLiteral("LBL_altmode"));
    wrap->addWidget(altModeLabel);
    m_defaultFrameEditor = new QComboBox(toolBar);
    m_defaultFrameEditor->setObjectName(QStringLiteral("CMB_altmode"));
    m_defaultFrameEditor->setMinimumContentsLength(8);
    wrap->addWidget(m_defaultFrameEditor);

    auto *altWarnLabel = new QLabel(tr("Alt Warn"), toolBar);
    altWarnLabel->setObjectName(QStringLiteral("label17"));
    wrap->addWidget(altWarnLabel);
    m_altWarnEditor = new QDoubleSpinBox(toolBar);
    m_altWarnEditor->setObjectName(QStringLiteral("TXT_altwarn"));
    m_altWarnEditor->setRange(0.0, 100000.0);
    m_altWarnEditor->setDecimals(2);
    m_altWarnEditor->setSuffix(tr(" m"));
    m_altWarnEditor->setKeyboardTracking(false);
    wrap->addWidget(m_altWarnEditor);
    auto *separator = new QFrame(toolBar);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    wrap->addWidget(separator);

    m_addAction = new QAction(tr("Add WP"), this);
    m_addAction->setObjectName(QStringLiteral("AddWaypoint"));
    m_addAction->setToolTip(tr("Add a waypoint at the home location"));
    auto *addButton = new QToolButton(toolBar);
    addButton->setObjectName(QStringLiteral("BUT_Add"));
    addButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addButton->setDefaultAction(m_addAction);
    wrap->addWidget(addButton);
    m_deleteAction = new QAction(tr("Delete"), this);
    m_deleteAction->setObjectName(QStringLiteral("DeleteWaypoint"));
    m_moveUpAction = new QAction(tr("Up"), this);
    m_moveUpAction->setObjectName(QStringLiteral("MoveWaypointUp"));
    m_moveDownAction = new QAction(tr("Down"), this);
    m_moveDownAction->setObjectName(QStringLiteral("MoveWaypointDown"));

    m_splineDefaultEditor = new QCheckBox(tr("Spline"), toolBar);
    m_splineDefaultEditor->setObjectName(QStringLiteral("CHK_splinedefault"));
    wrap->addWidget(m_splineDefaultEditor);
    m_verifyHeightEditor = new QCheckBox(tr("Verify Height"), toolBar);
    m_verifyHeightEditor->setObjectName(QStringLiteral("CHK_verifyheight"));
    m_verifyHeightEditor->setToolTip(tr(
        "Keep newly placed and dragged mission items at a constant height "
        "using available terrain elevation data."));
    wrap->addWidget(m_verifyHeightEditor);

    auto *zoomLabel = new QLabel(tr("Zoom"), toolBar);
    zoomLabel->setObjectName(QStringLiteral("LBL_zoom"));
    wrap->addWidget(zoomLabel);
    m_zoomSlider = new QSlider(Qt::Horizontal, toolBar);
    m_zoomSlider->setObjectName(QStringLiteral("ZoomSlider"));
    m_zoomSlider->setProperty("legacyObjectName", QStringLiteral("TRK_zoom"));
    m_zoomSlider->setRange(3, 20);
    m_zoomSlider->setValue(17);
    m_zoomSlider->setFixedWidth(120);
    wrap->addWidget(m_zoomSlider);

    m_totalDistanceLabel = new QLabel(toolBar);
    m_totalDistanceLabel->setObjectName(QStringLiteral("lbl_distance"));
    wrap->addWidget(m_totalDistanceLabel);
    m_homeDistanceLabel = new QLabel(toolBar);
    m_homeDistanceLabel->setObjectName(QStringLiteral("lbl_homedist"));
    wrap->addWidget(m_homeDistanceLabel);
    m_previousDistanceLabel = new QLabel(toolBar);
    m_previousDistanceLabel->setObjectName(QStringLiteral("lbl_prevdist"));
    wrap->addWidget(m_previousDistanceLabel);
    m_statusLabel = new QLabel(toolBar);
    m_statusLabel->setObjectName(QStringLiteral("lbl_status"));
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->setMaximumWidth(420);
    wrap->addWidget(m_statusLabel);

    const auto updateRadiusVisibility =
        [this, wpRadiusLabel, loiterRadiusLabel]() {
        const bool showWp = m_viewModel && m_viewModel->ShowWpRadius();
        const bool showLoiter = m_viewModel
            && m_viewModel->ShowLoiterRadius();
        wpRadiusLabel->setVisible(showWp);
        m_wpRadiusEditor->setVisible(showWp);
        loiterRadiusLabel->setVisible(showLoiter);
        m_loiterRadiusEditor->setVisible(showLoiter);
    };

    m_waypointTable = new QTableView(this);
    m_waypointTable->setObjectName(QStringLiteral("WpGrid"));
    m_waypointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_waypointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_waypointTable->setEditTriggers(QAbstractItemView::DoubleClicked
                                     | QAbstractItemView::EditKeyPressed
                                     | QAbstractItemView::SelectedClicked);
    m_waypointTable->setAlternatingRowColors(true);
    m_waypointTable->setWordWrap(false);
    m_waypointTable->setHorizontalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    m_waypointTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_waypointTable->verticalHeader()->setVisible(false);
    m_waypointTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Interactive);
    m_waypointTable->installEventFilter(this);
    layout->addWidget(m_waypointTable, 1);

    if (m_viewModel) {
        syncDistancePresentation();
        syncAltitudePresentation();
        m_defaultFrameEditor->addItems(m_viewModel->DefaultFrames());
        m_defaultFrameEditor->setCurrentText(m_viewModel->DefaultFrame());
        m_splineDefaultEditor->setChecked(m_viewModel->SplineDefault());
        m_verifyHeightEditor->setChecked(m_viewModel->VerifyHeight());
        updateRouteSummary();
        updateRadiusVisibility();
        FlightPlannerMissionModel *model = m_viewModel->Waypoints();
        m_waypointTable->setModel(model);
        m_waypointTable->setItemDelegateForColumn(
            FlightPlannerMissionModel::CommandColumn,
            new MissionCommandDelegate(m_viewModel, m_waypointTable));
        m_waypointTable->setItemDelegateForColumn(
            FlightPlannerMissionModel::FrameColumn,
            new WaypointComboDelegate(WpRow::FrameList(), m_waypointTable));

        connect(model, &QAbstractItemModel::rowsInserted,
                this, &FlightPlannerWaypointPanel::updateActions);
        connect(model, &QAbstractItemModel::rowsRemoved,
                this, &FlightPlannerWaypointPanel::updateActions);
        connect(model, &QAbstractItemModel::rowsMoved,
                this, &FlightPlannerWaypointPanel::updateActions);
        connect(model, &QAbstractItemModel::modelReset,
                this, [this]() {
            selectWaypoint(-1);
            syncCommandHeaders();
        });
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &topLeft,
                       const QModelIndex &bottomRight) {
            if (m_selectedWaypoint >= topLeft.row()
                && m_selectedWaypoint <= bottomRight.row()
                && topLeft.column()
                       <= FlightPlannerMissionModel::CommandColumn
                && bottomRight.column()
                       >= FlightPlannerMissionModel::CommandColumn) {
                syncCommandHeaders();
            }
        });
        connect(m_viewModel, &QObject::destroyed, this, [this]() {
            m_waypointTable->setModel(nullptr);
            m_wpRadiusEditor->setEnabled(false);
            m_selectedWaypoint = -1;
            updateActions();
            emit selectedWaypointChanged(-1);
        });

        connect(m_waypointTable->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this, &FlightPlannerWaypointPanel::handleCurrentRowChanged);
        connect(m_wpRadiusEditor,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setWpRadiusDisplay);
        connect(m_loiterRadiusEditor,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setLoiterRadiusDisplay);
        connect(m_defaultAltitudeEditor,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel,
                &FlightPlannerViewModel::setDefaultAltitudeDisplay);
        connect(m_defaultFrameEditor, &QComboBox::currentTextChanged,
                m_viewModel, &FlightPlannerViewModel::setDefaultFrame);
        connect(m_altWarnEditor,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setAltWarnDisplay);
        connect(m_splineDefaultEditor, &QCheckBox::toggled,
                m_viewModel, &FlightPlannerViewModel::setSplineDefault);
        connect(m_verifyHeightEditor, &QCheckBox::toggled,
                m_viewModel, &FlightPlannerViewModel::setVerifyHeight);
        connect(m_viewModel,
                &FlightPlannerViewModel::wpRadiusDisplayChanged,
                this, [this](double value) {
            const QSignalBlocker blocker(m_wpRadiusEditor);
            m_wpRadiusEditor->setValue(value);
        });
        connect(m_viewModel,
                &FlightPlannerViewModel::loiterRadiusDisplayChanged,
                this, [this](double value) {
            const QSignalBlocker blocker(m_loiterRadiusEditor);
            m_loiterRadiusEditor->setValue(value);
        });
        connect(m_viewModel,
                &FlightPlannerViewModel::defaultAltitudeDisplayChanged,
                this, [this](double value) {
            const QSignalBlocker blocker(m_defaultAltitudeEditor);
            m_defaultAltitudeEditor->setValue(value);
        });
        connect(m_viewModel, &FlightPlannerViewModel::defaultFrameChanged,
                this, [this](const QString &value) {
            const QSignalBlocker blocker(m_defaultFrameEditor);
            m_defaultFrameEditor->setCurrentText(value);
        });
        connect(m_viewModel, &FlightPlannerViewModel::altWarnDisplayChanged,
                this, [this](double value) {
            const QSignalBlocker blocker(m_altWarnEditor);
            m_altWarnEditor->setValue(value);
        });
        connect(m_viewModel, &FlightPlannerViewModel::altUnitsChanged,
                this, [this](const QString &) {
            syncAltitudePresentation();
        });
        connect(m_viewModel,
                &FlightPlannerViewModel::distancePresentationChanged,
                this, [this]() {
            syncDistancePresentation();
        });
        connect(m_viewModel, &FlightPlannerViewModel::splineDefaultChanged,
                this, [this](bool enabled) {
            const QSignalBlocker blocker(m_splineDefaultEditor);
            m_splineDefaultEditor->setChecked(enabled);
        });
        connect(m_viewModel, &FlightPlannerViewModel::verifyHeightChanged,
                this, [this](bool enabled) {
            const QSignalBlocker blocker(m_verifyHeightEditor);
            m_verifyHeightEditor->setChecked(enabled);
        });
        connect(m_viewModel, &FlightPlannerViewModel::vehicleTypeChanged,
                this, [updateRadiusVisibility](int) {
            updateRadiusVisibility();
        });
        connect(m_viewModel, &FlightPlannerViewModel::routeMetricsChanged,
                this, &FlightPlannerWaypointPanel::updateRouteSummary);
        connect(m_viewModel, &FlightPlannerViewModel::statusChanged,
                this, [this](const QString &) { updateRouteSummary(); });
        connect(m_viewModel, &FlightPlannerViewModel::transferBusyChanged,
                this, [this](bool) { updateActions(); });
    } else {
        m_wpRadiusEditor->setEnabled(false);
        updateRadiusVisibility();
    }

    connect(m_addAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::addWaypoint);
    connect(m_deleteAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::deleteSelectedWaypoint);
    connect(m_moveUpAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::moveSelectedWaypointUp);
    connect(m_moveDownAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::moveSelectedWaypointDown);
    connect(m_zoomSlider, &QSlider::valueChanged,
            this, &FlightPlannerWaypointPanel::zoomLevelRequested);

    const int widths[FlightPlannerMissionModel::ColumnCount] = {
        44, 190, 72, 72, 72, 72, 110, 110, 85,
        165, 75, 75, 80, 75, 60, 105, 105, 150
    };
    for (int column = 0; column < FlightPlannerMissionModel::ColumnCount;
         ++column) {
        m_waypointTable->setColumnWidth(column, widths[column]);
    }

    updateActions();
}

FlightPlannerWaypointPanel::~FlightPlannerWaypointPanel() = default;

void FlightPlannerWaypointPanel::syncDistancePresentation()
{
    if (!m_viewModel || !m_wpRadiusEditor || !m_loiterRadiusEditor) return;
    const double multiplier = m_viewModel->DistanceMultiplier();
    const QString suffix = QStringLiteral(" %1").arg(
        m_viewModel->DistanceUnit());
    const QSignalBlocker wpBlocker(m_wpRadiusEditor);
    const QSignalBlocker loiterBlocker(m_loiterRadiusEditor);
    m_wpRadiusEditor->setRange(0.0, 100000.0 * multiplier);
    m_loiterRadiusEditor->setRange(-100000.0 * multiplier,
                                   100000.0 * multiplier);
    m_wpRadiusEditor->setSuffix(suffix);
    m_loiterRadiusEditor->setSuffix(suffix);
    m_wpRadiusEditor->setValue(m_viewModel->WpRadiusDisplay());
    m_loiterRadiusEditor->setValue(m_viewModel->LoiterRadiusDisplay());
    updateRouteSummary();
}

void FlightPlannerWaypointPanel::syncAltitudePresentation()
{
    if (!m_viewModel || !m_defaultAltitudeEditor || !m_altWarnEditor) return;
    const double multiplier = m_viewModel->AltitudeMultiplier();
    const QString suffix = QStringLiteral(" %1").arg(m_viewModel->AltUnit());
    const QSignalBlocker defaultBlocker(m_defaultAltitudeEditor);
    const QSignalBlocker warningBlocker(m_altWarnEditor);
    m_defaultAltitudeEditor->setRange(-10000.0 * multiplier,
                                      100000.0 * multiplier);
    m_altWarnEditor->setRange(0.0, 100000.0 * multiplier);
    m_defaultAltitudeEditor->setSuffix(suffix);
    m_altWarnEditor->setSuffix(suffix);
    m_defaultAltitudeEditor->setValue(
        m_viewModel->DefaultAltitudeDisplay());
    m_altWarnEditor->setValue(m_viewModel->AltWarnDisplay());
}

FlightPlannerViewModel *FlightPlannerWaypointPanel::viewModel() const
{
    return m_viewModel.data();
}

QTableView *FlightPlannerWaypointPanel::waypointTable() const
{
    return m_waypointTable;
}

int FlightPlannerWaypointPanel::selectedWaypoint() const
{
    return m_selectedWaypoint;
}

int FlightPlannerWaypointPanel::zoomLevel() const
{
    return m_zoomSlider ? m_zoomSlider->value() : 0;
}

void FlightPlannerWaypointPanel::setZoomRange(int minimum, int maximum)
{
    if (!m_zoomSlider || minimum > maximum) return;
    m_zoomSlider->setRange(minimum, maximum);
}

void FlightPlannerWaypointPanel::setZoomLevel(int level)
{
    if (!m_zoomSlider) return;
    const QSignalBlocker blocker(m_zoomSlider);
    m_zoomSlider->setValue(level);
}

bool FlightPlannerWaypointPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_waypointTable && event->type() == QEvent::KeyPress) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            deleteSelectedWaypoint();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FlightPlannerWaypointPanel::addWaypoint()
{
    if (!m_viewModel) return;
    WpRow *added = m_viewModel->AddWaypointAt(
        m_viewModel->HomeLat(), m_viewModel->HomeLng());
    if (!added) return;

    const int row = m_viewModel->Waypoints()->rowCount() - 1;
    selectWaypoint(row);
    m_waypointTable->scrollTo(
        m_viewModel->Waypoints()->index(row,
            FlightPlannerMissionModel::CommandColumn));
}

void FlightPlannerWaypointPanel::deleteSelectedWaypoint()
{
    if (!m_viewModel || m_selectedWaypoint < 0) return;
    const int deletedRow = m_selectedWaypoint;
    if (!m_viewModel->DeleteWaypoint(deletedRow)) return;

    const int rowCount = m_viewModel->Waypoints()->rowCount();
    selectWaypoint(rowCount > 0 ? qMin(deletedRow, rowCount - 1) : -1);
}

void FlightPlannerWaypointPanel::moveSelectedWaypointUp()
{
    if (!m_viewModel || m_selectedWaypoint <= 0) return;
    const int destination = m_selectedWaypoint - 1;
    if (m_viewModel->MoveWaypointUp(m_selectedWaypoint)) {
        selectWaypoint(destination);
    }
}

void FlightPlannerWaypointPanel::moveSelectedWaypointDown()
{
    if (!m_viewModel || m_selectedWaypoint < 0
        || m_selectedWaypoint + 1 >= m_viewModel->Waypoints()->rowCount()) {
        return;
    }
    const int destination = m_selectedWaypoint + 1;
    if (m_viewModel->MoveWaypointDown(m_selectedWaypoint)) {
        selectWaypoint(destination);
    }
}

void FlightPlannerWaypointPanel::handleCurrentRowChanged(
    const QModelIndex &current, const QModelIndex &)
{
    const int row = current.isValid() ? current.row() : -1;
    if (m_selectedWaypoint != row) {
        m_selectedWaypoint = row;
        emit selectedWaypointChanged(row);
    }
    syncCommandHeaders();
    updateActions();
}

void FlightPlannerWaypointPanel::syncCommandHeaders()
{
    if (m_viewModel) {
        m_viewModel->Waypoints()->setParameterHeaderRow(
            m_selectedWaypoint);
    }
}

void FlightPlannerWaypointPanel::updateActions()
{
    const bool hasViewModel = !m_viewModel.isNull();
    const int rowCount = hasViewModel
        ? m_viewModel->Waypoints()->rowCount() : 0;
    if (m_selectedWaypoint >= rowCount) {
        m_selectedWaypoint = -1;
        emit selectedWaypointChanged(-1);
    }
    const bool selected = m_selectedWaypoint >= 0
        && m_selectedWaypoint < rowCount;
    const bool editable = hasViewModel && !m_viewModel->TransferBusy();

    m_addAction->setEnabled(editable);
    m_deleteAction->setEnabled(editable && selected);
    m_moveUpAction->setEnabled(editable && selected && m_selectedWaypoint > 0);
    m_moveDownAction->setEnabled(editable && selected
                                 && m_selectedWaypoint + 1 < rowCount);
    m_wpRadiusEditor->setEnabled(editable);
    m_loiterRadiusEditor->setEnabled(editable);
    m_defaultAltitudeEditor->setEnabled(editable);
    m_defaultFrameEditor->setEnabled(editable);
    m_altWarnEditor->setEnabled(editable);
    m_splineDefaultEditor->setEnabled(editable);
    m_verifyHeightEditor->setEnabled(editable);
    m_waypointTable->setEditTriggers(editable
        ? QAbstractItemView::DoubleClicked
              | QAbstractItemView::EditKeyPressed
              | QAbstractItemView::SelectedClicked
        : QAbstractItemView::NoEditTriggers);
}

void FlightPlannerWaypointPanel::updateRouteSummary()
{
    if (!m_viewModel) {
        m_totalDistanceLabel->setText(tr("Dist: 0.0000 km"));
        m_homeDistanceLabel->setText(tr("Home: 0.00 m"));
        m_previousDistanceLabel->setText(tr("Prev: 0.00 m"));
        m_statusLabel->clear();
        return;
    }
    m_totalDistanceLabel->setText(
        tr("Dist: %1").arg(m_viewModel->TotalDist()));
    m_homeDistanceLabel->setText(
        tr("Home: %1").arg(m_viewModel->HomeDist()));
    m_previousDistanceLabel->setText(
        tr("Prev: %1").arg(m_viewModel->PrevDist()));
    m_statusLabel->setText(m_viewModel->Status());
}

void FlightPlannerWaypointPanel::selectWaypoint(int row)
{
    if (!m_waypointTable->model() || row < 0
        || row >= m_waypointTable->model()->rowCount()) {
        m_waypointTable->clearSelection();
        m_waypointTable->setCurrentIndex(QModelIndex());
        if (m_selectedWaypoint != -1) {
            m_selectedWaypoint = -1;
            emit selectedWaypointChanged(-1);
        }
        syncCommandHeaders();
        updateActions();
        return;
    }

    const QModelIndex index = m_waypointTable->model()->index(row, 0);
    m_waypointTable->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect
               | QItemSelectionModel::Rows);
    if (m_selectedWaypoint != row) {
        m_selectedWaypoint = row;
        emit selectedWaypointChanged(row);
    }
    syncCommandHeaders();
    updateActions();
}
