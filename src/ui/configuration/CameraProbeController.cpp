#include "CameraProbeController.h"

#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QWidget>

CameraProbeController::CameraProbeController(
    CameraProbeService *service, QWidget *owner)
    : QObject(owner), m_owner(owner), m_service(service)
{
    if (m_owner)
        m_owner->installEventFilter(this);
    if (m_service) {
        connect(m_service, &CameraProbeService::stateChanged,
                this, &CameraProbeController::updateProgress);
        connect(m_service, &CameraProbeService::operationFinished,
                this, &CameraProbeController::handleFinished);
        connect(m_service, &CameraProbeService::logMessage,
                this, [this](const QString &message) {
            const QPointer<CameraProbeService> service(m_service);
            if (!m_destroying && service && m_ownedOperationId != 0
                && service->currentOperationId()
                    == m_ownedOperationId) {
                emitLog(message);
            }
        });
        connect(m_service, &QObject::destroyed,
                this, &CameraProbeController::handleServiceDestroyed);
    }
}

CameraProbeController::~CameraProbeController()
{
    m_destroying = true;
    if (m_owner)
        m_owner->removeEventFilter(this);
    const QPointer<CameraProbeService> service(m_service);
    if (service)
        disconnect(service, nullptr, this, nullptr);
    const quint64 operationId = m_ownedOperationId;
    m_ownedOperationId = 0;
    if (service && operationId != 0
        && service->busy()
        && service->currentOperationId() == operationId) {
        service->cancel(operationId);
    }
    if (QCoreApplication::closingDown()) {
        m_prompt.clear();
        m_progress.clear();
        return;
    }
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        disconnect(prompt, nullptr, nullptr, nullptr);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        disconnect(progress, nullptr, nullptr, nullptr);
        progress->hide();
        if (progress)
            progress->deleteLater();
    }
}

bool CameraProbeController::busy() const noexcept
{
    return m_phase != Phase::Idle || (m_service && m_service->busy());
}

void CameraProbeController::start()
{
    const QPointer<CameraProbeController> guard(this);
    const QPointer<CameraProbeService> service(m_service);
    if (m_destroying || m_phase != Phase::Idle || !service) {
        if (!service)
            emitLog(tr("Camera probe service is unavailable."));
        return;
    }
    if (service->busy()) {
        emitLog(tr("Camera probe: another camera probe is already running."));
        return;
    }

    const quint64 flow = ++m_flow;
    m_phase = Phase::Preparing;
    m_cancelRequested = false;
    emit busyChanged(true);
    if (!guard || m_destroying || flow != m_flow
        || m_phase != Phase::Preparing || m_service != service) {
        return;
    }

    CameraProbeService::Plan plan;
    QString error;
    const bool prepared = service->prepare(&plan, &error);
    if (!guard || !service || m_destroying || flow != m_flow
        || m_phase != Phase::Preparing || m_service != service) {
        return;
    }
    if (!prepared) {
        finishFlow(flow, error.isEmpty()
            ? tr("Camera probe is unavailable.") : error);
        return;
    }
    m_plan = plan;
    showConsent(flow, plan);
}

void CameraProbeController::showConsent(
    quint64 flow, const CameraProbeService::Plan &plan)
{
    if (m_destroying || !m_owner || !m_service || flow != m_flow
        || m_phase != Phase::Preparing || !plan.isValid()) {
        finishFlow(flow, tr("Camera probe was cancelled before confirmation."));
        return;
    }

    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Probe MAVLink Camera"),
        CameraProbeService::ConfirmationText(plan),
        QMessageBox::Yes | QMessageBox::Cancel, m_owner);
    dialog->setObjectName(
        QStringLiteral("DeveloperCameraProbeConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    if (auto *yes = qobject_cast<QPushButton *>(
            dialog->button(QMessageBox::Yes))) {
        yes->setAutoDefault(false);
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    m_phase = Phase::Consent;
    const QPointer<CameraProbeController> guard(this);
    const QPointer<CameraProbeService> service(m_service);
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, flow, plan](int result) {
        if (!guard || m_destroying || flow != m_flow
            || m_phase != Phase::Consent || m_service != service) {
            return;
        }
        m_prompt.clear();
        if (result != QMessageBox::Yes) {
            finishFlow(flow,
                tr("Camera probe cancelled; no probe command was sent."));
            return;
        }
        beginProbe(flow, plan);
    });
    dialog->open();
}

