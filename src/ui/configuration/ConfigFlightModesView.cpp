#include "ConfigFlightModesView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kRcFreshnessMs = 2000;

const QUrl kSuperSimpleHelpUrl(QStringLiteral(
    "https://ardupilot.org/copter/docs/simpleandsuper-simple-modes.html"));

bool valuesEqual(const QVariant &left, const QVariant &right)
{
    if (!left.isValid() || !right.isValid()) {
        return !left.isValid() && !right.isValid();
    }
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

} // namespace

ConfigFlightModesView::ConfigFlightModesView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigFlightModesViewModel(this)),
      m_rcFreshnessTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("ConfigFlightModesView"));
    m_rcFreshnessTimer->setObjectName(
        QStringLiteral("flightModesRcFreshnessTimer"));
    m_rcFreshnessTimer->setInterval(kRcFreshnessMs);
    m_rcFreshnessTimer->setSingleShot(true);

    buildUi();

    connect(m_viewModel, &ConfigFlightModesViewModel::optionsChanged,
            this, &ConfigFlightModesView::syncOptions);
    connect(m_viewModel, &ConfigFlightModesViewModel::rowsChanged,
            this, &ConfigFlightModesView::syncRows);
    connect(m_viewModel, &ConfigFlightModesViewModel::stateChanged,
            this, &ConfigFlightModesView::syncState);
    connect(m_viewModel, &ConfigFlightModesViewModel::writeRequested,
            this, &ConfigFlightModesView::writeRequested);
    connect(m_viewModel, &ConfigFlightModesViewModel::refreshRequested,
            this, &ConfigFlightModesView::refreshRequested);
    connect(m_rcFreshnessTimer, &QTimer::timeout,
            m_viewModel, &ConfigFlightModesViewModel::clearRcInput);

    syncOptions();
    syncRows();
    syncState();
}

ConfigFlightModesView::~ConfigFlightModesView() = default;

QSize ConfigFlightModesView::sizeHint() const
{
    return QSize(800, 640);
}

void ConfigFlightModesView::setFamily(
    ConfigFlightModesViewModel::Family family,
    const QList<ParamOption> &modeOptions)
{
    m_rcFreshnessTimer->stop();
    m_viewModel->setFamily(family, modeOptions);
}

void ConfigFlightModesView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot,
    bool preserveStagedEdits)
{
    m_rcFreshnessTimer->stop();
    m_viewModel->setParameterSnapshot(
        parameters, preferredComponent, completeSnapshot,
        preserveStagedEdits);
}

void ConfigFlightModesView::setConnected(bool connected)
{
    if (!connected) {
        m_rcFreshnessTimer->stop();
    }
    m_viewModel->setConnected(connected);
}

void ConfigFlightModesView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigFlightModesView::setHeartbeat(
    quint32 customMode, bool fresh, bool armed)
{
    m_viewModel->setHeartbeat(customMode, fresh, armed);
}

void ConfigFlightModesView::setHeartbeatFresh(bool fresh)
{
    m_viewModel->setHeartbeatFresh(fresh);
}

bool ConfigFlightModesView::setRcInput(
    int oneBasedChannel, int pwm)
{
    const bool accepted = m_viewModel->setRcInput(oneBasedChannel, pwm);
    if (accepted) {
        m_rcFreshnessTimer->start();
    }
    return accepted;
}

void ConfigFlightModesView::clearRcInput()
{
    m_rcFreshnessTimer->stop();
    m_viewModel->clearRcInput();
}

void ConfigFlightModesView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigFlightModesView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigFlightModesView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigFlightModesView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigFlightModesView::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    m_viewModel->parameterWriteCancelled(batchId, componentId, name);
}

