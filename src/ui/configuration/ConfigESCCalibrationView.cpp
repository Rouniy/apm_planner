#include "ConfigESCCalibrationView.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

namespace {
class EscWheelFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        Q_UNUSED(object)
        if (event->type() == QEvent::Wheel) {
            event->ignore();
            return true;
        }
        return false;
    }
};

class EscNumericEditor final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override
    {
        QString text = locale().toString(value, 'f', 6);
        const auto decimal = locale().decimalPoint();
        while (text.contains(decimal) && text.endsWith(QLatin1Char('0'))) {
            text.chop(1);
        }
        if (text.endsWith(decimal)) {
            text.chop(1);
        }
        return text;
    }
};

bool variantsEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue
            || std::abs(leftValue - rightValue) <= 1.0e-6;
    }
    return left == right;
}

int optionIndex(const QList<ParamOption> &options, const QVariant &value)
{
    for (int index = 0; index < options.size(); ++index) {
        if (variantsEqual(options.at(index).value, value)) {
            return index;
        }
    }
    return -1;
}

QString fieldLabel(int index)
{
    static const QStringList labels = {
        QStringLiteral("ESC Type:"),
        QStringLiteral("Output PWM Min"),
        QStringLiteral("Output PWM Max"),
        QStringLiteral("Spin when Armed"),
        QStringLiteral("Spin minimum"),
        QStringLiteral("Spin Maximum")
    };
    return labels.value(index);
}

QString fieldDescription(int index)
{
    static const QStringList descriptions = {
        QString(),
        QStringLiteral("Leave as 0 to use RX input range"),
        QStringLiteral("Leave as 0 to use RX input range"),
        QStringLiteral(
            "speed when motors are armed but throttle is at zero (idle)"),
        QStringLiteral(
            "minimum speed of motors while in flight (slightly higher than "
            "\"Spin when Armed\")"),
        QStringLiteral(
            "maximum speed of motors while in flight (almost all escs have "
            "a deadzone at the top)")
    };
    return descriptions.value(index);
}
} // namespace

struct ConfigESCCalibrationView::FieldWidgets
{
    QWidget *row = nullptr;
    QLabel *label = nullptr;
    QComboBox *combo = nullptr;
    QDoubleSpinBox *numeric = nullptr;
    QLabel *description = nullptr;
};