void CameraProbeController::beginProbe(
    quint64 flow, const CameraProbeService::Plan &plan)
{
    const QPointer<CameraProbeController> guard(this);
    const QPointer<CameraProbeService> service(m_service);
    if (m_destroying || !service || flow != m_flow
        || m_phase != Phase::Consent) {
        return;
    }

    QString error;
    const bool valid = service->validate(plan, &error);
    if (!guard || !service || m_destroying || flow != m_flow
        || m_phase != Phase::Consent || m_service != service) {
        return;
    }
    if (!valid) {
        finishFlow(flow, tr("Camera probe cancelled: %1").arg(error));
        return;
    }

    auto *progress = new QProgressDialog(
        tr("Starting the six-command camera probe…"), tr("Cancel"),
        0, CameraProbeService::Commands().size(), m_owner);
    progress->setObjectName(
        QStringLiteral("DeveloperCameraProbeProgressDialog"));
    progress->setWindowTitle(tr("Probe MAVLink Camera"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    // Qt 5 wires canceled() back to cancel(), which hides the dialog before
    // an asynchronous service can publish its terminal report.
    disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));
    m_progress = progress;
    m_phase = Phase::Starting;
    connect(progress, &QProgressDialog::canceled,
            this, &CameraProbeController::cancel);
    connect(progress, &QDialog::rejected,
            this, &CameraProbeController::cancel);
    const QPointer<QProgressDialog> progressGuard(progress);
    progress->show();
    if (!guard || !progressGuard || m_destroying || flow != m_flow
        || m_phase != Phase::Starting || m_service != service) {
        return;
    }

    m_ownedOperationId = 0;
    const bool started = service->execute(
        plan, &m_ownedOperationId, &error);
    if (!guard || !service || m_destroying || flow != m_flow
        || m_service != service) {
        return;
    }
    if (!started) {
        m_ownedOperationId = 0;
        finishFlow(flow,
            tr("Camera probe was not started: %1").arg(error));
        return;
    }
    // A fully synchronous test transport may already have delivered the
    // terminal report and returned this flow to Idle.
    if (m_phase == Phase::Starting)
        m_phase = Phase::Running;
    updateProgress();
}

void CameraProbeController::cancel()
{
    if (m_destroying || m_phase == Phase::Idle)
        return;
    const quint64 flow = m_flow;
    if (m_phase == Phase::Preparing || m_phase == Phase::Consent) {
        const QPointer<CameraProbeController> guard(this);
        ++m_flow;
        const quint64 terminalFlow = m_flow;
        m_phase = Phase::Idle;
        m_plan = {};
        dismissPrompt();
        if (!guard || terminalFlow != m_flow)
            return;
        dismissProgress();
        if (!guard || terminalFlow != m_flow)
            return;
        if (!emitLog(tr(
                "Camera probe cancelled; no probe command was sent."))
            || terminalFlow != m_flow) {
            return;
        }
        emit busyChanged(busy());
        return;
    }

    const QPointer<CameraProbeController> guard(this);
    const QPointer<CameraProbeService> service(m_service);
    const quint64 operationId = m_ownedOperationId;
    if (operationId == 0 && m_phase == Phase::Starting) {
        finishFlow(flow,
            tr("Camera probe cancelled before any command was sent."));
        return;
    }
    if (!service || operationId == 0
        || service->currentOperationId() != operationId) {
        finishFlow(flow,
            tr("Camera probe ownership changed; no foreign operation was cancelled."));
        return;
    }
    if (m_cancelRequested)
        return;
    m_cancelRequested = true;
    const QPointer<QProgressDialog> progress(m_progress);
    if (progress) {
        QPointer<QPushButton> cancelButton(
            progress->findChild<QPushButton *>());
        progress->setLabelText(tr(
            "Cancellation requested. Waiting for the exact probe to stop; "
            "commands already sent cannot be undone."));
        if (!guard || !progress)
            return;
        if (cancelButton)
            cancelButton->setEnabled(false);
        if (!guard || !service)
            return;
    }
    service->cancel(operationId);
}