void ConfigFlightModesView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigFlightModesView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigFlightModesView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigFlightModesView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigFlightModesView, QWidget#flightModesContent {"
        " background: #1A201D; color: #E6EDE9; }"
        "QLabel#flightModesTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#flightModesIntro, QLabel[flightModesHeader=\"true\"] {"
        " color: #C8C8C8; }"
        "QLabel#flightModesCurrentMode, QLabel#flightModesCurrentPwm,"
        " QLabel#flightModesStatus { color: #34D399; }"
        "QFrame[flightModeRow=\"true\"] { background: #202623;"
        " border: 1px solid #303A35; border-radius: 3px; }"
        "QFrame[flightModeActive=\"true\"] { background: #3D5A1E;"
        " border: 1px solid #34D399; }"
        "QComboBox, QPushButton { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #303A35; padding: 5px; }"
        "QComboBox:disabled, QPushButton:disabled, QCheckBox:disabled {"
        " color: #68736D; }"
        "QPushButton#flightModesSuperSimpleHelp { background: transparent;"
        " color: cornflowerblue; border: none; padding: 0; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("flightModesScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("flightModesContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Flight Modes"), content);
    title->setObjectName(QStringLiteral("flightModesTitle"));
    layout->addWidget(title);

    auto *intro = new QLabel(
        tr("Assign a vehicle flight mode to each position of the transmitter "
           "mode switch."), content);
    intro->setObjectName(QStringLiteral("flightModesIntro"));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    layout->addWidget(intro);

    auto *telemetry = new QWidget(content);
    telemetry->setObjectName(QStringLiteral("flightModesTelemetry"));
    auto *telemetryLayout = new QHBoxLayout(telemetry);
    telemetryLayout->setContentsMargins(0, 0, 0, 2);
    telemetryLayout->setSpacing(8);
    auto *currentModeLabel = new QLabel(tr("Current Mode:"), telemetry);
    currentModeLabel->setObjectName(
        QStringLiteral("flightModesCurrentModeLabel"));
    telemetryLayout->addWidget(currentModeLabel);
    m_currentMode = new QLabel(telemetry);
    m_currentMode->setObjectName(QStringLiteral("flightModesCurrentMode"));
    m_currentMode->setMinimumWidth(120);
    m_currentMode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    telemetryLayout->addWidget(m_currentMode);
    auto *currentPwmLabel = new QLabel(tr("Current PWM:"), telemetry);
    currentPwmLabel->setObjectName(
        QStringLiteral("flightModesCurrentPwmLabel"));
    currentPwmLabel->setContentsMargins(16, 0, 0, 0);
    telemetryLayout->addWidget(currentPwmLabel);
    m_currentPwm = new QLabel(telemetry);
    m_currentPwm->setObjectName(QStringLiteral("flightModesCurrentPwm"));
    m_currentPwm->setTextInteractionFlags(Qt::TextSelectableByMouse);
    telemetryLayout->addWidget(m_currentPwm);
    telemetryLayout->addStretch(1);
    layout->addWidget(telemetry);

    auto *header = new QWidget(content);
    header->setObjectName(QStringLiteral("flightModesHeader"));
    auto *headerLayout = new QGridLayout(header);
    headerLayout->setContentsMargins(4, 0, 4, 0);
    headerLayout->setHorizontalSpacing(8);
    headerLayout->setColumnMinimumWidth(0, 110);
    headerLayout->setColumnMinimumWidth(1, 220);
    headerLayout->setColumnMinimumWidth(2, 80);
    headerLayout->setColumnMinimumWidth(3, 110);
    headerLayout->setColumnStretch(4, 1);

    auto *modeHeader = new QLabel(tr("Mode"), header);
    modeHeader->setObjectName(QStringLiteral("flightModesModeHeader"));
    modeHeader->setProperty("flightModesHeader", true);
    headerLayout->addWidget(modeHeader, 0, 1);
    m_simpleHeader = new QLabel(tr("Simple"), header);
    m_simpleHeader->setObjectName(QStringLiteral("flightModesSimpleHeader"));
    m_simpleHeader->setProperty("flightModesHeader", true);
    headerLayout->addWidget(m_simpleHeader, 0, 2);
    m_superSimpleHeader = new QLabel(tr("Super Simple"), header);
    m_superSimpleHeader->setObjectName(
        QStringLiteral("flightModesSuperSimpleHeader"));
    m_superSimpleHeader->setProperty("flightModesHeader", true);
    headerLayout->addWidget(m_superSimpleHeader, 0, 3);
    auto *pwmHeader = new QLabel(tr("PWM Range"), header);
    pwmHeader->setObjectName(QStringLiteral("flightModesPwmHeader"));
    pwmHeader->setProperty("flightModesHeader", true);
    headerLayout->addWidget(pwmHeader, 0, 4);
    layout->addWidget(header);

    auto *rows = new QWidget(content);
    rows->setObjectName(QStringLiteral("flightModesRows"));
    auto *rowsLayout = new QVBoxLayout(rows);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(4);
    m_rows.reserve(6);
    for (int rowIndex = 0; rowIndex < 6; ++rowIndex) {
        const int position = rowIndex + 1;
        RowWidgets widgets;
        widgets.frame = new QFrame(rows);
        widgets.frame->setObjectName(
            QStringLiteral("flightModeRow%1").arg(position));
        widgets.frame->setProperty("flightModeRow", true);
        widgets.frame->setProperty("flightModeActive", false);
        auto *rowLayout = new QGridLayout(widgets.frame);
        rowLayout->setContentsMargins(4, 5, 4, 5);
        rowLayout->setHorizontalSpacing(8);
        rowLayout->setColumnMinimumWidth(0, 110);
        rowLayout->setColumnMinimumWidth(1, 220);
        rowLayout->setColumnMinimumWidth(2, 80);
        rowLayout->setColumnMinimumWidth(3, 110);
        rowLayout->setColumnStretch(4, 1);

        widgets.label = new QLabel(
            tr("Flight Mode %1").arg(position), widgets.frame);
        widgets.label->setObjectName(
            QStringLiteral("flightModeLabel%1").arg(position));
        rowLayout->addWidget(widgets.label, 0, 0);

        widgets.mode = new QComboBox(widgets.frame);
        widgets.mode->setObjectName(
            QStringLiteral("flightModeCombo%1").arg(position));
        widgets.mode->setMinimumWidth(200);
        rowLayout->addWidget(widgets.mode, 0, 1);

        widgets.simple = new QCheckBox(widgets.frame);
        widgets.simple->setObjectName(
            QStringLiteral("flightModeSimple%1").arg(position));
        widgets.simple->setAccessibleName(
            tr("Flight Mode %1 Simple").arg(position));
        rowLayout->addWidget(widgets.simple, 0, 2, Qt::AlignCenter);

        widgets.superSimple = new QCheckBox(widgets.frame);
        widgets.superSimple->setObjectName(
            QStringLiteral("flightModeSuperSimple%1").arg(position));
        widgets.superSimple->setAccessibleName(
            tr("Flight Mode %1 Super Simple").arg(position));
        rowLayout->addWidget(
            widgets.superSimple, 0, 3, Qt::AlignCenter);

        widgets.pwm = new QLabel(widgets.frame);
        widgets.pwm->setObjectName(
            QStringLiteral("flightModePwm%1").arg(position));
        rowLayout->addWidget(widgets.pwm, 0, 4);

        connect(widgets.mode, QOverload<int>::of(&QComboBox::activated),
                this, [this, rowIndex, combo = widgets.mode](int index) {
            if (index < 0
                || !m_viewModel->stageMode(
                    rowIndex, combo->itemData(index))) {
                syncRows();
            }
        });
        connect(widgets.simple, &QCheckBox::toggled,
                this, [this, rowIndex](bool checked) {
            if (!m_viewModel->stageSimple(rowIndex, checked)) {
                syncRows();
            }
        });
        connect(widgets.superSimple, &QCheckBox::toggled,
                this, [this, rowIndex](bool checked) {
            if (!m_viewModel->stageSuperSimple(rowIndex, checked)) {
                syncRows();
            }
        });

        rowsLayout->addWidget(widgets.frame);
        m_rows.append(widgets);
    }
    layout->addWidget(rows);

    m_status = new QLabel(content);
    m_status->setObjectName(QStringLiteral("flightModesStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);

    auto *actions = new QWidget(content);
    actions->setObjectName(QStringLiteral("flightModesActions"));
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 2, 0, 0);
    actionsLayout->setSpacing(12);
    m_save = new QPushButton(tr("Save Modes"), actions);
    m_save->setObjectName(QStringLiteral("flightModesSave"));
    actionsLayout->addWidget(m_save);
    m_refresh = new QPushButton(tr("Refresh Params"), actions);
    m_refresh->setObjectName(QStringLiteral("flightModesRefresh"));
    actionsLayout->addWidget(m_refresh);
    m_help = new QPushButton(tr("Super Simple Modes"), actions);
    m_help->setObjectName(QStringLiteral("flightModesSuperSimpleHelp"));
    m_help->setToolTip(kSuperSimpleHelpUrl.toString());
    m_help->setCursor(Qt::PointingHandCursor);
    m_help->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    actionsLayout->addWidget(m_help);
    actionsLayout->addStretch(1);
    layout->addWidget(actions);
    layout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_save, &QPushButton::clicked,
            m_viewModel, &ConfigFlightModesViewModel::Save);
    connect(m_refresh, &QPushButton::clicked,
            m_viewModel, &ConfigFlightModesViewModel::Refresh);
    connect(m_help, &QPushButton::clicked,
            this, [this]() { emit helpRequested(kSuperSimpleHelpUrl); });
}

