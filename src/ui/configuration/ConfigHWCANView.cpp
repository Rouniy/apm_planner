#include "ConfigHWCANView.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>

namespace {
bool numericValuesEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue;
    }
    return left == right;
}

int decimalPlaces(double increment)
{
    if (increment >= 1.0) {
        return 0;
    }
    int decimals = 0;
    double scaled = increment;
    while (decimals < 6 && std::abs(scaled - std::round(scaled)) > 1.0e-9) {
        scaled *= 10.0;
        ++decimals;
    }
    return decimals;
}
} // namespace

struct ConfigHWCANView::FieldWidgets
{
    QWidget *row = nullptr;
    QLabel *label = nullptr;
    QComboBox *combo = nullptr;
    QDoubleSpinBox *numeric = nullptr;
    QLabel *units = nullptr;
    QLabel *status = nullptr;
};

ConfigHWCANView::ConfigHWCANView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigHWCANViewModel(this))
{
    setObjectName(QStringLiteral("ConfigHWCANView"));
    setAutoFillBackground(true);
    QPalette darkPalette = palette();
    darkPalette.setColor(QPalette::Window, QColor(QStringLiteral("#1A201D")));
    darkPalette.setColor(QPalette::WindowText,
                         QColor(QStringLiteral("#E6EDE9")));
    setPalette(darkPalette);
    setStyleSheet(QStringLiteral(
        "ConfigHWCANView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#hwCanTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#hwCanEnableLabel, QLabel#hwCanRestartNote,"
        " QLabel[hwCanFieldLabel=\"true\"] { color: #C8C8C8; }"
        "QFrame#hwCanCommandPanel, QGroupBox#hwCanPortsGroup,"
        " QGroupBox#hwCanDriversGroup { border: 1px solid #2A322D;"
        " margin-top: 7px; padding-top: 7px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px;"
        " padding: 0 4px; color: #E6EDE9; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #2A322D; padding: 5px; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled,"
        " QCheckBox:disabled { color: #68736D; }"
        "QLabel#hwCanStatus { color: #34D399; }"
        "QLabel[hwCanFieldStatus=\"true\"] { color: #E5B94F; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("hwCanTitle"));
    root->addWidget(title);

    m_canEnableRow = new QWidget(this);
    m_canEnableRow->setObjectName(QStringLiteral("hwCanEnableRow"));
    auto *enableRow = new QHBoxLayout(m_canEnableRow);
    enableRow->setContentsMargins(0, 0, 0, 0);
    enableRow->setSpacing(8);
    auto *enableLabel = new QLabel(tr("Enable CAN"), m_canEnableRow);
    enableLabel->setObjectName(QStringLiteral("hwCanEnableLabel"));
    enableLabel->setFixedWidth(120);
    m_canEnableCombo = new QComboBox(m_canEnableRow);
    m_canEnableCombo->setObjectName(QStringLiteral("hwCanEnableCombo"));
    m_canEnableCombo->setMinimumWidth(200);
    enableRow->addWidget(enableLabel);
    enableRow->addWidget(m_canEnableCombo);
    enableRow->addStretch(1);
    root->addWidget(m_canEnableRow);

    m_restartNote = new QLabel(
        tr("NOTE: a restart is required after changing this option"), this);
    m_restartNote->setObjectName(QStringLiteral("hwCanRestartNote"));
    m_restartNote->setWordWrap(true);
    root->addWidget(m_restartNote);

    m_portsGroup = new QGroupBox(tr("CAN Ports"), this);
    m_portsGroup->setObjectName(QStringLiteral("hwCanPortsGroup"));
    m_portsLayout = new QVBoxLayout(m_portsGroup);
    m_portsLayout->setContentsMargins(12, 12, 12, 12);
    m_portsLayout->setSpacing(5);
    root->addWidget(m_portsGroup);

    m_driversGroup = new QGroupBox(tr("CAN Drivers"), this);
    m_driversGroup->setObjectName(QStringLiteral("hwCanDriversGroup"));
    m_driversLayout = new QVBoxLayout(m_driversGroup);
    m_driversLayout->setContentsMargins(12, 12, 12, 12);
    m_driversLayout->setSpacing(5);
    root->addWidget(m_driversGroup);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("hwCanCommandPanel"));
    auto *commands = new QVBoxLayout(panel);
    commands->setContentsMargins(12, 12, 12, 12);
    commands->setSpacing(8);

    m_startEnumerationButton = new QPushButton(
        tr("Start Enumeration"), panel);
    m_startEnumerationButton->setObjectName(
        QStringLiteral("hwCanStartEnumerationButton"));
    m_startEnumerationButton->setFixedWidth(200);
    commands->addWidget(m_startEnumerationButton, 0, Qt::AlignLeft);

    m_stopEnumerationButton = new QPushButton(
        tr("Stop Enumeration"), panel);
    m_stopEnumerationButton->setObjectName(
        QStringLiteral("hwCanStopEnumerationButton"));
    m_stopEnumerationButton->setFixedWidth(200);
    commands->addWidget(m_stopEnumerationButton, 0, Qt::AlignLeft);

    m_saveConfigButton = new QPushButton(tr("Save All Config"), panel);
    m_saveConfigButton->setObjectName(
        QStringLiteral("hwCanSaveConfigButton"));
    m_saveConfigButton->setFixedWidth(200);
    commands->addWidget(m_saveConfigButton, 0, Qt::AlignLeft);

    auto *resetRow = new QHBoxLayout;
    resetRow->setSpacing(8);
    m_factoryResetCheckBox = new QCheckBox(tr("Factory Reset"), panel);
    m_factoryResetCheckBox->setObjectName(
        QStringLiteral("hwCanFactoryResetCheckBox"));
    m_factoryResetButton = new QPushButton(tr("Reset config"), panel);
    m_factoryResetButton->setObjectName(
        QStringLiteral("hwCanFactoryResetButton"));
    m_factoryResetButton->setFixedWidth(200);
    resetRow->addWidget(m_factoryResetCheckBox);
    resetRow->addWidget(m_factoryResetButton);
    resetRow->addStretch(1);
    commands->addLayout(resetRow);
    root->addWidget(panel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("hwCanStatus"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);
    root->addStretch(1);

    connect(m_viewModel, &ConfigHWCANViewModel::optionsChanged,
            this, &ConfigHWCANView::rebuildOptions);
    connect(m_viewModel, &ConfigHWCANViewModel::structureChanged,
            this, &ConfigHWCANView::rebuildFields);
    connect(m_viewModel, &ConfigHWCANViewModel::fieldChanged,
            this, &ConfigHWCANView::syncField);
    connect(m_viewModel,
            &ConfigHWCANViewModel::selectedCanEnableChanged,
            this, &ConfigHWCANView::syncFromModel);
    connect(m_viewModel, &ConfigHWCANViewModel::statusChanged,
            this, &ConfigHWCANView::syncFromModel);
    connect(m_viewModel, &ConfigHWCANViewModel::stateChanged,
            this, &ConfigHWCANView::syncFromModel);
    connect(m_viewModel, &ConfigHWCANViewModel::writeRequested,
            this, &ConfigHWCANView::writeRequested);
    connect(m_viewModel, &ConfigHWCANViewModel::commandRequested,
            this, &ConfigHWCANView::commandRequested);

    connect(m_canEnableCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index >= 0) {
            m_viewModel->SetSelectedCanEnable(
                m_canEnableCombo->itemData(index));
        }
    });
    connect(m_startEnumerationButton, &QPushButton::clicked,
            m_viewModel, &ConfigHWCANViewModel::StartEnumeration);
    connect(m_stopEnumerationButton, &QPushButton::clicked,
            m_viewModel, &ConfigHWCANViewModel::StopEnumeration);
    connect(m_saveConfigButton, &QPushButton::clicked,
            m_viewModel, &ConfigHWCANViewModel::SaveConfig);
    connect(m_factoryResetCheckBox, &QCheckBox::toggled,
            m_viewModel, &ConfigHWCANViewModel::SetFactoryResetArmed);
    connect(m_factoryResetButton, &QPushButton::clicked,
            m_viewModel, &ConfigHWCANViewModel::FactoryReset);

    m_viewModel->setCatalog(catalog);
    rebuildOptions();
    rebuildFields();
    syncFromModel();
}

