#include "GeoRefWindow.h"

#include <QtConcurrentRun>

#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace {

QString modeName(GeoRefService::Mode mode)
{
    switch (mode) {
    case GeoRefService::Mode::Cam:
        return QObject::tr("CAM message sync");
    case GeoRefService::Mode::Trig:
        return QObject::tr("TRIG message sync");
    case GeoRefService::Mode::TimeOffset:
        return QObject::tr("Time offset");
    }
    return QObject::tr("Unknown");
}

QString formatNumber(double value, int precision = 8)
{
    return QString::number(value, 'f', precision);
}

} // namespace

struct GeoRefWindow::JobState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> completed{0};
    std::atomic<qint64> total{0};
};

struct GeoRefWindow::WorkerResult
{
    GeoRefService::PlanResult plan;
    GeoRefService::EstimateResult estimate;
    GeoRefService::Result execute;
};

GeoRefWindow::Operations GeoRefWindow::defaultOperations()
{
    Operations operations;
    operations.prepare = [](const GeoRefService::Options &options,
                            const GeoRefService::Cancel &cancel,
                            const GeoRefService::Progress &progress) {
        return GeoRefService::Prepare(options, cancel, progress);
    };
    operations.estimate = [](const GeoRefService::Options &options,
                             const GeoRefService::Cancel &cancel,
                             const GeoRefService::Progress &progress) {
        return GeoRefService::Estimate(options, cancel, progress);
    };
    operations.execute = [](const GeoRefService::Plan &plan,
                            const GeoRefService::Cancel &cancel,
                            const GeoRefService::Progress &progress) {
        return GeoRefService::Execute(plan, cancel, progress);
    };
    return operations;
}

GeoRefWindow::GeoRefWindow(QWidget *parent)
    : QWidget(parent)
    , m_operations(defaultOperations())
    , m_progressTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("GeoRefWindow"));
    setWindowTitle(tr("Geo Ref Images"));
    setWindowFlags(Qt::Window);
    resize(920, 660);
    setMinimumSize(760, 540);
    buildUi();

    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &GeoRefWindow::updateProgress);
    refreshControls();
}

GeoRefWindow::~GeoRefWindow()
{
    m_destroying = true;
    ++m_flow;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_release);
    if (m_progressTimer)
        m_progressTimer->stop();

    const auto detach = [](QPointer<QDialog> &stored) {
        const QPointer<QDialog> dialog(stored);
        stored.clear();
        if (dialog)
            QObject::disconnect(dialog.data(), nullptr, nullptr, nullptr);
    };
    QPointer<QDialog> file(m_filePrompt.data());
    detach(file);
    m_filePrompt.clear();
    detach(m_consent);
    detach(m_progressDialog);
    m_watcher.clear();
}

void GeoRefWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(7);

    auto addPathRow = [this, root](const QString &labelText,
                                   const QString &editName,
                                   const QString &buttonName,
                                   const QString &buttonText,
                                   QLineEdit **edit,
                                   QPushButton **button) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(labelText, this);
        label->setMinimumWidth(118);
        *edit = new QLineEdit(this);
        (*edit)->setObjectName(editName);
        *button = new QPushButton(buttonText, this);
        (*button)->setObjectName(buttonName);
        row->addWidget(label);
        row->addWidget(*edit, 1);
        row->addWidget(*button);
        root->addLayout(row);
    };

    addPathRow(tr("Log file:"), QStringLiteral("GeoRefLogPath"),
               QStringLiteral("GeoRefBrowseLogButton"), tr("Browse…"),
               &m_logPath, &m_browseLog);
    addPathRow(tr("Photo folder:"), QStringLiteral("GeoRefPhotoDirectory"),
               QStringLiteral("GeoRefBrowsePhotoButton"), tr("Browse…"),
               &m_photoDirectory, &m_browsePhoto);
    addPathRow(tr("Output folder:"), QStringLiteral("GeoRefOutputDirectory"),
               QStringLiteral("GeoRefBrowseOutputButton"), tr("Browse…"),
               &m_outputDirectory, &m_browseOutput);
    m_outputDirectory->setPlaceholderText(
        tr("Optional — defaults to <photo folder>/geotagged"));

    auto *matching = new QGroupBox(tr("Matching and altitude"), this);
    auto *matchingLayout = new QGridLayout(matching);
    m_camMode = new QRadioButton(tr("CAM message sync"), matching);
    m_camMode->setObjectName(QStringLiteral("GeoRefCamMode"));
    m_camMode->setChecked(true);
    m_trigMode = new QRadioButton(tr("TRIG message sync"), matching);
    m_trigMode->setObjectName(QStringLiteral("GeoRefTrigMode"));
    m_timeOffsetMode = new QRadioButton(tr("Time offset"), matching);
    m_timeOffsetMode->setObjectName(QStringLiteral("GeoRefTimeOffsetMode"));
    m_timeOffset = new QDoubleSpinBox(matching);
    m_timeOffset->setObjectName(QStringLiteral("GeoRefTimeOffsetSeconds"));
    m_timeOffset->setRange(-1000000000.0, 1000000000.0);
    m_timeOffset->setDecimals(6);
    m_timeOffset->setSuffix(tr(" s"));
    m_useGps2 = new QCheckBox(tr("Use GPS2"), matching);
    m_useGps2->setObjectName(QStringLiteral("GeoRefUseGps2"));

    m_shutterLag = new QSpinBox(matching);
    m_shutterLag->setObjectName(QStringLiteral("GeoRefShutterLag"));
    m_shutterLag->setRange(-60000, 60000);
    m_shutterLag->setSingleStep(10);
    m_shutterLag->setSuffix(tr(" ms"));
    m_useAmslAltitude = new QCheckBox(tr("Use AMSL altitude"), matching);
    m_useAmslAltitude->setObjectName(QStringLiteral("GeoRefUseAmslAltitude"));
    m_useGpsAltitude = new QCheckBox(tr("Prefer GPS altitude"), matching);
    m_useGpsAltitude->setObjectName(QStringLiteral("GeoRefUseGpsAltitude"));
    m_baseAltitude = new QDoubleSpinBox(matching);
    m_baseAltitude->setObjectName(
        QStringLiteral("GeoRefBaseAltitudeAdjustment"));
    m_baseAltitude->setRange(-10000.0, 10000.0);
    m_baseAltitude->setDecimals(3);
    m_baseAltitude->setSuffix(tr(" m"));

    matchingLayout->addWidget(m_camMode, 0, 0);
    matchingLayout->addWidget(m_trigMode, 0, 1);
    matchingLayout->addWidget(m_timeOffsetMode, 0, 2);
    matchingLayout->addWidget(new QLabel(tr("Offset:"), matching), 0, 3);
    matchingLayout->addWidget(m_timeOffset, 0, 4);
    matchingLayout->addWidget(m_useGps2, 0, 5);
    matchingLayout->addWidget(new QLabel(tr("Shutter lag:"), matching), 1, 0);
    matchingLayout->addWidget(m_shutterLag, 1, 1);
    matchingLayout->addWidget(m_useAmslAltitude, 1, 2);
    matchingLayout->addWidget(m_useGpsAltitude, 1, 3);
    matchingLayout->addWidget(new QLabel(tr("Base altitude adjustment:"), matching),
                              1, 4);
    matchingLayout->addWidget(m_baseAltitude, 1, 5);
    matchingLayout->setColumnStretch(6, 1);
    root->addWidget(matching);

    auto *actions = new QHBoxLayout;
    m_geoTag = new QPushButton(tr("Geo Tag"), this);
    m_geoTag->setObjectName(QStringLiteral("GeoRefGeoTagButton"));
    m_estimate = new QPushButton(tr("Estimate Offset"), this);
    m_estimate->setObjectName(QStringLiteral("GeoRefEstimateOffsetButton"));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("GeoRefCancelButton"));
    m_status = new QLabel(
        tr("Pick a log and a photo folder, then Geo Tag."), this);
    m_status->setObjectName(QStringLiteral("GeoRefStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    actions->addWidget(m_geoTag);
    actions->addWidget(m_estimate);
    actions->addWidget(m_cancel);
    actions->addWidget(m_status, 1);
    root->addLayout(actions);

    m_results = new QTableWidget(this);
    m_results->setObjectName(QStringLiteral("GeoRefResults"));
    m_results->setColumnCount(5);
    m_results->setHorizontalHeaderLabels(
        {tr("Photo"), tr("Lat"), tr("Lng"), tr("Alt"), tr("Matched time")});
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_results->verticalHeader()->setVisible(false);
    m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    root->addWidget(m_results, 1);

    m_outputLog = new QPlainTextEdit(this);
    m_outputLog->setObjectName(QStringLiteral("GeoRefOutputLog"));
    m_outputLog->setReadOnly(true);
    m_outputLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_outputLog->setMaximumHeight(130);
    m_outputLog->document()->setMaximumBlockCount(16000);
    root->addWidget(m_outputLog);

    connect(m_browseLog, &QPushButton::clicked, this, &GeoRefWindow::browseLog);
    connect(m_browsePhoto, &QPushButton::clicked,
            this, &GeoRefWindow::browsePhotoDirectory);
    connect(m_browseOutput, &QPushButton::clicked,
            this, &GeoRefWindow::browseOutputDirectory);
    connect(m_geoTag, &QPushButton::clicked, this, &GeoRefWindow::beginPrepare);
    connect(m_estimate, &QPushButton::clicked,
            this, &GeoRefWindow::beginEstimate);
    connect(m_cancel, &QPushButton::clicked,
            this, &GeoRefWindow::cancelCurrent);

    const auto changed = [this]() {
        const QPointer<GeoRefWindow> guard(this);
        invalidateResults();
        if (guard)
            refreshControls();
    };
    connect(m_logPath, &QLineEdit::textChanged, this, changed);
    connect(m_photoDirectory, &QLineEdit::textChanged, this, changed);
    connect(m_outputDirectory, &QLineEdit::textChanged, this, changed);
    connect(m_camMode, &QRadioButton::toggled, this, changed);
    connect(m_trigMode, &QRadioButton::toggled, this, changed);
    connect(m_timeOffsetMode, &QRadioButton::toggled, this, changed);
    connect(m_timeOffset,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this, changed);
    connect(m_useGps2, &QCheckBox::toggled, this, changed);
    connect(m_shutterLag, qOverload<int>(&QSpinBox::valueChanged), this, changed);
    connect(m_useAmslAltitude, &QCheckBox::toggled, this, changed);
    connect(m_useGpsAltitude, &QCheckBox::toggled, this, changed);
    connect(m_baseAltitude,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this, changed);
}

bool GeoRefWindow::isBusy() const noexcept
{
    return m_phase != Phase::Idle;
}

bool GeoRefWindow::isClosing() const noexcept
{
    return m_closing || m_shutdownPending;
}

QString GeoRefWindow::sourcePath() const
{
    return m_logPath ? m_logPath->text().trimmed() : QString();
}

QString GeoRefWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void GeoRefWindow::setSource(const QString &path)
{
    if (isBusy() || isClosing() || !m_logPath)
        return;
    const QString selected = path.trimmed().isEmpty()
        ? QString() : QFileInfo(path).absoluteFilePath();
    const QPointer<GeoRefWindow> guard(this);
    m_logPath->setText(selected);
    if (!guard)
        return;
    if (!selected.isEmpty() && m_photoDirectory->text().trimmed().isEmpty()) {
        m_photoDirectory->setText(QFileInfo(selected).absolutePath());
        if (!guard)
            return;
    }
    refreshControls();
}

void GeoRefWindow::setOperationsForTesting(const Operations &operations)
{
    if (isBusy() || isClosing())
        return;
    m_operations = operations;
}

bool GeoRefWindow::beginPhase(Phase phase, quint64 *flowOut)
{
    if (isBusy() || isClosing() || m_destroying)
        return false;
    const quint64 flow = ++m_flow;
    m_phase = phase;
    m_cancelReported = false;
    if (flowOut)
        *flowOut = flow;
    const QPointer<GeoRefWindow> guard(this);
    refreshControls();
    if (!guard || m_flow != flow || m_phase != phase || isClosing())
        return false;
    emit busyChanged(true);
    return guard && m_flow == flow && m_phase == phase && !isClosing();
}

void GeoRefWindow::browseLog()
{
    quint64 flow = 0;
    if (beginPhase(Phase::LogPrompt, &flow))
        showFilePrompt(Phase::LogPrompt);
}

void GeoRefWindow::browsePhotoDirectory()
{
    quint64 flow = 0;
    if (beginPhase(Phase::PhotoPrompt, &flow))
        showFilePrompt(Phase::PhotoPrompt);
}

void GeoRefWindow::browseOutputDirectory()
{
    quint64 flow = 0;
    if (beginPhase(Phase::OutputPrompt, &flow))
        showFilePrompt(Phase::OutputPrompt);
}

void GeoRefWindow::showFilePrompt(Phase phase)
{
    if (phase != m_phase || isClosing() || m_destroying)
        return;
    const quint64 flow = m_flow;
    auto *dialog = new QFileDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    if (phase == Phase::LogPrompt) {
        dialog->setObjectName(QStringLiteral("GeoRefLogDialog"));
        dialog->setWindowTitle(tr("Select flight log"));
        dialog->setFileMode(QFileDialog::ExistingFile);
        dialog->setNameFilters({tr("DataFlash logs (*.bin *.BIN *.log *.LOG)"),
                                tr("All files (*)")});
        const QString current = m_logPath->text().trimmed();
        if (!current.isEmpty())
            dialog->selectFile(current);
    } else {
        dialog->setObjectName(phase == Phase::PhotoPrompt
                                  ? QStringLiteral("GeoRefPhotoDirectoryDialog")
                                  : QStringLiteral("GeoRefOutputDirectoryDialog"));
        dialog->setWindowTitle(phase == Phase::PhotoPrompt
                                   ? tr("Select photo folder")
                                   : tr("Select GeoRef output folder"));
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly, true);
        const QString current = phase == Phase::PhotoPrompt
            ? m_photoDirectory->text().trimmed()
            : m_outputDirectory->text().trimmed();
        if (!current.isEmpty())
            dialog->setDirectory(current);
    }
    m_filePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow, phase](int result) {
        if (m_destroying || flow != m_flow || phase != m_phase
            || m_filePrompt != dialog)
            return;
        m_filePrompt.clear();
        const QStringList selected = dialog->selectedFiles();
        if (result != QDialog::Accepted || selected.size() != 1) {
            finishFlow(flow, tr("Selection cancelled."));
            return;
        }
        acceptFilePrompt(flow, phase, selected.first());
    });
    dialog->open();
}

