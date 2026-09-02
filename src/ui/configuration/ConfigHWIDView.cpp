#include "ConfigHWIDView.h"

#include "ConfigHWIDViewModel.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

ConfigHWIDView::ConfigHWIDView(QWidget *parent)
    : ConfigHWIDView(new ConfigHWIDViewModel, parent)
{
    m_viewModel->setParent(this);
}

ConfigHWIDView::ConfigHWIDView(ConfigHWIDViewModel *viewModel, QWidget *parent)
    : QWidget(parent), m_viewModel(viewModel)
{
    Q_ASSERT(m_viewModel);
    setObjectName(QStringLiteral("ConfigHWIDView"));
    buildUi();
}

void ConfigHWIDView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigHWIDView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#hwIdTitle { color: #E8E8E8; font-size: 16px; font-weight: bold; }"
        "ConfigHWIDView QPushButton { padding: 5px 12px; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(0);

    // MP10: horizontal StackPanel, spacing 8, bottom margin 12.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 12);
    header->setSpacing(8);
    m_title = new QLabel(tr("HW ID"), this);
    m_title->setObjectName(QStringLiteral("hwIdTitle"));
    header->addWidget(m_title, 0, Qt::AlignVCenter);
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("hwIdRefreshButton"));
    header->addWidget(m_refreshButton, 0, Qt::AlignVCenter);
    header->addStretch(1);
    layout->addLayout(header);

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setObjectName(QStringLiteral("hwIdSortProxy"));
    m_proxy->setSourceModel(m_viewModel);
    m_proxy->setSortRole(ConfigHWIDViewModel::SortRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(true);

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("hwIdTable"));
    m_table->setModel(m_proxy);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); // IsReadOnly
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(true);                                 // GridLinesVisibility=All
    m_table->setAlternatingRowColors(false);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    QHeaderView *columns = m_table->horizontalHeader();
    columns->setSectionResizeMode(QHeaderView::Interactive);    // CanUserResizeColumns
    columns->setStretchLastSection(true);                       // DevType Width="*"
    columns->setSortIndicator(0, Qt::AscendingOrder);
    columns->setSortIndicatorShown(true);
    const int widths[] = {200, 120, 100, 70, 90};
    for (int column = 0; column < static_cast<int>(sizeof(widths) / sizeof(widths[0]));
         ++column) {
        m_table->setColumnWidth(column, widths[column]);
    }
    m_table->setSortingEnabled(true);                           // CanUserSortColumns
    layout->addWidget(m_table, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        const int component = m_viewModel->selectedComponent();
        emit refreshRequested(component > 0 ? component : 1);
    });
}

void ConfigHWIDView::setParameterSnapshot(const QList<ParameterRecord> &records,
                                          int preferredComponent)
{
    m_viewModel->setParameterSnapshot(records, preferredComponent);
}
