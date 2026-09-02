#include "ConfigMotorTestView.h"

#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

ConfigMotorTestView::ConfigMotorTestView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigMotorTestViewModel(this))
{
    setObjectName(QStringLiteral("ConfigMotorTestView"));
    setStyleSheet(QStringLiteral(
        "ConfigMotorTestView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#motorTestTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QFrame#motorTestDanger { background: #5A2D2D;"
        " border-radius: 4px; }"
        "QLabel#motorTestInstructions { color: #FFD0D0; }"
        "QLabel[frameLabel=\"true\"] { color: #E6EDE9;"
        " font-weight: bold; }"
        "QSpinBox, QPushButton { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 4px; }"
        "QPushButton:disabled, QSpinBox:disabled { color: #68736D; }"
        "QLabel#motorTestStatus { color: #2F81F7; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(0);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("motorTestTitle"));
    root->addWidget(title);

    auto *danger = new QFrame(this);
    danger->setObjectName(QStringLiteral("motorTestDanger"));
    auto *dangerLayout = new QVBoxLayout(danger);
    dangerLayout->setContentsMargins(12, 12, 12, 12);
    auto *instructions = new QLabel(m_viewModel->Instructions(), danger);
    instructions->setObjectName(QStringLiteral("motorTestInstructions"));
    instructions->setWordWrap(true);
    dangerLayout->addWidget(instructions);
    root->addSpacing(8);
    root->addWidget(danger);
    root->addSpacing(8);

    auto *frameRow = new QWidget(this);
    frameRow->setObjectName(QStringLiteral("motorTestFrameRow"));
    auto *frameLayout = new QHBoxLayout(frameRow);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(16);
    m_frameClassLabel = new QLabel(frameRow);
    m_frameClassLabel->setObjectName(QStringLiteral("motorTestFrameClass"));
    m_frameClassLabel->setProperty("frameLabel", true);
    m_frameTypeLabel = new QLabel(frameRow);
    m_frameTypeLabel->setObjectName(QStringLiteral("motorTestFrameType"));
    m_frameTypeLabel->setProperty("frameLabel", true);
    frameLayout->addWidget(m_frameClassLabel);
    frameLayout->addWidget(m_frameTypeLabel);
    frameLayout->addStretch(1);
    root->addWidget(frameRow);
    root->addSpacing(8);

    auto *controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("motorTestControls"));
    auto *controlLayout = new QGridLayout(controls);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setHorizontalSpacing(12);
    auto *throttleLabel = new QLabel(tr("Throttle %"), controls);
    throttleLabel->setObjectName(QStringLiteral("motorTestThrottleLabel"));
    m_throttleEditor = new QSpinBox(controls);
    m_throttleEditor->setObjectName(
        QStringLiteral("motorTestThrottleEditor"));
    m_throttleEditor->setRange(0, 100);
    m_throttleEditor->setSingleStep(1);
    m_throttleEditor->setMinimumWidth(100);
    auto *durationLabel = new QLabel(tr("Duration (s)"), controls);
    durationLabel->setObjectName(QStringLiteral("motorTestDurationLabel"));
    m_durationEditor = new QSpinBox(controls);
    m_durationEditor->setObjectName(
        QStringLiteral("motorTestDurationEditor"));
    m_durationEditor->setRange(0, 60);
    m_durationEditor->setSingleStep(1);
    m_durationEditor->setMinimumWidth(100);
    m_setSpinArmButton = new QPushButton(
        tr("Set MOT_SPIN_ARM"), controls);
    m_setSpinArmButton->setObjectName(
        QStringLiteral("motorTestSetSpinArm"));
    m_setSpinMinButton = new QPushButton(
        tr("Set MOT_SPIN_MIN"), controls);
    m_setSpinMinButton->setObjectName(
        QStringLiteral("motorTestSetSpinMin"));
    controlLayout->addWidget(throttleLabel, 0, 0);
    controlLayout->addWidget(m_throttleEditor, 0, 1);
    controlLayout->addWidget(durationLabel, 0, 2);
    controlLayout->addWidget(m_durationEditor, 0, 3);
    controlLayout->addWidget(m_setSpinArmButton, 0, 4);
    controlLayout->addWidget(m_setSpinMinButton, 0, 5);
    controlLayout->setColumnStretch(6, 1);
    root->addWidget(controls);
    root->addSpacing(10);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("motorTestScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *motorContent = new QWidget(scroll);
    motorContent->setObjectName(QStringLiteral("motorTestMotorContent"));
    m_motorsLayout = new QVBoxLayout(motorContent);
    m_motorsLayout->setContentsMargins(0, 0, 0, 0);
    m_motorsLayout->setSpacing(6);
    scroll->setWidget(motorContent);
    root->addWidget(scroll, 1);

    m_sequenceRow = new QWidget(motorContent);
    m_sequenceRow->setObjectName(QStringLiteral("motorTestSequenceRow"));
    auto *sequenceLayout = new QHBoxLayout(m_sequenceRow);
    sequenceLayout->setContentsMargins(0, 8, 0, 0);
    sequenceLayout->setSpacing(8);
    m_testAllButton = new QPushButton(
        tr("Test all in sequence"), m_sequenceRow);
    m_testAllButton->setObjectName(QStringLiteral("motorTestAllButton"));
    m_stopAllButton = new QPushButton(
        tr("Stop all motors"), m_sequenceRow);
    m_stopAllButton->setObjectName(QStringLiteral("motorStopAllButton"));
    sequenceLayout->addWidget(m_testAllButton);
    sequenceLayout->addWidget(m_stopAllButton);
    sequenceLayout->addStretch(1);
    m_motorsLayout->addWidget(m_sequenceRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("motorTestStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setContentsMargins(0, 8, 0, 0);
    root->addWidget(m_statusLabel);

    connect(m_viewModel, &ConfigMotorTestViewModel::motorsChanged,
            this, &ConfigMotorTestView::rebuildMotors);
    connect(m_viewModel, &ConfigMotorTestViewModel::settingsChanged,
            this, &ConfigMotorTestView::syncSettings);
    connect(m_viewModel, &ConfigMotorTestViewModel::stateChanged,
            this, &ConfigMotorTestView::syncState);
    connect(m_viewModel, &ConfigMotorTestViewModel::statusChanged,
            this, &ConfigMotorTestView::syncState);
    connect(m_viewModel, &ConfigMotorTestViewModel::motorTestRequested,
            this, &ConfigMotorTestView::motorTestRequested);
    connect(m_viewModel, &ConfigMotorTestViewModel::writeRequested,
            this, &ConfigMotorTestView::writeRequested);
    connect(m_throttleEditor,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &ConfigMotorTestViewModel::setThrottlePercent);
    connect(m_durationEditor,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &ConfigMotorTestViewModel::setDurationSec);
    connect(m_setSpinArmButton, &QPushButton::clicked,
            m_viewModel, &ConfigMotorTestViewModel::SetSpinArm);
    connect(m_setSpinMinButton, &QPushButton::clicked,
            m_viewModel, &ConfigMotorTestViewModel::SetSpinMin);
    connect(m_testAllButton, &QPushButton::clicked, this, [this]() {
        m_viewModel->TestAllSequence(confirmMotorTest());
    });
    connect(m_stopAllButton, &QPushButton::clicked,
            m_viewModel, &ConfigMotorTestViewModel::StopAll);

    m_viewModel->setCatalog(catalog);
    syncSettings();
    syncState();
}

QString ConfigMotorTestView::SafetyConfirmationTitle()
{
    return tr("Motor Test Safety Warning");
}

QString ConfigMotorTestView::SafetyConfirmationText()
{
    return tr(
        "REMOVE ALL PROPELLERS before continuing.\n\n"
        "The selected motor or motor sequence will spin immediately at the "
        "configured throttle for the configured duration. Keep clear of the "
        "vehicle.\n\nContinue?");
}

QSize ConfigMotorTestView::sizeHint() const
{
    return QSize(800, 640);
}

void ConfigMotorTestView::hideEvent(QHideEvent *event)
{
    if (m_viewModel->Connected()
        && m_viewModel->ShouldAutoStop()) {
        m_viewModel->StopAll();
    }
    QWidget::hideEvent(event);
}

void ConfigMotorTestView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigMotorTestView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigMotorTestView::setMotorLayoutJson(const QByteArray &json)
{
    m_viewModel->setMotorLayoutJson(json);
}

void ConfigMotorTestView::setVehicleType(int mavType)
{
    m_viewModel->setVehicleType(mavType);
}

void ConfigMotorTestView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigMotorTestView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigMotorTestView::commandAckReceived(
    int componentId, int command, int result)
{
    m_viewModel->commandAckReceived(componentId, command, result);
}

void ConfigMotorTestView::commandSendFailed(
    int componentId, const QString &reason)
{
    m_viewModel->commandSendFailed(componentId, reason);
}

void ConfigMotorTestView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigMotorTestView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

bool ConfigMotorTestView::confirmMotorTest()
{
    if (!m_viewModel->Connected() || m_viewModel->Armed()
        || m_viewModel->Busy()) {
        return false;
    }
    return QMessageBox::warning(
        this, SafetyConfirmationTitle(), SafetyConfirmationText(),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel) == QMessageBox::Ok;
}

void ConfigMotorTestView::rebuildMotors()
{
    while (QLayoutItem *item = m_motorsLayout->takeAt(0)) {
        if (item->widget() != m_sequenceRow) {
            delete item->widget();
        }
        delete item;
    }
    m_motorButtons.clear();
    const QList<MotorTestItem> motors = m_viewModel->Motors();
    for (const MotorTestItem &motor : motors) {
        auto *row = new QWidget(this);
        row->setObjectName(
            QStringLiteral("motorTestRow_%1").arg(motor.TestOrder));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 2, 0, 2);
        layout->setSpacing(12);
        auto *button = new QPushButton(motor.Label, row);
        button->setObjectName(
            QStringLiteral("motorTestButton_%1").arg(motor.TestOrder));
        button->setMinimumWidth(220);
        auto *rotation = new QLabel(motor.Rotation, row);
        rotation->setObjectName(
            QStringLiteral("motorTestRotation_%1").arg(motor.TestOrder));
        layout->addWidget(button);
        layout->addWidget(rotation);
        layout->addStretch(1);
        m_motorButtons.insert(motor.TestOrder, button);
        m_motorsLayout->addWidget(row);
        connect(button, &QPushButton::clicked, this,
                [this, order = motor.TestOrder]() {
            m_viewModel->TestMotor(order, confirmMotorTest());
        });
    }
    m_motorsLayout->addWidget(m_sequenceRow);
    m_motorsLayout->addStretch(1);
    syncState();
}

void ConfigMotorTestView::syncSettings()
{
    const QSignalBlocker throttleBlocker(m_throttleEditor);
    const QSignalBlocker durationBlocker(m_durationEditor);
    m_throttleEditor->setValue(m_viewModel->ThrottlePercent());
    m_durationEditor->setValue(m_viewModel->DurationSec());
}

void ConfigMotorTestView::syncState()
{
    m_frameClassLabel->setText(m_viewModel->FrameClass());
    m_frameTypeLabel->setText(m_viewModel->FrameType());
    m_statusLabel->setText(m_viewModel->Status());
    const bool canRun = m_viewModel->CanRun();
    for (QPushButton *button : m_motorButtons) {
        button->setEnabled(canRun);
    }
    m_testAllButton->setEnabled(canRun);
    m_setSpinArmButton->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Armed()
        && !m_viewModel->Busy()
        && !m_viewModel->NeedsEmergencyStop());
    m_setSpinMinButton->setEnabled(m_setSpinArmButton->isEnabled());
    m_throttleEditor->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Armed()
        && !m_viewModel->Busy());
    m_durationEditor->setEnabled(m_throttleEditor->isEnabled());
    m_stopAllButton->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Motors().isEmpty());
}