ConfigHWCANView::~ConfigHWCANView()
{
    clearFields();
}

QSize ConfigHWCANView::sizeHint() const
{
    return m_viewModel->HasModernFields()
        ? QSize(720, 680) : QSize(520, 360);
}

void ConfigHWCANView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigHWCANView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigHWCANView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigHWCANView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigHWCANView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigHWCANView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigHWCANView::commandAckReceived(
    int componentId, int command, int result)
{
    m_viewModel->commandAckReceived(componentId, command, result);
}

void ConfigHWCANView::commandSendFailed(
    int componentId, int command, const QString &reason)
{
    m_viewModel->commandSendFailed(componentId, command, reason);
}

void ConfigHWCANView::rebuildOptions()
{
    const QSignalBlocker blocker(m_canEnableCombo);
    m_canEnableCombo->clear();
    for (const ParamOption &option : m_viewModel->CanEnableOptions()) {
        m_canEnableCombo->addItem(option.text, option.value);
    }
    syncFromModel();
}

void ConfigHWCANView::rebuildFields()
{
    clearFields();
    int physicalRows = 0;
    int driverRows = 0;
    for (const ParamField &field : m_viewModel->Fields()) {
        if (field.name == QLatin1String("BRD_CAN_ENABLE")) {
            continue;
        }
        QVBoxLayout *targetLayout = nullptr;
        if (field.name.startsWith(QLatin1String("CAN_P"))) {
            targetLayout = m_portsLayout;
            ++physicalRows;
        } else if (field.name.startsWith(QLatin1String("CAN_D"))) {
            targetLayout = m_driversLayout;
            ++driverRows;
        }
        if (!targetLayout) {
            continue;
        }

        auto *widgets = new FieldWidgets;
        QWidget *group = targetLayout == m_portsLayout
            ? static_cast<QWidget *>(m_portsGroup)
            : static_cast<QWidget *>(m_driversGroup);
        widgets->row = new QWidget(group);
        widgets->row->setObjectName(
            QStringLiteral("hwCanFieldRow_%1").arg(field.name));
        auto *grid = new QGridLayout(widgets->row);
        grid->setContentsMargins(0, 2, 0, 2);
        grid->setHorizontalSpacing(8);
        grid->setColumnMinimumWidth(0, 250);
        grid->setColumnMinimumWidth(1, 210);
        grid->setColumnStretch(4, 1);

        widgets->label = new QLabel(field.label, widgets->row);
        widgets->label->setObjectName(
            QStringLiteral("hwCanFieldLabel_%1").arg(field.name));
        widgets->label->setProperty("hwCanFieldLabel", true);
        widgets->label->setToolTip(field.description);
        widgets->label->setWordWrap(true);
        grid->addWidget(widgets->label, 0, 0);

        if (field.editorKind == ParamField::EditorKind::Combo) {
            widgets->combo = new QComboBox(widgets->row);
            widgets->combo->setObjectName(
                QStringLiteral("hwCanFieldEditor_%1").arg(field.name));
            for (const ParamOption &option : field.options) {
                widgets->combo->addItem(option.text, option.value);
            }
            connect(widgets->combo,
                    QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, field, widgets](int index) {
                if (index >= 0) {
                    m_viewModel->setFieldValue(
                        field.name, widgets->combo->itemData(index));
                }
            });
            grid->addWidget(widgets->combo, 0, 1);
        } else {
            widgets->numeric = new QDoubleSpinBox(widgets->row);
            widgets->numeric->setObjectName(
                QStringLiteral("hwCanFieldEditor_%1").arg(field.name));
            widgets->numeric->setKeyboardTracking(false);
            widgets->numeric->setDecimals(decimalPlaces(field.increment));
            widgets->numeric->setSingleStep(field.increment > 0.0
                                                ? field.increment : 1.0);
            if (field.hasRange) {
                widgets->numeric->setRange(field.minimum, field.maximum);
            } else {
                widgets->numeric->setRange(-2147483647.0, 2147483647.0);
            }
            connect(widgets->numeric, &QDoubleSpinBox::editingFinished,
                    this, [this, field, widgets]() {
                m_viewModel->setFieldValue(
                    field.name, widgets->numeric->value());
            });
            grid->addWidget(widgets->numeric, 0, 1);
        }

        widgets->units = new QLabel(field.units, widgets->row);
        widgets->units->setObjectName(
            QStringLiteral("hwCanFieldUnits_%1").arg(field.name));
        grid->addWidget(widgets->units, 0, 2);
        widgets->status = new QLabel(field.status, widgets->row);
        widgets->status->setObjectName(
            QStringLiteral("hwCanFieldStatus_%1").arg(field.name));
        widgets->status->setProperty("hwCanFieldStatus", true);
        grid->addWidget(widgets->status, 0, 3);
        targetLayout->addWidget(widgets->row);
        m_fields.insert(field.name, widgets);
    }
    m_portsGroup->setVisible(physicalRows > 0);
    m_driversGroup->setVisible(driverRows > 0);
    syncFromModel();
}

