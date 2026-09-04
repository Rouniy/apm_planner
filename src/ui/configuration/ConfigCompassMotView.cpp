#include "ConfigCompassMotView.h"

#include "comm/CompassCalibrationService.h"
#include "qcustomplot.h"

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

bool sameTarget(const VehicleTargetLease &left,
                const VehicleTargetLease &right)
{
    return left.isValid() && right.isValid()
        && left.generation == right.generation
        && left.endpoint.sameIdentity(right.endpoint);
}

} // namespace

ConfigCompassMotView::ConfigCompassMotView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ConfigCompassMotView"));
    buildUi();
    m_freshnessTimer.setInterval(500);
    connect(&m_freshnessTimer, &QTimer::timeout,
            this, &ConfigCompassMotView::syncState);
    m_freshnessTimer.start();
    syncState();
}

ConfigCompassMotView::~ConfigCompassMotView()
{
    deactivate();
}

QSize ConfigCompassMotView::sizeHint() const
{
    return QSize(980, 760);
}

QString ConfigCompassMotView::SafetyConfirmationTitle()
{
    return tr("Compass/Motor Safety Warning");
}

QString ConfigCompassMotView::SafetyConfirmationText()
{
    return tr(
        "DANGER: Compass/Motor calibration temporarily ARMS THE MOTORS.\n\n"
        "REMOVE ALL PROPELLERS, secure the vehicle, set throttle to zero, "
        "and keep an immediate way to disconnect power. Use only a dedicated "
        "direct link to this autopilot — not a router, radio network or "
        "multiplexed connection. After Start, slowly raise throttle to "
        "maximum over a few seconds.\n\n"
        "Start calibration now?");
}

void ConfigCompassMotView::setCalibrationContext(
    CompassCalibrationService *service,
    const VehicleTargetLease &target)
{
    QObject::disconnect(m_changedConnection);
    QObject::disconnect(m_destroyedConnection);
    m_service = service;
    m_target = target;
    m_terminalNotified = false;
    if (service) {
        m_changedConnection = connect(
            service, &CompassCalibrationService::changed,
            this, &ConfigCompassMotView::syncState);
        m_destroyedConnection = connect(
            service, &QObject::destroyed, this, [this]() {
                m_service = nullptr;
                syncState();
            });
    }
    syncState();
}

void ConfigCompassMotView::setConnected(bool connected)
{
    m_connected = connected;
    syncState();
}

void ConfigCompassMotView::setArmed(bool armed)
{
    m_armed = armed;
    syncState();
}

void ConfigCompassMotView::setSupportedVehicle(
    bool supported, const QString &reason)
{
    m_supportedVehicle = supported;
    m_unsupportedReason = reason.trimmed();
    syncState();
}

void ConfigCompassMotView::setParameterSnapshotReady(bool ready)
{
    m_parameterSnapshotReady = ready;
    syncState();
}

void ConfigCompassMotView::deactivate()
{
    if (m_service && m_service->motorMayBeActive()
        && m_service->canStopMotor()) {
        m_service->stopMotor();
    }
}

