#include "ConfigPlannerAdvView.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {
class PlannerSettingItem final : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override
    {
        const int insensitive = QString::compare(
            text(), other.text(), Qt::CaseInsensitive);
        return insensitive == 0
            ? QString::compare(text(), other.text(), Qt::CaseSensitive) < 0
            : insensitive < 0;
    }
};
}

ConfigPlannerAdvView::ConfigPlannerAdvView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigPlannerAdvViewModel(this))
{
    setObjectName(QStringLiteral("ConfigPlannerAdvView"));
    setStyleSheet(QStringLiteral(
        "ConfigPlannerAdvView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#plannerAdvancedTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QTableWidget#settingsTable { background: #0D1210; color: #E6EDE9;"
        " gridline-color: #2A322D; }"
        "QHeaderView::section { background: #202623; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 5px; }"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(0);

    auto *title = new QLabel(tr("Planner Advanced (config.xml)"), this);
    title->setObjectName(QStringLiteral("plannerAdvancedTitle"));
    layout->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 8, 0, 8);
    toolbar->setSpacing(8);
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    refreshButton->setObjectName(QStringLiteral("refreshButton"));
    toolbar->addWidget(refreshButton);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    m_settingsTable = new QTableWidget(this);
    m_settingsTable->setObjectName(QStringLiteral("settingsTable"));
    m_settingsTable->setColumnCount(2);
    m_settingsTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Value")});
    m_settingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_settingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_settingsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_settingsTable->verticalHeader()->hide();
    m_settingsTable->horizontalHeader()->setStretchLastSection(true);
    m_settingsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Interactive);
    m_settingsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    layout->addWidget(m_settingsTable, 1);

    connect(refreshButton, &QPushButton::clicked,
            this, &ConfigPlannerAdvView::refresh);
    connect(m_viewModel, &ConfigPlannerAdvViewModel::paramsChanged,
            this, &ConfigPlannerAdvView::syncRows);
    syncRows();
}

void ConfigPlannerAdvView::refresh()
{
    m_viewModel->Activate();
}

void ConfigPlannerAdvView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    syncColumnWidths();
    QTimer::singleShot(0, this, [this]() { syncColumnWidths(); });
}

void ConfigPlannerAdvView::syncRows()
{
    const QList<PlannerAdvancedSettingRow> params = m_viewModel->Params();
    const int sortColumn = m_settingsTable->isSortingEnabled()
        ? m_settingsTable->horizontalHeader()->sortIndicatorSection() : 0;
    const Qt::SortOrder sortOrder = m_settingsTable->isSortingEnabled()
        ? m_settingsTable->horizontalHeader()->sortIndicatorOrder()
        : Qt::AscendingOrder;
    m_settingsTable->setSortingEnabled(false);
    m_settingsTable->setRowCount(params.size());
    for (int row = 0; row < params.size(); ++row) {
        auto *nameItem = new PlannerSettingItem(params.at(row).Name);
        nameItem->setToolTip(params.at(row).Name);
        auto *valueItem = new PlannerSettingItem(params.at(row).Value);
        valueItem->setToolTip(params.at(row).Value);
        m_settingsTable->setItem(row, 0, nameItem);
        m_settingsTable->setItem(row, 1, valueItem);
    }
    m_settingsTable->setSortingEnabled(true);
    m_settingsTable->sortItems(
        qBound(0, sortColumn, m_settingsTable->columnCount() - 1), sortOrder);
    syncColumnWidths();
}

void ConfigPlannerAdvView::syncColumnWidths()
{
    const int available = m_settingsTable->viewport()->width();
    if (available > 0) {
        m_settingsTable->setColumnWidth(0, available * 2 / 5);
    }
}
