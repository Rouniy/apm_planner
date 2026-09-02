#include "ConfigParachuteView.h"

#include <QComboBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <cmath>

namespace {
class ParachuteWheelFilter final : public QObject
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

class ParachuteNumericEditor final : public QDoubleSpinBox
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
} // namespace

struct ConfigParachuteView::FieldWidgets
{
    QWidget *row = nullptr;
    QLabel *label = nullptr;
    QComboBox *combo = nullptr;
    QDoubleSpinBox *numeric = nullptr;
    QLabel *units = nullptr;
    QLabel *status = nullptr;
};

ConfigParachuteView::ConfigParachuteView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigParachuteViewModel(this)),
      m_wheelFilter(new ParachuteWheelFilter(this))
{
    setObjectName(QStringLiteral("ConfigParachuteView"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    QPalette opaquePalette = palette();
    opaquePalette.setColor(QPalette::Window, QColor(QStringLiteral("#1A201D")));
    opaquePalette.setColor(QPalette::WindowText,
                           QColor(QStringLiteral("#E6EDE9")));
    setPalette(opaquePalette);
    setStyleSheet(QStringLiteral(
        "ConfigParachuteView, QWidget#parachuteContent, "
        "QWidget#parachuteFields { background: #1A201D; color: #E6EDE9; }"
        "QLabel#parachuteTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#parachuteIntro, QLabel[parachuteFieldLabel=\"true\"] {"
        " color: #C8C8C8; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled {"
        " color: #68736D; }"
        "QLabel#ServoStatus, QLabel#parachuteStatus, "
        "QLabel[parachuteFieldStatus=\"true\"] { color: #34D399; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("parachuteScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("parachuteContent"));
    content->setAutoFillBackground(true);
    content->setPalette(opaquePalette);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(10);

    auto *title = new QLabel(m_viewModel->Title(), content);
    title->setObjectName(QStringLiteral("parachuteTitle"));
    contentLayout->addWidget(title);

    auto *intro = new QLabel(m_viewModel->Intro(), content);
    intro->setObjectName(QStringLiteral("parachuteIntro"));
    intro->setWordWrap(true);
    contentLayout->addWidget(intro);

    m_refreshButton = new QPushButton(tr("Refresh Params"), content);
    m_refreshButton->setObjectName(QStringLiteral("parachuteRefreshButton"));
    m_refreshButton->setSizePolicy(QSizePolicy::Maximum,
                                   QSizePolicy::Preferred);
    contentLayout->addWidget(m_refreshButton, 0, Qt::AlignLeft);

    auto *servoRow = new QWidget(content);
    servoRow->setObjectName(QStringLiteral("parachuteServoRow"));
    auto *servoLayout = new QGridLayout(servoRow);
    servoLayout->setContentsMargins(0, 6, 0, 0);
    servoLayout->setHorizontalSpacing(0);
    servoLayout->setColumnMinimumWidth(0, 220);
    servoLayout->setColumnMinimumWidth(1, 200);
    servoLayout->setColumnStretch(3, 1);

    auto *servoLabel = new QLabel(tr("Servo Channel"), servoRow);
    servoLabel->setObjectName(QStringLiteral("parachuteServoLabel"));
    servoLabel->setProperty("parachuteFieldLabel", true);
    servoLayout->addWidget(servoLabel, 0, 0);

    m_servoOptions = new QComboBox(servoRow);
    m_servoOptions->setObjectName(QStringLiteral("ServoOptions"));
    m_servoOptions->setMinimumWidth(180);
    m_servoOptions->installEventFilter(m_wheelFilter);
    m_servoOptions->addItems(m_viewModel->ServoOptions());
    servoLayout->addWidget(m_servoOptions, 0, 1, Qt::AlignLeft);

    m_servoStatus = new QLabel(servoRow);
    m_servoStatus->setObjectName(QStringLiteral("ServoStatus"));
    servoLayout->addWidget(m_servoStatus, 0, 2);
    contentLayout->addWidget(servoRow);

    m_fieldsWidget = new QWidget(content);
    m_fieldsWidget->setObjectName(QStringLiteral("parachuteFields"));
    m_fieldsWidget->setAutoFillBackground(true);
    m_fieldsWidget->setPalette(opaquePalette);
    m_fieldsLayout = new QVBoxLayout(m_fieldsWidget);
    m_fieldsLayout->setContentsMargins(0, 0, 0, 0);
    m_fieldsLayout->setSpacing(0);
    m_fieldsLayout->addStretch(1);
    contentLayout->addWidget(m_fieldsWidget);

    m_statusLabel = new QLabel(content);
    m_statusLabel->setObjectName(QStringLiteral("parachuteStatus"));
    m_statusLabel->setWordWrap(true);
    contentLayout->addWidget(m_statusLabel);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_viewModel, &ConfigParachuteViewModel::structureChanged,
            this, &ConfigParachuteView::rebuildFields);
    connect(m_viewModel, &ConfigParachuteViewModel::fieldChanged,
            this, &ConfigParachuteView::syncField);
    connect(m_viewModel, &ConfigParachuteViewModel::selectedServoChanged,
            this, &ConfigParachuteView::syncServo);
    connect(m_viewModel, &ConfigParachuteViewModel::servoStatusChanged,
            this, &ConfigParachuteView::syncServo);
    connect(m_viewModel, &ConfigParachuteViewModel::statusChanged,
            this, &ConfigParachuteView::syncState);
    connect(m_viewModel, &ConfigParachuteViewModel::stateChanged,
            this, &ConfigParachuteView::syncState);
    connect(m_viewModel, &ConfigParachuteViewModel::writeRequested,
            this, &ConfigParachuteView::writeRequested);
    connect(m_viewModel, &ConfigParachuteViewModel::refreshRequested,
            this, &ConfigParachuteView::refreshRequested);
    connect(m_servoOptions,
            QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        if (index < 0
            || !m_viewModel->AssignServo(
                m_servoOptions->itemText(index))) {
            syncServo();
        }
    });
    connect(m_refreshButton, &QPushButton::clicked,
            this, &ConfigParachuteView::requestRefresh);

    m_viewModel->setCatalog(catalog);
    syncServo();
    syncState();
}

ConfigParachuteView::~ConfigParachuteView()
{
    clearFields();
}

QSize ConfigParachuteView::sizeHint() const
{
    return QSize(800, 560);
}

QString ConfigParachuteView::ArmedRefreshWarningTitle()
{
    return tr("Refresh Params");
}

QString ConfigParachuteView::ArmedRefreshWarningText()
{
    return tr("Update Params\nDON'T DO THIS IF YOU ARE IN THE AIR\n");
}

void ConfigParachuteView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigParachuteView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigParachuteView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigParachuteView::setArmed(bool armed)
{
    m_armed = armed;
}

void ConfigParachuteView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigParachuteView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigParachuteView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigParachuteView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigParachuteView::rebuildFields()
{
    clearFields();
    const QList<ParamField> fields = m_viewModel->Fields();
    for (const ParamField &field : fields) {
        auto *widgets = new FieldWidgets;
        widgets->row = new QWidget(m_fieldsWidget);
        widgets->row->setObjectName(
            QStringLiteral("parachuteFieldRow_%1").arg(field.name));
        widgets->row->setProperty("parameterName", field.name);
        auto *grid = new QGridLayout(widgets->row);
        grid->setContentsMargins(0, 3, 0, 3);
        grid->setHorizontalSpacing(0);
        grid->setColumnMinimumWidth(0, 220);
        grid->setColumnMinimumWidth(1, 200);
        grid->setColumnStretch(3, 1);

        widgets->label = new QLabel(field.label, widgets->row);
        widgets->label->setObjectName(
            QStringLiteral("parachuteFieldLabel_%1").arg(field.name));
        widgets->label->setProperty("parachuteFieldLabel", true);
        widgets->label->setWordWrap(true);
        widgets->label->setToolTip(field.description);
        grid->addWidget(widgets->label, 0, 0);

        if (field.editorKind == ParamField::EditorKind::Combo) {
            widgets->combo = new QComboBox(widgets->row);
            widgets->combo->setObjectName(
                QStringLiteral("parachuteFieldEditor_%1").arg(field.name));
            widgets->combo->setMinimumWidth(180);
            widgets->combo->installEventFilter(m_wheelFilter);
            for (const ParamOption &option : field.options) {
                widgets->combo->addItem(option.text, option.value);
            }
            grid->addWidget(widgets->combo, 0, 1, Qt::AlignLeft);
            connect(widgets->combo,
                    QOverload<int>::of(&QComboBox::activated),
                    this, [this, name = field.name,
                           combo = widgets->combo](int index) {
                if (index < 0
                    || !m_viewModel->setFieldValue(
                        name, combo->itemData(index))) {
                    syncField(name);
                }
            });
        } else {
            widgets->numeric = new ParachuteNumericEditor(widgets->row);
            widgets->numeric->setObjectName(
                QStringLiteral("parachuteFieldEditor_%1").arg(field.name));
            widgets->numeric->setDecimals(6);
            widgets->numeric->setKeyboardTracking(false);
            widgets->numeric->setMinimumWidth(180);
            widgets->numeric->installEventFilter(m_wheelFilter);
            grid->addWidget(widgets->numeric, 0, 1, Qt::AlignLeft);
            connect(widgets->numeric,
                    &QDoubleSpinBox::editingFinished,
                    this, [this, name = field.name,
                           editor = widgets->numeric]() {
                if (!m_viewModel->setFieldValue(name, editor->value())) {
                    syncField(name);
                }
            });
        }

        widgets->units = new QLabel(field.units, widgets->row);
        widgets->units->setObjectName(
            QStringLiteral("parachuteFieldUnits_%1").arg(field.name));
        widgets->units->setContentsMargins(8, 0, 8, 0);
        grid->addWidget(widgets->units, 0, 2);

        widgets->status = new QLabel(field.status, widgets->row);
        widgets->status->setObjectName(
            QStringLiteral("parachuteFieldStatus_%1").arg(field.name));
        widgets->status->setProperty("parachuteFieldStatus", true);
        grid->addWidget(widgets->status, 0, 3);

        m_fields.insert(field.name, widgets);
        m_fieldsLayout->insertWidget(m_fieldsLayout->count() - 1,
                                     widgets->row);
        syncField(field.name);
    }
    syncState();
}

void ConfigParachuteView::syncField(const QString &name)
{
    FieldWidgets *widgets = m_fields.value(name);
    if (!widgets) {
        return;
    }
    ParamField field;
    bool found = false;
    for (const ParamField &candidate : m_viewModel->Fields()) {
        if (candidate.name == name) {
            field = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    const bool enabled = m_viewModel->CanEdit() && !field.readOnly;
    if (widgets->combo) {
        const QSignalBlocker blocker(widgets->combo);
        widgets->combo->setCurrentIndex(
            optionIndex(field.options, field.value));
        widgets->combo->setEnabled(enabled && !field.options.isEmpty());
    }
    if (widgets->numeric) {
        const QSignalBlocker blocker(widgets->numeric);
        widgets->numeric->setRange(field.minimum, field.maximum);
        widgets->numeric->setSingleStep(field.increment);
        widgets->numeric->setValue(field.value.toDouble());
        widgets->numeric->setEnabled(enabled);
    }
    widgets->status->setText(field.status);
}

void ConfigParachuteView::requestRefresh()
{
    if (m_armed
        && QMessageBox::warning(
               this, ArmedRefreshWarningTitle(), ArmedRefreshWarningText(),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_viewModel->Refresh();
}

void ConfigParachuteView::syncState()
{
    m_refreshButton->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Busy());
    m_statusLabel->setText(m_viewModel->Status());
    for (const QString &name : m_fields.keys()) {
        syncField(name);
    }
    syncServo();
}

void ConfigParachuteView::syncServo()
{
    const QSignalBlocker blocker(m_servoOptions);
    const int index = m_servoOptions->findText(
        m_viewModel->SelectedServo(), Qt::MatchFixedString);
    m_servoOptions->setCurrentIndex(index);
    m_servoOptions->setEnabled(m_viewModel->CanEdit());
    m_servoStatus->setText(m_viewModel->ServoStatus());
}

void ConfigParachuteView::clearFields()
{
    for (FieldWidgets *widgets : m_fields) {
        delete widgets->row;
        delete widgets;
    }
    m_fields.clear();
}