void ConfigHWCANView::syncField(const QString &name)
{
    FieldWidgets *widgets = m_fields.value(name);
    if (!widgets) {
        return;
    }
    ParamField current;
    bool found = false;
    for (const ParamField &field : m_viewModel->Fields()) {
        if (field.name == name) {
            current = field;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    if (widgets->combo) {
        const QSignalBlocker blocker(widgets->combo);
        int index = -1;
        for (int option = 0; option < widgets->combo->count(); ++option) {
            if (numericValuesEqual(widgets->combo->itemData(option),
                                   current.value)) {
                index = option;
                break;
            }
        }
        widgets->combo->setCurrentIndex(index);
    }
    if (widgets->numeric) {
        const QSignalBlocker blocker(widgets->numeric);
        widgets->numeric->setValue(current.value.toDouble());
    }
    const bool enabled = m_viewModel->IsConnected()
        && m_viewModel->SnapshotReady() && !m_viewModel->VehicleArmed()
        && !m_viewModel->Busy()
        && !current.readOnly;
    if (widgets->combo) {
        widgets->combo->setEnabled(enabled);
    }
    if (widgets->numeric) {
        widgets->numeric->setEnabled(enabled);
    }
    widgets->status->setText(current.status);
}

void ConfigHWCANView::syncFromModel()
{
    m_canEnableRow->setVisible(m_viewModel->HasCanEnableParameter());
    m_restartNote->setVisible(m_viewModel->HasCanEnableParameter());
    {
        const QSignalBlocker blocker(m_canEnableCombo);
        int selected = -1;
        for (int index = 0; index < m_canEnableCombo->count(); ++index) {
            if (numericValuesEqual(m_canEnableCombo->itemData(index),
                                   m_viewModel->SelectedCanEnable())) {
                selected = index;
                break;
            }
        }
        m_canEnableCombo->setCurrentIndex(selected);
    }
    {
        const QSignalBlocker blocker(m_factoryResetCheckBox);
        m_factoryResetCheckBox->setChecked(
            m_viewModel->FactoryResetArmed());
    }
    m_canEnableCombo->setEnabled(m_viewModel->CanEditCanEnable());
    m_startEnumerationButton->setEnabled(
        m_viewModel->CanIssueCommands());
    m_stopEnumerationButton->setEnabled(
        m_viewModel->CanIssueCommands());
    m_saveConfigButton->setEnabled(m_viewModel->CanIssueCommands());
    m_factoryResetCheckBox->setEnabled(
        m_viewModel->CanIssueCommands());
    m_factoryResetButton->setEnabled(m_viewModel->CanFactoryReset());
    m_statusLabel->setText(m_viewModel->Status());
    const QStringList fieldNames = m_fields.keys();
    for (const QString &name : fieldNames) {
        syncField(name);
    }
}

void ConfigHWCANView::clearFields()
{
    const QList<FieldWidgets *> widgets = m_fields.values();
    m_fields.clear();
    for (FieldWidgets *field : widgets) {
        if (field) {
            delete field->row;
            delete field;
        }
    }
}
