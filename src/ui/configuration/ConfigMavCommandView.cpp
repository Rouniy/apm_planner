#include "ConfigMavCommandView.h"

#include "ConfigMavCommandViewModel.h"
#include "ui/BackstageView.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

ConfigMavCommandView::ConfigMavCommandView(QWidget *parent)
    : ConfigMavCommandView(new ConfigMavCommandViewModel, parent)
{
    m_viewModel->setParent(this);
}

ConfigMavCommandView::ConfigMavCommandView(
    ConfigMavCommandViewModel *viewModel, QWidget *parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
{
    Q_ASSERT(m_viewModel);
    setObjectName(QStringLiteral("ConfigMavCommandView"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(0);

    auto *title = new QLabel(tr("Mission Command List"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_statusLabel = new QLabel(m_viewModel->Status(), this);
    m_statusLabel->setWordWrap(true);
    layout->addSpacing(4);
    layout->addWidget(m_statusLabel);
    layout->addSpacing(12);

    m_table = new QTableView(this);
    m_table->setModel(m_viewModel);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const int widths[] = {70, 190, 100, 100, 100, 100, 100, 100, 100};
    for (int column = 0; column < ConfigMavCommandViewModel::ColumnCount;
         ++column) {
        m_table->setColumnWidth(column, widths[column]);
    }
    layout->addWidget(m_table, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->setContentsMargins(0, 12, 0, 0);
    auto *add = new QPushButton(tr("Add"), this);
    auto *remove = new QPushButton(tr("Remove"), this);
    auto *save = new QPushButton(tr("Save"), this);
    auto *reload = new QPushButton(tr("Reload"), this);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(save);
    buttons->addWidget(reload);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(m_viewModel, &ConfigMavCommandViewModel::statusChanged,
            m_statusLabel, &QLabel::setText);
    connect(add, &QPushButton::clicked,
            this, &ConfigMavCommandView::addCommand);
    connect(remove, &QPushButton::clicked,
            this, &ConfigMavCommandView::removeCommand);
    connect(save, &QPushButton::clicked,
            m_viewModel, &ConfigMavCommandViewModel::SaveCommand);
    connect(reload, &QPushButton::clicked, this, [this]() {
        m_viewModel->ReloadCommand();
        m_table->clearSelection();
    });
}

ConfigMavCommandViewModel *ConfigMavCommandView::viewModel() const
{
    return m_viewModel;
}

QTableView *ConfigMavCommandView::commandTable() const
{
    return m_table;
}

void ConfigMavCommandView::addCommand()
{
    bool accepted = false;
    const int id = QInputDialog::getInt(
        this, tr("Add Mission Command"),
        tr("MAV_CMD numeric ID (0..65535)"), 0, 0, 65535, 1,
        &accepted);
    if (!accepted) return;

    const QVector<MissionCommandDefinition> commands =
        m_viewModel->Commands();
    const bool duplicateId = std::any_of(
        commands.cbegin(), commands.cend(), [id](const auto &row) {
            return row.Id == id;
        });
    if (duplicateId) {
        m_viewModel->AddCommand(id);
        return;
    }

    QString name = MissionCommandCatalog::CanonicalName(
        static_cast<quint16>(id));
    if (name.isEmpty()) {
        name = QInputDialog::getText(
            this, tr("Add Mission Command"),
            tr("Command name (letters, digits and underscores)"),
            QLineEdit::Normal, QStringLiteral("NEW_COMMAND"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
    }
    const int row = m_viewModel->AddCommand(id, name);
    if (row >= 0) {
        m_table->selectRow(row);
        m_table->scrollTo(m_viewModel->index(row, 0));
    }
}

void ConfigMavCommandView::removeCommand()
{
    const QModelIndex current = m_table->currentIndex();
    if (current.isValid() && m_viewModel->RemoveCommand(current.row())) {
        m_table->clearSelection();
    }
}

BackstagePage configMavCommandBackstagePage()
{
    BackstagePage page;
    page.id = QStringLiteral("ConfigMavCommandView");
    page.header = ConfigMavCommandView::tr("Mission Command List");
    page.isSub = true;
    page.isAdvanced = true;
    page.requiresConnection = false;
    page.allowsPartialParameters = true;
    page.factory = [](QWidget *parent) {
        return new ConfigMavCommandView(parent);
    };
    return page;
}