void ConfigFlightModesView::syncOptions()
{
    const QList<ParamOption> options = m_viewModel->ModeOptions();
    for (RowWidgets &widgets : m_rows) {
        const QSignalBlocker blocker(widgets.mode);
        widgets.mode->clear();
        for (const ParamOption &option : options) {
            widgets.mode->addItem(option.text, option.value);
        }
    }
    syncRows();
}

void ConfigFlightModesView::syncRows()
{
    const QList<FlightModeRow> rows = m_viewModel->Rows();
    const bool showSimple = m_viewModel->ShowSimple();
    const bool showSuperSimple = m_viewModel->ShowSuperSimple();
    m_simpleHeader->setVisible(showSimple);
    m_superSimpleHeader->setVisible(showSuperSimple);
    m_help->setVisible(showSimple || showSuperSimple);

    for (int index = 0; index < m_rows.size(); ++index) {
        RowWidgets &widgets = m_rows[index];
        const bool hasRow = index < rows.size();
        const FlightModeRow row = hasRow ? rows.at(index) : FlightModeRow();
        widgets.frame->setVisible(hasRow);
        if (!hasRow) {
            continue;
        }
        widgets.label->setText(tr("Flight Mode %1").arg(row.position));
        widgets.frame->setProperty("flightModePosition", row.position);
        widgets.frame->setProperty("parameterName", row.parameterName);
        {
            const QSignalBlocker blocker(widgets.mode);
            widgets.mode->setCurrentIndex(
                optionIndex(widgets.mode, row.mode));
        }
        {
            const QSignalBlocker blocker(widgets.simple);
            widgets.simple->setChecked(row.simple);
        }
        {
            const QSignalBlocker blocker(widgets.superSimple);
            widgets.superSimple->setChecked(row.superSimple);
        }
        widgets.simple->setVisible(showSimple);
        widgets.superSimple->setVisible(showSuperSimple);
        widgets.pwm->setText(row.pwmBand);
        updateActiveStyle(widgets.frame, row.active);
    }
    syncState();
}