void CameraProbeController::shutdown()
{
    if (m_destroying)
        return;
    cancel();
}

void CameraProbeController::updateProgress()
{
    const QPointer<CameraProbeController> guard(this);
    const QPointer<CameraProbeService> service(m_service);
    const QPointer<QProgressDialog> progress(m_progress);
    const quint64 flow = m_flow;
    const quint64 operationId = m_ownedOperationId;
    if (m_destroying || QCoreApplication::closingDown()
        || !service || !progress
        || m_ownedOperationId == 0
        || service->currentOperationId() != m_ownedOperationId) {
        return;
    }
    const CameraProbeService::Report report = service->currentReport();
    int completed = 0;
    for (const CameraProbeService::StepResult &step : report.steps) {
        if (step.outcome != CameraProbeService::StepOutcome::NotSent)
            ++completed;
    }
    progress->setValue(completed);
    if (!guard || !service || !progress || m_cancelRequested
        || flow != m_flow || operationId != m_ownedOperationId
        || m_service != service || m_progress != progress
        || service->currentOperationId() != operationId) {
        return;
    }
    progress->setLabelText(tr(
        "Camera probe: %1 of %2 commands reached a terminal result. "
        "Responses remain visible in MAVLink Inspector.")
        .arg(completed).arg(report.steps.size()));
}

void CameraProbeController::handleFinished(
    const CameraProbeService::Report &report)
{
    if (m_destroying || report.operationId == 0
        || report.operationId != m_ownedOperationId) {
        return;
    }
    const quint64 flow = m_flow;
    m_ownedOperationId = 0;
    // CameraProbeService publishes the terminal description through its
    // bounded history/log signal before operationFinished(). Do not duplicate
    // it in the page log here.
    finishFlow(flow);
}

void CameraProbeController::handleServiceDestroyed()
{
    if (m_destroying)
        return;
    m_service.clear();
    m_ownedOperationId = 0;
    if (QCoreApplication::closingDown()) {
        ++m_flow;
        m_phase = Phase::Idle;
        m_plan = {};
        m_cancelRequested = false;
        m_prompt.clear();
        m_progress.clear();
        return;
    }
    finishFlow(m_flow, tr("Camera probe service became unavailable."));
}

void CameraProbeController::finishFlow(
    quint64 flow, const QString &message)
{
    if (m_destroying || flow != m_flow)
        return;
    const quint64 terminalFlow = ++m_flow;
    m_phase = Phase::Idle;
    m_plan = {};
    m_cancelRequested = false;
    if (!dismissPrompt() || terminalFlow != m_flow)
        return;
    if (!dismissProgress() || terminalFlow != m_flow)
        return;
    if (!message.isEmpty() && !emitLog(message))
        return;
    if (terminalFlow != m_flow)
        return;
    emit busyChanged(busy());
}

bool CameraProbeController::dismissPrompt()
{
    const QPointer<CameraProbeController> guard(this);
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        const bool wasBlocked = prompt->blockSignals(true);
        prompt->reject();
        if (!guard)
            return false;
        if (prompt) {
            prompt->blockSignals(wasBlocked);
            prompt->deleteLater();
        }
    }
    return guard;
}

bool CameraProbeController::dismissProgress()
{
    const QPointer<CameraProbeController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        const bool wasBlocked = progress->blockSignals(true);
        progress->hide();
        if (!guard)
            return false;
        if (progress) {
            progress->blockSignals(wasBlocked);
            progress->deleteLater();
        }
    }
    return guard;
}

bool CameraProbeController::emitLog(const QString &message)
{
    const QPointer<CameraProbeController> guard(this);
    if (!message.isEmpty())
        emit logMessage(message);
    return guard;
}

bool CameraProbeController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_owner && event && event->type() == QEvent::Close) {
        const QPointer<QObject> watchedGuard(watched);
        const QPointer<CameraProbeController> guard(this);
        cancel();
        if (!watchedGuard)
            return true;
        if (!guard)
            return false;
    }
    return QObject::eventFilter(watched, event);
}