void GeoRefWindow::acceptFilePrompt(quint64 flow, Phase phase,
                                    const QString &path)
{
    if (m_destroying || isClosing() || flow != m_flow || phase != m_phase)
        return;
    const QString selected = phase == Phase::LogPrompt
        ? QFileInfo(path).absoluteFilePath() : QDir(path).absolutePath();
    const QPointer<GeoRefWindow> guard(this);
    if (phase == Phase::LogPrompt) {
        m_logPath->setText(selected);
        if (!guard)
            return;
        if (m_photoDirectory->text().trimmed().isEmpty()) {
            m_photoDirectory->setText(QFileInfo(selected).absolutePath());
            if (!guard)
                return;
        }
    } else if (phase == Phase::PhotoPrompt) {
        m_photoDirectory->setText(selected);
        if (!guard)
            return;
    } else {
        m_outputDirectory->setText(selected);
        if (!guard)
            return;
    }
    finishFlow(flow, tr("Selection updated."));
}

GeoRefService::Options GeoRefWindow::currentOptions() const
{
    GeoRefService::Options options;
    const QString log = m_logPath->text().trimmed();
    const QString photos = m_photoDirectory->text().trimmed();
    const QString output = m_outputDirectory->text().trimmed();
    options.logPath = log.isEmpty() ? QString() : QFileInfo(log).absoluteFilePath();
    options.photoDirectory = photos.isEmpty() ? QString() : QDir(photos).absolutePath();
    options.outputDirectory = output.isEmpty() ? QString() : QDir(output).absolutePath();
    options.mode = m_camMode->isChecked() ? GeoRefService::Mode::Cam
        : (m_trigMode->isChecked() ? GeoRefService::Mode::Trig
                                   : GeoRefService::Mode::TimeOffset);
    options.timeOffsetSeconds = m_timeOffset->value();
    options.useGps2 = m_useGps2->isChecked();
    options.shutterLagMilliseconds = m_shutterLag->value();
    options.useAmslAltitude = m_useAmslAltitude->isChecked();
    options.useGpsAltitude = m_useGpsAltitude->isChecked();
    options.baseAltitudeAdjustmentMeters = m_baseAltitude->value();
    return options;
}

