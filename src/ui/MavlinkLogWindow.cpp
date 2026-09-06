#include "MavlinkLogWindow.h"

#include "comm/TlogExportService.h"
#include "configuration.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QThread>
#include <QTimer>

#include <utility>

namespace
{
QPushButton *makeButton(const QString &text, const QString &objectName,
                        QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setMinimumWidth(80);
    return button;
}

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                               : canonical);
}
}

struct MavlinkLogWindow::PendingExport
{
    Operation operation = Operation::Csv;
    QString input;
    QString label;
    quint64 flow = 0;
};

struct MavlinkLogWindow::ProgressState
{
    std::atomic<qint64> completed{0};
    std::atomic<qint64> total{0};
};

MavlinkLogWindow::MavlinkLogWindow(QWidget *owner)
    : MavlinkLogWindow(Dependencies(), owner)
{
}

MavlinkLogWindow::MavlinkLogWindow(Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_dependencies(std::move(dependencies))
{
    buildUi(owner);
    setTlogPath(QString());
}

MavlinkLogWindow::~MavlinkLogWindow()
{
    m_closing = true;
    ++m_flow;
    if (m_progressTimer)
        m_progressTimer->stop();
    stopWorker();
}

MavlinkLogWindow *MavlinkLogWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new MavlinkLogWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void MavlinkLogWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("MavlinkLogWindow"));
    setWindowTitle(tr("Tlog Convert"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFixedSize(WindowWidth, WindowHeight);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QGridLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(8);
    root->setVerticalSpacing(8);

    auto *pickRow = new QHBoxLayout;
    pickRow->setSpacing(8);
    m_pick = makeButton(tr("Pick .tlog…"),
                        QStringLiteral("pickTlogButton"), this);
    m_tlogName = new QLabel(this);
    m_tlogName->setObjectName(QStringLiteral("tlogName"));
    m_tlogName->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_tlogName->setTextFormat(Qt::PlainText);
    pickRow->addWidget(m_pick);
    pickRow->addWidget(m_tlogName, 1);
    root->addLayout(pickRow, 0, 0);

    auto *convertLabel = new QLabel(tr("Convert to:"), this);
    convertLabel->setObjectName(QStringLiteral("convertToLabel"));
    convertLabel->setContentsMargins(0, 6, 0, 0);
    root->addWidget(convertLabel, 1, 0);

    auto *formats = new QGridLayout;
    formats->setContentsMargins(0, 0, 0, 0);
    formats->setHorizontalSpacing(8);
    formats->setVerticalSpacing(8);
    struct FormatButton
    {
        const char *text;
        const char *objectName;
        Operation operation;
        int row;
        int column;
    };
    const FormatButton formatButtons[] = {
        {"KML", "convertKmlButton", Operation::Kml, 0, 0},
        {"GPX", "convertGpxButton", Operation::Gpx, 0, 1},
        {"CSV", "convertCsvButton", Operation::Csv, 0, 3},
        {"Text", "convertTextButton", Operation::Text, 1, 0},
    };
    for (const FormatButton &definition : formatButtons) {
        QPushButton *button = makeButton(
            tr(definition.text), QLatin1String(definition.objectName), this);
        formats->addWidget(button, definition.row, definition.column);
        m_exportButtons.append(button);
        connect(button, &QPushButton::clicked, this,
                [this, definition]() { beginExport(definition.operation); });
    }

    auto *matlab = makeButton(tr("Matlab"),
                              QStringLiteral("convertMatlabButton"), this);
    matlab->setToolTip(tr("Export numeric scalar fields to a new MATLAB Level-5 file. "
                         "Arrays are omitted. Uses Mission Planner's local-time convention "
                         "and the bundled MAVLink message definitions."));
    m_exportButtons.append(matlab);
    connect(matlab, &QPushButton::clicked, this,
            [this]() { beginExport(Operation::Matlab); });
    formats->addWidget(matlab, 0, 2);
    formats->setColumnStretch(4, 1);
    root->addLayout(formats, 2, 0);

    auto *extractRow = new QHBoxLayout;
    extractRow->setSpacing(8);
    auto *parameters = makeButton(
        tr("Extract Parameters"), QStringLiteral("extractParametersButton"),
        this);
    auto *missions = makeButton(
        tr("Extract Missions"), QStringLiteral("extractMissionsButton"),
        this);
    extractRow->addWidget(parameters);
    extractRow->addWidget(missions);
    extractRow->addStretch(1);
    root->addLayout(extractRow, 3, 0);
    m_exportButtons.append(parameters);
    m_exportButtons.append(missions);
    connect(parameters, &QPushButton::clicked, this,
            [this]() { beginExport(Operation::Parameters); });
    connect(missions, &QPushButton::clicked, this,
            [this]() { beginExport(Operation::Missions); });

    root->setRowStretch(4, 1);
    auto *progressRow = new QHBoxLayout;
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("MavlinkLogExportProgressBar"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->hide();
    progressRow->addWidget(m_progress, 1);
    m_cancel = makeButton(tr("Cancel export"),
                          QStringLiteral("MavlinkLogExportCancelButton"), this);
    m_cancel->hide();
    progressRow->addWidget(m_cancel);
    root->addLayout(progressRow, 5, 0);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tlogConvertStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: #99AADD;"));
    root->addWidget(m_status, 6, 0);

    connect(m_pick, &QPushButton::clicked, this, &MavlinkLogWindow::pickTlog);
    connect(m_cancel, &QPushButton::clicked,
            this, &MavlinkLogWindow::cancelCurrent);
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &MavlinkLogWindow::updateProgress);
}

void MavlinkLogWindow::setTlogPath(const QString &path)
{
    if (m_busy || m_closing) {
        return;
    }

    const QFileInfo file(path);
    const bool valid = file.exists() && file.isFile()
        && file.suffix().compare(QStringLiteral("tlog"),
                                 Qt::CaseInsensitive) == 0;
    m_tlogPath = valid ? file.absoluteFilePath() : QString();
    if (valid) {
        m_tlogName->setText(file.fileName());
        m_tlogName->setToolTip(m_tlogPath);
        m_status->setText(tr("Ready. Choose a conversion."));
    } else {
        m_tlogName->setText(tr("(no file selected)"));
        m_tlogName->setToolTip(QString());
        m_status->setText(path.isEmpty()
                              ? tr("Pick a .tlog to convert.")
                              : tr("Select an existing .tlog file."));
    }
    setBusy(false);
}

QString MavlinkLogWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void MavlinkLogWindow::pickTlog()
{
    if (m_busy || m_closing) {
        return;
    }
    const quint64 flow = ++m_flow;
    setBusy(true);
    auto *dialog = new QFileDialog(
        this, tr("Select telemetry log"), QGC::MAVLinkLogDirectory(),
        tr("Telemetry log (*.tlog)"));
    dialog->setObjectName(QStringLiteral("MavlinkLogInputDialog"));
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    m_inputDialog = dialog;
    connect(dialog, &QFileDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_inputDialog != dialog || flow != m_flow)
            return;
        const QString selected = dialog->selectedFiles().value(0);
        m_inputDialog = nullptr;
        setBusy(false);
        if (result == QDialog::Accepted && !selected.isEmpty())
            setTlogPath(selected);
        else
            m_status->setText(tr("Telemetry-log selection cancelled."));
    });
    dialog->open();
}

