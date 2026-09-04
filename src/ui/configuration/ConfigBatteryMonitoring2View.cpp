#include "ConfigBatteryMonitoring2View.h"

#include "services/SpeechSettings.h"

#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QVBoxLayout>

#include <cmath>

namespace
{

QDoubleSpinBox *numericEditor(
    const QString &objectName, int decimals, double minimum,
    double maximum, double step, QWidget *parent)
{
    auto *editor = new QDoubleSpinBox(parent);
    editor->setObjectName(objectName);
    editor->setDecimals(decimals);
    editor->setRange(minimum, maximum);
    editor->setSingleStep(step);
    editor->setMinimumWidth(160);
    editor->setKeyboardTracking(false);
    return editor;
}

QLabel *fieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("batteryFieldLabel", true);
    label->setWordWrap(true);
    return label;
}

} // namespace

ConfigBatteryMonitoring2View::ConfigBatteryMonitoring2View(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigBatteryMonitoring2ViewModel(this)),
      m_catalog(catalog)
{
    setObjectName(QStringLiteral("ConfigBatteryMonitoring2View"));
    setAutoFillBackground(true);
    QPalette pagePalette = palette();
    pagePalette.setColor(QPalette::Window,
                         QColor(QStringLiteral("#1A201D")));
    pagePalette.setColor(QPalette::WindowText,
                         QColor(QStringLiteral("#E6EDE9")));
    setPalette(pagePalette);
    setStyleSheet(QStringLiteral(
        "ConfigBatteryMonitoring2View, QWidget#battery2EditorPanel {"
        " background: #1A201D; color: #E6EDE9; }"
        "QLabel#battery2Title { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel[batteryFieldLabel=\"true\"] { color: #C8C8C8; }"
        "QComboBox, QSpinBox, QDoubleSpinBox, QPushButton {"
        " background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 4px; }"
        "QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,"
        " QPushButton:disabled { color: #68736D; }"
        "QGroupBox#Calibration { border: 1px solid #5A5A5A;"
        " border-radius: 4px; margin-top: 8px; padding-top: 10px; }"
        "QGroupBox#Calibration::title { subcontrol-origin: margin;"
        " left: 10px; padding: 0 4px; }"
        "QLabel#CalibrationStatus { color: #34D399; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("battery2Title"));
    root->addWidget(title);

    m_editorPanel = new QWidget(this);
    m_editorPanel->setObjectName(QStringLiteral("battery2EditorPanel"));
    m_editorPanel->setMaximumWidth(560);
    auto *fields = new QGridLayout(m_editorPanel);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(7);
    fields->setColumnMinimumWidth(0, 180);
    fields->setColumnStretch(1, 1);

    int row = 0;
    fields->addWidget(fieldLabel(tr("Monitor"), m_editorPanel), row, 0);
    m_monitor = new QComboBox(m_editorPanel);
    m_monitor->setObjectName(QStringLiteral("Monitor"));
    m_monitor->setMinimumWidth(280);
    fields->addWidget(m_monitor, row++, 1);

    fields->addWidget(fieldLabel(tr("Capacity (mAh)"), m_editorPanel),
                      row, 0);
    m_capacity = numericEditor(QStringLiteral("Capacity"), 0, 0.0,
                               1000000000.0, 100.0, m_editorPanel);
    fields->addWidget(m_capacity, row++, 1, Qt::AlignLeft);

    fields->addWidget(fieldLabel(tr("Voltage pin"), m_editorPanel),
                      row, 0);
    m_voltPin = new QComboBox(m_editorPanel);
    m_voltPin->setObjectName(QStringLiteral("VoltPin"));
    m_voltPin->setMinimumWidth(280);
    fields->addWidget(m_voltPin, row++, 1, Qt::AlignLeft);

    fields->addWidget(fieldLabel(tr("Current pin"), m_editorPanel),
                      row, 0);
    m_currPin = new QComboBox(m_editorPanel);
    m_currPin->setObjectName(QStringLiteral("CurrPin"));
    m_currPin->setMinimumWidth(280);
    fields->addWidget(m_currPin, row++, 1, Qt::AlignLeft);

    fields->addWidget(fieldLabel(
                          tr("Volt mult (BATT2_VOLT_MULT)"),
                          m_editorPanel), row, 0);
    m_voltMult = numericEditor(QStringLiteral("VoltMult"), 6, 0.0,
                               1000000.0, 0.001, m_editorPanel);
    fields->addWidget(m_voltMult, row++, 1, Qt::AlignLeft);

    m_ampPerVoltLabel = fieldLabel(QString(), m_editorPanel);
    fields->addWidget(m_ampPerVoltLabel, row, 0);
    m_ampPerVolt = numericEditor(QStringLiteral("AmpPerVolt"), 6,
                                 -1000000.0, 1000000.0, 0.001,
                                 m_editorPanel);
    fields->addWidget(m_ampPerVolt, row++, 1, Qt::AlignLeft);

    fields->addWidget(fieldLabel(
                          tr("Amp offset (BATT2_AMP_OFFSET)"),
                          m_editorPanel), row, 0);
    m_ampOffset = numericEditor(QStringLiteral("AmpOffset"), 6,
                                -1000000.0, 1000000.0, 0.001,
                                m_editorPanel);
    fields->addWidget(m_ampOffset, row++, 1, Qt::AlignLeft);

    auto *calibration = new QGroupBox(tr("Calibration"), m_editorPanel);
    calibration->setObjectName(QStringLiteral("Calibration"));
    auto *calibrationGrid = new QGridLayout(calibration);
    calibrationGrid->setColumnMinimumWidth(0, 280);
    calibrationGrid->setHorizontalSpacing(8);
    calibrationGrid->setVerticalSpacing(5);

    calibrationGrid->addWidget(
        fieldLabel(tr("1. Measured battery voltage"), calibration),
        0, 0);
    m_measuredVoltage = numericEditor(
        QStringLiteral("MeasuredVoltage"), 2, 0.0, 100000.0, 0.1,
        calibration);
    calibrationGrid->addWidget(m_measuredVoltage, 0, 1);
    m_applyVoltage = new QPushButton(tr("Apply"), calibration);
    m_applyVoltage->setObjectName(
        QStringLiteral("ApplyVoltageCalibration"));
    calibrationGrid->addWidget(m_applyVoltage, 0, 2);

    calibrationGrid->addWidget(
        fieldLabel(tr("2. Battery voltage (Calced)"), calibration),
        1, 0);
    m_liveVoltage = new QLabel(QStringLiteral("—"), calibration);
    m_liveVoltage->setObjectName(QStringLiteral("LiveVoltage"));
    calibrationGrid->addWidget(m_liveVoltage, 1, 1);

    calibrationGrid->addWidget(
        fieldLabel(tr("3. Voltage divider (Calced)"), calibration),
        2, 0);
    m_calculatedVoltMult = numericEditor(
        QStringLiteral("CalculatedVoltMult"), 6, 0.0,
        1000000.0, 0.001, calibration);
    calibrationGrid->addWidget(m_calculatedVoltMult, 2, 1);

    calibrationGrid->addWidget(
        fieldLabel(tr("4. Measured current"), calibration), 3, 0);
    m_measuredCurrent = numericEditor(
        QStringLiteral("MeasuredCurrent"), 2, -100000.0, 100000.0,
        0.1, calibration);
    calibrationGrid->addWidget(m_measuredCurrent, 3, 1);
    m_applyCurrent = new QPushButton(tr("Apply"), calibration);
    m_applyCurrent->setObjectName(
        QStringLiteral("ApplyCurrentCalibration"));
    calibrationGrid->addWidget(m_applyCurrent, 3, 2);

    calibrationGrid->addWidget(
        fieldLabel(tr("5. Current (Calced)"), calibration), 4, 0);
    m_liveCurrent = new QLabel(QStringLiteral("—"), calibration);
    m_liveCurrent->setObjectName(QStringLiteral("LiveCurrent"));
    calibrationGrid->addWidget(m_liveCurrent, 4, 1);

    calibrationGrid->addWidget(
        fieldLabel(tr("6. Amps per volt"), calibration), 5, 0);
    m_calculatedAmpPerVolt = numericEditor(
        QStringLiteral("CalculatedAmpPerVolt"), 6, -1000000.0,
        1000000.0, 0.001, calibration);
    calibrationGrid->addWidget(m_calculatedAmpPerVolt, 5, 1);

    m_status = new QLabel(calibration);
    m_status->setObjectName(QStringLiteral("CalibrationStatus"));
    m_status->setWordWrap(true);
    calibrationGrid->addWidget(m_status, 6, 0, 1, 3);
    fields->addWidget(calibration, row++, 0, 1, 2);

    m_alertOnLowBattery = new QCheckBox(
        tr("MP Alert on Low Battery"), m_editorPanel);
    m_alertOnLowBattery->setObjectName(
        QStringLiteral("CHK_speechbattery"));
    const QSettings settings;
    m_alertOnLowBattery->setChecked(
        settings.value(QStringLiteral("speechbatteryenabled"), false)
            .toBool()
        && settings.value(QStringLiteral("speechenable"), false).toBool());
    fields->addWidget(m_alertOnLowBattery, row++, 0, 1, 2);

    root->addWidget(m_editorPanel, 0, Qt::AlignLeft);
    root->addStretch(1);

    connect(m_viewModel, &BatteryMonitorInstanceModel::fieldChanged,
            this, &ConfigBatteryMonitoring2View::syncFields);
    connect(m_viewModel, &BatteryMonitorInstanceModel::structureChanged,
            this, &ConfigBatteryMonitoring2View::syncFields);
    connect(m_viewModel, &BatteryMonitorInstanceModel::liveValuesChanged,
            this, &ConfigBatteryMonitoring2View::syncLiveValues);
    connect(m_viewModel, &BatteryMonitorInstanceModel::stateChanged,
            this, &ConfigBatteryMonitoring2View::syncFields);
    connect(m_viewModel, &BatteryMonitorInstanceModel::statusChanged,
            this, &ConfigBatteryMonitoring2View::syncState);
    connect(m_viewModel, &BatteryMonitorInstanceModel::writeRequested,
            this, &ConfigBatteryMonitoring2View::writeRequested);

    connect(m_monitor, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        writeField(m_viewModel->MonitorParameter(),
                   m_monitor->itemData(index));
    });
    connect(m_capacity, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->CapacityParameter(), m_capacity->value());
    });
    connect(m_voltPin, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        writeField(m_viewModel->VoltPinParameter(),
                   m_voltPin->itemData(index));
    });
    connect(m_currPin, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        writeField(m_viewModel->CurrPinParameter(),
                   m_currPin->itemData(index));
    });
    connect(m_voltMult, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->VoltMultiplierParameter(),
                   m_voltMult->value());
    });
    connect(m_ampPerVolt, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->AmpPerVoltParameter(),
                   m_ampPerVolt->value());
    });
    connect(m_ampOffset, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->AmpOffsetParameter(),
                   m_ampOffset->value());
    });
    connect(m_calculatedVoltMult, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->VoltMultiplierParameter(),
                   m_calculatedVoltMult->value());
    });
    connect(m_calculatedAmpPerVolt, &QDoubleSpinBox::editingFinished,
            this, [this]() {
        writeField(m_viewModel->AmpPerVoltParameter(),
                   m_calculatedAmpPerVolt->value());
    });
    connect(m_applyVoltage, &QPushButton::clicked, this, [this]() {
        m_viewModel->ApplyVoltageCalibration(m_measuredVoltage->value());
    });
    connect(m_applyCurrent, &QPushButton::clicked, this, [this]() {
        m_viewModel->ApplyCurrentCalibration(m_measuredCurrent->value());
    });
    connect(m_alertOnLowBattery, &QCheckBox::toggled,
            this, [](bool enabled) {
        QSettings settings;
        settings.setValue(QStringLiteral("speechbatteryenabled"), enabled);
        SpeechSettings::instance()->setEnabled(true);
        if (enabled) {
            if (!settings.contains(QStringLiteral("speechbattery"))) {
                settings.setValue(
                    QStringLiteral("speechbattery"),
                    QStringLiteral(
                        "WARNING, Battery at {batv} Volt, {batp} percent"));
            }
            if (!settings.contains(QStringLiteral("speechbatteryvolt"))) {
                settings.setValue(
                    QStringLiteral("speechbatteryvolt"), 9.6);
            }
            if (!settings.contains(QStringLiteral("speechbatterypercent"))) {
                settings.setValue(
                    QStringLiteral("speechbatterypercent"), 20);
            }
        }
    });

    configureMetadataEditors();
    syncFields();
    syncLiveValues();
    syncState();
}