void GeoRefWindow::beginEstimate()
{
    const GeoRefService::Options options = currentOptions();
    const QPointer<GeoRefWindow> guard(this);
    invalidateResults();
    if (!guard)
        return;
    quint64 flow = 0;
    if (!beginPhase(Phase::Working, &flow))
        return;
    if (options.logPath.isEmpty() || options.photoDirectory.isEmpty()) {
        finishFlow(flow, tr("Select a log and photo folder before estimating."));
        return;
    }
    startWorker(flow, Work::Estimate, options);
}

void GeoRefWindow::beginPrepare()
{
    const GeoRefService::Options options = currentOptions();
    quint64 flow = 0;
    if (!beginPhase(Phase::Working, &flow))
        return;
    if (options.logPath.isEmpty() || options.photoDirectory.isEmpty()) {
        finishFlow(flow, tr("Select a log and photo folder before Geo Tag."));
        return;
    }
    startWorker(flow, Work::Prepare, options);
}

void GeoRefWindow::startWorker(
    quint64 flow, Work work, const GeoRefService::Options &options,
    std::shared_ptr<const GeoRefService::Plan> plan)
{
    if (m_destroying || isClosing() || flow != m_flow
        || m_phase != Phase::Working || m_job)
        return;
    auto state = std::make_shared<JobState>();
    m_job = state;
    m_work = work;

    auto *progress = new QDialog(this);
    progress->setObjectName(QStringLiteral("GeoRefProgressDialog"));
    progress->setWindowTitle(tr("Geo Reference Images"));
    progress->setWindowModality(Qt::NonModal);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(progress);
    auto *label = new QLabel(work == Work::Estimate
                                 ? tr("Estimating camera/log time offset…")
                                 : (work == Work::Prepare
                                        ? tr("Preparing exact GeoRef outputs…")
                                        : tr("Writing confirmed GeoRef outputs…")),
                             progress);
    label->setObjectName(QStringLiteral("GeoRefProgressLabel"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    auto *bar = new QProgressBar(progress);
    bar->setObjectName(QStringLiteral("GeoRefProgressBar"));
    bar->setRange(0, 0);
    auto *cancel = new QPushButton(tr("Cancel"), progress);
    cancel->setObjectName(QStringLiteral("GeoRefProgressCancelButton"));
    cancel->setDefault(true);
    layout->addWidget(label);
    layout->addWidget(bar);
    layout->addWidget(cancel, 0, Qt::AlignRight);
    m_progressDialog = progress;
    connect(cancel, &QPushButton::clicked, this, &GeoRefWindow::cancelCurrent);
    connect(progress, &QDialog::rejected, this, &GeoRefWindow::cancelCurrent);
    const QPointer<GeoRefWindow> guard(this);
    progress->show();
    if (!guard)
        return;
    if (flow != m_flow || m_phase != Phase::Working || m_job != state
        || isClosing()) {
        if (flow == m_flow && m_phase == Phase::Working && m_job == state) {
            m_job.reset();
            m_work = Work::None;
            finishFlow(flow, tr("GeoRef work cancelled before it started."));
        }
        return;
    }

    auto *watcher = new QFutureWatcher<WorkerResult>(this);
    m_watcher = watcher;
    const Operations operations = m_operations;
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, flow, work, state]() {
        const WorkerResult result = watcher->result();
        watcher->deleteLater();
        finishWorker(flow, work, state, result);
    });
    const QPointer<QFutureWatcherBase> watcherGuard(watcher);
    watcher->setFuture(QtConcurrent::run(
        [operations, options, plan = std::move(plan), state, work]() {
        WorkerResult result;
        const GeoRefService::Cancel cancelled = [state]() {
            return state->cancelled.load(std::memory_order_acquire);
        };
        const GeoRefService::Progress report = [state](qint64 done, qint64 total) {
            state->completed.store(qMax<qint64>(0, done), std::memory_order_relaxed);
            state->total.store(qMax<qint64>(0, total), std::memory_order_relaxed);
        };
        try {
            if (work == Work::Estimate) {
                if (operations.estimate)
                    result.estimate = operations.estimate(options, cancelled, report);
                else
                    result.estimate.error = QStringLiteral("Estimate operation is unavailable.");
            } else if (work == Work::Prepare) {
                if (operations.prepare)
                    result.plan = operations.prepare(options, cancelled, report);
                else
                    result.plan.error = QStringLiteral("GeoRef preparation is unavailable.");
            } else if (work == Work::Execute) {
                if (operations.execute && plan && plan->isValid())
                    result.execute = operations.execute(*plan, cancelled, report);
                else
                    result.execute.error = QStringLiteral("GeoRef export plan is unavailable.");
            }
        } catch (const std::exception &exception) {
            const QString error = QStringLiteral("GeoRef worker failed: %1")
                                      .arg(QString::fromUtf8(exception.what()));
            if (work == Work::Estimate) result.estimate.error = error;
            else if (work == Work::Prepare) result.plan.error = error;
            else result.execute.error = error;
        } catch (...) {
            const QString error = QStringLiteral("GeoRef worker failed with an unknown error.");
            if (work == Work::Estimate) result.estimate.error = error;
            else if (work == Work::Prepare) result.plan.error = error;
            else result.execute.error = error;
        }
        return result;
    }));
    if (!guard || !watcherGuard || m_watcher != watcher
        || flow != m_flow || m_job != state)
        return;
    m_progressTimer->start();
    updateProgress();
}

