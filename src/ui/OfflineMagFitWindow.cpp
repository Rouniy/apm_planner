#include "OfflineMagFitWindow.h"

#include <QtConcurrentRun>

#include <QAbstractButton>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>

struct OfflineMagFitWindow::AnalysisState
{
    std::atomic_bool cancelled{false};
    std::atomic_int progressPermille{0};
};

class OfflineMagFitWindow::ApplyContext
{
public:
    OfflineMagFitApplyService::Plan plan;
    quint64 analysisRevision = 0;
    quint64 bindingRevision = 0;
};

namespace
{
QString vectorText(const MagVector &value)
{
    return QStringLiteral("%1, %2, %3")
        .arg(value.x, 0, 'f', 2)
        .arg(value.y, 0, 'f', 2)
        .arg(value.z, 0, 'f', 2);
}

QString outcomeText(OfflineMagFitApplyService::Outcome outcome)
{
    switch (outcome) {
    case OfflineMagFitApplyService::Outcome::Completed:
        return OfflineMagFitWindow::tr("completed");
    case OfflineMagFitApplyService::Outcome::Cancelled:
        return OfflineMagFitWindow::tr("cancelled before any uncertain write");
    case OfflineMagFitApplyService::Outcome::Rejected:
        return OfflineMagFitWindow::tr("rejected");
    case OfflineMagFitApplyService::Outcome::OutcomeUncertain:
        return OfflineMagFitWindow::tr("stopped with an uncertain partial outcome");
    }
    return OfflineMagFitWindow::tr("finished");
}
} // namespace

OfflineMagFitWindow::OfflineMagFitWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("OfflineMagFitWindow"));
    setWindowTitle(tr("Offline Magnetometer Calibration (MagFit)"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1180, 620);
    setMinimumSize(900, 460);
    buildUi();

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(50);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_closing || QCoreApplication::closingDown())
            return;
        const std::shared_ptr<AnalysisState> state = m_analysisState;
        if (!state)
            return;
        const int value = std::clamp(
            state->progressPermille.load(std::memory_order_relaxed) / 10,
            0, 100);
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(value);
        m_status->setText(value < 72
            ? tr("Reading magnetometer samples…")
            : tr("Fitting calibration model…"));
    });

    connect(m_browseButton, &QPushButton::clicked,
            this, &OfflineMagFitWindow::browse);
    connect(m_analyzeButton, &QPushButton::clicked,
            this, &OfflineMagFitWindow::analyze);
    connect(m_cancelAnalysisButton, &QPushButton::clicked,
            this, &OfflineMagFitWindow::cancelAnalysis);
    connect(m_applyButton, &QPushButton::clicked,
            this, &OfflineMagFitWindow::beginApply);
    connect(m_throttle, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
        if (analysisBusy()) {
            m_analysisState->cancelled.store(true, std::memory_order_relaxed);
            m_status->setText(tr("The throttle threshold changed; discarding the active analysis."));
        } else {
            invalidateAnalysis(tr("The throttle threshold changed; analyze the log again."));
        }
    });
    connect(m_ellipsoid, &QCheckBox::toggled, this, [this](bool) {
        if (analysisBusy()) {
            m_analysisState->cancelled.store(true, std::memory_order_relaxed);
            m_status->setText(tr("The fit model changed; discarding the active analysis."));
        } else {
            invalidateAnalysis(tr("The fit model changed; analyze the log again."));
        }
    });
    refreshControls();
}

OfflineMagFitWindow::~OfflineMagFitWindow()
{
    m_closing = true;
    ++m_analysisRevision;
    ++m_applyPromptRevision;
    if (m_analysisState)
        m_analysisState->cancelled.store(true, std::memory_order_relaxed);
    if (m_applyService && m_ownedApplyOperationId != 0
        && m_applyService->currentOperationId() == m_ownedApplyOperationId) {
        m_applyService->cancel(m_ownedApplyOperationId);
    }
    if (m_sourceDialog) {
        const QSignalBlocker blocker(m_sourceDialog);
        m_sourceDialog->reject();
    }
    if (m_applyPrompt) {
        const QSignalBlocker blocker(m_applyPrompt);
        m_applyPrompt->reject();
    }
    if (m_applyProgress) {
        const QSignalBlocker blocker(m_applyProgress);
        m_applyProgress->cancel();
    }
}

void OfflineMagFitWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("Offline Magnetometer Calibration (MagFit)"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *description = new QLabel(
        tr("Port of Mission Planner MagCalib.ProcessLog. It removes offsets recorded "
           "in the log, fits a sphere or full soft-iron ellipsoid, and reports each "
           "detected compass separately."), this);
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    auto *sourceLayout = new QGridLayout;
    m_sourcePath = new QLineEdit(this);
    m_sourcePath->setObjectName(QStringLiteral("OfflineMagFitSourcePath"));
    m_sourcePath->setReadOnly(true);
    m_sourcePath->setPlaceholderText(tr("Select .tlog, .bin or .log"));
    m_browseButton = new QPushButton(tr("Browse…"), this);
    m_browseButton->setObjectName(QStringLiteral("OfflineMagFitBrowseButton"));
    m_analyzeButton = new QPushButton(tr("Analyze"), this);
    m_analyzeButton->setObjectName(QStringLiteral("OfflineMagFitAnalyzeButton"));
    m_cancelAnalysisButton = new QPushButton(tr("Cancel"), this);
    m_cancelAnalysisButton->setObjectName(QStringLiteral("OfflineMagFitCancelButton"));
    sourceLayout->addWidget(m_sourcePath, 0, 0);
    sourceLayout->addWidget(m_browseButton, 0, 1);
    sourceLayout->addWidget(m_analyzeButton, 0, 2);
    sourceLayout->addWidget(m_cancelAnalysisButton, 0, 3);
    sourceLayout->setColumnStretch(0, 1);
    root->addLayout(sourceLayout);

    auto *options = new QGridLayout;
    options->addWidget(new QLabel(tr("Minimum throttle for .tlog (%)"), this), 0, 0);
    m_throttle = new QDoubleSpinBox(this);
    m_throttle->setObjectName(QStringLiteral("OfflineMagFitThrottleThreshold"));
    m_throttle->setRange(0.0, 100.0);
    m_throttle->setDecimals(0);
    m_throttle->setSingleStep(1.0);
    m_throttle->setValue(30.0);
    options->addWidget(m_throttle, 0, 1);
    m_ellipsoid = new QCheckBox(tr("Fit soft-iron ellipsoid (DIA/ODI)"), this);
    m_ellipsoid->setObjectName(QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
    m_ellipsoid->setChecked(true);
    options->addWidget(m_ellipsoid, 0, 2);
    auto *dataFlash = new QLabel(
        tr("BIN/LOG analysis uses all MAG samples, matching the official workflow."), this);
    dataFlash->setTextFormat(Qt::PlainText);
    options->addWidget(dataFlash, 0, 3);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("OfflineMagFitProgressBar"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFixedWidth(150);
    options->addWidget(m_progress, 0, 4);
    options->setColumnStretch(3, 1);
    root->addLayout(options);

    m_results = new QTableWidget(this);
    m_results->setObjectName(QStringLiteral("OfflineMagFitResultsTable"));
    m_results->setColumnCount(9);
    m_results->setHorizontalHeaderLabels({
        tr("Compass"), tr("Model"), tr("Samples"), tr("Coverage"),
        tr("Logged OFS X, Y, Z"), tr("New OFS X, Y, Z"),
        tr("DIA X, Y, Z"), tr("ODI X, Y, Z"), tr("RMS")});
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setAlternatingRowColors(true);
    m_results->verticalHeader()->setVisible(false);
    m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_results->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(m_results, 1);

    auto *safety = new QLabel(
        tr("Safety: analyze first, inspect coverage and RMS, then apply only to the "
           "same disarmed vehicle that produced the log. Applying is never automatic."), this);
    safety->setWordWrap(true);
    safety->setTextFormat(Qt::PlainText);
    safety->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(160,48,48,45); border: 1px solid #c05050; "
        "padding: 7px; }"));
    root->addWidget(safety);

    auto *bottom = new QGridLayout;
    m_status = new QLabel(
        tr("Choose a .tlog, .bin or .log file. Analysis is offline; applying values "
           "is a separate confirmed action."), this);
    m_status->setObjectName(QStringLiteral("OfflineMagFitStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    bottom->addWidget(m_status, 0, 0);
    m_applyButton = new QPushButton(tr("Apply to connected vehicle…"), this);
    m_applyButton->setObjectName(QStringLiteral("OfflineMagFitApplyButton"));
    m_applyButton->setMinimumWidth(210);
    bottom->addWidget(m_applyButton, 0, 1);
    bottom->setColumnStretch(0, 1);
    root->addLayout(bottom);

    m_history = new QPlainTextEdit(this);
    m_history->setObjectName(QStringLiteral("OfflineMagFitHistory"));
    m_history->setReadOnly(true);
    m_history->setMaximumBlockCount(256);
    m_history->setMaximumHeight(85);
    m_history->setPlaceholderText(tr("Offline MagFit activity and apply results"));
    root->addWidget(m_history);
}

void OfflineMagFitWindow::setApplyService(OfflineMagFitApplyService *service)
{
    if (m_applyService == service) {
        refreshApplyState();
        return;
    }

    const quint64 revision = ++m_applyBindingRevision;
    ++m_applyPromptRevision;
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<OfflineMagFitApplyService> incoming(service);
    const QPointer<OfflineMagFitApplyService> old(m_applyService);
    const quint64 owned = m_ownedApplyOperationId;
    dismissApplyPrompt();
    if (!guard || revision != m_applyBindingRevision)
        return;
    if (old && owned != 0 && old->currentOperationId() == owned)
        old->cancel(owned);
    if (!guard || revision != m_applyBindingRevision)
        return;
    if (old)
        disconnect(old, nullptr, this, nullptr);

    m_applyService = incoming;
    m_ownedApplyOperationId = 0;
    m_applyContext.reset();
    m_seenApplyHistory.clear();
    if (m_applyProgress) {
        const QPointer<QProgressDialog> progress(m_applyProgress);
        m_applyProgress.clear();
        if (progress) {
            const QSignalBlocker blocker(progress);
            progress->cancel();
            progress->deleteLater();
        }
    }

    if (incoming) {
        connect(incoming, &OfflineMagFitApplyService::stateChanged,
                this, &OfflineMagFitWindow::refreshApplyState);
        connect(incoming, &OfflineMagFitApplyService::operationFinished,
                this, [this, revision](const OfflineMagFitApplyService::Report &report) {
            handleApplyFinished(revision, report);
        });
        connect(incoming, &QObject::destroyed, this, [this, revision]() {
            if (revision != m_applyBindingRevision)
                return;
            const QPointer<OfflineMagFitWindow> guard(this);
            ++m_applyBindingRevision;
            ++m_applyPromptRevision;
            m_applyService.clear();
            m_ownedApplyOperationId = 0;
            m_applyContext.reset();
            const QPointer<QDialog> prompt(m_applyPrompt);
            const QPointer<QProgressDialog> progress(m_applyProgress);
            m_applyPrompt.clear();
            m_applyProgress.clear();
            if (m_closing || QCoreApplication::closingDown())
                return;
            if (prompt) {
                const QSignalBlocker blocker(prompt);
                prompt->reject();
            }
            if (!guard)
                return;
            if (progress) {
                const QSignalBlocker blocker(progress);
                progress->cancel();
                progress->deleteLater();
            }
            appendHistory(tr("Apply service became unavailable."));
            refreshControls();
        });
    }
    refreshApplyState();
}

bool OfflineMagFitWindow::setSource(const QString &path)
{
    if (busy()) {
        m_status->setText(tr("Finish or cancel the current MagFit operation before opening another log."));
        return false;
    }
    if (path.trimmed().isEmpty()) {
        m_status->setText(tr("Choose a flight log first."));
        return false;
    }
    const QString fullPath = QFileInfo(path).absoluteFilePath();
    if (m_source != fullPath) {
        invalidateAnalysis(tr("The selected log changed; analyze it again."));
        m_source = fullPath;
        m_sourcePath->setText(fullPath);
        m_sourcePath->setToolTip(fullPath);
    }
    m_status->setText(tr("Ready to analyze %1.").arg(QFileInfo(fullPath).fileName()));
    refreshControls();
    return true;
}

bool OfflineMagFitWindow::analysisBusy() const noexcept
{
    return static_cast<bool>(m_analysisState);
}

bool OfflineMagFitWindow::busy() const noexcept
{
    return analysisBusy() || m_sourceDialog || m_applyPrompt
        || static_cast<bool>(m_applyContext)
        || m_ownedApplyOperationId != 0
        || (m_applyService && m_applyService->busy());
}

QString OfflineMagFitWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void OfflineMagFitWindow::browse()
{
    if (busy() || m_closing || m_sourceDialog)
        return;
    auto *dialog = new QFileDialog(
        this, tr("Select log for offline magnetometer calibration"));
    // Match the other Developer tools: one themed, inspectable Qt dialog on
    // every platform, with the same asynchronous lifetime contract.
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setObjectName(QStringLiteral("OfflineMagFitSourceDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setNameFilters({tr("Flight logs (*.tlog *.bin *.log)"),
                            tr("All files (*)")});
    m_sourceDialog = dialog;
    const QPointer<OfflineMagFitWindow> guard(this);
    connect(dialog, &QDialog::finished, this, [this, guard, dialog](int result) {
        if (!guard || m_sourceDialog != dialog)
            return;
        m_sourceDialog.clear();
        if (result == QDialog::Accepted && !dialog->selectedFiles().isEmpty())
            setSource(dialog->selectedFiles().constFirst());
        else
            refreshControls();
    });
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void OfflineMagFitWindow::invalidateAnalysis(const QString &reason)
{
    if (analysisBusy())
        return;
    ++m_analysisRevision;
    if (m_report.success || !m_report.results.isEmpty()) {
        m_report = {};
        const QSignalBlocker resultSignals(m_results);
        m_results->setRowCount(0);
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(0);
        m_status->setText(reason);
    }
    refreshControls();
}

void OfflineMagFitWindow::analyze()
{
    if (busy() || m_closing)
        return;
    const QString path = m_source;
    if (path.isEmpty()) {
        m_status->setText(tr("Choose a flight log first."));
        return;
    }

    const quint64 revision = ++m_analysisRevision;
    const int throttle = qBound(0, qRound(m_throttle->value()), 100);
    const bool ellipsoid = m_ellipsoid->isChecked();
    const auto state = std::make_shared<AnalysisState>();
    m_analysisState = state;
    m_report = {};
    {
        const QSignalBlocker resultSignals(m_results);
        m_results->setRowCount(0);
    }
    {
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(0);
    }
    m_status->setText(tr("Reading magnetometer samples…"));

    auto *watcher = new QFutureWatcher<OfflineMagFitReport>(this);
    m_analysisWatcher = watcher;
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<QFutureWatcher<OfflineMagFitReport>> watched(watcher);
    connect(watcher, &QFutureWatcher<OfflineMagFitReport>::finished, this,
            [this, guard, watched, revision, state]() {
        if (!guard || !watched)
            return;
        const OfflineMagFitReport completed = watched->result();
        watched->deleteLater();
        finishAnalysis(revision, state, completed);
    });
    watcher->setFuture(QtConcurrent::run([path, throttle, ellipsoid, state]() {
        OfflineMagFitService::Options options;
        options.throttleThreshold = throttle;
        options.useEllipsoid = ellipsoid;
        options.isCancelled = [state]() {
            return state->cancelled.load(std::memory_order_relaxed);
        };
        options.progress = [state](double value) {
            if (!std::isfinite(value))
                return;
            state->progressPermille.store(
                std::clamp(qRound(value * 1000.0), 0, 1000),
                std::memory_order_relaxed);
        };
        return OfflineMagFitService::analyze(path, options);
    }));
    m_progressTimer->start();
    refreshControls();
}

void OfflineMagFitWindow::cancelAnalysis()
{
    const auto state = m_analysisState;
    if (!state)
        return;
    state->cancelled.store(true, std::memory_order_relaxed);
    m_status->setText(tr("Cancellation requested…"));
    refreshControls();
}

void OfflineMagFitWindow::finishAnalysis(
    quint64 revision, const std::shared_ptr<AnalysisState> &state,
    const OfflineMagFitReport &report)
{
    if (m_closing || QCoreApplication::closingDown()
        || revision != m_analysisRevision || m_analysisState != state)
        return;
    m_analysisState.reset();
    m_analysisWatcher.clear();
    m_progressTimer->stop();

    if (report.cancelled || state->cancelled.load(std::memory_order_relaxed)) {
        m_report = {};
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(0);
        m_status->setText(tr("Offline MagFit cancelled."));
        appendHistory(m_status->text());
        refreshControls();
        return;
    }
    if (!report.success) {
        m_report = {};
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(0);
        m_status->setText(tr("Offline MagFit failed: %1").arg(report.error));
        appendHistory(m_status->text());
        refreshControls();
        return;
    }

    m_report = report;
    populateResults();
    {
        const QSignalBlocker progressSignals(m_progress);
        m_progress->setValue(100);
    }
    const bool weak = std::any_of(
        report.results.cbegin(), report.results.cend(),
        [](const OfflineMagFitResult &result) {
            return result.coverageOctants < 8;
        });
    m_status->setText(
        tr("Fitted %1 compass%2 from %3. Review every value before applying.%4")
            .arg(report.results.size())
            .arg(report.results.size() == 1 ? QString() : tr("es"))
            .arg(QFileInfo(report.sourcePath).fileName())
            .arg(weak ? tr(" Warning: at least one compass does not cover all eight octants.")
                      : QString()));
    if (!report.applyEligible) {
        const QString reason = report.applyUnavailableReason.trimmed().isEmpty()
            ? tr("This log does not prove an uncompensated, stable compass identity.")
            : report.applyUnavailableReason;
        m_status->setText(m_status->text()
                          + tr(" Apply is unavailable: %1").arg(reason));
    }
    appendHistory(m_status->text());
    for (const QString &warning : report.warnings)
        appendHistory(tr("Analysis warning: %1").arg(warning));
    refreshControls();
}

void OfflineMagFitWindow::populateResults()
{
    const QSignalBlocker resultSignals(m_results);
    m_results->setRowCount(m_report.results.size());
    for (int row = 0; row < m_report.results.size(); ++row) {
        const OfflineMagFitResult &result = m_report.results.at(row);
        const QStringList cells = {
            tr("Compass %1").arg(result.compass),
            result.hasEllipsoid ? tr("Ellipsoid") : tr("Sphere"),
            QString::number(result.usedSamples),
            tr("%1/8 octants").arg(result.coverageOctants),
            vectorText(result.loggedOffsets), vectorText(result.offsets),
            vectorText(result.diagonals), vectorText(result.offDiagonals),
            QString::number(result.rmsError, 'f', 2)};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(cells.at(column));
            m_results->setItem(row, column, item);
        }
    }
}

void OfflineMagFitWindow::beginApply()
{
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    if (m_closing || !service || !m_report.success || !m_report.applyEligible
        || m_report.results.isEmpty()
        || busy()) {
        return;
    }

    const quint64 bindingRevision = m_applyBindingRevision;
    const quint64 promptRevision = ++m_applyPromptRevision;
    const quint64 analysisRevision = m_analysisRevision;
    QString error;
    OfflineMagFitApplyService::Plan plan;
    const bool prepared = service->prepare(m_report, &plan, &error);
    if (!guard || !service || m_closing || m_applyService != service
        || bindingRevision != m_applyBindingRevision
        || promptRevision != m_applyPromptRevision
        || analysisRevision != m_analysisRevision) {
        return;
    }
    if (!prepared) {
        m_status->setText(tr("Cannot prepare calibration apply: %1").arg(error));
        appendHistory(m_status->text());
        refreshControls();
        return;
    }

    auto context = std::make_shared<ApplyContext>();
    context->plan = plan;
    context->analysisRevision = analysisRevision;
    context->bindingRevision = bindingRevision;
    m_applyContext = context;
    confirmApply();
}

void OfflineMagFitWindow::confirmApply()
{
    const auto context = m_applyContext;
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    if (!context || !service || m_closing || m_applyPrompt
        || context->analysisRevision != m_analysisRevision
        || context->bindingRevision != m_applyBindingRevision) {
        return;
    }
    const VehicleEndpoint endpoint = context->plan.target().endpoint;
    QStringList summary;
    bool weakCoverage = false;
    for (const OfflineMagFitResult &result : context->plan.results()) {
        weakCoverage = weakCoverage || result.coverageOctants < 8;
        summary.append(tr("Compass %1: offsets %2; RMS %3; coverage %4/8")
                           .arg(result.compass)
                           .arg(vectorText(result.offsets))
                           .arg(result.rmsError, 0, 'f', 2)
                           .arg(result.coverageOctants));
    }

    QString warning = tr("Apply offline magnetometer calibration?\n\n");
    warning += tr("Target: link %1, system %2, component %3")
                   .arg(endpoint.linkId)
                   .arg(endpoint.systemId)
                   .arg(endpoint.componentId);
    if (!endpoint.linkName.isEmpty())
        warning += QStringLiteral(" (") + endpoint.linkName + QLatin1Char(')');
    warning += QLatin1Char('\n');
    warning += tr("Source: ");
    warning += context->plan.sourcePath();
    warning += QStringLiteral("\n\n") + summary.join(QLatin1Char('\n'));
    warning += tr("\n\nA poor or incomplete log can make heading unreliable. Apply only to "
                  "the same disarmed vehicle that produced this log. Only the listed "
                  "compasses are changed. COMPASS_LEARN may be disabled. Writes are "
                  "sequential; a partial write cannot be rolled back automatically.");
    warning += tr("\n\nThis analysis was marked apply-eligible only because the DataFlash "
                  "log contained a stable device identity and a supported uncompensated "
                  "MAG context. The live service must match every logged device ID again; "
                  "that metadata is not proof that the file came from the selected vehicle.");
    if (weakCoverage) {
        warning += tr("\n\nWARNING: at least one result has incomplete heading coverage "
                      "(fewer than 8 octants).");
    }

    auto *dialog = new QMessageBox(
        QMessageBox::Critical, tr("Apply Offline MagFit"), warning,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("OfflineMagFitApplyConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (QAbstractButton *yes = dialog->button(QMessageBox::Yes))
        yes->setText(tr("Apply calibration"));
    m_applyPrompt = dialog;
    const quint64 promptRevision = m_applyPromptRevision;
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<QMessageBox> prompt(dialog);
    connect(dialog, &QDialog::finished, this,
            [this, guard, service, context, prompt, promptRevision](int result) {
        if (!guard || !prompt || m_applyPrompt != prompt
            || promptRevision != m_applyPromptRevision
            || m_applyService != service || m_applyContext != context) {
            return;
        }
        m_applyPrompt.clear();
        if (result == QMessageBox::Yes)
            startApply();
        else {
            m_applyContext.reset();
            m_status->setText(tr("Calibration values were not applied."));
            refreshControls();
        }
    });
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void OfflineMagFitWindow::startApply()
{
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    const auto context = m_applyContext;
    if (m_closing || !service || !context
        || context->analysisRevision != m_analysisRevision
        || context->bindingRevision != m_applyBindingRevision) {
        m_applyContext.reset();
        refreshControls();
        return;
    }

    QString error;
    const bool valid = service->validate(context->plan, &error);
    if (!guard || !service || m_closing || m_applyService != service
        || m_applyContext != context
        || context->analysisRevision != m_analysisRevision
        || context->bindingRevision != m_applyBindingRevision) {
        return;
    }
    if (!valid) {
        m_applyContext.reset();
        m_status->setText(tr("Calibration apply cancelled: %1").arg(error));
        appendHistory(m_status->text());
        refreshControls();
        return;
    }

    m_ownedApplyOperationId = 0;
    const auto result = service->execute(
        context->plan, &m_ownedApplyOperationId, &error);
    if (!guard || !service || m_applyService != service
        || context->bindingRevision != m_applyBindingRevision) {
        return;
    }
    // execute() publishes the operation ID before any injected callback.  It
    // may also finish synchronously during final validation; in that case the
    // terminal signal has already cleared this exact context and owns the
    // truthful status/report.
    if (m_applyContext != context) {
        refreshControls();
        return;
    }
    if (result != OfflineMagFitApplyService::SubmitResult::Started
        || m_ownedApplyOperationId == 0) {
        m_ownedApplyOperationId = 0;
        m_applyContext.reset();
        m_status->setText(tr("Calibration apply was not started: %1").arg(error));
        appendHistory(m_status->text());
        refreshControls();
        return;
    }
    if (m_ownedApplyOperationId != 0
        && service->currentOperationId() == m_ownedApplyOperationId) {
        showApplyProgress(m_ownedApplyOperationId);
    }
    if (!guard)
        return;
    refreshApplyState();
}

void OfflineMagFitWindow::dismissApplyPrompt()
{
    ++m_applyPromptRevision;
    m_applyContext.reset();
    const QPointer<QDialog> prompt(m_applyPrompt);
    m_applyPrompt.clear();
    if (prompt) {
        const QSignalBlocker blocker(prompt);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
}

void OfflineMagFitWindow::showApplyProgress(quint64 operationId)
{
    if (m_applyProgress || operationId == 0)
        return;
    auto *progress = new QProgressDialog(
        tr("Writing calibration parameters…"), tr("Cancel"), 0, 1, this);
    progress->setObjectName(QStringLiteral("OfflineMagFitApplyProgressDialog"));
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowModality(Qt::NonModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    m_applyProgress = progress;
    connect(progress, &QProgressDialog::canceled, this,
            [this, operationId]() {
        if (m_ownedApplyOperationId == operationId)
            cancelOwnedApply();
    });
    progress->show();
}

void OfflineMagFitWindow::handleApplyFinished(
    quint64 bindingRevision, const OfflineMagFitApplyService::Report &report)
{
    if (bindingRevision != m_applyBindingRevision || m_closing
        || QCoreApplication::closingDown()) {
        return;
    }
    if (report.operationId != m_ownedApplyOperationId) {
        refreshApplyState();
        return;
    }

    m_ownedApplyOperationId = 0;
    m_applyContext.reset();
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<QProgressDialog> progress(m_applyProgress);
    m_applyProgress.clear();
    if (progress) {
        const QSignalBlocker blocker(progress);
        progress->setValue(progress->maximum());
        progress->close();
        progress->deleteLater();
    }
    if (!guard || bindingRevision != m_applyBindingRevision)
        return;

    const QString summary = tr("Offline MagFit apply %1: %2/%3 writes confirmed, "
                               "%4 remaining. %5")
        .arg(outcomeText(report.outcome))
        .arg(report.confirmedWrites)
        .arg(report.totalWrites)
        .arg(report.remainingWrites)
        .arg(report.description);
    m_status->setText(summary);
    appendHistory(summary);
    refreshApplyState();
}

void OfflineMagFitWindow::refreshApplyState()
{
    if (m_refreshing || m_closing || QCoreApplication::closingDown())
        return;
    m_refreshing = true;
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    if (service) {
        const QStringList history = service->history();
        int overlap = qMin(m_seenApplyHistory.size(), history.size());
        while (overlap > 0
               && m_seenApplyHistory.mid(m_seenApplyHistory.size() - overlap)
                    != history.mid(0, overlap)) {
            --overlap;
        }
        for (int i = overlap; i < history.size(); ++i) {
            appendHistory(history.at(i));
            if (!guard)
                return;
            if (m_applyService != service) {
                m_refreshing = false;
                return;
            }
        }
        m_seenApplyHistory = history;
        if (m_ownedApplyOperationId != 0 && !service->busy()
            && service->currentOperationId() != m_ownedApplyOperationId) {
            const OfflineMagFitApplyService::Report terminal =
                service->lastReport();
            if (terminal.operationId == m_ownedApplyOperationId) {
                m_refreshing = false;
                handleApplyFinished(m_applyBindingRevision, terminal);
                return;
            }
        }
        if (m_ownedApplyOperationId != 0 && service->busy()) {
            if (!m_applyProgress)
                showApplyProgress(m_ownedApplyOperationId);
            if (!guard)
                return;
            if (!service || m_applyService != service) {
                m_refreshing = false;
                return;
            }
            if (m_applyProgress) {
                const QSignalBlocker blocker(m_applyProgress);
                const int total = qMax(1, service->progressTotal());
                m_applyProgress->setRange(0, total);
                m_applyProgress->setValue(
                    qBound(0, service->progressCompleted(), total));
                m_applyProgress->setLabelText(service->status());
            }
            m_status->setText(service->status());
        }
    }
    m_refreshing = false;
    refreshControls();
}

void OfflineMagFitWindow::refreshControls()
{
    if (m_closing || QCoreApplication::closingDown())
        return;
    const bool analysis = analysisBusy();
    const bool applyServiceBusy = m_applyService && m_applyService->busy();
    const bool pending = m_sourceDialog || m_applyPrompt || m_applyContext;
    const bool gate = analysis || applyServiceBusy || pending;
    m_browseButton->setEnabled(!gate);
    m_analyzeButton->setEnabled(!gate && !m_source.isEmpty());
    m_cancelAnalysisButton->setEnabled(analysis);
    m_throttle->setEnabled(!gate);
    m_ellipsoid->setEnabled(!gate);
    m_applyButton->setEnabled(!gate && m_applyService
                              && m_report.success
                              && m_report.applyEligible
                              && !m_report.results.isEmpty());
}

void OfflineMagFitWindow::appendHistory(const QString &line)
{
    if (line.trimmed().isEmpty() || m_closing || QCoreApplication::closingDown())
        return;
    const QSignalBlocker historySignals(m_history);
    m_history->appendPlainText(line);
}

void OfflineMagFitWindow::cancelOwnedApply()
{
    const QPointer<OfflineMagFitWindow> guard(this);
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    const quint64 operationId = m_ownedApplyOperationId;
    if (!service || operationId == 0
        || service->currentOperationId() != operationId) {
        if (!m_closing)
            m_status->setText(tr("No Offline MagFit apply started by this window is active."));
        refreshControls();
        return;
    }
    const bool requested = service->cancel(operationId);
    if (!guard || !service || m_applyService != service)
        return;
    if (requested && m_ownedApplyOperationId == operationId)
        m_status->setText(tr("Calibration apply cancellation requested…"));
    refreshApplyState();
}

void OfflineMagFitWindow::closeEvent(QCloseEvent *event)
{
    const QPointer<OfflineMagFitWindow> guard(this);
    m_closing = true;
    ++m_analysisRevision;
    const std::shared_ptr<AnalysisState> analysis = m_analysisState;
    if (analysis)
        analysis->cancelled.store(true, std::memory_order_relaxed);
    m_analysisState.reset();
    m_analysisWatcher.clear();
    m_progressTimer->stop();
    if (m_sourceDialog) {
        const QPointer<QFileDialog> dialog(m_sourceDialog);
        m_sourceDialog.clear();
        if (dialog) {
            const QSignalBlocker blocker(dialog);
            dialog->reject();
        }
    }
    if (!guard)
        return;
    dismissApplyPrompt();
    if (!guard)
        return;
    const QPointer<OfflineMagFitApplyService> service(m_applyService);
    const quint64 operationId = m_ownedApplyOperationId;
    if (service && operationId != 0
        && service->currentOperationId() == operationId) {
        service->cancel(operationId);
    }
    if (!guard)
        return;
    QWidget::closeEvent(event);
}

void OfflineMagFitWindow::showEvent(QShowEvent *event)
{
    m_closing = false;
    QWidget::showEvent(event);
    refreshApplyState();
}