void MavlinkLogWindow::beginExport(Operation operation)
{
    if (m_busy || m_closing || m_tlogPath.isEmpty()) {
        return;
    }
    auto pending = std::make_shared<PendingExport>();
    pending->operation = operation;
    pending->input = m_tlogPath;
    pending->label = operationLabel(operation);
    pending->flow = ++m_flow;
    m_pending = pending;
    setBusy(true);
    const auto confirm = m_dependencies.confirmExport;
    if (confirm) {
        QPointer<MavlinkLogWindow> guard(this);
        const bool accepted = confirm(pending->label);
        if (!guard || m_pending != pending || pending->flow != m_flow
            || m_closing)
            return;
        if (!accepted) {
            finishPending(tr("%1 export cancelled.").arg(pending->label));
            return;
        }
        continueAfterConfirmation(pending);
        return;
    }

    auto *box = new QMessageBox(
        QMessageBox::Warning, tr("Export %1").arg(pending->label),
        tr("Exported vehicle data can contain precise GPS coordinates, vehicle "
           "identifiers, missions, network details and sensitive parameter "
           "values. Save the file only to a trusted location and review it "
           "before sharing. Cancel is the default action."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("MavlinkLogSensitiveExportConfirmation"));
    box->setTextFormat(Qt::PlainText);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *accept = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        accept->setObjectName(QStringLiteral("MavlinkLogExportConfirmButton"));
        accept->setText(tr("EXPORT FILE"));
        accept->setAutoDefault(false);
    }
    m_confirmationDialog = box;
    connect(box, &QMessageBox::finished, this,
            [this, box, pending](int result) {
        if (m_confirmationDialog != box || m_pending != pending
            || pending->flow != m_flow)
            return;
        m_confirmationDialog = nullptr;
        if (result != QMessageBox::Yes) {
            finishPending(tr("%1 export cancelled.").arg(pending->label));
            return;
        }
        continueAfterConfirmation(pending);
    });
    box->open();
}