void GeoRefWindow::finishWorker(
    quint64 flow, Work work, const std::shared_ptr<JobState> &state,
    const WorkerResult &result)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Working
        || m_work != work || m_job != state)
        return;
    m_progressTimer->stop();
    m_watcher.clear();
    m_job.reset();
    const QPointer<GeoRefWindow> guard(this);
    if (!dismissDialog(m_progressDialog) || !guard || flow != m_flow
        || m_phase != Phase::Working || m_work != work || m_job)
        return;
    m_work = Work::None;

    if (m_shutdownPending || m_closeWhenIdle) {
        finishFlow(flow, tr("GeoRef work stopped while closing."));
        return;
    }

    if (work == Work::Estimate) {
        const auto &estimate = result.estimate;
        QStringList messages = estimate.warnings;
        if (estimate.cancelled || state->cancelled.load(std::memory_order_acquire)) {
            finishFlow(flow, tr("Offset estimation cancelled."), messages);
            return;
        }
        if (!estimate.success) {
            finishFlow(flow,
                       estimate.error.isEmpty() ? tr("Offset estimation failed.")
                                                : estimate.error,
                       messages);
            return;
        }
        if (!estimate.hasEstimate) {
            finishFlow(flow, tr("Valid photo or log timestamps are missing."), messages);
            return;
        }
        {
            const QSignalBlocker blocker(m_timeOffset);
            m_timeOffset->setValue(estimate.offsetSeconds);
        }
        messages.append(tr("Estimated camera minus log offset: %1 s")
                            .arg(QString::number(estimate.offsetSeconds, 'f', 3)));
        finishFlow(flow, tr("Offset estimated. Review it, then Geo Tag."), messages);
        return;
    }

    if (work == Work::Prepare) {
        const auto &prepared = result.plan;
        QStringList warnings = prepared.warnings;
        if (prepared.cancelled || state->cancelled.load(std::memory_order_acquire)) {
            finishFlow(flow, tr("GeoRef preparation cancelled."), warnings);
            return;
        }
        if (!prepared.success || !prepared.plan || !prepared.plan->isValid()) {
            finishFlow(flow,
                       prepared.error.isEmpty() ? tr("GeoRef preparation failed.")
                                                : prepared.error,
                       warnings);
            return;
        }
        m_phase = Phase::Consent;
        m_plan = prepared.plan;
        refreshControls();
        if (!guard || flow != m_flow || m_phase != Phase::Consent
            || m_plan != prepared.plan)
            return;
        showConsent(flow, prepared.plan, warnings);
        return;
    }

    const auto &executed = result.execute;
    QStringList messages = executed.warnings;
    for (const QString &path : executed.publishedPaths)
        messages.append(tr("Published: %1").arg(path));
    for (const QString &path : executed.failedPaths)
        messages.append(tr("Failed: %1").arg(path));
    if (!executed.matches.isEmpty())
        populateResults(executed.matches);
    if (!guard)
        return;
    if (executed.success) {
        finishFlow(flow,
                   tr("Geo tagging done — %1 matched, %2 tagged, %3 failed.")
                       .arg(executed.matches.size())
                       .arg(executed.taggedPhotos)
                       .arg(executed.failedPhotos),
                   messages);
    } else if (executed.cancelled
               || state->cancelled.load(std::memory_order_acquire)) {
        finishFlow(flow,
                   executed.publishedPaths.isEmpty()
                       ? tr("Geo tagging cancelled; no outputs were published.")
                       : tr("Geo tagging cancelled after partial publication. Review the exact paths below."),
                   messages);
    } else {
        finishFlow(flow,
                   executed.error.isEmpty() ? tr("Geo tagging failed.")
                                            : executed.error,
                   messages);
    }
}