void ConfigBatteryMonitoring2View::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    configureMetadataEditors();
    syncFields();
}

void ConfigBatteryMonitoring2View::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    const double liveVoltage = m_viewModel->LiveVoltage();
    const double liveCurrent = m_viewModel->LiveCurrent();
    const bool hasLiveVoltage = m_viewModel->HasLiveVoltage();
    const bool hasLiveCurrent = m_viewModel->HasLiveCurrent();
    m_viewModel->Reset(preferredComponent);
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == preferredComponent) {
            m_viewModel->parameterChanged(
                parameter.componentId, parameter.name, parameter.value);
        }
    }
    m_viewModel->setLiveValues(liveVoltage, hasLiveVoltage,
                               liveCurrent, hasLiveCurrent);
    syncFields();
    syncLiveValues();
    syncState();
}

void ConfigBatteryMonitoring2View::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
    syncState();
}

void ConfigBatteryMonitoring2View::observeMavlinkMessage(
    const mavlink_message_t &message)
{
    m_viewModel->observeMavlinkMessage(message);
}

void ConfigBatteryMonitoring2View::parameterChanged(
    int componentId, const QString &parameterName, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, parameterName, value);
}

void ConfigBatteryMonitoring2View::parameterWriteAcknowledged(
    int componentId, const QString &name, const QVariant &value, int type)
{
    m_viewModel->parameterWriteAcknowledged(
        componentId, name, value, type);
}

