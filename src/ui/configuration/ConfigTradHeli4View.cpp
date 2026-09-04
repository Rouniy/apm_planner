#include "ConfigTradHeli4View.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaType>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>

namespace {
bool valuesEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

int optionIndex(const QList<ParamOption> &options, const QVariant &value)
{
    for (int index = 0; index < options.size(); ++index) {
        if (valuesEqual(options.at(index).value, value)) {
            return index;
        }
    }
    return -1;
}

int decimalsFor(const ParamField &field)
{
    double step = std::abs(field.increment);
    for (int decimals = 0; decimals < 6; ++decimals) {
        if (step >= 1.0 || std::abs(step - std::round(step)) < 1.0e-9) {
            return decimals;
        }
        step *= 10.0;
    }
    return 6;
}

} // namespace

ConfigTradHeli4View::ConfigTradHeli4View(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigTradHeli4ViewModel(this))
{
    setObjectName(QStringLiteral("ConfigTradHeli4View"));
    buildUi();
    connect(m_viewModel, &ConfigTradHeli4ViewModel::structureChanged,
            this, &ConfigTradHeli4View::rebuildFields);
    connect(m_viewModel, &ConfigTradHeli4ViewModel::fieldChanged,
            this, &ConfigTradHeli4View::syncField);
    connect(m_viewModel, &ConfigTradHeli4ViewModel::stateChanged,
            this, &ConfigTradHeli4View::syncState);
    connect(m_viewModel, &ConfigTradHeli4ViewModel::writeRequested,
            this, &ConfigTradHeli4View::writeRequested);
    connect(m_viewModel, &ConfigTradHeli4ViewModel::refreshRequested,
            this, &ConfigTradHeli4View::refreshRequested);
    connect(m_viewModel, &ConfigTradHeli4ViewModel::manualSafetyWarning,
            this, [this](const QString &warning) {
        m_safetyWarning->setText(warning);
        m_safetyWarning->show();
        emit safetyWarning(warning);
    });
    rebuildFields();
    syncState();
}

ConfigTradHeli4View::~ConfigTradHeli4View() = default;

QSize ConfigTradHeli4View::sizeHint() const
{
    return QSize(1060, 780);
}

void ConfigTradHeli4View::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_viewModel->setCatalog(catalog, enforceMetadataRanges);
}

void ConfigTradHeli4View::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigTradHeli4View::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigTradHeli4View::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigTradHeli4View::setActive(bool active)
{
    m_viewModel->setActive(active);
}