void GeoRefWindow::showConsent(
    quint64 flow, const std::shared_ptr<const GeoRefService::Plan> &plan,
    const QStringList &warnings)
{
    if (m_destroying || isClosing() || flow != m_flow
        || m_phase != Phase::Consent || !plan || plan != m_plan
        || !plan->isValid()) {
        finishFlow(flow, tr("The prepared GeoRef plan is no longer available."));
        return;
    }
    const GeoRefService::Options options = plan->options();
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("GeoRefConfirmationDialog"));
    dialog->setWindowTitle(tr("Confirm Geo Reference Images"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(760, 540);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("GeoRefConfirmationSummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    QString text = tr("Source log: ") + options.logPath
        + tr("\nPhoto folder: ") + options.photoDirectory
        + tr("\nOutput folder: ")
        + (options.outputDirectory.isEmpty()
               ? QDir(options.photoDirectory).filePath(QStringLiteral("geotagged"))
               : options.outputDirectory)
        + tr("\nMode: ") + modeName(options.mode)
        + tr("\nMatched photos: ") + QString::number(plan->matches().size())
        + tr("\nOffset: ") + QString::number(options.timeOffsetSeconds, 'g', 12)
        + tr(" s; shutter lag: ") + QString::number(options.shutterLagMilliseconds)
        + tr(" ms; base altitude adjustment: ")
        + QString::number(options.baseAltitudeAdjustmentMeters, 'g', 12) + tr(" m")
        + tr("\nGPS source: ") + (options.useGps2 ? tr("GPS2") : tr("GPS"))
        + tr("; AMSL: ") + (options.useAmslAltitude ? tr("yes") : tr("no"))
        + tr("; prefer GPS altitude: ") + (options.useGpsAltitude ? tr("yes") : tr("no"))
        + tr("\n\nEXIF photo times have no timezone. This plan freezes the host-local interpretation used during preparation. Sources are not modified. Existing output paths are refused, not replaced. Multi-file publication can be partial if storage fails or cancellation arrives after publication starts.");
    summary->setText(text);
    layout->addWidget(summary);

    auto *paths = new QPlainTextEdit(dialog);
    paths->setObjectName(QStringLiteral("GeoRefConfirmationPaths"));
    paths->setReadOnly(true);
    paths->setLineWrapMode(QPlainTextEdit::NoWrap);
    QStringList lines;
    for (const QString &path : plan->outputPaths())
        lines.append(tr("Create: %1").arg(path));
    if (!warnings.isEmpty()) {
        lines.append(QString());
        lines.append(tr("Warnings:"));
        for (const QString &warning : warnings)
            lines.append(QStringLiteral("- ") + warning);
    }
    paths->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(paths, 1);

    auto *buttons = new QDialogButtonBox(dialog);
    auto *confirm = buttons->addButton(tr("Geo Tag"), QDialogButtonBox::AcceptRole);
    confirm->setObjectName(QStringLiteral("GeoRefConfirmButton"));
    confirm->setAutoDefault(false);
    auto *cancel = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    cancel->setObjectName(QStringLiteral("GeoRefConfirmationCancelButton"));
    cancel->setDefault(true);
    cancel->setAutoDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    m_consent = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow, plan](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::Consent
            || m_consent != dialog || m_plan != plan)
            return;
        m_consent.clear();
        if (result != QDialog::Accepted) {
            finishFlow(flow, tr("Geo tagging cancelled before publication."));
            return;
        }
        beginExecute(flow, plan);
    });
    dialog->show();
}