void ConfigBatteryMonitoring2View::parameterBatchSubmitted(
    int componentId, const QString &name, qulonglong batchId)
{
    m_viewModel->parameterBatchSubmitted(componentId, name, batchId);
    syncState();
}

void ConfigBatteryMonitoring2View::parameterWriteFailed(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name, int reason, const QString &message)
{
    m_viewModel->parameterWriteFailed(
        transactionId, batchId, componentId, name, reason, message);
    syncFields();
}

void ConfigBatteryMonitoring2View::parameterWriteCancelled(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name)
{
    m_viewModel->parameterWriteCancelled(
        transactionId, batchId, componentId, name);
    syncFields();
}

void ConfigBatteryMonitoring2View::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
    syncFields();
}

void ConfigBatteryMonitoring2View::parameterWriteSubmissionFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(
        componentId, name, reason);
    syncFields();
}

void ConfigBatteryMonitoring2View::syncFields()
{
    const auto setDouble = [this](QDoubleSpinBox *editor,
                                  const QString &name) {
        const QSignalBlocker blocker(editor);
        bool ok = false;
        const double value = m_viewModel->ParameterValue(name).toDouble(&ok);
        // Parameter echoes for another field must not replace text the user
        // is still editing in the focused spin box.
        if (ok && !editor->hasFocus()) {
            editor->setValue(value);
        }
        editor->setEnabled(m_viewModel->CanEdit()
                           && m_viewModel->HasParameter(name));
    };
    {
        const QSignalBlocker blocker(m_monitor);
        bool ok = false;
        const QVariant value = m_viewModel->ParameterValue(
            m_viewModel->MonitorParameter());
        value.toDouble(&ok);
        setComboValue(m_monitor, value, ok);
        m_monitor->setEnabled(
            m_viewModel->CanEdit()
            && m_viewModel->HasParameter(m_viewModel->MonitorParameter()));
    }
    setDouble(m_capacity, m_viewModel->CapacityParameter());
    {
        const QSignalBlocker blocker(m_voltPin);
        bool ok = false;
        const QVariant value = m_viewModel->ParameterValue(
            m_viewModel->VoltPinParameter());
        value.toDouble(&ok);
        setComboValue(m_voltPin, value, ok);
        m_voltPin->setEnabled(
            m_viewModel->CanEdit()
            && m_viewModel->HasParameter(m_viewModel->VoltPinParameter()));
    }
    {
        const QSignalBlocker blocker(m_currPin);
        bool ok = false;
        const QVariant value = m_viewModel->ParameterValue(
            m_viewModel->CurrPinParameter());
        value.toDouble(&ok);
        setComboValue(m_currPin, value, ok);
        m_currPin->setEnabled(
            m_viewModel->CanEdit()
            && m_viewModel->HasParameter(m_viewModel->CurrPinParameter()));
    }
    setDouble(m_voltMult, m_viewModel->VoltMultiplierParameter());
    setDouble(m_ampPerVolt, m_viewModel->AmpPerVoltParameter());
    setDouble(m_ampOffset, m_viewModel->AmpOffsetParameter());

    const QVariant voltMult = m_viewModel->ParameterValue(
        m_viewModel->VoltMultiplierParameter());
    const QVariant ampPerVolt = m_viewModel->ParameterValue(
        m_viewModel->AmpPerVoltParameter());
    {
        const QSignalBlocker blocker(m_calculatedVoltMult);
        if (voltMult.isValid() && !m_calculatedVoltMult->hasFocus()) {
            m_calculatedVoltMult->setValue(voltMult.toDouble());
        }
        m_calculatedVoltMult->setEnabled(
            m_viewModel->CanEdit() && voltMult.isValid());
    }
    {
        const QSignalBlocker blocker(m_calculatedAmpPerVolt);
        if (ampPerVolt.isValid() && !m_calculatedAmpPerVolt->hasFocus()) {
            m_calculatedAmpPerVolt->setValue(ampPerVolt.toDouble());
        }
        m_calculatedAmpPerVolt->setEnabled(
            m_viewModel->CanEdit() && ampPerVolt.isValid());
    }

    m_ampPerVoltLabel->setText(
        tr("Amp per volt (%1)").arg(m_viewModel->AmpPerVoltParameter()));
    m_ampPerVolt->setProperty(
        "parameterName", m_viewModel->AmpPerVoltParameter());
    m_editorPanel->setEnabled(m_viewModel->CanEdit());
    syncState();
}

