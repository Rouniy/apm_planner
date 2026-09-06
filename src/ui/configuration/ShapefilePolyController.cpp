#include "ShapefilePolyController.h"

#include <QtConcurrentRun>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <exception>
#include <limits>
#include <utility>

namespace
{
QString projectionDescription(const QString &projection)
{
    return projection.trimmed().isEmpty()
        ? QObject::tr("coordinates treated as WGS84")
        : QObject::tr("reprojected from %1").arg(projection);
}

QString warningsSummary(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return {};
    const int displayed = qMin(3, warnings.size());
    QString result = warnings.mid(0, displayed).join(QStringLiteral(" "));
    if (warnings.size() > displayed) {
        result += QObject::tr(" (+%1 more warning(s))")
            .arg(warnings.size() - displayed);
    }
    return result;
}
} // namespace

struct ShapefilePolyController::JobState
{
    std::atomic_bool cancelled{false};
    QMutex mutex;
    qint64 completed = 0;
    qint64 total = 0;
    QString phase;
};

ShapefilePolyController::ShapefilePolyController(QWidget *owner)
    : QObject(owner)
    , m_owner(owner)
    , m_progressTimer(new QTimer(this))
    , m_prepare([](const QString &input, const Shapefile::Cancel &cancel,
                   const Shapefile::Progress &progress) {
        return ShapefileImportService::prepare(input, cancel, progress);
    })
    , m_export([](const ShapefileImportService::Plan &plan,
                  const Shapefile::Cancel &cancel,
                  const Shapefile::Progress &progress) {
        return ShapefileImportService::exportPolyFiles(plan, cancel, progress);
    })
{
    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &ShapefilePolyController::updateProgress);
    if (owner)
        owner->installEventFilter(this);
}

ShapefilePolyController::~ShapefilePolyController()
{
    m_destroying = true;
    ++m_flow;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_release);
    if (m_progressTimer)
        m_progressTimer->stop();
    if (m_prompt) {
        const QPointer<QDialog> prompt(m_prompt);
        m_prompt.clear();
        disconnect(prompt.data(), nullptr, nullptr, nullptr);
        prompt->blockSignals(true);
        if (!QCoreApplication::closingDown()) {
            prompt->reject();
            if (prompt)
                prompt->deleteLater();
        }
    }
    if (m_progress) {
        const QPointer<QProgressDialog> progress(m_progress);
        m_progress.clear();
        disconnect(progress.data(), nullptr, nullptr, nullptr);
        progress->blockSignals(true);
        if (!QCoreApplication::closingDown()) {
            progress->hide();
            if (progress)
                progress->deleteLater();
        }
    }
}

bool ShapefilePolyController::busy() const noexcept
{
    return m_phase != Phase::Idle;
}

void ShapefilePolyController::setBackend(
    Prepare prepare, Export exportFiles)
{
    if (busy()) {
        emitLog(tr("Shapefile to POLY: the backend cannot change during an active workflow."));
        return;
    }
    m_prepare = std::move(prepare);
    m_export = std::move(exportFiles);
}

bool ShapefilePolyController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_owner && event && event->type() == QEvent::Close
        && busy()) {
        const QPointer<ShapefilePolyController> guard(this);
        const QPointer<QObject> watchedGuard(watched);
        cancel();
        if (!guard || !watchedGuard)
            return true;
    }
    return QObject::eventFilter(watched, event);
}

bool ShapefilePolyController::emitLog(const QString &message)
{
    const QPointer<ShapefilePolyController> guard(this);
    emit logMessage(message);
    return !guard.isNull();
}

void ShapefilePolyController::start()
{
    if (m_destroying)
        return;
    if (busy()) {
        emitLog(tr("Shapefile to POLY: this controller is already busy."));
        return;
    }
    if (!m_owner) {
        emitLog(tr("Shapefile to POLY: no window is available for selecting a shapefile."));
        return;
    }
    if (!m_prepare || !m_export) {
        emitLog(tr("Shapefile to POLY: the conversion backend is unavailable."));
        return;
    }

    m_input.clear();
    m_plan = ShapefileImportService::Plan();
    m_job.reset();
    m_cancelReported = false;
    m_lastReportedPercent = -1;
    m_phase = Phase::InputPrompt;
    ++m_flow;
    if (m_flow == 0)
        ++m_flow;
    const quint64 flow = m_flow;
    const QPointer<ShapefilePolyController> guard(this);
    emit busyChanged(true);
    if (!guard || m_destroying || flow != m_flow
        || m_phase != Phase::InputPrompt) {
        return;
    }
    showInputPrompt(flow);
}