void ConfigTradHeli4View::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigTradHeli4View::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigTradHeli4View::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigTradHeli4View::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigTradHeli4View::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigTradHeli4View::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigTradHeli4View::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigTradHeli4View::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigTradHeli4View { background: #1A201D; color: #E6EDE9; }"
        "QLabel#tradHeli4Title { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#tradHeli4Intro { color: #C8C8C8; }"
        "QLabel#tradHeli4Status { color: #34D399; }"
        "QGroupBox { color: #E6EDE9; border: 1px solid #303A35;"
        " margin-top: 12px; padding-top: 8px; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 9px;"
        " padding: 0 4px; }"
        "QWidget[heli4FieldRow=\"true\"] { background: #202623;"
        " border: 1px solid #303A35; }"
        "QPushButton, QComboBox, QDoubleSpinBox { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #303A35; padding: 4px; }"
        "QPushButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled {"
        " color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(tr("Heli Setup"), this);
    title->setObjectName(QStringLiteral("tradHeli4Title"));
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    m_refresh = new QPushButton(tr("Refresh Params"), this);
    m_refresh->setObjectName(QStringLiteral("tradHeli4Refresh"));
    toolbar->addWidget(m_refresh);
    auto *intro = new QLabel(
        tr("Traditional helicopter setup (ArduPilot 4.0+). "
           "Remove blades before testing servos."), this);
    intro->setObjectName(QStringLiteral("tradHeli4Intro"));
    intro->setWordWrap(true);
    toolbar->addWidget(intro, 1);
    root->addLayout(toolbar);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tradHeli4Status"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    m_safetyWarning = new QLabel(this);
    m_safetyWarning->setObjectName(QStringLiteral("tradHeli4SafetyWarning"));
    m_safetyWarning->setStyleSheet(QStringLiteral("color: #FF6B6B;"));
    m_safetyWarning->setWordWrap(true);
    m_safetyWarning->hide();
    root->addWidget(m_safetyWarning);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("tradHeli4Scroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sections = new QWidget(scroll);
    m_sections->setObjectName(QStringLiteral("tradHeli4Sections"));
    m_sectionsLayout = new QVBoxLayout(m_sections);
    m_sectionsLayout->setContentsMargins(0, 0, 0, 0);
    m_sectionsLayout->setSpacing(10);
    scroll->setWidget(m_sections);
    root->addWidget(scroll, 1);

    connect(m_refresh, &QPushButton::clicked,
            m_viewModel, &ConfigTradHeli4ViewModel::Refresh);
}

void ConfigTradHeli4View::rebuildFields()
{
    while (QLayoutItem *item = m_sectionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_fieldEditors.clear();

    const QList<ParamField> fields = m_viewModel->Fields();
    const QList<Heli4ParameterSection> sections =
        ConfigTradHeli4ViewModel::Sections();
    const QStringList sectionObjectSuffixes = {
        QStringLiteral("ServoOutputs"), QStringLiteral("Swashplate"),
        QStringLiteral("RotorSpeedControl"), QStringLiteral("Governor"),
        QStringLiteral("Miscellaneous")
    };
    for (int sectionIndex = 0; sectionIndex < sections.size();
         ++sectionIndex) {
        const Heli4ParameterSection &section = sections.at(sectionIndex);
        auto *group = new QGroupBox(section.title, m_sections);
        group->setObjectName(
            QStringLiteral("tradHeli4Section")
                + sectionObjectSuffixes.at(sectionIndex));
        auto *grid = new QGridLayout(group);
        grid->setContentsMargins(10, 12, 10, 10);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);

        if (sectionIndex == 0) {
            const QStringList headers = {
                tr("#"), tr("Reverse"), tr("Function"),
                tr("Min"), tr("Trim"), tr("Max")
            };
            for (int column = 0; column < headers.size(); ++column) {
                auto *header = new QLabel(headers.at(column), group);
                header->setProperty("heli4Header", true);
                grid->addWidget(header, 0, column);
            }
            for (int channel = 1; channel <= 8; ++channel) {
                auto *number = new QLabel(QString::number(channel), group);
                number->setObjectName(
                    QStringLiteral("tradHeli4ServoRow_%1").arg(channel));
                number->setProperty("heli4ServoRow", true);
                grid->addWidget(number, channel, 0);
                const QStringList names = {
                    QStringLiteral("SERVO%1_REVERSED").arg(channel),
                    QStringLiteral("SERVO%1_FUNCTION").arg(channel),
                    QStringLiteral("SERVO%1_MIN").arg(channel),
                    QStringLiteral("SERVO%1_TRIM").arg(channel),
                    QStringLiteral("SERVO%1_MAX").arg(channel)
                };
                for (int fieldIndex = 0; fieldIndex < names.size();
                     ++fieldIndex) {
                    const ParamField *field = findField(
                        names.at(fieldIndex), fields);
                    if (!field) {
                        continue;
                    }
                    FieldEditors editors = makeEditors(*field, group, true);
                    grid->addWidget(editors.row, channel, fieldIndex + 1);
                    m_fieldEditors.insert(field->name, editors);
                }
            }
            grid->setColumnStretch(2, 1);
        } else {
            int rowIndex = 0;
            for (const QString &name : section.parameters) {
                const ParamField *field = findField(name, fields);
                if (!field) {
                    continue;
                }
                FieldEditors editors = makeEditors(*field, group, false);
                editors.row->setProperty("heli4FieldRow", true);
                grid->addWidget(editors.row, rowIndex++, 0);
                m_fieldEditors.insert(field->name, editors);
            }
        }
        m_sectionsLayout->addWidget(group);
    }
    m_sectionsLayout->addStretch(1);
    for (const ParamField &field : fields) {
        syncField(field.name);
    }
    syncState();
}

ConfigTradHeli4View::FieldEditors ConfigTradHeli4View::makeEditors(
    const ParamField &field, QWidget *parent, bool compactServoRow)
{
    FieldEditors result;
    result.row = new QWidget(parent);
    result.row->setObjectName(
        QStringLiteral("tradHeli4Field_%1").arg(objectSuffix(field.name)));
    auto *layout = new QHBoxLayout(result.row);
    layout->setContentsMargins(compactServoRow ? 0 : 8,
                               compactServoRow ? 0 : 5,
                               compactServoRow ? 0 : 8,
                               compactServoRow ? 0 : 5);
    layout->setSpacing(6);
    if (!compactServoRow) {
        result.label = new QLabel(field.label, result.row);
        result.label->setMinimumWidth(230);
        result.label->setToolTip(field.description);
        layout->addWidget(result.label, 1);
    }

    const bool reversed = field.name.endsWith(QStringLiteral("_REVERSED"));
    if (reversed) {
        result.check = new QCheckBox(result.row);
        result.check->setObjectName(
            QStringLiteral("tradHeli4ServoReversed_%1")
                .arg(field.name.mid(5, field.name.indexOf(QLatin1Char('_')) - 5)));
        layout->addWidget(result.check, 0, Qt::AlignCenter);
        connect(result.check, &QCheckBox::toggled,
                this, [this, name = field.name](bool checked) {
            if (!m_viewModel->setFieldValue(name, checked ? 1 : 0)) {
                syncField(name);
            }
        });
    } else if (field.editorKind == ParamField::EditorKind::Combo) {
        result.combo = new QComboBox(result.row);
        result.combo->setObjectName(
            QStringLiteral("tradHeli4Editor_%1").arg(objectSuffix(field.name)));
        if (field.name.contains(QStringLiteral("_FUNCTION"))) {
            result.combo->setObjectName(
                QStringLiteral("tradHeli4ServoFunction_%1")
                    .arg(field.name.mid(5, field.name.indexOf(QLatin1Char('_')) - 5)));
            result.combo->setMinimumWidth(190);
        } else {
            result.combo->setMinimumWidth(220);
        }
        for (const ParamOption &option : field.options) {
            result.combo->addItem(option.text, option.value);
        }
        layout->addWidget(result.combo);
        connect(result.combo, QOverload<int>::of(&QComboBox::activated),
                this, [this, name = field.name,
                       combo = result.combo](int index) {
            if (index < 0) {
                return;
            }
            const QVariant value = combo->itemData(index);
            if (!confirmManualOverride(
                    name == QLatin1String("H_SV_MAN") ? value : 0)
                || !m_viewModel->setFieldValue(name, value)) {
                syncField(name);
            }
        });
    } else {
        result.numeric = new QDoubleSpinBox(result.row);
        result.numeric->setObjectName(
            QStringLiteral("tradHeli4Editor_%1").arg(objectSuffix(field.name)));
        result.numeric->setRange(field.hasRange ? field.minimum : -1.0e9,
                                 field.hasRange ? field.maximum : 1.0e9);
        result.numeric->setSingleStep(
            field.increment > 0.0 ? field.increment : 0.01);
        result.numeric->setDecimals(decimalsFor(field));
        result.numeric->setMinimumWidth(compactServoRow ? 82 : 150);
        layout->addWidget(result.numeric);
        connect(result.numeric, &QDoubleSpinBox::editingFinished,
                this, [this, name = field.name,
                       spin = result.numeric]() {
            const QVariant value = spin->value();
            if (!confirmManualOverride(
                    name == QLatin1String("H_SV_MAN") ? value : 0)
                || !m_viewModel->setFieldValue(name, value)) {
                syncField(name);
            }
        });
    }

    if (!compactServoRow) {
        result.units = new QLabel(field.units, result.row);
        result.units->setMinimumWidth(55);
        layout->addWidget(result.units);
        result.status = new QLabel(field.status, result.row);
        result.status->setMinimumWidth(90);
        result.status->setObjectName(
            QStringLiteral("tradHeli4FieldStatus_%1")
                .arg(objectSuffix(field.name)));
        layout->addWidget(result.status);
    }
    return result;
}

void ConfigTradHeli4View::syncField(const QString &name)
{
    auto editors = m_fieldEditors.find(name.trimmed().toUpper());
    if (editors == m_fieldEditors.end()) {
        return;
    }
    const QList<ParamField> fields = m_viewModel->Fields();
    const ParamField *field = findField(name, fields);
    if (!field) {
        return;
    }
    if (editors->check) {
        const QSignalBlocker blocker(editors->check);
        editors->check->setChecked(field->value.toInt() != 0);
    } else if (editors->combo) {
        const QSignalBlocker blocker(editors->combo);
        editors->combo->setCurrentIndex(
            optionIndex(field->options, field->value));
    } else if (editors->numeric) {
        const QSignalBlocker blocker(editors->numeric);
        editors->numeric->setValue(field->value.toDouble());
    }
    if (editors->units) {
        editors->units->setText(field->units);
    }
    if (editors->status) {
        editors->status->setText(field->status);
    }
    syncState();
}

void ConfigTradHeli4View::syncState()
{
    const bool baseEnabled = m_viewModel->Connected()
        && m_viewModel->SnapshotReady()
        && !m_viewModel->HasPendingWrites();
    const QList<ParamField> fields = m_viewModel->Fields();
    for (auto iterator = m_fieldEditors.begin();
         iterator != m_fieldEditors.end(); ++iterator) {
        const ParamField *field = findField(iterator.key(), fields);
        const bool enabled = field && baseEnabled && !field->readOnly;
        if (iterator->check) {
            iterator->check->setEnabled(enabled);
        }
        if (iterator->combo) {
            iterator->combo->setEnabled(enabled
                                        && !field->options.isEmpty());
        }
        if (iterator->numeric) {
            iterator->numeric->setEnabled(enabled);
        }
    }
    m_refresh->setEnabled(baseEnabled);
    m_status->setText(m_viewModel->Status());
}

bool ConfigTradHeli4View::confirmManualOverride(const QVariant &value)
{
    if (value.toDouble() == 0.0) {
        return true;
    }
    QMessageBox warning(
        QMessageBox::Warning, tr("Heli Setup safety"),
        tr("Remove all main and tail rotor blades before enabling manual "
           "servo override. Continue only if the vehicle is disarmed and "
           "the blades are removed."),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    warning.setObjectName(QStringLiteral("tradHeli4BladesRemovedWarning"));
    warning.setDefaultButton(QMessageBox::Cancel);
    warning.setEscapeButton(QMessageBox::Cancel);
    return warning.exec() == QMessageBox::Ok;
}

const ParamField *ConfigTradHeli4View::findField(
    const QString &name, const QList<ParamField> &fields) const
{
    const QString normalized = name.trimmed().toUpper();
    for (const ParamField &field : fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

QString ConfigTradHeli4View::objectSuffix(const QString &name)
{
    QString result = name.trimmed().toUpper();
    result.replace(QLatin1Char('-'), QLatin1Char('_'));
    return result;
}
