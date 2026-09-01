#include "ConfigInitialParamsView.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

ConfigInitialParamsView::ConfigInitialParamsView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigInitialParamsViewModel(this))
{
    setObjectName(QStringLiteral("ConfigInitialParamsView"));
    setStyleSheet(QStringLiteral(
        "ConfigInitialParamsView { background: #1A201D; color: #E6EDE9; }"
        "ConfigInitialParamsView QLineEdit, ConfigInitialParamsView QComboBox,"
        " ConfigInitialParamsView QTableWidget { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "ConfigInitialParamsView QPushButton { padding: 5px 12px; }"
        "QLabel#initialParamsTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#initialParamsStatus { color: #55A6D9; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("Initial Parameters"), this);
    title->setObjectName(QStringLiteral("initialParamsTitle"));
    root->addWidget(title);

    auto *description = new QLabel(
        tr("This screen calculates a sensible set of initial tuning "
           "parameters from your prop size and battery. Use it on a fresh "
           "build, then verify before flight."),
        this);
    description->setObjectName(QStringLiteral("initialParamsDescription"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *inputs = new QWidget(this);
    inputs->setObjectName(QStringLiteral("initialParamsInputs"));
    auto *grid = new QGridLayout(inputs);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setColumnMinimumWidth(0, 220);
    grid->setColumnMinimumWidth(1, 120);
    grid->setColumnMinimumWidth(2, 160);

    const QStringList labels = {
        tr("Airscrew size in inch:"),
        tr("Battery cellcount:"),
        tr("Battery cell fully charged voltage:"),
        tr("Battery cell fully discharged voltage:")
    };
    for (int row = 0; row < labels.size(); ++row) {
        auto *label = new QLabel(labels.at(row), inputs);
        label->setObjectName(
            QStringLiteral("initialParamsInputLabel%1").arg(row));
        grid->addWidget(label, row, 0);
    }

    m_propSize = new QLineEdit(inputs);
    m_propSize->setObjectName(QStringLiteral("propSizeEdit"));
    grid->addWidget(m_propSize, 0, 1);
    m_cellCount = new QLineEdit(inputs);
    m_cellCount->setObjectName(QStringLiteral("cellCountEdit"));
    grid->addWidget(m_cellCount, 1, 1);
    m_cellMax = new QLineEdit(inputs);
    m_cellMax->setObjectName(QStringLiteral("cellMaxEdit"));
    grid->addWidget(m_cellMax, 2, 1);
    m_cellMin = new QLineEdit(inputs);
    m_cellMin->setObjectName(QStringLiteral("cellMinEdit"));
    grid->addWidget(m_cellMin, 3, 1);

    auto *chemistry = new QWidget(inputs);
    auto *chemistryLayout = new QHBoxLayout(chemistry);
    chemistryLayout->setContentsMargins(8, 0, 0, 0);
    chemistryLayout->setSpacing(6);
    chemistryLayout->addWidget(new QLabel(tr("Chemistry"), chemistry));
    m_batteryType = new QComboBox(chemistry);
    m_batteryType->setObjectName(QStringLiteral("batteryTypeCombo"));
    m_batteryType->setFixedWidth(100);
    m_batteryType->addItems(ConfigInitialParamsViewModel::BatteryTypes());
    chemistryLayout->addWidget(m_batteryType);
    chemistryLayout->addStretch(1);
    grid->addWidget(chemistry, 2, 2);
    root->addWidget(inputs);

    m_tMotor = new QCheckBox(tr("Using T-Motor Flame ESC?"), this);
    m_tMotor->setObjectName(QStringLiteral("tMotorCheck"));
    root->addWidget(m_tMotor);
    m_suggested = new QCheckBox(
        tr("Add suggested settings for 4.0 and up (Battery failsafe and "
           "Fence) ?"),
        this);
    m_suggested->setObjectName(QStringLiteral("suggestedCheck"));
    root->addWidget(m_suggested);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_calculateButton = new QPushButton(
        tr("Calculate Initial Parameters"), this);
    m_calculateButton->setObjectName(
        QStringLiteral("calculateInitialParamsButton"));
    m_writeButton = new QPushButton(tr("Write to FC"), this);
    m_writeButton->setObjectName(QStringLiteral("writeInitialParamsButton"));
    buttons->addWidget(m_calculateButton);
    buttons->addWidget(m_writeButton);
    buttons->addStretch(1);
    root->addLayout(buttons);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("initialParamsStatus"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_resultsTable = new QTableWidget(this);
    m_resultsTable->setObjectName(QStringLiteral("initialParamsResults"));
    m_resultsTable->setColumnCount(4);
    m_resultsTable->setHorizontalHeaderLabels(
        {tr("Use"), tr("Parameter"), tr("Current"), tr("New")});
    m_resultsTable->setMinimumHeight(160);
    m_resultsTable->setFixedHeight(280);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Fixed);
    m_resultsTable->setColumnWidth(0, 50);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Interactive);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Interactive);
    m_resultsTable->setColumnWidth(2, 120);
    m_resultsTable->setColumnWidth(3, 120);
    root->addWidget(m_resultsTable);

    auto *docsText = new QLabel(
        tr("You can find a detailed description of initial parameter "
           "settings and tuning here. PLEASE READ IT!"),
        this);
    docsText->setObjectName(QStringLiteral("initialParamsDocsText"));
    docsText->setWordWrap(true);
    root->addWidget(docsText);
    auto *docsUrl = new QLabel(m_viewModel->DocsUrl(), this);
    docsUrl->setObjectName(QStringLiteral("initialParamsDocsUrl"));
    docsUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(docsUrl);
    root->addStretch(1);

    connect(m_propSize, &QLineEdit::textChanged,
            m_viewModel, &ConfigInitialParamsViewModel::setPropSize);
    connect(m_cellCount, &QLineEdit::textChanged,
            m_viewModel, &ConfigInitialParamsViewModel::setCellCount);
    connect(m_cellMax, &QLineEdit::textChanged,
            m_viewModel, &ConfigInitialParamsViewModel::setCellMax);
    connect(m_cellMin, &QLineEdit::textChanged,
            m_viewModel, &ConfigInitialParamsViewModel::setCellMin);
    connect(m_batteryType, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigInitialParamsViewModel::setBatteryType);
    connect(m_tMotor, &QCheckBox::toggled,
            m_viewModel, &ConfigInitialParamsViewModel::setTMotor);
    connect(m_suggested, &QCheckBox::toggled,
            m_viewModel, &ConfigInitialParamsViewModel::setSuggested);
    connect(m_calculateButton, &QPushButton::clicked,
            m_viewModel, &ConfigInitialParamsViewModel::Calculate);
    connect(m_writeButton, &QPushButton::clicked,
            this, &ConfigInitialParamsView::writeToFcRequested);
    connect(m_resultsTable, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem *item) {
        if (m_syncing || !item || item->column() != 0) {
            return;
        }
        QTableWidgetItem *nameItem =
            m_resultsTable->item(item->row(), 1);
        if (nameItem) {
            m_viewModel->setResultUse(
                nameItem->text(), item->checkState() == Qt::Checked);
        }
    });
    connect(m_viewModel, &ConfigInitialParamsViewModel::inputsChanged,
            this, &ConfigInitialParamsView::syncInputs);
    connect(m_viewModel, &ConfigInitialParamsViewModel::resultsChanged,
            this, &ConfigInitialParamsView::rebuildResults);
    connect(m_viewModel, &ConfigInitialParamsViewModel::statusChanged,
            this, &ConfigInitialParamsView::syncStatus);
    connect(m_viewModel, &ConfigInitialParamsViewModel::writingChanged,
            this, &ConfigInitialParamsView::syncActions);
    connect(m_viewModel, &ConfigInitialParamsViewModel::writeRequested,
            this, &ConfigInitialParamsView::writeRequested);

    syncInputs();
    rebuildResults();
    syncStatus();
    syncActions();
}

void ConfigInitialParamsView::setVehicleContext(
    bool plane, int firmwareMajor)
{
    m_viewModel->setVehicleContext(plane, firmwareMajor);
}

void ConfigInitialParamsView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

bool ConfigInitialParamsView::WriteToFc(
    const QStringList &availableParameters, bool connected)
{
    return m_viewModel->WriteToFc(availableParameters, connected);
}

void ConfigInitialParamsView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigInitialParamsView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigInitialParamsView::syncInputs()
{
    m_syncing = true;
    m_propSize->setText(m_viewModel->PropSize());
    m_cellCount->setText(m_viewModel->CellCount());
    m_cellMax->setText(m_viewModel->CellMax());
    m_cellMin->setText(m_viewModel->CellMin());
    m_batteryType->setCurrentText(m_viewModel->BatteryType());
    m_tMotor->setChecked(m_viewModel->TMotor());
    m_suggested->setChecked(m_viewModel->Suggested());
    m_syncing = false;
}

void ConfigInitialParamsView::rebuildResults()
{
    m_syncing = true;
    const QList<ParamCompareRow> rows = m_viewModel->Results();
    m_resultsTable->setRowCount(rows.size());
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const ParamCompareRow &row = rows.at(rowIndex);
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        if (!row.exists) {
            use->setFlags(Qt::NoItemFlags);
        }
        use->setCheckState(row.use ? Qt::Checked : Qt::Unchecked);
        m_resultsTable->setItem(rowIndex, 0, use);

        const QStringList values = {row.name, row.current, row.newValue};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_resultsTable->setItem(rowIndex, column + 1, item);
        }
    }
    m_resultsTable->setVisible(!rows.isEmpty());
    m_syncing = false;
    syncActions();
}

void ConfigInitialParamsView::syncStatus()
{
    m_statusLabel->setText(m_viewModel->Status());
}

void ConfigInitialParamsView::syncActions()
{
    const bool editable = !m_viewModel->Writing();
    m_propSize->setEnabled(editable);
    m_cellCount->setEnabled(editable);
    m_cellMax->setEnabled(editable);
    m_cellMin->setEnabled(editable);
    m_batteryType->setEnabled(editable);
    m_tMotor->setEnabled(editable);
    m_suggested->setEnabled(editable);
    m_calculateButton->setEnabled(editable);
    m_resultsTable->setEnabled(editable);
    m_writeButton->setEnabled(
        m_viewModel->HasResults() && editable);
}
