#include "FlightPlannerWaypointPanel.h"

#include "FlightPlannerMissionModel.h"
#include "FlightPlannerViewModel.h"
#include "WpRow.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QModelIndex>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
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

    auto *toolBar = new QToolBar(this);
    toolBar->setObjectName(QStringLiteral("WaypointToolBar"));
    toolBar->setMovable(false);
    toolBar->setFloatable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    layout->addWidget(toolBar);

    m_addAction = toolBar->addAction(tr("Add"));
    m_addAction->setObjectName(QStringLiteral("AddWaypoint"));
    m_addAction->setToolTip(tr("Add a waypoint at the home location"));
    m_deleteAction = toolBar->addAction(tr("Delete"));
    m_deleteAction->setObjectName(QStringLiteral("DeleteWaypoint"));
    m_moveUpAction = toolBar->addAction(tr("Up"));
    m_moveUpAction->setObjectName(QStringLiteral("MoveWaypointUp"));
    m_moveDownAction = toolBar->addAction(tr("Down"));
    m_moveDownAction->setObjectName(QStringLiteral("MoveWaypointDown"));

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
        FlightPlannerMissionModel *model = m_viewModel->Waypoints();
        m_waypointTable->setModel(model);
        m_waypointTable->setItemDelegateForColumn(
            FlightPlannerMissionModel::CommandColumn,
            new WaypointComboDelegate(WpRow::CommandList(), m_waypointTable));
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
                this, [this]() { selectWaypoint(-1); });
        connect(m_viewModel, &QObject::destroyed, this, [this]() {
            m_waypointTable->setModel(nullptr);
            m_selectedWaypoint = -1;
            updateActions();
            emit selectedWaypointChanged(-1);
        });

        connect(m_waypointTable->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this, &FlightPlannerWaypointPanel::handleCurrentRowChanged);
    }

    connect(m_addAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::addWaypoint);
    connect(m_deleteAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::deleteSelectedWaypoint);
    connect(m_moveUpAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::moveSelectedWaypointUp);
    connect(m_moveDownAction, &QAction::triggered,
            this, &FlightPlannerWaypointPanel::moveSelectedWaypointDown);

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
        m_viewModel->HomeLat(), m_viewModel->HomeLng(),
        m_viewModel->DefaultAltitude());
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
    updateActions();
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

    m_addAction->setEnabled(hasViewModel);
    m_deleteAction->setEnabled(selected);
    m_moveUpAction->setEnabled(selected && m_selectedWaypoint > 0);
    m_moveDownAction->setEnabled(selected
                                 && m_selectedWaypoint + 1 < rowCount);
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
    updateActions();
}