void ConfigFlightModesView::syncState()
{
    const bool canEdit = m_viewModel->CanEdit();
    for (RowWidgets &widgets : m_rows) {
        widgets.mode->setEnabled(canEdit && widgets.mode->count() > 0);
        widgets.simple->setEnabled(
            canEdit && m_viewModel->HasSimpleParameter());
        widgets.superSimple->setEnabled(
            canEdit && m_viewModel->HasSuperSimpleParameter());
    }
    m_save->setEnabled(m_viewModel->CanSave());
    m_refresh->setEnabled(
        m_viewModel->Connected()
        && m_viewModel->VehicleFamily()
            != ConfigFlightModesViewModel::Family::Unsupported
        && !m_viewModel->HasPendingWrites());
    m_currentMode->setText(m_viewModel->CurrentModeText());
    m_currentPwm->setText(m_viewModel->CurrentPwmText());
    m_status->setText(m_viewModel->Status());
}

void ConfigFlightModesView::updateActiveStyle(
    QFrame *frame, bool active)
{
    if (frame->property("flightModeActive").toBool() == active) {
        return;
    }
    frame->setProperty("flightModeActive", active);
    frame->style()->unpolish(frame);
    frame->style()->polish(frame);
    frame->update();
}

int ConfigFlightModesView::optionIndex(
    const QComboBox *combo, const QVariant &value)
{
    for (int index = 0; index < combo->count(); ++index) {
        if (valuesEqual(combo->itemData(index), value)) {
            return index;
        }
    }
    return -1;
}
