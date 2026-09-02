#include "ConfigRadioOutputView.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr int kMinimumPwm = 800;
constexpr int kMaximumPwm = 2200;
constexpr int kTelemetryIntervalMs = 100;

class ServoOutputWheelFilter final : public QObject
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

void configureColumns(QGridLayout *layout)
{
    layout->setColumnMinimumWidth(0, 40);
    layout->setColumnStretch(1, 1);
    layout->setColumnMinimumWidth(2, 70);
    layout->setColumnMinimumWidth(3, 170);
    layout->setColumnMinimumWidth(4, 70);
    layout->setColumnMinimumWidth(5, 70);
    layout->setColumnMinimumWidth(6, 70);
}
} // namespace

struct ConfigRadioOutputView::RowWidgets
{
    QFrame *frame = nullptr;
    QLabel *number = nullptr;
    QProgressBar *position = nullptr;
    QCheckBox *reversed = nullptr;
    QComboBox *function = nullptr;
    QSpinBox *minimum = nullptr;
    QSpinBox *trim = nullptr;
    QSpinBox *maximum = nullptr;
};

ConfigRadioOutputView::ConfigRadioOutputView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigRadioOutputViewModel(this)),
      m_wheelFilter(new ServoOutputWheelFilter(this)),
      m_telemetryTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("ConfigRadioOutputView"));
    setStyleSheet(QStringLiteral(
        "ConfigRadioOutputView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#servoOutputTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel[servoOutputHeader=\"true\"] { color: #AEB8B2;"
        " font-weight: bold; }"
        "QFrame[servoOutputRow=\"true\"] { border: none; }"
        "QProgressBar { background: #161B18; color: #FFFFFF;"
        " border: 1px solid #2A322D; text-align: center; }"
        "QProgressBar::chunk { background: #2F81F7; }"
        "QComboBox, QSpinBox { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 3px; }"
        "QComboBox:disabled, QSpinBox:disabled, QCheckBox:disabled {"
        " color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(0);

    auto *title = new QLabel(tr("Servo Output"), this);
    title->setObjectName(QStringLiteral("servoOutputTitle"));
    root->addWidget(title);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("servoOutputHeader"));
    auto *headerLayout = new QGridLayout(header);
    headerLayout->setContentsMargins(0, 10, 0, 4);
    headerLayout->setHorizontalSpacing(0);
    configureColumns(headerLayout);
    const QStringList labels = {
        tr("#"), tr("Position"), tr("Reverse"), tr("Function"),
        tr("Min"), tr("Trim"), tr("Max")
    };
    for (int column = 0; column < labels.size(); ++column) {
        auto *label = new QLabel(labels.at(column), header);
        label->setObjectName(
            QStringLiteral("servoOutputHeader%1").arg(column));
        label->setProperty("servoOutputHeader", true);
        if (column >= 4) {
            label->setAlignment(Qt::AlignCenter);
        }
        headerLayout->addWidget(label, 0, column);
    }
    root->addWidget(header);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("servoOutputScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rowsWidget = new QWidget(scroll);
    m_rowsWidget->setObjectName(QStringLiteral("servoOutputRows"));
    m_rowsLayout = new QVBoxLayout(m_rowsWidget);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    m_rowsLayout->addStretch(1);
    scroll->setWidget(m_rowsWidget);
    root->addWidget(scroll, 1);

    m_telemetryTimer->setObjectName(QStringLiteral("servoOutputTimer"));
    m_telemetryTimer->setInterval(kTelemetryIntervalMs);
    connect(m_telemetryTimer, &QTimer::timeout,
            m_viewModel,
            &ConfigRadioOutputViewModel::flushServoOutputs);
    connect(m_viewModel, &ConfigRadioOutputViewModel::structureChanged,
            this, &ConfigRadioOutputView::rebuildRows);
    connect(m_viewModel, &ConfigRadioOutputViewModel::rowChanged,
            this, &ConfigRadioOutputView::syncRow);
    connect(m_viewModel, &ConfigRadioOutputViewModel::writeRequested,
            this, &ConfigRadioOutputView::writeRequested);

    m_viewModel->setCatalog(catalog);
}

ConfigRadioOutputView::~ConfigRadioOutputView()
{
    clearRows();
}

void ConfigRadioOutputView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigRadioOutputView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

bool ConfigRadioOutputView::telemetryTimerActive() const
{
    return m_telemetryTimer->isActive();
}

void ConfigRadioOutputView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigRadioOutputView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigRadioOutputView::servoOutputChanged(
    int oneBasedChannel, int pwm)
{
    m_viewModel->setServoOutput(oneBasedChannel, pwm);
}

void ConfigRadioOutputView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_viewModel->flushServoOutputs();
    m_telemetryTimer->start();
}

void ConfigRadioOutputView::hideEvent(QHideEvent *event)
{
    m_telemetryTimer->stop();
    QWidget::hideEvent(event);
}