void ConfigCompassMotView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigCompassMotView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#compassMotTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QFrame#compassMotDanger { background: #3B211D;"
        " border: 1px solid #E05A47; border-radius: 4px; }"
        "QLabel#compassMotDangerText { color: #FFD6CF; font-weight: bold; }"
        "QLabel#compassMotState, QLabel#compassMotTarget { color: #E0A030; }"
        "QGroupBox { border: 1px solid #56615B; border-radius: 4px;"
        " margin-top: 9px; padding-top: 9px; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px;"
        " padding: 0 4px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Compass/Motor Calibration"), content);
    title->setObjectName(QStringLiteral("compassMotTitle"));
    layout->addWidget(title);

    auto *instructions = new QLabel(
        tr("Measures interference on the compass from the motors. REMOVE ALL "
           "PROPELLERS, secure the vehicle, then press Start and slowly raise "
           "the throttle to maximum over a few seconds. Press Finish to stop "
           "and store the result. Requires ArduCopter 3.2+."), content);
    instructions->setObjectName(QStringLiteral("compassMotInstructions"));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto *danger = new QFrame(content);
    danger->setObjectName(QStringLiteral("compassMotDanger"));
    auto *dangerLayout = new QVBoxLayout(danger);
    dangerLayout->setContentsMargins(12, 10, 12, 10);
    auto *dangerText = new QLabel(
        tr("DANGER — this procedure arms the motor outputs. Remove every "
           "propeller and secure the airframe before continuing."), danger);
    dangerText->setObjectName(QStringLiteral("compassMotDangerText"));
    dangerText->setWordWrap(true);
    dangerLayout->addWidget(dangerText);
    m_safetyCheck = new QCheckBox(
        tr("I removed all propellers, secured the vehicle and set throttle "
           "to zero, and this is a dedicated direct link to one autopilot."),
        danger);
    m_safetyCheck->setObjectName(QStringLiteral("compassMotSafetyCheck"));
    dangerLayout->addWidget(m_safetyCheck);
    layout->addWidget(danger);

    m_targetStatus = new QLabel(content);
    m_targetStatus->setObjectName(QStringLiteral("compassMotTarget"));
    m_targetStatus->setWordWrap(true);
    layout->addWidget(m_targetStatus);

    auto *controls = new QHBoxLayout;
    m_start = new QPushButton(tr("Start"), content);
    m_start->setObjectName(QStringLiteral("compassMotStart"));
    m_finish = new QPushButton(tr("Finish / Stop Motors"), content);
    m_finish->setObjectName(QStringLiteral("compassMotFinish"));
    m_clearUnsafe = new QPushButton(
        tr("Power disconnected — clear unsafe session"), content);
    m_clearUnsafe->setObjectName(QStringLiteral("compassMotClearUnsafe"));
    controls->addWidget(m_start);
    controls->addWidget(m_finish);
    controls->addWidget(m_clearUnsafe);
    controls->addStretch(1);
    layout->addLayout(controls);

    auto *live = new QGroupBox(tr("Live result"), content);
    live->setObjectName(QStringLiteral("compassMotLiveGroup"));
    auto *liveGrid = new QGridLayout(live);
    liveGrid->addWidget(new QLabel(tr("Throttle:"), live), 0, 0);
    m_throttle = new QLabel(QStringLiteral("0 %"), live);
    m_throttle->setObjectName(QStringLiteral("compassMotThrottle"));
    liveGrid->addWidget(m_throttle, 0, 1);
    liveGrid->addWidget(new QLabel(tr("Current:"), live), 0, 2);
    m_current = new QLabel(QStringLiteral("0.00 A"), live);
    m_current->setObjectName(QStringLiteral("compassMotCurrent"));
    liveGrid->addWidget(m_current, 0, 3);
    liveGrid->addWidget(new QLabel(tr("Interference:"), live), 1, 0);
    m_interference = new QLabel(QStringLiteral("0 %"), live);
    m_interference->setObjectName(QStringLiteral("compassMotInterference"));
    liveGrid->addWidget(m_interference, 1, 1);
    liveGrid->addWidget(new QLabel(tr("Comp X/Y/Z:"), live), 1, 2);
    m_compensation = new QLabel(QStringLiteral("0.00, 0.00, 0.00"), live);
    m_compensation->setObjectName(QStringLiteral("compassMotCompensation"));
    liveGrid->addWidget(m_compensation, 1, 3);
    layout->addWidget(live);

    m_plot = new QCustomPlot(content);
    m_plot->setObjectName(QStringLiteral("compassMotPlot"));
    m_plot->setMinimumHeight(300);
    m_plot->addGraph();
    m_plot->graph(0)->setName(tr("Interference"));
    m_plot->graph(0)->setPen(QPen(QColor(210, 70, 60)));
    m_plot->addGraph(m_plot->xAxis, m_plot->yAxis2);
    m_plot->graph(1)->setName(tr("Current"));
    m_plot->graph(1)->setPen(QPen(QColor(55, 155, 90)));
    m_plot->xAxis->setLabel(tr("Throttle %"));
    m_plot->xAxis->setRange(0, 100);
    m_plot->yAxis->setLabel(tr("Interference %"));
    m_plot->yAxis->setRange(0, 100);
    m_plot->yAxis2->setVisible(true);
    m_plot->yAxis2->setLabel(tr("Amps"));
    m_plot->yAxis2->setRange(0, 50);
    m_plot->legend->setVisible(true);
    layout->addWidget(m_plot);

    m_stateStatus = new QLabel(content);
    m_stateStatus->setObjectName(QStringLiteral("compassMotState"));
    m_stateStatus->setWordWrap(true);
    layout->addWidget(m_stateStatus);

    m_log = new QPlainTextEdit(content);
    m_log->setObjectName(QStringLiteral("compassMotLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(140);
    layout->addWidget(m_log);
    layout->addStretch(1);
    scroll->setWidget(content);

    connect(m_safetyCheck, &QCheckBox::toggled,
            this, &ConfigCompassMotView::syncState);
    connect(m_start, &QPushButton::clicked,
            this, &ConfigCompassMotView::startCalibration);
    connect(m_finish, &QPushButton::clicked,
            this, &ConfigCompassMotView::stopCalibration);
    connect(m_clearUnsafe, &QPushButton::clicked,
            this, &ConfigCompassMotView::clearUnsafeSession);
}

void ConfigCompassMotView::startCalibration()
{
    if (!m_service || !baseReady() || !m_safetyCheck->isChecked()) {
        return;
    }
    if (QMessageBox::warning(
            this, SafetyConfirmationTitle(), SafetyConfirmationText(),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const CompassCalibrationService::RequestResult result =
        m_service->startMotor(m_target, false, true);
    if (result == CompassCalibrationService::RequestResult::Started) {
        m_safetyCheck->setChecked(false);
        m_terminalNotified = false;
    }
    syncState();
}

void ConfigCompassMotView::stopCalibration()
{
    if (m_service && m_service->canStopMotor()) {
        m_service->stopMotor();
    }
    syncState();
}

void ConfigCompassMotView::clearUnsafeSession()
{
    if (!m_service
        || !m_service->canAcknowledgeMotorPowerDisconnected()) {
        return;
    }
    if (QMessageBox::warning(
            this, tr("Confirm Vehicle Power Is Removed"),
            tr("Only continue after the vehicle battery and every other "
               "motor-power source have been physically disconnected. "
               "This clears the warning but cannot stop a powered vehicle.\n\n"
               "Is all vehicle power disconnected?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_service->acknowledgeMotorPowerDisconnected();
}

bool ConfigCompassMotView::serviceOwnsTarget() const
{
    return m_service && sameTarget(m_service->activeTarget(), m_target);
}

bool ConfigCompassMotView::baseReady() const
{
    return m_service && m_connected
        && m_parameterSnapshotReady && m_target.isValid()
        && m_service->isCurrentTarget(m_target)
        && m_service->supportsMotorCalibration(m_target)
        && !m_service->isBusy() && !m_service->isOnboardActive();
}

void ConfigCompassMotView::syncPlot()
{
    QVector<double> throttle;
    QVector<double> interference;
    QVector<double> current;
    if (m_service && (serviceOwnsTarget() || m_service->motorMayBeActive())) {
        const QVector<CompassCalibrationService::MotorSample> samples =
            m_service->motorSamples();
        throttle.reserve(samples.size());
        interference.reserve(samples.size());
        current.reserve(samples.size());
        for (const auto &sample : samples) {
            throttle.append(sample.throttlePercent);
            interference.append(sample.interferencePercent);
            current.append(sample.currentAmps);
        }
    }
    m_plot->graph(0)->setData(throttle, interference);
    m_plot->graph(1)->setData(throttle, current);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ConfigCompassMotView::syncState()
{
    const bool ownsTarget = serviceOwnsTarget();
    const bool globalMotorDanger = m_service && m_service->motorMayBeActive();
    m_start->setEnabled(baseReady() && m_safetyCheck->isChecked());
    m_finish->setEnabled(m_service && m_service->canStopMotor());
    m_clearUnsafe->setVisible(
        m_service && m_service->canAcknowledgeMotorPowerDisconnected());
    m_clearUnsafe->setEnabled(m_clearUnsafe->isVisible());
    m_safetyCheck->setEnabled(!globalMotorDanger);

    const VehicleTargetLease displayedTarget = globalMotorDanger
        ? m_service->activeTarget() : m_target;
    if (!displayedTarget.isValid()) {
        m_targetStatus->setText(tr("No exact vehicle target selected."));
    } else {
        m_targetStatus->setText(tr(
            "Pinned target: link %1, system %2, component %3, generation %4")
            .arg(displayedTarget.endpoint.linkId)
            .arg(displayedTarget.endpoint.systemId)
            .arg(displayedTarget.endpoint.componentId)
            .arg(displayedTarget.generation));
    }

    if (globalMotorDanger) {
        const QString details = m_service->resultText();
        m_stateStatus->setText(details.isEmpty()
            ? stateText(static_cast<int>(m_service->state())) : details);
    } else if (!m_service) {
        m_stateStatus->setText(tr(
            "Compass calibration service is unavailable."));
    } else if (ownsTarget) {
        const QString details = m_service->resultText();
        m_stateStatus->setText(details.isEmpty()
            ? stateText(static_cast<int>(m_service->state())) : details);
    } else if (!m_connected) {
        m_stateStatus->setText(tr("Connect to a vehicle first."));
    } else if (!m_parameterSnapshotReady) {
        m_stateStatus->setText(tr(
            "Wait for the complete vehicle parameter snapshot."));
    } else if (!m_service->supportsMotorCalibration(m_target)
               && !m_service->isBusy()) {
        m_stateStatus->setText(tr(
            "Exact target is not ready: Compass/Motor requires a fresh "
            "disarmed ArduCopter multirotor heartbeat, a point-to-point "
            "transport and one autopilot on the physical link. Resolve any "
            "displayed uncertain or reboot-required state before retrying."));
    } else if (!m_service->hasActiveTarget()) {
        const QString details = m_service->resultText();
        m_stateStatus->setText(details.isEmpty()
            ? stateText(static_cast<int>(m_service->state())) : details);
    } else {
        m_stateStatus->setText(tr(
            "Another exact vehicle owns the active compass operation."));
    }

    CompassCalibrationService::MotorSample sample;
    if (m_service && (ownsTarget || globalMotorDanger)
        && m_service->motorHasSample()) {
        sample = m_service->latestMotorSample();
        m_throttle->setText(QStringLiteral("%1 %")
            .arg(sample.throttlePercent, 0, 'f', 1));
        m_current->setText(QStringLiteral("%1 A")
            .arg(sample.currentAmps, 0, 'f', 2));
        m_interference->setText(QStringLiteral("%1 %")
            .arg(sample.interferencePercent));
        m_compensation->setText(QStringLiteral("%1, %2, %3")
            .arg(sample.compensationX, 0, 'f', 2)
            .arg(sample.compensationY, 0, 'f', 2)
            .arg(sample.compensationZ, 0, 'f', 2));
    } else {
        m_throttle->setText(QStringLiteral("0 %"));
        m_current->setText(QStringLiteral("0.00 A"));
        m_interference->setText(QStringLiteral("0 %"));
        m_compensation->setText(QStringLiteral("0.00, 0.00, 0.00"));
    }
    m_log->setPlainText(m_service && (ownsTarget || globalMotorDanger)
        ? m_service->motorLog() : QString());
    syncPlot();

    if (m_service && ownsTarget && !m_terminalNotified
        && (m_service->state() == CompassCalibrationService::State::MotorSucceeded
            || m_service->state() == CompassCalibrationService::State::MotorFailed
            || m_service->state()
                == CompassCalibrationService::State::MotorCompletedUnverified)) {
        m_terminalNotified = true;
        emit calibrationFinished(m_target);
    }
}

QString ConfigCompassMotView::stateText(int state)
{
    switch (static_cast<CompassCalibrationService::State>(state)) {
    case CompassCalibrationService::State::MotorStartPending:
        return tr("Waiting for the vehicle start acknowledgement...");
    case CompassCalibrationService::State::MotorRunning:
        return tr("Calibration active. Slowly raise throttle, then Finish.");
    case CompassCalibrationService::State::MotorStopPending:
        return tr("Stopping motors and waiting for terminal evidence...");
    case CompassCalibrationService::State::MotorStopSettling:
        return tr("Motors stopped; waiting for the stored-result message...");
    case CompassCalibrationService::State::MotorSucceeded:
        return tr("Calibration succeeded and was stored by the vehicle.");
    case CompassCalibrationService::State::MotorFailed:
        return tr("Calibration failed or produced no usable data.");
    case CompassCalibrationService::State::MotorCompletedUnverified:
        return tr("Calibration stopped; stored result is unverified.");
    case CompassCalibrationService::State::MotorOutcomeUncertain:
        return tr("CRITICAL: stop outcome is unknown; motors may still run.");
    default:
        return tr("Ready for Compass/Motor calibration.");
    }
}