void ShapefilePolyController::showInputPrompt(quint64 flow)
{
    if (m_destroying || flow != m_flow
        || m_phase != Phase::InputPrompt || !m_owner) {
        return;
    }
    auto *dialog = new QFileDialog(
        m_owner, tr("Select shapefile to convert"));
    dialog->setObjectName(QStringLiteral("DeveloperShapefilePolyInputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setNameFilters({
        tr("ESRI Shapefile (*.shp *.SHP)"), tr("All files (*)")});
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow
            || m_phase != Phase::InputPrompt || m_prompt != dialog) {
            return;
        }
        m_prompt.clear();
        const QStringList selected = dialog->selectedFiles();
        if (result != QDialog::Accepted || selected.size() != 1) {
            finishWorkflow(flow, {tr(
                "Shapefile to POLY: cancelled before a source file was selected.")});
            return;
        }
        startPreparation(flow, selected.first());
    });
    const QPointer<ShapefilePolyController> guard(this);
    dialog->open();
    if (!guard)
        return;
}

void ShapefilePolyController::showProgress(
    const QString &objectName, const QString &title, const QString &label)
{
    if (!m_owner)
        return;
    auto *progress = new QProgressDialog(label, tr("Cancel"), 0, 0, m_owner);
    progress->setObjectName(objectName);
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    m_progress = progress;
    // Preserve a visible draining state after the button is clicked. Qt5's
    // built-in old-style connection otherwise hides/resets before our slot.
    disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));
    connect(progress, &QProgressDialog::canceled,
            this, &ShapefilePolyController::cancel);
    connect(progress, &QDialog::rejected,
            this, &ShapefilePolyController::cancel);
    progress->show();
}

void ShapefilePolyController::startPreparation(
    quint64 flow, const QString &input)
{
    if (m_destroying || flow != m_flow
        || m_phase != Phase::InputPrompt || !m_owner
        || input.isEmpty() || !m_prepare) {
        finishWorkflow(flow, {tr(
            "Shapefile to POLY failed: the frozen source or backend is unavailable.")});
        return;
    }

    const Prepare prepare = m_prepare;
    auto state = std::make_shared<JobState>();
    m_input = input;
    m_job = state;
    m_phase = Phase::Preparing;
    m_lastReportedPercent = -1;
    const QPointer<ShapefilePolyController> guard(this);
    showProgress(QStringLiteral("DeveloperShapefilePolyPrepareProgressDialog"),
                 tr("Convert Shapefile to POLY"),
                 tr("Reading shapefile geometry and projection…"));
    if (!guard || flow != m_flow || m_phase != Phase::Preparing
        || m_job != state || !m_progress) {
        return;
    }

    auto *watcher = new QFutureWatcher<ShapefileImportService::Preparation>(this);
    m_watcher = watcher;
    connect(watcher,
            &QFutureWatcher<ShapefileImportService::Preparation>::finished,
            this, [this, watcher, state, flow]() {
        const ShapefileImportService::Preparation result = watcher->result();
        watcher->deleteLater();
        finishPreparation(flow, state, result);
    });
    const QString frozenInput = m_input;
    watcher->setFuture(QtConcurrent::run(
        [prepare, frozenInput, state]() {
        try {
            const auto cancel = [state]() {
                return state->cancelled.load(std::memory_order_acquire);
            };
            const auto progress = [state](qint64 completed, qint64 total,
                                          const QString &phase) {
                QMutexLocker locker(&state->mutex);
                state->completed = qMax<qint64>(0, completed);
                state->total = qMax<qint64>(0, total);
                state->phase = phase;
            };
            return prepare(frozenInput, cancel, progress);
        } catch (const std::exception &exception) {
            ShapefileImportService::Preparation result;
            result.error = QStringLiteral("Unexpected preparation failure: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
            return result;
        } catch (...) {
            ShapefileImportService::Preparation result;
            result.error = QStringLiteral("Unexpected preparation failure.");
            return result;
        }
    }));
    if (!guard || flow != m_flow || m_phase != Phase::Preparing
        || m_job != state || m_watcher != watcher) {
        return;
    }
    m_progressTimer->start();
    emitLog(tr("Shapefile to POLY: reading %1 …").arg(m_input));
}

QStringList ShapefilePolyController::preparationFailureMessages(
    const ShapefileImportService::Preparation &preparation)
{
    if (preparation.cancelled) {
        return {tr("Shapefile to POLY: preparation cancelled; no output files were changed.")};
    }
    return {tr("Shapefile to POLY preparation failed: %1").arg(
        preparation.error.isEmpty()
            ? tr("the service returned an invalid plan")
            : preparation.error)};
}

void ShapefilePolyController::finishPreparation(
    quint64 flow, const std::shared_ptr<JobState> &state,
    const ShapefileImportService::Preparation &preparation)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Preparing
        || m_job != state) {
        return;
    }
    m_progressTimer->stop();
    m_job.reset();
    m_watcher.clear();
    if (state->cancelled.load(std::memory_order_acquire)) {
        finishWorkflow(flow, {tr(
            "Shapefile to POLY: preparation cancelled; no output files were changed.")});
        return;
    }
    if (preparation.cancelled || !preparation.error.isEmpty()
        || !preparation.plan.isValid()) {
        finishWorkflow(flow, preparationFailureMessages(preparation));
        return;
    }
    m_plan = preparation.plan;
    if (m_plan.outputs().isEmpty()) {
        QStringList messages{tr(
            "Shapefile to POLY: no non-empty geometry with valid WGS84 coordinates found; no output files were changed.")};
        const QString warning = warningsSummary(m_plan.warnings());
        if (!warning.isEmpty())
            messages.append(tr("Shapefile to POLY warnings: %1").arg(warning));
        finishWorkflow(flow, messages);
        return;
    }

    m_phase = Phase::Consent;
    if (!dismissProgress() || flow != m_flow
        || m_phase != Phase::Consent) {
        return;
    }
    showConsent(flow);
}

