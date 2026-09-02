#include "ConfigDroneCanView.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

ConfigDroneCanView::ConfigDroneCanView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigDroneCanViewModel(this))
{
    setObjectName(QStringLiteral("ConfigDroneCanView"));
    setAutoFillBackground(true);
    QPalette darkPalette = palette();
    darkPalette.setColor(QPalette::Window, QColor(QStringLiteral("#1A201D")));
    darkPalette.setColor(QPalette::WindowText,
                         QColor(QStringLiteral("#E6EDE9")));
    setPalette(darkPalette);
    setStyleSheet(QStringLiteral(
        "ConfigDroneCanView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#droneCanTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel { color: #C8C8C8; }"
        "QComboBox, QLineEdit, QTableWidget {"
        " background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 4px; }"
        "QHeaderView::section { background: #232B27; color: #E6EDE9;"
        " border: 1px solid #34403A; padding: 4px; }"
        "QComboBox:disabled, QCheckBox:disabled,"
        " QLineEdit:disabled { color: #68736D; }"
        "QLabel#droneCanStatus, QLabel#droneCanNodeStatus {"
        " color: #34D399; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("droneCanTitle"));
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_interfaceSelector = new QComboBox(this);
    m_interfaceSelector->setObjectName(
        QStringLiteral("DroneCanInterfaceSelector"));
    m_interfaceSelector->setMinimumWidth(160);
    m_interfaceSelector->addItems(m_viewModel->BusOptions());
    toolbar->addWidget(m_interfaceSelector);

    m_connectButton = new QPushButton(this);
    m_connectButton->setObjectName(
        QStringLiteral("DroneCanConnectButton"));
    toolbar->addWidget(m_connectButton);
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(
        QStringLiteral("DroneCanRefreshButton"));
    toolbar->addWidget(m_refreshButton);
    m_filterButton = new QPushButton(tr("Filter"), this);
    m_filterButton->setObjectName(
        QStringLiteral("DroneCanFilterButton"));
    toolbar->addWidget(m_filterButton);
    m_statsButton = new QPushButton(tr("Stats"), this);
    m_statsButton->setObjectName(QStringLiteral("DroneCanStatsButton"));
    toolbar->addWidget(m_statsButton);
    m_inspectorButton = new QPushButton(tr("Bus Inspector"), this);
    m_inspectorButton->setObjectName(QStringLiteral("BusInspectorBtn"));
    toolbar->addWidget(m_inspectorButton);
    m_logCheckBox = new QCheckBox(tr("Log"), this);
    m_logCheckBox->setObjectName(QStringLiteral("DroneCanLogCheckBox"));
    toolbar->addWidget(m_logCheckBox);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    auto *scopeNote = new QLabel(
        tr("This slice supports node discovery and GetNodeInfo identity "
           "plus read-only DroneCAN parameters over MAVLink CAN1/CAN2. "
           "Direct SLCAN, multicast, frame filters, logging, parameter "
           "writes, restart and firmware operations are being ported next."),
        this);
    scopeNote->setObjectName(QStringLiteral("droneCanTransportScope"));
    scopeNote->setWordWrap(true);
    root->addWidget(scopeNote);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("droneCanStatus"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_nodesTable = new QTableWidget(this);
    m_nodesTable->setObjectName(QStringLiteral("droneCanNodes"));
    m_nodesTable->setColumnCount(9);
    m_nodesTable->setHorizontalHeaderLabels({
        tr("ID"), tr("Name"), tr("Mode"), tr("Health"), tr("Uptime"),
        tr("HW Version"), tr("SW Version"), tr("SW CRC"), tr("Menu")
    });
    m_nodesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nodesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nodesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nodesTable->verticalHeader()->hide();
    m_nodesTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_nodesTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    root->addWidget(m_nodesTable, 2);

    auto *actions = new QHBoxLayout;
    const QStringList actionLabels = {
        tr("Get Parameters"), tr("Restart Node"), tr("Update Firmware…"),
        tr("Write"), tr("Save to Flash"), tr("Erase"),
        tr("Import .param…"), tr("Export .param…")
    };
    const QStringList actionNames = {
        QStringLiteral("DroneCanGetParametersButton"),
        QStringLiteral("DroneCanRestartNodeButton"),
        QStringLiteral("FirmwareUpdateBtn"),
        QStringLiteral("DroneCanWriteButton"),
        QStringLiteral("DroneCanSaveConfigButton"),
        QStringLiteral("DroneCanEraseConfigButton"),
        QStringLiteral("DroneCanImportButton"),
        QStringLiteral("DroneCanExportButton")
    };
    for (int index = 0; index < actionLabels.size(); ++index) {
        auto *button = new QPushButton(actionLabels.at(index), this);
        button->setObjectName(actionNames.at(index));
        button->setEnabled(false);
        actions->addWidget(button);
        m_nodeOperationButtons.append(button);
        if (index == 0) {
            m_getParametersButton = button;
        }
    }
    actions->addStretch(1);
    root->addLayout(actions);

    m_nodeStatusLabel = new QLabel(this);
    m_nodeStatusLabel->setObjectName(QStringLiteral("droneCanNodeStatus"));
    m_nodeStatusLabel->setWordWrap(true);
    root->addWidget(m_nodeStatusLabel);

    auto *filterRow = new QHBoxLayout;
    m_parameterSearch = new QLineEdit(this);
    m_parameterSearch->setObjectName(
        QStringLiteral("DroneCanParameterSearch"));
    m_parameterSearch->setPlaceholderText(
        tr("Search parameters (≥2 chars)…"));
    m_parameterSearch->setEnabled(false);
    m_parameterSearch->setMaximumWidth(260);
    filterRow->addWidget(m_parameterSearch);
    m_modifiedOnlyCheckBox = new QCheckBox(tr("Modified only"), this);
    m_modifiedOnlyCheckBox->setObjectName(
        QStringLiteral("DroneCanModifiedOnlyCheckBox"));
    m_modifiedOnlyCheckBox->setEnabled(false);
    filterRow->addWidget(m_modifiedOnlyCheckBox);
    filterRow->addStretch(1);
    root->addLayout(filterRow);

    m_paramsTable = new QTableWidget(0, 6, this);
    m_paramsTable->setObjectName(QStringLiteral("droneCanNodeParams"));
    m_paramsTable->setHorizontalHeaderLabels({
        tr("Fav"), tr("Name"), tr("Value"), tr("Min"), tr("Max"),
        tr("Default")
    });
    m_paramsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_paramsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_paramsTable->verticalHeader()->hide();
    m_paramsTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_paramsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_paramsTable->setEnabled(false);
    m_paramsTable->setMaximumHeight(180);
    root->addWidget(m_paramsTable);

    connect(m_viewModel, &ConfigDroneCanViewModel::statusChanged,
            this, &ConfigDroneCanView::syncFromModel);
    connect(m_viewModel, &ConfigDroneCanViewModel::nodeStatusChanged,
            this, &ConfigDroneCanView::syncFromModel);
    connect(m_viewModel, &ConfigDroneCanViewModel::stateChanged,
            this, &ConfigDroneCanView::syncFromModel);
    connect(m_viewModel, &ConfigDroneCanViewModel::nodesChanged,
            this, &ConfigDroneCanView::rebuildNodes);
    connect(m_viewModel, &ConfigDroneCanViewModel::parametersChanged,
            this, &ConfigDroneCanView::rebuildParameters);
    connect(m_viewModel, &ConfigDroneCanViewModel::selectionChanged,
            this, &ConfigDroneCanView::syncSelection);
    connect(m_viewModel,
            &ConfigDroneCanViewModel::canForwardingRequested,
            this, &ConfigDroneCanView::canForwardingRequested);

    connect(m_interfaceSelector,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_viewModel, &ConfigDroneCanViewModel::SetSelectedBusIndex);
    connect(m_connectButton, &QPushButton::clicked,
            m_viewModel, &ConfigDroneCanViewModel::ToggleConnect);
    connect(m_refreshButton, &QPushButton::clicked,
            m_viewModel, &ConfigDroneCanViewModel::Refresh);
    connect(m_getParametersButton, &QPushButton::clicked,
            m_viewModel, &ConfigDroneCanViewModel::GetParameters);
    connect(m_parameterSearch, &QLineEdit::textChanged,
            this, &ConfigDroneCanView::applyParameterFilter);
    connect(m_nodesTable, &QTableWidget::cellClicked,
            this, [this](int row, int column) {
        Q_UNUSED(column)
        if (auto *item = m_nodesTable->item(row, 0)) {
            m_viewModel->SelectNode(item->data(Qt::UserRole).toInt());
        }
    });

    m_filterButton->setEnabled(false);
    m_filterButton->setToolTip(tr("DroneCAN frame filters are not ported yet."));
    m_statsButton->setEnabled(false);
    m_statsButton->setToolTip(tr("DroneCAN statistics are not ported yet."));
    m_inspectorButton->setEnabled(false);
    m_inspectorButton->setToolTip(
        tr("The DroneCAN bus inspector is a separate porting slice."));
    m_logCheckBox->setEnabled(false);
    m_logCheckBox->setToolTip(tr("Raw CAN logging is not ported yet."));
    syncFromModel();
}

ConfigDroneCanView::~ConfigDroneCanView()
{
    m_viewModel->Disconnect();
}

QSize ConfigDroneCanView::sizeHint() const
{
    return QSize(980, 760);
}

void ConfigDroneCanView::setVehicleConnected(bool connected)
{
    m_viewModel->setVehicleConnected(connected);
}

void ConfigDroneCanView::setVehicleArmed(bool armed)
{
    m_viewModel->setVehicleArmed(armed);
}

void ConfigDroneCanView::canFrameReceived(
    int bus, quint32 id, const QByteArray &data, bool canFd, qint64 nowMs)
{
    m_viewModel->observeCanFrame(bus, id, data, canFd, nowMs);
}

void ConfigDroneCanView::forwardingAckReceived(int result)
{
    m_viewModel->forwardingAckReceived(result);
}

void ConfigDroneCanView::forwardingSendFailed(const QString &reason)
{
    m_viewModel->forwardingSendFailed(reason);
}

void ConfigDroneCanView::syncFromModel()
{
    {
        const QSignalBlocker blocker(m_interfaceSelector);
        m_interfaceSelector->setCurrentIndex(
            m_viewModel->SelectedBusIndex());
    }
    m_interfaceSelector->setEnabled(m_viewModel->CanChangeInterface());
    m_connectButton->setText(m_viewModel->ConnectLabel());
    m_connectButton->setEnabled(m_viewModel->CanToggleConnection());
    m_refreshButton->setEnabled(
        m_viewModel->IsConnected() && !m_viewModel->IsBusy());
    m_statusLabel->setText(m_viewModel->Status());
    m_nodeStatusLabel->setText(m_viewModel->NodeStatus());
    m_getParametersButton->setEnabled(
        m_viewModel->CanGetParameters());
    m_parameterSearch->setEnabled(
        m_viewModel->CanFilterParameters());
    m_paramsTable->setEnabled(
        m_viewModel->IsConnected()
        && m_viewModel->SelectedNodeId() >= 1);
}

void ConfigDroneCanView::rebuildNodes()
{
    const QList<DroneCanNode> nodes = m_viewModel->Nodes();
    const QSignalBlocker blocker(m_nodesTable);
    m_nodesTable->setRowCount(nodes.size());
    for (int row = 0; row < nodes.size(); ++row) {
        const DroneCanNode &node = nodes.at(row);
        const QStringList values = {
            QString::number(node.id), node.name, node.mode, node.health,
            QString::number(node.uptimeSeconds), node.hardwareVersion,
            node.softwareVersion, node.softwareCrc, tr("Menu")
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                item->setData(Qt::UserRole, node.id);
            }
            m_nodesTable->setItem(row, column, item);
        }
    }
    syncSelection();
}

void ConfigDroneCanView::rebuildParameters()
{
    const QList<DroneCanParameter> parameters = m_viewModel->Parameters();
    const QSignalBlocker blocker(m_paramsTable);
    m_paramsTable->setRowCount(parameters.size());
    for (int row = 0; row < parameters.size(); ++row) {
        const DroneCanParameter &parameter = parameters.at(row);
        const QStringList values = {
            QString(), parameter.name, parameter.value,
            parameter.minimumValue, parameter.maximumValue,
            parameter.defaultValue
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            if (column == 1) {
                item->setData(Qt::UserRole, parameter.index);
            }
            m_paramsTable->setItem(row, column, item);
        }
    }
    if (parameters.isEmpty() && !m_parameterSearch->text().isEmpty()) {
        const QSignalBlocker searchBlocker(m_parameterSearch);
        m_parameterSearch->clear();
    }
    applyParameterFilter();
}

void ConfigDroneCanView::applyParameterFilter()
{
    const QString filter = m_parameterSearch->text().trimmed();
    const bool active = filter.size() >= 2;
    for (int row = 0; row < m_paramsTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_paramsTable->item(row, 1);
        m_paramsTable->setRowHidden(
            row, active && name
                && !name->text().contains(filter, Qt::CaseInsensitive));
    }
}

void ConfigDroneCanView::syncSelection()
{
    const int selectedNodeId = m_viewModel->SelectedNodeId();
    const QSignalBlocker blocker(m_nodesTable);
    m_nodesTable->clearSelection();
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_nodesTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == selectedNodeId) {
            m_nodesTable->selectRow(row);
            break;
        }
    }
}