ConfigESCCalibrationView::ConfigESCCalibrationView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigESCCalibrationViewModel(this)),
      m_wheelFilter(new EscWheelFilter(this))
{
    setObjectName(QStringLiteral("ConfigESCCalibrationView"));
    setStyleSheet(QStringLiteral(
        "ConfigESCCalibrationView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#escCalibrationTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#escRemoveProps { color: #E8E8E8; font-weight: bold; }"
        "QLabel[escDescription=\"true\"] { color: #C8C8C8; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "QPushButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled {"
        " color: #68736D; }"
        "QLabel#escCalibrationStatus { color: #2F81F7; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("escCalibrationScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("escCalibrationContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(0);

    auto *title = new QLabel(m_viewModel->Title(), content);
    title->setObjectName(QStringLiteral("escCalibrationTitle"));
    contentLayout->addWidget(title);

    auto *actionRow = new QWidget(content);
    actionRow->setObjectName(QStringLiteral("escCalibrationActionRow"));
    auto *actionLayout = new QGridLayout(actionRow);
    actionLayout->setContentsMargins(0, 4, 0, 0);
    actionLayout->setHorizontalSpacing(16);
    m_calibrateButton = new QPushButton(actionRow);
    m_calibrateButton->setObjectName(
        QStringLiteral("escCalibrateButton"));
    m_calibrateButton->setMinimumWidth(120);
    actionLayout->addWidget(m_calibrateButton, 0, 0,
                            Qt::AlignTop | Qt::AlignLeft);

    auto *instructions = new QWidget(actionRow);
    instructions->setObjectName(QStringLiteral("escCalibrationInstructions"));
    auto *instructionsLayout = new QVBoxLayout(instructions);
    instructionsLayout->setContentsMargins(0, 0, 0, 0);
    instructionsLayout->setSpacing(0);
    const QStringList lines = {
        tr("Remove Props!"),
        tr("After pushing this button:"),
        tr("-Disconnect USB and battery"),
        tr("-Plug in battery"),
        tr("-when LEDs flash, push Saftey Switch (if present)"),
        tr("-ESCs should beep as they are calibrated"),
        tr("- restart flight controller normally")
    };
    for (int index = 0; index < lines.size(); ++index) {
        auto *line = new QLabel(lines.at(index), instructions);
        line->setObjectName(index == 0
            ? QStringLiteral("escRemoveProps")
            : QStringLiteral("escInstruction_%1").arg(index));
        line->setWordWrap(true);
        instructionsLayout->addWidget(line);
    }
    actionLayout->addWidget(instructions, 0, 1);
    actionLayout->setColumnStretch(1, 1);
    contentLayout->addWidget(actionRow);

    m_fieldsWidget = new QWidget(content);
    m_fieldsWidget->setObjectName(QStringLiteral("escCalibrationFields"));
    m_fieldsLayout = new QVBoxLayout(m_fieldsWidget);
    m_fieldsLayout->setContentsMargins(0, 18, 0, 0);
    m_fieldsLayout->setSpacing(0);
    contentLayout->addWidget(m_fieldsWidget);

    auto *bottomRow = new QWidget(content);
    bottomRow->setObjectName(QStringLiteral("escCalibrationBottomRow"));
    auto *bottomLayout = new QHBoxLayout(bottomRow);
    bottomLayout->setContentsMargins(0, 14, 0, 0);
    bottomLayout->setSpacing(8);
    m_refreshButton = new QPushButton(tr("Refresh Params"), bottomRow);
    m_refreshButton->setObjectName(QStringLiteral("escRefreshButton"));
    bottomLayout->addWidget(m_refreshButton);
    m_statusLabel = new QLabel(bottomRow);
    m_statusLabel->setObjectName(QStringLiteral("escCalibrationStatus"));
    m_statusLabel->setWordWrap(true);
    bottomLayout->addWidget(m_statusLabel, 1);
    contentLayout->addWidget(bottomRow);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_viewModel,
            &ConfigESCCalibrationViewModel::structureChanged,
            this, &ConfigESCCalibrationView::rebuildFields);
    connect(m_viewModel, &ConfigESCCalibrationViewModel::fieldChanged,
            this, &ConfigESCCalibrationView::syncField);
    connect(m_viewModel, &ConfigESCCalibrationViewModel::stateChanged,
            this, &ConfigESCCalibrationView::syncState);
    connect(m_viewModel, &ConfigESCCalibrationViewModel::statusChanged,
            this, &ConfigESCCalibrationView::syncState);
    connect(m_viewModel, &ConfigESCCalibrationViewModel::writeRequested,
            this, &ConfigESCCalibrationView::writeRequested);
    connect(m_viewModel, &ConfigESCCalibrationViewModel::refreshRequested,
            this, &ConfigESCCalibrationView::refreshRequested);
    connect(m_calibrateButton, &QPushButton::clicked,
            this, &ConfigESCCalibrationView::calibrateClicked);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &ConfigESCCalibrationView::refreshClicked);

    m_viewModel->setCatalog(catalog);
    syncState();
}

ConfigESCCalibrationView::~ConfigESCCalibrationView()
{
    clearFields();
}

QString ConfigESCCalibrationView::CalibrationConfirmationTitle()
{
    return tr("ESC Calibration Safety Warning");
}

QString ConfigESCCalibrationView::CalibrationConfirmationText()
{
    return tr(
        "REMOVE ALL PROPELLERS before continuing.\n\n"
        "This writes ESC_CALIBRATION = 3. Afterward you must power-cycle "
        "the vehicle: disconnect USB and the battery, then reconnect battery "
        "power to enter ESC calibration.\n\nContinue?");
}

QString ConfigESCCalibrationView::ArmedRefreshConfirmationText()
{
    return tr("The vehicle is armed. Refreshing parameters can disrupt "
              "telemetry. Continue?");
}

QSize ConfigESCCalibrationView::sizeHint() const
{
    return QSize(820, 560);
}

void ConfigESCCalibrationView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigESCCalibrationView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigESCCalibrationView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigESCCalibrationView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigESCCalibrationView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigESCCalibrationView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigESCCalibrationView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigESCCalibrationView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigESCCalibrationView::rebuildFields()
{
    clearFields();
    const QList<ParamField> fields = m_viewModel->Fields();
    for (int index = 0; index < fields.size(); ++index) {
        const ParamField &field = fields.at(index);
        auto *widgets = new FieldWidgets;
        widgets->row = new QWidget(m_fieldsWidget);
        widgets->row->setObjectName(
            QStringLiteral("escFieldRow_%1").arg(field.name));
        widgets->row->setProperty("parameterName", field.name);
        auto *grid = new QGridLayout(widgets->row);
        grid->setContentsMargins(0, index == 0 ? 0 : 6, 0, 0);
        grid->setHorizontalSpacing(0);
        grid->setColumnMinimumWidth(0, 150);
        grid->setColumnStretch(2, 1);

        widgets->label = new QLabel(fieldLabel(index), widgets->row);
        widgets->label->setObjectName(
            QStringLiteral("escFieldLabel_%1").arg(field.name));
        grid->addWidget(widgets->label, 0, 0);

        if (field.editorKind == ParamField::EditorKind::Combo) {
            widgets->combo = new QComboBox(widgets->row);
            widgets->combo->setObjectName(
                QStringLiteral("escFieldEditor_%1").arg(field.name));
            widgets->combo->setMinimumWidth(200);
            widgets->combo->installEventFilter(m_wheelFilter);
            for (const ParamOption &option : field.options) {
                widgets->combo->addItem(option.text, option.value);
            }
            grid->addWidget(widgets->combo, 0, 1, Qt::AlignLeft);
            connect(widgets->combo,
                    QOverload<int>::of(&QComboBox::activated),
                    this, [this, name = field.name,
                           combo = widgets->combo](int selected) {
                if (selected < 0
                    || !m_viewModel->setFieldValue(
                        name, combo->itemData(selected))) {
                    syncField(name);
                }
            });
        } else {
            widgets->numeric = new EscNumericEditor(widgets->row);
            widgets->numeric->setObjectName(
                QStringLiteral("escFieldEditor_%1").arg(field.name));
            widgets->numeric->setDecimals(6);
            widgets->numeric->setKeyboardTracking(false);
            widgets->numeric->setMinimumWidth(120);
            widgets->numeric->installEventFilter(m_wheelFilter);
            grid->addWidget(widgets->numeric, 0, 1, Qt::AlignLeft);
            connect(widgets->numeric, &QDoubleSpinBox::editingFinished,
                    this, [this, name = field.name,
                           editor = widgets->numeric]() {
                if (!m_viewModel->setFieldValue(name, editor->value())) {
                    syncField(name);
                }
            });
        }

        widgets->description = new QLabel(
            fieldDescription(index), widgets->row);
        widgets->description->setObjectName(
            QStringLiteral("escFieldDescription_%1").arg(field.name));
        widgets->description->setProperty("escDescription", true);
        widgets->description->setWordWrap(true);
        widgets->description->setContentsMargins(10, 0, 0, 0);
        grid->addWidget(widgets->description, 0, 2);

        m_fields.insert(field.name, widgets);
        m_fieldsLayout->addWidget(widgets->row);
        syncField(field.name);
    }
}

void ConfigESCCalibrationView::syncField(const QString &name)
{
    const QString normalized = name.trimmed().toUpper();
    FieldWidgets *widgets = m_fields.value(normalized);
    if (!widgets) {
        return;
    }
    ParamField field;
    bool found = false;
    for (const ParamField &candidate : m_viewModel->Fields()) {
        if (candidate.name == normalized) {
            field = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    if (widgets->combo) {
        const QSignalBlocker blocker(widgets->combo);
        widgets->combo->setCurrentIndex(
            optionIndex(field.options, field.value));
        widgets->combo->setEnabled(
            m_viewModel->Connected() && !m_viewModel->Armed()
            && !m_viewModel->HasPendingWrites()
            && !field.readOnly && !field.options.isEmpty());
    }
    if (widgets->numeric) {
        const QSignalBlocker blocker(widgets->numeric);
        widgets->numeric->setRange(field.minimum, field.maximum);
        widgets->numeric->setSingleStep(field.increment);
        widgets->numeric->setValue(field.value.toDouble());
        widgets->numeric->setEnabled(
            m_viewModel->Connected() && !m_viewModel->Armed()
            && !m_viewModel->HasPendingWrites() && !field.readOnly);
    }
}

void ConfigESCCalibrationView::syncState()
{
    m_calibrateButton->setText(m_viewModel->CalButtonText());
    m_calibrateButton->setEnabled(m_viewModel->CanCalibrate());
    m_refreshButton->setEnabled(
        m_viewModel->Connected() && !m_viewModel->HasPendingWrites());
    m_statusLabel->setText(m_viewModel->Status());
    const QStringList names = m_fields.keys();
    for (const QString &name : names) {
        syncField(name);
    }
}

void ConfigESCCalibrationView::clearFields()
{
    for (FieldWidgets *widgets : m_fields) {
        delete widgets->row;
        delete widgets;
    }
    m_fields.clear();
}

void ConfigESCCalibrationView::calibrateClicked()
{
    if (!m_viewModel->Connected() || m_viewModel->Armed()) {
        m_viewModel->CalibrateEsc(false);
        return;
    }
    const bool confirmed = QMessageBox::warning(
        this, CalibrationConfirmationTitle(), CalibrationConfirmationText(),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel) == QMessageBox::Ok;
    m_viewModel->CalibrateEsc(confirmed);
}

void ConfigESCCalibrationView::refreshClicked()
{
    bool armedConfirmed = true;
    if (m_viewModel->Armed()) {
        armedConfirmed = QMessageBox::warning(
            this, tr("Refresh Params"), ArmedRefreshConfirmationText(),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
    }
    m_viewModel->Refresh(armedConfirmed);
}