void ShapefilePolyController::showConsent(quint64 flow)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Consent
        || !m_owner || !m_plan.isValid() || m_plan.outputs().isEmpty()) {
        return;
    }

    auto *dialog = new QDialog(m_owner);
    dialog->setObjectName(QStringLiteral("DeveloperShapefilePolyConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Convert Shapefile to POLY"));
    dialog->resize(900, 600);
    dialog->setMinimumSize(700, 400);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("DeveloperShapefilePolySummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    const QVector<ShapefileImportService::Output> outputs = m_plan.outputs();
    const QStringList planWarnings = m_plan.warnings();
    summary->setText(tr(
        "Create one poly-N.poly file per non-empty SHP feature next to the selected shapefile.\n\n"
        "Source: %1\nOutput directory: %2\nOutputs: %3\nPoints: %4\nProjection: %5\nDiscarded invalid points: %6\n\n"
        "Every exact path below is part of the immutable plan. Existing paths marked Replace will be replaced atomically; Create paths must remain absent. Cancellation or failure after export starts can leave already published listed files. No vehicle is contacted.")
        .arg(m_plan.source(), m_plan.directory(),
             QString::number(outputs.size()),
             QString::number(m_plan.pointCount()),
             projectionDescription(m_plan.projectionName()),
             QString::number(m_plan.discardedPoints())));
    layout->addWidget(summary);

    auto *tree = new QTreeWidget(dialog);
    tree->setObjectName(QStringLiteral("DeveloperShapefilePolyOutputs"));
    tree->setColumnCount(3);
    tree->setHeaderLabels({tr("Action"), tr("Points"), tr("Exact output path")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const ShapefileImportService::Output &output : outputs) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, output.replaces ? tr("Replace") : tr("Create"));
        item->setText(1, QString::number(output.pointCount));
        item->setText(2, output.path);
        item->setToolTip(2, output.path);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->setColumnWidth(2, 540);
    layout->addWidget(tree, 1);

    if (!planWarnings.isEmpty()) {
        auto *warnings = new QPlainTextEdit(dialog);
        warnings->setObjectName(QStringLiteral("DeveloperShapefilePolyWarnings"));
        warnings->setReadOnly(true);
        warnings->setMaximumHeight(100);
        warnings->setPlainText(planWarnings.join(QLatin1Char('\n')));
        layout->addWidget(warnings);
    }

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Yes | QDialogButtonBox::Cancel, dialog);
    auto *convert = buttons->button(QDialogButtonBox::Yes);
    convert->setObjectName(QStringLiteral("DeveloperShapefilePolyConfirmButton"));
    convert->setText(tr("Convert and replace"));
    convert->setAutoDefault(false);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::Consent
            || m_prompt != dialog) {
            return;
        }
        m_prompt.clear();
        if (result != QDialog::Accepted) {
            finishWorkflow(flow, {tr(
                "Shapefile to POLY: cancelled before any output file was changed.")});
            return;
        }
        startExport(flow);
    });
    const QPointer<ShapefilePolyController> guard(this);
    dialog->open();
    if (!guard)
        return;
}