void ConfigBatteryMonitoring2View::configureMetadataEditors()
{
    const auto configureCombo = [this](
        QComboBox *combo, const QString &name,
        const QList<QPair<QVariant, QString>> &fallback) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        const ParameterMetaData metadata = m_catalog.value(name);
        if (m_catalog.contains(name) && !metadata.values.isEmpty()) {
            for (const ParameterMetaDataOption &option : metadata.values) {
                combo->addItem(option.label, option.value);
            }
        } else {
            for (const auto &option : fallback) {
                combo->addItem(option.second, option.first);
            }
        }
    };
    configureCombo(
        m_monitor, m_viewModel->MonitorParameter(),
        {{0, tr("Disabled")},
         {3, tr("Analog voltage only")},
         {4, tr("Analog voltage and current")}});
    configureCombo(m_voltPin, m_viewModel->VoltPinParameter(), {});
    configureCombo(m_currPin, m_viewModel->CurrPinParameter(), {});

    const ParameterMetaData capacity =
        m_catalog.value(m_viewModel->CapacityParameter());
    if (m_catalog.contains(m_viewModel->CapacityParameter())
        && capacity.hasRange) {
        m_capacity->setRange(capacity.minimum, capacity.maximum);
    } else {
        m_capacity->setRange(0.0, 1000000000.0);
    }
    m_capacity->setSingleStep(
        capacity.hasIncrement && capacity.increment > 0.0
            ? capacity.increment : 100.0);
}