void GeoRefWindow::beginExecute(
    quint64 flow, const std::shared_ptr<const GeoRefService::Plan> &plan)
{
    if (m_destroying || isClosing() || flow != m_flow
        || m_phase != Phase::Consent || !plan || plan != m_plan) {
        finishFlow(flow, tr("The prepared GeoRef plan changed before confirmation."));
        return;
    }
    m_phase = Phase::Working;
    const QPointer<GeoRefWindow> guard(this);
    refreshControls();
    if (!guard || flow != m_flow || m_phase != Phase::Working
        || m_plan != plan)
        return;
    startWorker(flow, Work::Execute, plan->options(), plan);
}

void GeoRefWindow::cancelCurrent()
{
    if (!isBusy())
        return;
    const quint64 flow = m_flow;
    if (m_job) {
        m_job->cancelled.store(true, std::memory_order_release);
        const QPointer<GeoRefWindow> guard(this);
        if (!m_cancelReported) {
            m_cancelReported = true;
            if (!setStatus(tr("Cancelling; waiting for the worker to stop…"))
                || !guard || flow != m_flow || !m_job) {
                return;
            }
        }
        updateProgress();
        if (!guard || flow != m_flow || !m_job)
            return;
        refreshControls();
        return;
    }
    const QPointer<GeoRefWindow> guard(this);
    dismissPrompts();
    if (!guard || flow != m_flow)
        return;
    finishFlow(flow, tr("GeoRef operation cancelled."));
}

void GeoRefWindow::updateProgress()
{
    const auto state = m_job;
    const QPointer<QDialog> progress(m_progressDialog);
    if (!state || !progress)
        return;
    auto *bar = progress->findChild<QProgressBar *>(QStringLiteral("GeoRefProgressBar"));
    auto *label = progress->findChild<QLabel *>(QStringLiteral("GeoRefProgressLabel"));
    auto *cancel = progress->findChild<QPushButton *>(
        QStringLiteral("GeoRefProgressCancelButton"));
    if (!bar || !label || !cancel)
        return;
    const bool cancelling = state->cancelled.load(std::memory_order_acquire);
    const qint64 done = state->completed.load(std::memory_order_relaxed);
    const qint64 total = state->total.load(std::memory_order_relaxed);
    const QSignalBlocker blockBar(bar);
    const QSignalBlocker blockCancel(cancel);
    if (cancelling) {
        label->setText(tr("Cancelling; waiting for the current worker step to stop…"));
        cancel->setEnabled(false);
    } else {
        cancel->setEnabled(true);
    }
    if (total > 0) {
        bar->setRange(0, 1000);
        const long double ratio = qBound<long double>(
            0.0L, static_cast<long double>(done) / total, 1.0L);
        bar->setValue(static_cast<int>(ratio * 1000.0L));
    } else {
        bar->setRange(0, 0);
    }
}

void GeoRefWindow::refreshControls()
{
    if (!m_logPath)
        return;
    const bool idle = !isBusy() && !isClosing();
    const bool inputsReady = !m_logPath->text().trimmed().isEmpty()
        && !m_photoDirectory->text().trimmed().isEmpty();
    m_logPath->setEnabled(idle);
    m_browseLog->setEnabled(idle);
    m_photoDirectory->setEnabled(idle);
    m_browsePhoto->setEnabled(idle);
    m_outputDirectory->setEnabled(idle);
    m_browseOutput->setEnabled(idle);
    m_camMode->setEnabled(idle);
    m_trigMode->setEnabled(idle);
    m_timeOffsetMode->setEnabled(idle);
    m_timeOffset->setEnabled(idle);
    m_useGps2->setEnabled(idle);
    m_shutterLag->setEnabled(idle);
    m_useAmslAltitude->setEnabled(idle);
    m_useGpsAltitude->setEnabled(idle);
    m_baseAltitude->setEnabled(idle);
    m_geoTag->setEnabled(idle && inputsReady);
    m_estimate->setEnabled(idle && inputsReady);
    m_cancel->setEnabled(isBusy() && !m_cancelReported);
}

void GeoRefWindow::invalidateResults()
{
    if (!m_results || isBusy())
        return;
    m_plan.reset();
    const QPointer<QTableWidget> table(m_results);
    const bool blocked = table->blockSignals(true);
    table->setRowCount(0);
    if (table)
        table->blockSignals(blocked);
}