void ConfigRadioOutputView::rebuildRows()
{
    clearRows();
    const QList<ServoOutputRow> rows = m_viewModel->Rows();
    for (const ServoOutputRow &row : rows) {
        auto *widgets = new RowWidgets;
        widgets->frame = new QFrame(m_rowsWidget);
        widgets->frame->setObjectName(
            QStringLiteral("servoOutputRow_%1").arg(row.Number));
        widgets->frame->setProperty("servoOutputRow", true);
        widgets->frame->setProperty("servoNumber", row.Number);

        auto *grid = new QGridLayout(widgets->frame);
        grid->setContentsMargins(0, 3, 0, 3);
        grid->setHorizontalSpacing(0);
        configureColumns(grid);

        widgets->number = new QLabel(
            QString::number(row.Number), widgets->frame);
        widgets->number->setObjectName(
            QStringLiteral("servoNumber_%1").arg(row.Number));
        widgets->number->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        grid->addWidget(widgets->number, 0, 0);

        widgets->position = new QProgressBar(widgets->frame);
        widgets->position->setObjectName(
            QStringLiteral("servoPosition_%1").arg(row.Number));
        widgets->position->setRange(kMinimumPwm, kMaximumPwm);
        widgets->position->setFixedHeight(22);
        widgets->position->setTextVisible(true);
        widgets->position->setProperty("servoNumber", row.Number);
        grid->addWidget(widgets->position, 0, 1);
        grid->setColumnMinimumWidth(1, 100);

        widgets->reversed = new QCheckBox(widgets->frame);
        widgets->reversed->setObjectName(
            QStringLiteral("servoReversed_%1").arg(row.Number));
        widgets->reversed->setProperty("servoNumber", row.Number);
        grid->addWidget(widgets->reversed, 0, 2, Qt::AlignCenter);

        widgets->function = new QComboBox(widgets->frame);
        widgets->function->setObjectName(
            QStringLiteral("servoFunction_%1").arg(row.Number));
        widgets->function->setMinimumWidth(160);
        widgets->function->setProperty("servoNumber", row.Number);
        widgets->function->installEventFilter(m_wheelFilter);
        for (const ParamOption &option : row.Function.options) {
            widgets->function->addItem(option.text, option.value);
        }
        grid->addWidget(widgets->function, 0, 3);

        auto makePwmSpin = [this, widgets, row, grid](
                               const QString &name, int column) {
            auto *spin = new QSpinBox(widgets->frame);
            spin->setObjectName(
                QStringLiteral("servo%1_%2").arg(name).arg(row.Number));
            spin->setRange(kMinimumPwm, kMaximumPwm);
            spin->setSingleStep(1);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setFixedWidth(66);
            spin->setAlignment(Qt::AlignCenter);
            spin->setProperty("servoNumber", row.Number);
            spin->installEventFilter(m_wheelFilter);
            grid->addWidget(spin, 0, column, Qt::AlignCenter);
            return spin;
        };
        widgets->minimum = makePwmSpin(QStringLiteral("Min"), 4);
        widgets->trim = makePwmSpin(QStringLiteral("Trim"), 5);
        widgets->maximum = makePwmSpin(QStringLiteral("Max"), 6);

        connect(widgets->reversed, &QCheckBox::toggled,
                this, [this, number = row.Number](bool checked) {
            if (!m_viewModel->setReversed(number, checked)) {
                syncRow(number);
            }
        });
        connect(widgets->function,
                QOverload<int>::of(&QComboBox::activated),
                this, [this, number = row.Number,
                       combo = widgets->function](int index) {
            if (index < 0
                || !m_viewModel->setFunction(
                    number, combo->itemData(index))) {
                syncRow(number);
            }
        });
        connect(widgets->minimum, &QSpinBox::editingFinished,
                this, [this, number = row.Number,
                       spin = widgets->minimum]() {
            if (!m_viewModel->setMin(number, spin->value())) {
                syncRow(number);
            }
        });
        connect(widgets->trim, &QSpinBox::editingFinished,
                this, [this, number = row.Number,
                       spin = widgets->trim]() {
            if (!m_viewModel->setTrim(number, spin->value())) {
                syncRow(number);
            }
        });
        connect(widgets->maximum, &QSpinBox::editingFinished,
                this, [this, number = row.Number,
                       spin = widgets->maximum]() {
            if (!m_viewModel->setMax(number, spin->value())) {
                syncRow(number);
            }
        });

        m_rows.insert(row.Number, widgets);
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1,
                                   widgets->frame);
        syncRow(row.Number);
    }
}

void ConfigRadioOutputView::syncRow(int oneBasedChannel)
{
    RowWidgets *widgets = m_rows.value(oneBasedChannel);
    if (!widgets) {
        return;
    }
    const QList<ServoOutputRow> rows = m_viewModel->Rows();
    if (oneBasedChannel < 1 || oneBasedChannel > rows.size()) {
        return;
    }
    const ServoOutputRow &row = rows.at(oneBasedChannel - 1);

    widgets->position->setValue(row.Pwm);
    widgets->position->setFormat(QString::number(row.Pwm));
    {
        const QSignalBlocker blocker(widgets->reversed);
        widgets->reversed->setChecked(row.Reversed.value.toInt() != 0);
    }
    widgets->reversed->setEnabled(!row.Reversed.readOnly);
    {
        const QSignalBlocker blocker(widgets->function);
        widgets->function->setCurrentIndex(
            optionIndex(row.Function.options, row.Function.value));
    }
    widgets->function->setEnabled(
        !row.Function.readOnly && !row.Function.options.isEmpty());

    auto syncSpin = [](QSpinBox *spin, const ParamField &field) {
        const QSignalBlocker blocker(spin);
        spin->setValue(field.value.toInt());
        spin->setEnabled(!field.readOnly);
    };
    syncSpin(widgets->minimum, row.Min);
    syncSpin(widgets->trim, row.Trim);
    syncSpin(widgets->maximum, row.Max);
    widgets->frame->setToolTip(row.Status);
}

void ConfigRadioOutputView::clearRows()
{
    for (RowWidgets *widgets : m_rows) {
        delete widgets->frame;
        delete widgets;
    }
    m_rows.clear();
}