void ShapefilePolyController::startExport(quint64 flow)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Consent
        || !m_owner || !m_plan.isValid() || m_plan.outputs().isEmpty()
        || !m_export) {
        finishWorkflow(flow, {tr(
            "Shapefile to POLY failed: the frozen output plan or backend is unavailable.")});
        return;
    }
    const Export exportFiles = m_export;
    const ShapefileImportService::Plan plan = m_plan;
    auto state = std::make_shared<JobState>();
    m_job = state;
    m_phase = Phase::Exporting;
    m_lastReportedPercent = -1;
    const QPointer<ShapefilePolyController> guard(this);
    showProgress(QStringLiteral("DeveloperShapefilePolyExportProgressDialog"),
                 tr("Convert Shapefile to POLY"),
                 tr("Writing the confirmed POLY output plan…"));
    if (!guard || flow != m_flow || m_phase != Phase::Exporting
        || m_job != state || !m_progress) {
        return;
    }

    auto *watcher = new QFutureWatcher<ShapefileImportService::Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<ShapefileImportService::Result>::finished,
            this, [this, watcher, state, flow]() {
        const ShapefileImportService::Result result = watcher->result();
        watcher->deleteLater();
        finishExport(flow, state, result);
    });
    watcher->setFuture(QtConcurrent::run(
        [exportFiles, plan, state]() {
        try {
            const auto cancel = [state]() {
                return state->cancelled.load(std::memory_order_acquire);
            };
            const auto progress = [state](qint64 completed, qint64 total,
                                          const QString &phase) {
                QMutexLocker locker(&state->mutex);
                state->completed = qMax<qint64>(0, completed);
                state->total = qMax<qint64>(0, total);
                state->phase = phase;
            };
            return exportFiles(plan, cancel, progress);
        } catch (const std::exception &exception) {
            ShapefileImportService::Result result;
            result.error = QStringLiteral("Unexpected export failure: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
            return result;
        } catch (...) {
            ShapefileImportService::Result result;
            result.error = QStringLiteral("Unexpected export failure.");
            return result;
        }
    }));
    if (!guard || flow != m_flow || m_phase != Phase::Exporting
        || m_job != state || m_watcher != watcher) {
        return;
    }
    m_progressTimer->start();
    emitLog(tr("Shapefile to POLY: writing %1 confirmed output file(s) …")
                .arg(plan.outputs().size()));
}

QStringList ShapefilePolyController::exportResultMessages(
    const ShapefileImportService::Result &result)
{
    QStringList messages;
    const QString outputs = result.files.isEmpty()
        ? QString()
        : tr(" First published output: %1%2")
            .arg(result.files.first(), result.files.size() > 1
                ? tr("; last: %1").arg(result.files.last()) : QString());
    if (result.cancelled) {
        messages.append(tr(
            "Shapefile to POLY: export cancelled after publishing %1 file(s) and %2 point(s).%3 No unlisted success is implied.")
            .arg(QString::number(result.files.size()),
                 QString::number(result.pointCount), outputs));
    } else if (!result.success) {
        messages.append(tr(
            "Shapefile to POLY failed: %1 Already published: %2 file(s), %3 point(s).%4")
            .arg(result.error.isEmpty() ? tr("unknown export failure") : result.error,
                 QString::number(result.files.size()),
                 QString::number(result.pointCount), outputs));
    } else if (result.files.isEmpty()) {
        messages.append(tr(
            "Shapefile to POLY: no non-empty geometry with valid WGS84 coordinates found; no output files were changed."));
    } else {
        messages.append(tr(
            "Shapefile to POLY: wrote %1 file(s), %2 point(s), %3.%4")
            .arg(QString::number(result.files.size()),
                 QString::number(result.pointCount),
                 projectionDescription(result.projectionName), outputs));
    }
    const QString warning = warningsSummary(result.warnings);
    if (!warning.isEmpty())
        messages.append(tr("Shapefile to POLY warnings: %1").arg(warning));
    return messages;
}

void ShapefilePolyController::finishExport(
    quint64 flow, const std::shared_ptr<JobState> &state,
    const ShapefileImportService::Result &result)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Exporting
        || m_job != state) {
        return;
    }
    finishWorkflow(flow, exportResultMessages(result));
}