void MavlinkLogWindow::continueAfterConfirmation(
    const std::shared_ptr<PendingExport> &pending)
{
    if (!pending || pending != m_pending || pending->flow != m_flow
        || m_closing || !m_busy)
        return;
    const QString suggested = suggestedOutput(pending->operation, pending->input);
    const QString extension = operationExtension(pending->operation);
    const auto choose = m_dependencies.chooseOutput;
    if (choose) {
        QPointer<MavlinkLogWindow> guard(this);
        const QString output = choose(suggested, pending->label, extension);
        if (!guard || m_pending != pending || pending->flow != m_flow
            || m_closing)
            return;
        if (output.isEmpty()) {
            finishPending(tr("%1 output selection cancelled.").arg(pending->label));
            return;
        }
        continueAfterOutput(output, pending);
        return;
    }

    auto *dialog = new QFileDialog(
        this,
        pending->operation == Operation::Matlab
            ? tr("Save new Matlab file (existing files are refused)")
            : tr("Save converted log"),
        suggested,
        tr("%1 files (*.%2)").arg(pending->label, extension));
    dialog->setObjectName(QStringLiteral("MavlinkLogOutputDialog"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setDefaultSuffix(extension);
    dialog->setOption(QFileDialog::DontConfirmOverwrite,
                      pending->operation == Operation::Matlab);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    m_outputDialog = dialog;
    connect(dialog, &QFileDialog::finished, this,
            [this, dialog, pending](int result) {
        if (m_outputDialog != dialog || m_pending != pending
            || pending->flow != m_flow)
            return;
        const QString output = dialog->selectedFiles().value(0);
        m_outputDialog = nullptr;
        if (result != QDialog::Accepted || output.isEmpty()) {
            finishPending(tr("%1 output selection cancelled.").arg(pending->label));
            return;
        }
        continueAfterOutput(output, pending);
    });
    dialog->open();
}

void MavlinkLogWindow::continueAfterOutput(
    const QString &output, const std::shared_ptr<PendingExport> &pending)
{
    if (!pending || pending != m_pending || pending->flow != m_flow
        || m_closing || !m_busy)
        return;
    if (comparablePath(output) == comparablePath(pending->input)) {
        finishPending(tr("The export destination must not replace the selected .tlog."));
        return;
    }
    if (pending->operation == Operation::Matlab && QFileInfo::exists(output)) {
        finishPending(tr("The Matlab export destination already exists. Choose a new path; existing MAT files are never replaced."));
        return;
    }
    startExportWorker(output, pending);
}

void MavlinkLogWindow::startExportWorker(
    const QString &output, const std::shared_ptr<PendingExport> &pending)
{
    if (!pending || pending != m_pending || pending->flow != m_flow
        || m_closing || m_thread)
        return;
    const TlogExportFormat format = exportFormat(pending->operation);
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_progressState = std::make_shared<ProgressState>();
    const auto cancelFlag = m_cancelFlag;
    const auto progressState = m_progressState;
    const auto exporter = m_dependencies.exportLog;
    const auto exporterWithProgress = m_dependencies.exportLogWithProgress;
    const auto result = std::make_shared<TlogExportResult>();
    const QString input = pending->input;
    const QString label = pending->label;
    const quint64 flow = pending->flow;

    const bool progressSignalsBlocked = m_progress->blockSignals(true);
    m_progress->setRange(0, 0);
    m_progress->blockSignals(progressSignalsBlocked);
    m_progress->show();
    m_cancel->setEnabled(true);
    m_cancel->show();
    m_progressTimer->start();
    m_status->setText(
        (pending->operation == Operation::Kml
         || pending->operation == Operation::Gpx
         || pending->operation == Operation::Matlab)
            ? tr("Converting to %1…").arg(label)
            : tr("Exporting %1…").arg(label));

    QThread *thread = QThread::create(
        [cancelFlag, progressState, format, input, output, exporter,
         exporterWithProgress, result]() {
        const TlogExportService::CancelRequested cancel = [cancelFlag]() {
            return cancelFlag->load(std::memory_order_relaxed);
        };
        const TlogExportService::Progress progress =
            [progressState](qint64 completed, qint64 total) {
            progressState->completed.store(qMax<qint64>(0, completed),
                                           std::memory_order_relaxed);
            progressState->total.store(qMax<qint64>(0, total),
                                       std::memory_order_relaxed);
        };
        if (exporterWithProgress) {
            *result = exporterWithProgress(
                format, input, output, cancel, progress);
        } else if (exporter) {
            *result = exporter(format, input, output, cancel);
        } else {
            *result = TlogExportService::Export(
                format, input, output, cancel, progress);
        }
    });
    m_thread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, result, label, flow]() {
        if (m_thread != thread || !m_pending
            || m_pending->flow != flow || m_flow != flow)
            return;
        m_thread = nullptr;
        finishExport(*result, label, flow);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MavlinkLogWindow::finishExport(const TlogExportResult &result,
                                    const QString &label, quint64 flow)
{
    if (!m_pending || m_pending->flow != flow || m_flow != flow)
        return;
    m_progressTimer->stop();
    m_progress->hide();
    m_cancel->hide();
    m_progressState.reset();
    m_pending.reset();
    setBusy(false);
    m_cancelFlag.reset();
    if (result.cancelled) {
        m_status->setText(tr("%1 export cancelled.").arg(label));
    } else if (!result.success) {
        const bool track = label == QStringLiteral("KML")
            || label == QStringLiteral("GPX");
        m_status->setText(
            track ? tr("%1 conversion failed: %2").arg(label, result.error)
                  : tr("%1 export failed: %2").arg(label, result.error));
    } else {
        m_status->setText(result.message);
    }
    if (m_closeWhenIdle) {
        m_closeWhenIdle = false;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void MavlinkLogWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_pick->setEnabled(!busy && !m_closing);
    const bool canExport = !busy && !m_closing && !m_tlogPath.isEmpty();
    for (QPushButton *button : m_exportButtons) {
        button->setEnabled(canExport);
    }
}

void MavlinkLogWindow::stopWorker()
{
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }
    QThread *thread = m_thread.data();
    m_thread = nullptr;
    if (!thread) {
        return;
    }
    thread->requestInterruption();
    if (thread->isRunning()) {
        thread->wait();
    }
    disconnect(thread, nullptr, this, nullptr);
    disconnect(thread, &QThread::finished, thread, &QObject::deleteLater);
    delete thread;
}

void MavlinkLogWindow::cancelCurrent()
{
    if (!m_busy)
        return;
    if (m_thread && m_cancelFlag) {
        m_cancelFlag->store(true, std::memory_order_relaxed);
        m_cancel->setEnabled(false);
        m_status->setText(tr("Cancelling export; waiting for the worker to stop safely…"));
        return;
    }
    ++m_flow;
    if (!dismissDialogs())
        return;
    m_pending.reset();
    setBusy(false);
    m_status->setText(tr("Export cancelled before background work started."));
}

void MavlinkLogWindow::updateProgress()
{
    if (!m_thread || !m_progressState)
        return;
    const qint64 completed = m_progressState->completed.load(
        std::memory_order_relaxed);
    const qint64 total = m_progressState->total.load(
        std::memory_order_relaxed);
    const bool blocked = m_progress->blockSignals(true);
    if (total > 0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(int(qMin<qint64>(1000,
            completed > total ? 1000 : completed * 1000 / total)));
    } else {
        m_progress->setRange(0, 0);
    }
    m_progress->blockSignals(blocked);
}

bool MavlinkLogWindow::dismissDialogs()
{
    const auto dismiss = [](auto &member) {
        auto dialog = member;
        member = nullptr;
        if (!dialog)
            return;
        const bool blocked = dialog->blockSignals(true);
        dialog->reject();
        if (!dialog)
            return;
        dialog->blockSignals(blocked);
        dialog->deleteLater();
    };
    QPointer<MavlinkLogWindow> guard(this);
    dismiss(m_inputDialog);
    if (!guard)
        return false;
    dismiss(m_outputDialog);
    if (!guard)
        return false;
    dismiss(m_confirmationDialog);
    return bool(guard);
}

void MavlinkLogWindow::finishPending(const QString &status)
{
    m_pending.reset();
    setBusy(false);
    if (!status.isEmpty())
        m_status->setText(status);
}

QString MavlinkLogWindow::suggestedOutput(
    Operation operation, const QString &inputPath) const
{
    const QFileInfo input(inputPath);
    if (operation == Operation::Matlab)
        return input.absoluteFilePath() + QStringLiteral(".mat");
    return input.dir().filePath(
        input.completeBaseName() + QLatin1Char('.')
        + operationExtension(operation));
}

void MavlinkLogWindow::closeEvent(QCloseEvent *event)
{
    m_closing = true;
    if (m_thread) {
        event->ignore();
        m_closeWhenIdle = true;
        if (m_cancelFlag)
            m_cancelFlag->store(true, std::memory_order_relaxed);
        m_cancel->setEnabled(false);
        m_status->setText(tr("Cancelling export; this window will close after the worker stops safely…"));
        return;
    }
    ++m_flow;
    if (!dismissDialogs())
        return;
    m_pending.reset();
    setBusy(false);
    event->accept();
}

QString MavlinkLogWindow::operationLabel(Operation operation) const
{
    switch (operation) {
    case Operation::Kml: return QStringLiteral("KML");
    case Operation::Gpx: return QStringLiteral("GPX");
    case Operation::Matlab: return QStringLiteral("Matlab");
    case Operation::Csv: return QStringLiteral("CSV");
    case Operation::Text: return tr("human-readable text");
    case Operation::Parameters: return tr("parameters");
    case Operation::Missions: return tr("mission snapshots");
    }
    return QString();
}

QString MavlinkLogWindow::operationExtension(Operation operation) const
{
    switch (operation) {
    case Operation::Kml: return QStringLiteral("kml");
    case Operation::Gpx: return QStringLiteral("gpx");
    case Operation::Matlab: return QStringLiteral("mat");
    case Operation::Csv: return QStringLiteral("csv");
    case Operation::Text: return QStringLiteral("txt");
    case Operation::Parameters: return QStringLiteral("param");
    case Operation::Missions: return QStringLiteral("waypoints");
    }
    return QString();
}

TlogExportFormat MavlinkLogWindow::exportFormat(Operation operation) const
{
    switch (operation) {
    case Operation::Kml: return TlogExportFormat::Kml;
    case Operation::Gpx: return TlogExportFormat::Gpx;
    case Operation::Matlab: return TlogExportFormat::Matlab;
    case Operation::Csv: return TlogExportFormat::Csv;
    case Operation::Text: return TlogExportFormat::Text;
    case Operation::Parameters: return TlogExportFormat::Parameters;
    case Operation::Missions: return TlogExportFormat::Missions;
    }
    return TlogExportFormat::Csv;
}