void ConfigBatteryMonitoring2View::setComboValue(
    QComboBox *combo, const QVariant &value, bool valid)
{
    for (int index = combo->count() - 1; index >= 0; --index) {
        if (combo->itemData(index, Qt::UserRole + 1).toBool()) {
            combo->removeItem(index);
        }
    }
    if (!valid) {
        combo->setCurrentIndex(-1);
        return;
    }
    bool numericOk = false;
    const double numeric = value.toDouble(&numericOk);
    int selected = -1;
    for (int index = 0; index < combo->count(); ++index) {
        bool optionOk = false;
        const double option = combo->itemData(index).toDouble(&optionOk);
        if ((numericOk && optionOk && std::abs(numeric - option) <= 1.0e-6)
            || (!numericOk && combo->itemData(index) == value)) {
            selected = index;
            break;
        }
    }
    if (selected < 0) {
        combo->addItem(tr("%1: Vehicle value").arg(value.toString()), value);
        selected = combo->count() - 1;
        combo->setItemData(selected, true, Qt::UserRole + 1);
    }
    combo->setCurrentIndex(selected);
}

void ConfigBatteryMonitoring2View::syncLiveValues()
{
    const bool initializeMeasuredVoltage =
        m_viewModel->HasLiveVoltage()
        && qFuzzyIsNull(m_measuredVoltage->value());
    m_liveVoltage->setText(
        m_viewModel->HasLiveVoltage()
            ? tr("%1 V").arg(m_viewModel->LiveVoltage(), 0, 'f', 2)
            : QStringLiteral("—"));
    m_liveCurrent->setText(
        m_viewModel->HasLiveCurrent()
            ? tr("%1 A").arg(m_viewModel->LiveCurrent(), 0, 'f', 2)
            : QStringLiteral("—"));
    if (initializeMeasuredVoltage) {
        const QSignalBlocker blocker(m_measuredVoltage);
        m_measuredVoltage->setValue(m_viewModel->LiveVoltage());
    }
    syncState();
}

void ConfigBatteryMonitoring2View::syncState()
{
    m_status->setText(m_viewModel->Status());
    m_applyVoltage->setEnabled(
        m_viewModel->CanEdit() && m_viewModel->HasLiveVoltage()
        && m_viewModel->HasParameter(
            m_viewModel->VoltMultiplierParameter()));
    m_applyCurrent->setEnabled(
        m_viewModel->CanEdit() && m_viewModel->HasLiveCurrent()
        && m_viewModel->HasParameter(
            m_viewModel->AmpPerVoltParameter()));
}

void ConfigBatteryMonitoring2View::writeField(
    const QString &name, const QVariant &value)
{
    if (!m_viewModel->setFieldValue(name, value)) {
        syncFields();
    }
}