void ShapefilePolyController::finishWorkflow(
    quint64 flow, const QStringList &messages)
{
    if (m_destroying || flow != m_flow || m_phase == Phase::Idle)
        return;
    ++m_flow;
    const quint64 terminalFlow = m_flow;
    m_phase = Phase::Idle;
    m_progressTimer->stop();
    m_job.reset();
    m_watcher.clear();
    m_plan = ShapefileImportService::Plan();
    m_input.clear();
    if (!dismissPrompt()
        || m_flow != terminalFlow || m_phase != Phase::Idle) {
        return;
    }
    if (!dismissProgress()
        || m_flow != terminalFlow || m_phase != Phase::Idle) {
        return;
    }
    const QPointer<ShapefilePolyController> guard(this);
    emit busyChanged(false);
    if (!guard)
        return;
    for (const QString &message : messages) {
        if (!emitLog(message))
            return;
    }
}

void ShapefilePolyController::cancel()
{
    if (m_destroying)
        return;
    if (m_phase == Phase::Idle) {
        emitLog(tr("Shapefile to POLY: no conversion is running."));
        return;
    }
    if (m_phase == Phase::InputPrompt || m_phase == Phase::Consent) {
        finishWorkflow(m_flow, {tr(
            "Shapefile to POLY: cancelled before any output file was changed.")});
        return;
    }
    const std::shared_ptr<JobState> state = m_job;
    if (!state)
        return;
    state->cancelled.store(true, std::memory_order_release);
    const bool shouldReport = !m_cancelReported;
    m_cancelReported = true;
    const QPointer<ShapefilePolyController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    if (progress) {
        const QString detail = m_phase == Phase::Exporting
            ? tr("Cancellation requested; waiting at the next output boundary. Already published files remain changed…")
            : tr("Cancellation requested; waiting at the next read boundary. No output files have been changed…");
        progress->setLabelText(detail);
        if (!guard || !progress || m_progress != progress)
            return;
        const auto buttons = progress->findChildren<QPushButton *>(
            QString(), Qt::FindDirectChildrenOnly);
        const QPointer<QPushButton> cancelButton(
            buttons.isEmpty() ? nullptr : buttons.first());
        if (cancelButton)
            cancelButton->setEnabled(false);
        if (!guard || !progress || m_progress != progress)
            return;
    }
    if (!guard || !shouldReport)
        return;
    emitLog(m_phase == Phase::Exporting
        ? tr("Shapefile to POLY: export cancellation requested; already published exact outputs will be reported.")
        : tr("Shapefile to POLY: preparation cancellation requested; no outputs have been written."));
}

bool ShapefilePolyController::dismissPrompt()
{
    const QPointer<ShapefilePolyController> guard(this);
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        disconnect(prompt.data(), nullptr, this, nullptr);
        prompt->blockSignals(true);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
    return !guard.isNull();
}

bool ShapefilePolyController::dismissProgress()
{
    const QPointer<ShapefilePolyController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        disconnect(progress.data(), nullptr, this, nullptr);
        progress->blockSignals(true);
        progress->hide();
        if (progress)
            progress->deleteLater();
    }
    return !guard.isNull();
}

void ShapefilePolyController::updateProgress()
{
    const std::shared_ptr<JobState> state = m_job;
    if (m_destroying || (m_phase != Phase::Preparing
                         && m_phase != Phase::Exporting)
        || !state || !m_progress || m_cancelReported
        || state->cancelled.load(std::memory_order_acquire)) {
        return;
    }
    qint64 completed = 0;
    qint64 total = 0;
    QString detail;
    {
        QMutexLocker locker(&state->mutex);
        completed = state->completed;
        total = state->total;
        detail = state->phase;
    }
    const QPointer<ShapefilePolyController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    const auto stale = [this, guard, progress, state]() {
        return !guard || !progress || m_progress != progress || m_job != state
            || (m_phase != Phase::Preparing && m_phase != Phase::Exporting)
            || m_cancelReported
            || state->cancelled.load(std::memory_order_acquire);
    };
    if (total > 0 && total <= std::numeric_limits<int>::max()) {
        const int maximum = int(total);
        const int value = int(qBound<qint64>(0, completed, total));
        progress->setRange(0, maximum);
        if (stale())
            return;
        progress->setValue(value);
    } else {
        progress->setRange(0, 0);
    }
    if (stale())
        return;
    if (!detail.isEmpty())
        progress->setLabelText(detail);
    if (stale() || total <= 0)
        return;
    const qint64 boundedCompleted = qBound<qint64>(0, completed, total);
    const int percent = boundedCompleted >= total
        ? 100
        : int((static_cast<long double>(boundedCompleted) * 100.0L)
              / static_cast<long double>(total));
    if (percent != 100 && percent != 0
        && m_lastReportedPercent >= 0
        && percent - m_lastReportedPercent < 5) {
        return;
    }
    m_lastReportedPercent = percent;
    emitLog(tr("Shapefile to POLY: %1% — %2")
                .arg(percent).arg(detail));
}