void GeoRefWindow::populateResults(const QVector<GeoRefService::Match> &matches)
{
    const QPointer<GeoRefWindow> guard(this);
    const QPointer<QTableWidget> table(m_results);
    const quint64 presentationFlow = m_flow;
    if (!table)
        return;
    const bool blocked = table->blockSignals(true);
    table->setSortingEnabled(false);
    if (!guard || !table || m_flow != presentationFlow
        || m_phase != Phase::Working) {
        if (table)
            table->blockSignals(blocked);
        return;
    }
    table->setRowCount(matches.size());
    if (!guard || !table || m_flow != presentationFlow
        || m_phase != Phase::Working) {
        if (table)
            table->blockSignals(blocked);
        return;
    }
    for (int row = 0; row < matches.size(); ++row) {
        const auto &match = matches.at(row);
        const QStringList values{
            QFileInfo(match.sourcePath).fileName(),
            formatNumber(match.latitude),
            formatNumber(match.longitude),
            formatNumber(match.altitude, 3),
            match.timeUtc.toUTC().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz 'UTC'"))
        };
        for (int column = 0; column < 5; ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setToolTip(column == 0 ? match.sourcePath : item->text());
            table->setItem(row, column, item);
            if (!guard || !table || m_flow != presentationFlow
                || m_phase != Phase::Working) {
                if (table)
                    table->blockSignals(blocked);
                return;
            }
        }
    }
    table->blockSignals(blocked);
}

bool GeoRefWindow::appendLog(const QString &line)
{
    if (line.trimmed().isEmpty() || !m_outputLog
        || QCoreApplication::closingDown())
        return true;
    const QPointer<GeoRefWindow> guard(this);
    const QSignalBlocker blocker(m_outputLog);
    m_outputLog->appendPlainText(line);
    return guard;
}

bool GeoRefWindow::setStatus(const QString &text)
{
    if (!m_status || QCoreApplication::closingDown())
        return true;
    const QPointer<GeoRefWindow> guard(this);
    m_status->setText(text);
    return guard;
}

bool GeoRefWindow::dismissDialog(QPointer<QDialog> &stored)
{
    const QPointer<GeoRefWindow> guard(this);
    const QPointer<QDialog> dialog(stored);
    stored.clear();
    if (!dialog)
        return true;
    const bool blocked = dialog->blockSignals(true);
    dialog->reject();
    if (!guard)
        return false;
    if (dialog) {
        dialog->blockSignals(blocked);
        dialog->deleteLater();
    }
    return true;
}

void GeoRefWindow::dismissPrompts()
{
    const QPointer<GeoRefWindow> guard(this);
    QPointer<QDialog> file(m_filePrompt.data());
    m_filePrompt.clear();
    if (!dismissDialog(file) || !guard)
        return;
    dismissDialog(m_consent);
}

void GeoRefWindow::finishFlow(quint64 flow, const QString &status,
                              const QStringList &messages)
{
    if (m_destroying || flow != m_flow)
        return;
    const quint64 terminalFlow = ++m_flow;
    m_phase = Phase::Idle;
    m_work = Work::None;
    m_job.reset();
    m_watcher.clear();
    m_plan.reset();
    m_cancelReported = false;
    if (m_progressTimer)
        m_progressTimer->stop();
    const QPointer<GeoRefWindow> guard(this);
    if (!dismissDialog(m_progressDialog) || !guard || m_flow != terminalFlow)
        return;
    dismissPrompts();
    if (!guard || m_flow != terminalFlow)
        return;
    for (const QString &message : messages) {
        if (!appendLog(message) || !guard || m_flow != terminalFlow)
            return;
    }
    if (!setStatus(status) || !guard || m_flow != terminalFlow)
        return;
    refreshControls();
    if (!guard || m_flow != terminalFlow || m_phase != Phase::Idle)
        return;
    emit busyChanged(false);
    if (!guard || m_flow != terminalFlow || m_phase != Phase::Idle)
        return;
    resolveDeferredClose();
}

void GeoRefWindow::resolveDeferredClose()
{
    if (isBusy() || m_destroying)
        return;
    if (m_shutdownPending) {
        if (!m_shutdownReadyEmitted) {
            m_shutdownReadyEmitted = true;
            emit shutdownReady();
        }
        return;
    }
    if (!m_closeWhenIdle)
        return;
    m_allowClose = true;
    QPointer<GeoRefWindow> guard(this);
    QTimer::singleShot(0, this, [guard]() {
        if (guard)
            guard->close();
    });
}

void GeoRefWindow::requestShutdown()
{
    if (m_destroying || m_shutdownPending)
        return;
    m_shutdownPending = true;
    m_closing = true;
    m_closeWhenIdle = false;
    const QPointer<GeoRefWindow> guard(this);
    if (isBusy())
        cancelCurrent();
    if (!guard)
        return;
    resolveDeferredClose();
}

void GeoRefWindow::closeEvent(QCloseEvent *event)
{
    if (m_destroying || m_allowClose
        || (m_shutdownPending && !isBusy())) {
        m_closing = true;
        event->accept();
        return;
    }
    if (!isBusy()) {
        m_closing = true;
        m_allowClose = true;
        event->accept();
        return;
    }
    event->ignore();
    m_closing = true;
    m_closeWhenIdle = true;
    cancelCurrent();
}
