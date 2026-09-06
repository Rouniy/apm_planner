#include "DataFlashLogToolsController.h"

#include "DataFlashLogsWidget.h"
#include "ui/Loghandling/DataFlashGpxExporter.h"

#include <QtConcurrentRun>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <atomic>
#include <exception>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{
QString shortWarnings(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return {};
    const int count = qMin(3, warnings.size());
    QString text = warnings.mid(0, count).join(QStringLiteral(" "));
    if (warnings.size() > count)
        text += QObject::tr(" (+%1 more warning(s))").arg(warnings.size() - count);
    return text;
}

bool regularUnlinkedFile(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymLink()
        && !info.canonicalFilePath().isEmpty();
}

QString changedSourceError()
{
    return QObject::tr(
        "The source log changed while it was being frozen. Stop active log writers and retry.");
}

bool hashFile(const QString &path, qint64 expectedSize, const std::function<bool()> &cancel,
              QByteArray *digest, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancel && cancel())
            return false;
        const QByteArray bytes = file.read(1024 * 1024);
        if (file.pos() > expectedSize) {
            if (error) *error = changedSourceError();
            return false;
        }
        if (bytes.isEmpty() && file.error() != QFile::NoError) {
            if (error)
                *error = file.errorString();
            return false;
        }
        hash.addData(bytes);
    }
    if (file.pos() != expectedSize) {
        if (error) *error = changedSourceError();
        return false;
    }
    if (digest)
        *digest = hash.result();
    return true;
}

bool freezeSource(const QString &source, const QString &snapshot,
                  const std::function<bool()> &cancel,
                  const std::function<void(qint64, qint64)> &progress,
                  QString *error)
{
    const QFileInfo before(source);
    if (!regularUnlinkedFile(source)) {
        if (error)
            *error = QObject::tr("The source must be a readable regular, non-symlink file.");
        return false;
    }
    const QString canonical = before.canonicalFilePath();
    const qint64 expected = before.size();
    const QDateTime modified = before.lastModified();
    if (expected < 0 || expected > DataFlashLogAnalyzer::MaximumInputBytes) {
        if (error)
            *error = QObject::tr("The source exceeds the 1 GiB offline-analysis bound.");
        return false;
    }
    QFile input(source);
    QFile output(snapshot);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = input.errorString();
        return false;
    }
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error)
            *error = output.errorString();
        return false;
    }
    QCryptographicHash copiedHash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!input.atEnd()) {
        if (cancel && cancel()) {
            output.close();
            QFile::remove(snapshot);
            return false;
        }
        const QByteArray bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() && input.error() != QFile::NoError) {
            if (error)
                *error = input.errorString();
            output.close();
            QFile::remove(snapshot);
            return false;
        }
        if (bytes.size() > expected - copied) {
            if (error)
                *error = changedSourceError();
            output.close();
            QFile::remove(snapshot);
            return false;
        }
        if (output.write(bytes) != bytes.size()) {
            if (error)
                *error = output.errorString();
            output.close();
            QFile::remove(snapshot);
            return false;
        }
        copiedHash.addData(bytes);
        copied += bytes.size();
        if (progress)
            progress(copied, qMax<qint64>(1, expected * 2));
    }
    if (!output.flush()) {
        if (error)
            *error = output.errorString();
        output.close();
        QFile::remove(snapshot);
        return false;
    }
    output.close();

    const QFileInfo after(source);
    if (copied != expected || after.size() != expected
        || after.canonicalFilePath() != canonical
        || after.lastModified() != modified) {
        if (error)
            *error = changedSourceError();
        QFile::remove(snapshot);
        return false;
    }
    QByteArray verifiedHash;
    QString hashError;
    if (!hashFile(source, expected, cancel, &verifiedHash, &hashError)) {
        if (cancel && cancel()) {
            QFile::remove(snapshot);
            return false;
        }
        if (error)
            *error = hashError;
        QFile::remove(snapshot);
        return false;
    }
    if (verifiedHash != copiedHash.result()) {
        if (error)
            *error = changedSourceError();
        QFile::remove(snapshot);
        return false;
    }
    if (progress)
        progress(qMax<qint64>(1, expected * 2), qMax<qint64>(1, expected * 2));
    return true;
}

bool publishNewFile(const QString &staging, const QString &destination,
                    const QString &expectedParentCanonical, QString *error)
{
    const QString currentParent = QFileInfo(
        QFileInfo(destination).absolutePath()).canonicalFilePath();
    if (currentParent.isEmpty() || currentParent != expectedParentCanonical) {
        if (error)
            *error = QObject::tr("The output directory changed after confirmation: %1")
                .arg(QFileInfo(destination).absolutePath());
        return false;
    }
    const QFileInfo destinationInfo(destination);
    if (destinationInfo.exists() || destinationInfo.isSymLink()) {
        if (error)
            *error = QObject::tr("Output already exists: %1").arg(destination);
        return false;
    }
#ifdef Q_OS_UNIX
    const QByteArray stagingName = QFile::encodeName(staging);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::link(stagingName.constData(), destinationName.constData()) != 0) {
        if (error)
            *error = QObject::tr(
                "Could not atomically publish a new output without replacement: %1")
                .arg(destination);
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(staging.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        if (error)
            *error = QObject::tr(
                "Could not atomically publish a new output without replacement: %1")
                .arg(destination);
        return false;
    }
    return true;
#else
    Q_UNUSED(staging)
    Q_UNUSED(destination)
    if (error)
        *error = QObject::tr(
            "Atomic no-replace publication is unavailable on this platform.");
    return false;
#endif
}
} // namespace

struct DataFlashLogToolsController::JobState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> completed{0};
    std::atomic<qint64> total{0};
};

struct DataFlashLogToolsController::WorkerResult
{
    struct MapResult {
        bool success = false;
        bool cancelled = false;
        QString error;
        QStringList warnings;
        QStringList published;
        qint64 kmlPoints = 0;
        qint64 gpxPoints = 0;
    } map;

    Work work = Work::None;
    DataFlashBinToLogConverter::Result converted;
    DataFlashLogAnalyzer::Result analysis;
    DataFlashMatlabExporter::PlanResult matlabPreparation;
    DataFlashMatlabExporter::Result matlabExport;
    FlightLogOrganizer::Analysis organizerAnalysis;
    FlightLogOrganizer::Result organizerResult;
};

DataFlashLogToolsController::Operations
DataFlashLogToolsController::defaultOperations()
{
    Operations operations;
    operations.convertBinToLog = [](
        const QString &input, const QString &output,
        const DataFlashBinToLogConverter::CancelCheck &cancel,
        const DataFlashBinToLogConverter::Progress &progress) {
        return DataFlashBinToLogConverter::Convert(input, output, cancel, progress);
    };
    operations.exportKml = [](
        const QString &input, const QString &output,
        const DataFlashKmlExporter::CancellationCheck &cancel) {
        return DataFlashKmlExporter::Export(input, output, cancel);
    };
    operations.exportGpx = [](
        const QString &input, const QString &output,
        const DataFlashGpxExporter::CancelCheck &cancel,
        const DataFlashGpxExporter::Progress &progress) {
        return DataFlashGpxExporter::Export(input, output, cancel, progress);
    };
    operations.analyze = [](
        const QString &input, DataFlashLogAnalyzer::Cancel cancel,
        DataFlashLogAnalyzer::Progress progress) {
        return DataFlashLogAnalyzer::Analyze(input, std::move(cancel), std::move(progress));
    };
    operations.prepareMatlab = [](
        const QString &input, const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        return DataFlashMatlabExporter::Prepare(input, cancel, progress);
    };
    operations.exportMatlab = [](
        const DataFlashMatlabExporter::Plan &plan,
        const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        return DataFlashMatlabExporter::Export(plan, cancel, progress);
    };
    operations.analyzeDirectory = [](
        const QString &root, const FlightLogOrganizer::Cancel &cancel,
        const FlightLogOrganizer::Progress &progress) {
        return FlightLogOrganizer::Analyze(root, cancel, progress);
    };
    operations.executeOrganizer = [](
        const FlightLogOrganizer::Plan &plan,
        const FlightLogOrganizer::Cancel &cancel,
        const FlightLogOrganizer::Progress &progress) {
        return FlightLogOrganizer::Execute(plan, cancel, progress);
    };
    return operations;
}

DataFlashLogToolsController::DataFlashLogToolsController(
    DataFlashLogsWidget *widget, QWidget *dialogParent)
    : QObject(widget ? static_cast<QObject *>(widget)
                     : static_cast<QObject *>(dialogParent))
    , m_widget(widget)
    , m_dialogParent(dialogParent ? dialogParent : widget)
    , m_progressTimer(new QTimer(this))
    , m_operations(defaultOperations())
{
    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &DataFlashLogToolsController::updateProgress);
    if (widget) {
        connect(widget, &DataFlashLogsWidget::reviewRequested,
                this, &DataFlashLogToolsController::startReview);
        connect(widget, &DataFlashLogsWidget::autoAnalysisRequested,
                this, &DataFlashLogToolsController::startAutoAnalysis);
        connect(widget, &DataFlashLogsWidget::kmlGpxRequested,
                this, &DataFlashLogToolsController::startKmlGpx);
        connect(widget, &DataFlashLogsWidget::binToLogRequested,
                this, &DataFlashLogToolsController::startBinToLog);
        connect(widget, &DataFlashLogsWidget::matlabRequested,
                this, &DataFlashLogToolsController::startMatlab);
        connect(widget, &DataFlashLogsWidget::organizeRequested,
                this, &DataFlashLogToolsController::startOrganize);
    }
}

DataFlashLogToolsController::~DataFlashLogToolsController()
{
    m_destroying = true;
    ++m_flow;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_release);
    if (m_progressTimer)
        m_progressTimer->stop();
    const auto retire = [](QPointer<QDialog> &stored) {
        const QPointer<QDialog> dialog(stored);
        stored.clear();
        if (!dialog)
            return;
        QObject::disconnect(dialog.data(), nullptr, nullptr, nullptr);
        dialog->blockSignals(true);
        if (!QCoreApplication::closingDown()) {
            dialog->hide();
            if (dialog)
                dialog->deleteLater();
        }
    };
    retire(m_prompt);
    QPointer<QDialog> progress(m_progress.data());
    retire(progress);
    m_progress.clear();
    retire(m_report);
}

bool DataFlashLogToolsController::busy() const noexcept
{
    return m_phase != Phase::Idle;
}

bool DataFlashLogToolsController::shutdownPending() const noexcept
{
    return m_shutdownPending;
}

QString DataFlashLogToolsController::selectedLogPath() const
{
    return m_selectedLog;
}

void DataFlashLogToolsController::setSelectedLogPath(const QString &path)
{
    if (busy() || m_shutdownPending)
        return;
    m_selectedLog = path.isEmpty()
        ? QString() : QFileInfo(path).absoluteFilePath();
}

void DataFlashLogToolsController::setOperationsForTesting(
    const Operations &operations)
{
    if (busy() || m_shutdownPending) {
        publishLog(tr("DataFlash Logs: operations cannot change while work is active."));
        return;
    }
    m_operations = operations;
}

void DataFlashLogToolsController::startReview()
{
    begin(Intent::Review);
}

void DataFlashLogToolsController::startAutoAnalysis()
{
    begin(Intent::Analyze);
}

void DataFlashLogToolsController::startKmlGpx()
{
    begin(Intent::KmlGpx);
}

void DataFlashLogToolsController::startBinToLog()
{
    begin(Intent::BinToLog);
}

void DataFlashLogToolsController::startMatlab()
{
    begin(Intent::Matlab);
}

void DataFlashLogToolsController::startOrganize()
{
    begin(Intent::Organize);
}

bool DataFlashLogToolsController::selectedPathUsableFor(Intent intent) const
{
    if (!regularUnlinkedFile(m_selectedLog))
        return false;
    const QString suffix = QFileInfo(m_selectedLog).suffix();
    if (intent == Intent::BinToLog)
        return suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0;
    return suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0;
}

void DataFlashLogToolsController::begin(Intent intent)
{
    if (m_destroying || m_shutdownPending)
        return;
    if (busy()) {
        publishLog(tr("DataFlash Logs: another local log operation is already active."));
        return;
    }
    if (!m_widget || !m_dialogParent) {
        publishLog(tr("DataFlash Logs: the DATA page is unavailable."));
        return;
    }
    if (intent == Intent::None)
        return;

    ++m_flow;
    if (m_flow == 0)
        ++m_flow;
    const quint64 flow = m_flow;
    m_intent = intent;
    m_work = Work::None;
    m_cancelReported = false;
    m_outputPath.clear();
    m_mapOutputs.clear();
    m_mapParentCanonical.clear();
    m_organizerRoot.clear();
    m_organizerPlan = FlightLogOrganizer::Plan();
    m_matlabPlan.reset();
    m_matlabWarnings.clear();
    m_phase = intent == Intent::Organize
        ? Phase::DirectoryPrompt : Phase::InputPrompt;
    const QPointer<DataFlashLogToolsController> guard(this);
    m_widget->setOperationBusy(true);
    if (!guard)
        return;
    emit busyChanged(true);
    if (!guard || flow != m_flow || m_shutdownPending || m_destroying)
        return;
    if (!m_widget || !m_dialogParent) {
        finishFlow(flow, {tr("DataFlash Logs: the dialog owner disappeared; nothing was started.")});
        return;
    }

    if (intent == Intent::Organize) {
        showOrganizerDirectoryPrompt(flow);
    } else if (intent != Intent::Review && selectedPathUsableFor(intent)) {
        acceptInput(flow, intent, m_selectedLog);
    } else {
        showInputPrompt(flow, intent);
    }
}

void DataFlashLogToolsController::showInputPrompt(quint64 flow, Intent intent)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::InputPrompt || !m_dialogParent)
        return;
    auto *dialog = new QFileDialog(m_dialogParent, tr("Open dataflash log"));
    dialog->setObjectName(QStringLiteral("DataFlashLogInputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    if (intent == Intent::BinToLog) {
        dialog->setNameFilters({tr("DataFlash binary log (*.bin *.BIN)"),
                                tr("All files (*)")});
    } else {
        dialog->setNameFilters({tr("DataFlash log (*.bin *.BIN *.log *.LOG)"),
                                tr("All files (*)")});
    }
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow, intent](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::InputPrompt
            || m_prompt != dialog)
            return;
        m_prompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result != QDialog::Accepted || files.size() != 1) {
            finishFlow(flow, {tr("DataFlash Logs: source selection cancelled.")});
            return;
        }
        acceptInput(flow, intent, files.first());
    });
    dialog->open();
}

void DataFlashLogToolsController::acceptInput(
    quint64 flow, Intent intent, const QString &path)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::InputPrompt)
        return;
    const QString frozen = QFileInfo(path).absoluteFilePath();
    if (!regularUnlinkedFile(frozen)) {
        finishFlow(flow, {tr("DataFlash Logs: select a readable regular, non-symlink log file.")});
        return;
    }
    const QString suffix = QFileInfo(frozen).suffix();
    if (intent == Intent::BinToLog
        && suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) != 0) {
        finishFlow(flow, {tr("BIN to LOG requires a .bin source file.")});
        return;
    }
    if (intent != Intent::BinToLog
        && suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) != 0
        && suffix.compare(QStringLiteral("log"), Qt::CaseInsensitive) != 0) {
        finishFlow(flow, {tr("Select a DataFlash .bin or .log source file.")});
        return;
    }
    m_selectedLog = frozen;

    if (intent == Intent::Review) {
        const QPointer<DataFlashLogToolsController> guard(this);
        finishFlow(flow);
        if (!guard || m_flow != flow || m_phase != Phase::Idle || m_shutdownPending)
            return;
        emit reviewLogRequested(frozen);
        if (!guard || m_flow != flow || m_phase != Phase::Idle || m_shutdownPending)
            return;
        setWidgetStatus(tr("Opening %1 in Log Browser…").arg(QFileInfo(frozen).fileName()));
        return;
    }
    if (intent == Intent::Analyze) {
        startWorker(flow, Work::Analyze);
    } else if (intent == Intent::KmlGpx) {
        showKmlGpxConsent(flow);
    } else if (intent == Intent::BinToLog) {
        showBinOutputPrompt(flow);
    } else if (intent == Intent::Matlab) {
        startWorker(flow, Work::MatlabPrepare);
    }
}

QString DataFlashLogToolsController::suggestedLogOutput(const QString &input)
{
    const QFileInfo info(input);
    return info.dir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".log"));
}

QStringList DataFlashLogToolsController::suggestedMapOutputs(const QString &input)
{
    const QFileInfo info(input);
    const QString base = info.dir().absoluteFilePath(info.completeBaseName());
    return {base + QStringLiteral(".kml"), base + QStringLiteral(".gpx")};
}

void DataFlashLogToolsController::showBinOutputPrompt(quint64 flow)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::InputPrompt || !m_dialogParent)
        return;
    m_phase = Phase::OutputPrompt;
    auto *dialog = new QFileDialog(m_dialogParent, tr("Save text log"));
    dialog->setObjectName(QStringLiteral("DataFlashBinToLogOutputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilters({tr("DataFlash text log (*.log)"), tr("All files (*)")});
    dialog->setDefaultSuffix(QStringLiteral("log"));
    dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
    dialog->selectFile(suggestedLogOutput(m_selectedLog));
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::OutputPrompt
            || m_prompt != dialog)
            return;
        m_prompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result != QDialog::Accepted || files.size() != 1) {
            finishFlow(flow, {tr("BIN to LOG: output selection cancelled; no file was written.")});
            return;
        }
        m_outputPath = QFileInfo(files.first()).absoluteFilePath();
        const QFileInfo output(m_outputPath);
        if (output.exists() || output.isSymLink()) {
            finishFlow(flow, {tr(
                "BIN to LOG refused because the output already exists. Choose a new path: %1")
                    .arg(m_outputPath)});
            return;
        }
        startWorker(flow, Work::BinToLog);
    });
    dialog->open();
}

void DataFlashLogToolsController::showKmlGpxConsent(quint64 flow)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::InputPrompt || !m_dialogParent)
        return;
    m_mapOutputs = suggestedMapOutputs(m_selectedLog);
    m_mapParentCanonical = QFileInfo(
        QFileInfo(m_mapOutputs.value(0)).absolutePath()).canonicalFilePath();
    if (m_mapParentCanonical.isEmpty()) {
        finishFlow(flow, {tr("KML + GPX refused because the output directory is unavailable.")});
        return;
    }
    for (const QString &path : std::as_const(m_mapOutputs)) {
        const QFileInfo info(path);
        if (info.exists() || info.isSymLink()) {
            finishFlow(flow, {tr(
                "KML + GPX refused because an exact target already exists. Move it or choose a differently named source; existing files are never replaced: %1")
                    .arg(path)});
            return;
        }
    }
    m_phase = Phase::Consent;
    auto *dialog = new QDialog(m_dialogParent);
    dialog->setObjectName(QStringLiteral("DataFlashKmlGpxConfirmationDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Create KML + gpx"));
    dialog->resize(760, 360);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("DataFlashKmlGpxSummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setText(tr(
        "Read the frozen DataFlash source and create both new map files next to it.\n\n"
        "Source: %1\nKML: %2\nGPX: %3\n\n"
        "Existing targets will not be replaced. KML is published first; a later GPX failure can leave the listed KML as a truthful partial result. Stop active log writers before continuing. No vehicle is contacted.")
        .arg(m_selectedLog, m_mapOutputs.value(0), m_mapOutputs.value(1)));
    layout->addWidget(summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::Cancel,
                                         dialog);
    auto *yes = buttons->button(QDialogButtonBox::Yes);
    yes->setObjectName(QStringLiteral("DataFlashKmlGpxCreateButton"));
    yes->setText(tr("Create new files"));
    yes->setAutoDefault(false);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("DataFlashKmlGpxCancelButton"));
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::Consent
            || m_prompt != dialog)
            return;
        m_prompt.clear();
        if (result != QDialog::Accepted) {
            finishFlow(flow, {tr("KML + GPX: cancelled before conversion; no output was written.")});
            return;
        }
        for (const QString &path : std::as_const(m_mapOutputs)) {
            const QFileInfo info(path);
            if (info.exists() || info.isSymLink()) {
                finishFlow(flow, {tr(
                    "KML + GPX refused because an exact target appeared before admission: %1")
                        .arg(path)});
                return;
            }
        }
        startWorker(flow, Work::KmlGpx);
    });
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QDialog> dialogGuard(dialog);
    dialog->show();
    if (!guard || !dialogGuard)
        return;
    dialog->raise();
    if (!guard || !dialogGuard)
        return;
    dialog->activateWindow();
}

void DataFlashLogToolsController::showMatlabConsent(quint64 flow)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::Consent || !m_dialogParent
        || !m_matlabPlan || !m_matlabPlan->isValid()) {
        return;
    }
    const std::shared_ptr<const DataFlashMatlabExporter::Plan> plan = m_matlabPlan;
    auto *dialog = new QDialog(m_dialogParent);
    dialog->setObjectName(QStringLiteral("DataFlashMatlabConfirmationDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Create Matlab File"));
    dialog->resize(760, 430);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("DataFlashMatlabSummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setText(tr(
        "Export the frozen DataFlash schema and records to a MATLAB Level-5 file.\n\n"
        "Source: %1\nExact new output: %2\nRecords: %3\nVariables: %4\nEstimated output bytes: %5\n\n"
        "The record count is part of the filename. The exact target must remain absent and will never be replaced. Preparation and export revalidate the immutable source plan. Cancellation before publication leaves no output. This offline conversion does not contact or change a vehicle.")
        .arg(plan->sourcePath(), plan->outputPath(),
             QString::number(plan->recordCount()),
             QString::number(plan->variableCount()),
             QString::number(plan->estimatedBytes())));
    layout->addWidget(summary);
    QStringList warnings = m_matlabWarnings;
    for (const QString &warning : plan->warnings()) {
        if (!warnings.contains(warning))
            warnings.append(warning);
    }
    if (!warnings.isEmpty()) {
        auto *warningView = new QPlainTextEdit(dialog);
        warningView->setObjectName(QStringLiteral("DataFlashMatlabWarnings"));
        warningView->setReadOnly(true);
        warningView->setMaximumHeight(120);
        warningView->setPlainText(warnings.join(QLatin1Char('\n')));
        layout->addWidget(warningView);
    }
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Yes | QDialogButtonBox::Cancel, dialog);
    auto *exportButton = buttons->button(QDialogButtonBox::Yes);
    exportButton->setObjectName(QStringLiteral("DataFlashMatlabExportButton"));
    exportButton->setText(tr("Create new MATLAB file"));
    exportButton->setAutoDefault(false);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("DataFlashMatlabCancelButton"));
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow, plan](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::Consent
            || m_prompt != dialog || m_matlabPlan != plan) {
            return;
        }
        m_prompt.clear();
        if (result != QDialog::Accepted) {
            finishFlow(flow, {tr(
                "MATLAB export cancelled after read-only preparation; no output was written.")});
            return;
        }
        const QFileInfo output(plan->outputPath());
        if (output.exists() || output.isSymLink()) {
            finishFlow(flow, {tr(
                "MATLAB export refused because the exact output appeared after confirmation: %1")
                    .arg(plan->outputPath())});
            return;
        }
        startWorker(flow, Work::MatlabExport);
    });
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QDialog> dialogGuard(dialog);
    dialog->show();
    if (!guard || !dialogGuard)
        return;
    dialog->raise();
    if (!guard || !dialogGuard)
        return;
    dialog->activateWindow();
}

void DataFlashLogToolsController::showOrganizerDirectoryPrompt(quint64 flow)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::DirectoryPrompt || !m_dialogParent)
        return;
    auto *dialog = new QFileDialog(m_dialogParent, tr("Choose log directory to organize"));
    dialog->setObjectName(QStringLiteral("DataFlashLogOrganizerDirectoryDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, false);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::DirectoryPrompt
            || m_prompt != dialog)
            return;
        m_prompt.clear();
        const QStringList files = dialog->selectedFiles();
        if (result != QDialog::Accepted || files.size() != 1) {
            finishFlow(flow, {tr("Organize logs: directory selection cancelled.")});
            return;
        }
        const QFileInfo root(files.first());
        if (!root.exists() || !root.isDir() || root.isSymLink()) {
            finishFlow(flow, {tr("Organize logs requires a real, non-symlink directory.")});
            return;
        }
        m_organizerRoot = root.absoluteFilePath();
        startWorker(flow, Work::OrganizeAnalyze);
    });
    dialog->open();
}

void DataFlashLogToolsController::showOrganizerConsent(quint64 flow)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || m_phase != Phase::Consent || !m_dialogParent
        || !m_organizerPlan.isValid())
        return;
    auto *dialog = new QDialog(m_dialogParent);
    dialog->setObjectName(QStringLiteral("DataFlashLogOrganizerPlanDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Organize tlog/rlog/bin/log"));
    dialog->resize(980, 620);
    dialog->setMinimumSize(720, 420);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("DataFlashLogOrganizerSummary"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setText(tr(
        "Review every planned filesystem change under:\n%1\n\n"
        "Candidates inspected: %2. Planned changes: %3. Empty log deletion is permanent. Moves never overwrite. Stop recording and close active log writers first; live writers are not detected or locked. Corrected classification may relocate logs sorted by older Mission Planner versions. No date is inferred.")
        .arg(m_organizerPlan.root())
        .arg(m_organizerPlan.candidateCount())
        .arg(m_organizerPlan.entries().size()));
    layout->addWidget(summary);
    auto *tree = new QTreeWidget(dialog);
    tree->setObjectName(QStringLiteral("DataFlashLogOrganizerPlanTree"));
    tree->setColumnCount(4);
    tree->setHeaderLabels({tr("Action"), tr("Bytes"), tr("Source"), tr("Destination")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    for (const FlightLogOrganizer::Entry &entry : m_organizerPlan.entries()) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, entry.operation == FlightLogOrganizer::Operation::Move
            ? tr("Move") : tr("Delete empty"));
        item->setText(1, QString::number(entry.bytes));
        item->setText(2, entry.source);
        item->setText(3, entry.destination);
        item->setToolTip(2, entry.source);
        item->setToolTip(3, entry.destination);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tree->setColumnWidth(2, 320);
    tree->setColumnWidth(3, 320);
    layout->addWidget(tree, 1);
    if (!m_organizerPlan.warnings().isEmpty()) {
        auto *warnings = new QPlainTextEdit(dialog);
        warnings->setObjectName(QStringLiteral("DataFlashLogOrganizerWarnings"));
        warnings->setReadOnly(true);
        warnings->setMaximumHeight(120);
        warnings->setPlainText(m_organizerPlan.warnings().join(QLatin1Char('\n')));
        layout->addWidget(warnings);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::Cancel,
                                         dialog);
    auto *execute = buttons->button(QDialogButtonBox::Yes);
    execute->setObjectName(QStringLiteral("DataFlashLogOrganizerExecuteButton"));
    execute->setText(tr("Execute exact plan"));
    execute->setAutoDefault(false);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("DataFlashLogOrganizerCancelButton"));
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow || m_phase != Phase::Consent
            || m_prompt != dialog)
            return;
        m_prompt.clear();
        if (result != QDialog::Accepted) {
            finishFlow(flow, {tr("Organize logs: exact plan cancelled; no planned changes were made.")});
            return;
        }
        startWorker(flow, Work::OrganizeExecute);
    });
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QDialog> dialogGuard(dialog);
    dialog->show();
    if (!guard || !dialogGuard)
        return;
    dialog->raise();
    if (!guard || !dialogGuard)
        return;
    dialog->activateWindow();
}

void DataFlashLogToolsController::showProgress(const QString &label)
{
    if (!m_dialogParent)
        return;
    auto *progress = new QProgressDialog(label, tr("Cancel"), 0, 0, m_dialogParent);
    progress->setObjectName(QStringLiteral("DataFlashLogProgressDialog"));
    progress->setWindowTitle(tr("DataFlash Logs"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));
    connect(progress, &QProgressDialog::canceled,
            this, &DataFlashLogToolsController::cancel);
    connect(progress, &QDialog::rejected,
            this, &DataFlashLogToolsController::cancel);
    m_progress = progress;
    progress->show();
}

void DataFlashLogToolsController::startWorker(quint64 flow, Work work)
{
    if (m_destroying || m_shutdownPending || flow != m_flow
        || (m_phase != Phase::InputPrompt && m_phase != Phase::OutputPrompt
            && m_phase != Phase::Consent && m_phase != Phase::DirectoryPrompt)) {
        return;
    }
    const Operations operations = m_operations;
    if (!operations.convertBinToLog || !operations.exportKml
        || !operations.exportGpx || !operations.analyze
        || !operations.prepareMatlab || !operations.exportMatlab
        || !operations.analyzeDirectory || !operations.executeOrganizer) {
        finishFlow(flow, {tr("DataFlash Logs: an offline operation backend is unavailable.")});
        return;
    }
    auto state = std::make_shared<JobState>();
    m_job = state;
    m_work = work;
    m_phase = Phase::Working;
    QString label;
    switch (work) {
    case Work::Analyze: label = tr("Analyzing the DataFlash log…"); break;
    case Work::KmlGpx: label = tr("Freezing the source and creating KML + GPX…"); break;
    case Work::BinToLog: label = tr("Converting BIN to text LOG…"); break;
    case Work::MatlabPrepare: label = tr("Preparing an immutable MATLAB export plan…"); break;
    case Work::MatlabExport: label = tr("Writing the confirmed MATLAB Level-5 file…"); break;
    case Work::OrganizeAnalyze: label = tr("Analyzing the log directory without changing files…"); break;
    case Work::OrganizeExecute: label = tr("Executing the confirmed log organization plan…"); break;
    default: break;
    }
    const QPointer<DataFlashLogToolsController> guard(this);
    showProgress(label);
    if (!guard || flow != m_flow || m_phase != Phase::Working
        || m_job != state)
        return;
    if (!m_progress) {
        finishFlow(flow, {tr("DataFlash Logs: progress window unavailable; nothing was started.")});
        return;
    }

    const QString input = m_selectedLog;
    const QString output = m_outputPath;
    const QStringList mapOutputs = m_mapOutputs;
    const QString mapParentCanonical = m_mapParentCanonical;
    const QString organizerRoot = m_organizerRoot;
    const FlightLogOrganizer::Plan organizerPlan = m_organizerPlan;
    const std::shared_ptr<const DataFlashMatlabExporter::Plan> matlabPlan =
        m_matlabPlan;
    auto *watcher = new QFutureWatcher<WorkerResult>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<WorkerResult>::finished,
            this, [this, watcher, state, flow]() {
        const WorkerResult result = watcher->result();
        watcher->deleteLater();
        finishWorker(flow, state, result);
    });
    watcher->setFuture(QtConcurrent::run(
        [operations, input, output, mapOutputs, mapParentCanonical, organizerRoot,
         organizerPlan, matlabPlan, state, work]() {
        WorkerResult result;
        result.work = work;
        if (work == Work::KmlGpx)
            result.map.published.reserve(2);
        const auto cancel = [state]() {
            return state->cancelled.load(std::memory_order_acquire);
        };
        const auto progress = [state](qint64 completed, qint64 total) {
            state->completed.store(qMax<qint64>(0, completed), std::memory_order_release);
            state->total.store(qMax<qint64>(0, total), std::memory_order_release);
        };
        try {
            if (work == Work::Analyze) {
                result.analysis = operations.analyze(input, cancel, progress);
            } else if (work == Work::BinToLog) {
                result.converted = operations.convertBinToLog(input, output, cancel, progress);
            } else if (work == Work::MatlabPrepare) {
                result.matlabPreparation = operations.prepareMatlab(
                    input, cancel, progress);
            } else if (work == Work::MatlabExport) {
                if (!matlabPlan || !matlabPlan->isValid()) {
                    result.matlabExport.error = QObject::tr(
                        "The immutable MATLAB export plan is unavailable.");
                } else {
                    result.matlabExport = operations.exportMatlab(
                        *matlabPlan, cancel, progress);
                }
            } else if (work == Work::OrganizeAnalyze) {
                result.organizerAnalysis = operations.analyzeDirectory(
                    organizerRoot, cancel, progress);
            } else if (work == Work::OrganizeExecute) {
                result.organizerResult = operations.executeOrganizer(
                    organizerPlan, cancel, progress);
            } else if (work == Work::KmlGpx) {
                if (mapOutputs.size() != 2) {
                    result.map.error = QObject::tr("The frozen output plan is incomplete.");
                    return result;
                }
                QTemporaryDir snapshotDirectory;
                if (!snapshotDirectory.isValid()) {
                    result.map.error = QObject::tr("Could not create a private source snapshot directory.");
                    return result;
                }
                const QString suffix = QFileInfo(input).suffix().toLower();
                const QString snapshot = snapshotDirectory.filePath(
                    QStringLiteral("source.%1").arg(suffix));
                QString freezeError;
                if (!freezeSource(input, snapshot, cancel, progress, &freezeError)) {
                    result.map.cancelled = cancel();
                    if (!result.map.cancelled)
                        result.map.error = freezeError;
                    return result;
                }
                if (cancel()) {
                    result.map.cancelled = true;
                    return result;
                }
                const QFileInfo kmlTarget(mapOutputs.at(0));
                if (QFileInfo(kmlTarget.absolutePath()).canonicalFilePath()
                    != mapParentCanonical) {
                    result.map.error = QObject::tr(
                        "The output directory changed after confirmation.");
                    return result;
                }
                QTemporaryDir stagingDirectory(kmlTarget.dir().absoluteFilePath(
                    QStringLiteral(".apm-map-export-XXXXXX")));
                if (!stagingDirectory.isValid()) {
                    result.map.error = QObject::tr(
                        "Could not create a private sibling staging directory.");
                    return result;
                }
                const QString kmlStage = stagingDirectory.filePath(QStringLiteral("track.kml"));
                const QString gpxStage = stagingDirectory.filePath(QStringLiteral("track.gpx"));
                const DataFlashKmlExporter::Result kml =
                    operations.exportKml(snapshot, kmlStage, cancel);
                result.map.kmlPoints = static_cast<qint64>(kml.pointCount);
                if (kml.cancelled || cancel()) {
                    result.map.cancelled = true;
                    return result;
                }
                if (!kml.succeeded) {
                    result.map.error = kml.error.isEmpty()
                        ? QObject::tr("KML export failed.") : kml.error;
                    return result;
                }
                QString publishError;
                if (!publishNewFile(kmlStage, mapOutputs.at(0),
                                    mapParentCanonical, &publishError)) {
                    result.map.error = publishError;
                    return result;
                }
                result.map.published.append(mapOutputs.at(0));
                if (cancel()) {
                    result.map.cancelled = true;
                    return result;
                }
                const DataFlashGpxExporter::Result gpx =
                    operations.exportGpx(snapshot, gpxStage, cancel, progress);
                result.map.gpxPoints = gpx.pointCount;
                result.map.warnings += gpx.warnings;
                if (gpx.cancelled || cancel()) {
                    result.map.cancelled = true;
                    return result;
                }
                if (!gpx.success) {
                    result.map.error = gpx.error.isEmpty()
                        ? QObject::tr("GPX export failed.") : gpx.error;
                    return result;
                }
                if (!publishNewFile(gpxStage, mapOutputs.at(1),
                                    mapParentCanonical, &publishError)) {
                    result.map.error = publishError;
                    return result;
                }
                result.map.published.append(mapOutputs.at(1));
                result.map.success = true;
            }
        } catch (const std::exception &exception) {
            const QString message = QObject::tr("Unexpected worker failure: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
            if (work == Work::Analyze) result.analysis.error = message;
            else if (work == Work::BinToLog) result.converted.error = message;
            else if (work == Work::MatlabPrepare) result.matlabPreparation.error = message;
            else if (work == Work::MatlabExport) result.matlabExport.error = message;
            else if (work == Work::OrganizeAnalyze) result.organizerAnalysis.error = message;
            else if (work == Work::OrganizeExecute) result.organizerResult.error = message;
            else result.map.error = message;
        } catch (...) {
            const QString message = QObject::tr("Unexpected worker failure.");
            if (work == Work::Analyze) result.analysis.error = message;
            else if (work == Work::BinToLog) result.converted.error = message;
            else if (work == Work::MatlabPrepare) result.matlabPreparation.error = message;
            else if (work == Work::MatlabExport) result.matlabExport.error = message;
            else if (work == Work::OrganizeAnalyze) result.organizerAnalysis.error = message;
            else if (work == Work::OrganizeExecute) result.organizerResult.error = message;
            else result.map.error = message;
        }
        return result;
    }));
    if (!guard || flow != m_flow || m_phase != Phase::Working
        || m_job != state || m_watcher != watcher)
        return;
    m_progressTimer->start();
}

QStringList DataFlashLogToolsController::workerMessages(
    const WorkerResult &result) const
{
    if (result.work == Work::BinToLog) {
        if (result.converted.cancelled)
            return {tr("BIN to LOG cancelled; no output was published.")};
        if (!result.converted.success)
            return {tr("BIN to LOG failed: %1").arg(result.converted.error)};
        QString message = tr("BIN to LOG wrote %1 record(s) to %2.")
            .arg(result.converted.recordsWritten).arg(result.converted.outputPath);
        const QString warnings = shortWarnings(result.converted.warnings);
        if (!warnings.isEmpty()) message += QLatin1Char(' ') + warnings;
        return {message};
    }
    if (result.work == Work::KmlGpx) {
        if (result.map.cancelled) {
            if (result.map.published.isEmpty())
                return {tr("KML + GPX cancelled before publication; no output was published.")};
            return {tr("KML + GPX cancelled after publishing: %1. The remaining output was not published.")
                        .arg(result.map.published.join(QStringLiteral(", ")))};
        }
        if (!result.map.success) {
            QString message = tr("KML + GPX failed: %1").arg(result.map.error);
            if (!result.map.published.isEmpty())
                message += tr(" Published before the failure: %1")
                    .arg(result.map.published.join(QStringLiteral(", ")));
            return {message};
        }
        QString message = tr("Created new KML (%1 points) and GPX (%2 points): %3")
            .arg(result.map.kmlPoints).arg(result.map.gpxPoints)
            .arg(result.map.published.join(QStringLiteral(", ")));
        const QString warnings = shortWarnings(result.map.warnings);
        if (!warnings.isEmpty()) message += QLatin1Char(' ') + warnings;
        return {message};
    }
    if (result.work == Work::MatlabExport) {
        const DataFlashMatlabExporter::Result &value = result.matlabExport;
        if (value.cancelled)
            return {tr("MATLAB export cancelled; no output was published.")};
        if (!value.success) {
            return {tr("MATLAB export failed; no output was published: %1")
                        .arg(value.error)};
        }
        QString message = tr(
            "Created MATLAB Level-5 file with %1 record(s), %2 variable(s), and %3 byte(s): %4")
            .arg(value.recordCount).arg(value.variableCount)
            .arg(value.bytesWritten).arg(value.outputPath);
        const QString warnings = shortWarnings(value.warnings);
        if (!warnings.isEmpty())
            message += QLatin1Char(' ') + warnings;
        return {message};
    }
    if (result.work == Work::OrganizeExecute) {
        const FlightLogOrganizer::Result &value = result.organizerResult;
        QString message;
        if (value.cancelled) {
            message = tr("Log organization cancelled after %1 completed change(s); %2 remain.")
                .arg(value.completed.size()).arg(value.remaining);
        } else if (!value.success) {
            message = tr("Log organization failed after %1 completed change(s); %2 remain: %3")
                .arg(value.completed.size()).arg(value.remaining).arg(value.error);
        } else {
            message = tr("Log organization completed %1 exact change(s).")
                .arg(value.completed.size());
        }
        const QString warnings = shortWarnings(value.warnings);
        if (!warnings.isEmpty()) message += QLatin1Char(' ') + warnings;
        return {message};
    }
    return {};
}

void DataFlashLogToolsController::finishWorker(
    quint64 flow, const std::shared_ptr<JobState> &state,
    const WorkerResult &result)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Working
        || m_job != state || result.work != m_work)
        return;
    m_progressTimer->stop();
    m_job.reset();
    m_watcher.clear();
    if (!dismissProgress() || flow != m_flow || m_phase != Phase::Working)
        return;

    if (result.work == Work::Analyze) {
        if (state->cancelled.load(std::memory_order_acquire)
            || result.analysis.cancelled) {
            finishFlow(flow, {tr("Auto Analysis cancelled; the source was not changed.")});
        } else if (!result.analysis.success) {
            finishFlow(flow, {tr("Auto Analysis failed: %1").arg(result.analysis.error)});
        } else {
            const QPointer<DataFlashLogToolsController> guard(this);
            showAnalysisReport(result.analysis);
            if (!guard || flow != m_flow || m_phase != Phase::Working)
                return;
            finishFlow(flow, {tr("Auto Analysis completed %1 test(s); this is an offline advisory report.")
                                  .arg(result.analysis.tests.size())});
        }
        return;
    }
    if (result.work == Work::MatlabPrepare) {
        const DataFlashMatlabExporter::PlanResult &value =
            result.matlabPreparation;
        if (state->cancelled.load(std::memory_order_acquire)
            || value.cancelled) {
            finishFlow(flow, {tr(
                "MATLAB export preparation cancelled; no output was written.")});
        } else if (!value.success || !value.plan || !value.plan->isValid()) {
            finishFlow(flow, {tr("MATLAB export preparation failed: %1")
                                  .arg(value.error.isEmpty()
                                      ? tr("the exporter returned an invalid plan")
                                      : value.error)});
        } else if (!m_dialogParent) {
            finishFlow(flow, {tr(
                "MATLAB export stopped because the confirmation window owner disappeared; no output was written.")});
        } else {
            m_matlabPlan = value.plan;
            m_matlabWarnings = value.warnings;
            m_phase = Phase::Consent;
            showMatlabConsent(flow);
        }
        return;
    }
    if (result.work == Work::OrganizeAnalyze) {
        if (state->cancelled.load(std::memory_order_acquire)
            || result.organizerAnalysis.cancelled) {
            finishFlow(flow, {tr("Log directory analysis cancelled; no files were changed.")});
        } else if (!result.organizerAnalysis.success
                   || !result.organizerAnalysis.plan.isValid()) {
            finishFlow(flow, {tr("Log directory analysis failed: %1")
                                  .arg(result.organizerAnalysis.error)});
        } else if (result.organizerAnalysis.plan.entries().isEmpty()) {
            QString message = tr("No changes planned for %1.")
                .arg(result.organizerAnalysis.plan.root());
            const QString warnings = shortWarnings(
                result.organizerAnalysis.plan.warnings());
            if (!warnings.isEmpty()) message += QLatin1Char(' ') + warnings;
            finishFlow(flow, {message});
        } else {
            m_organizerPlan = result.organizerAnalysis.plan;
            m_phase = Phase::Consent;
            showOrganizerConsent(flow);
        }
        return;
    }
    finishFlow(flow, workerMessages(result));
}

void DataFlashLogToolsController::showAnalysisReport(
    const DataFlashLogAnalyzer::Result &result)
{
    if (!m_dialogParent || m_destroying)
        return;
    if (m_report) {
        const QPointer<DataFlashLogToolsController> guard(this);
        const QPointer<QDialog> old(m_report);
        m_report.clear();
        old->hide();
        if (!guard)
            return;
        if (old)
            old->deleteLater();
    }
    auto *dialog = new QDialog(m_dialogParent);
    dialog->setObjectName(QStringLiteral("DataFlashAutoAnalysisDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("DataFlash Log Auto Analysis"));
    dialog->resize(900, 620);
    dialog->setMinimumSize(620, 400);
    auto *layout = new QVBoxLayout(dialog);
    auto *heading = new QLabel(tr(
        "Offline heuristic report for %1. It does not change the vehicle or source log.")
        .arg(m_selectedLog), dialog);
    heading->setObjectName(QStringLiteral("DataFlashAutoAnalysisSummary"));
    heading->setTextFormat(Qt::PlainText);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *report = new QPlainTextEdit(dialog);
    report->setObjectName(QStringLiteral("DataFlashAutoAnalysisReport"));
    report->setReadOnly(true);
    QString text = DataFlashLogAnalyzer::Format(result.tests);
    if (!result.warnings.isEmpty()) {
        text += tr("\n\nWarnings:\n%1")
            .arg(result.warnings.join(QLatin1Char('\n')));
    }
    report->setPlainText(text);
    layout->addWidget(report, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setObjectName(
        QStringLiteral("DataFlashAutoAnalysisCloseButton"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(buttons);
    m_report = dialog;
    connect(dialog, &QObject::destroyed, this, [this, dialog]() {
        if (m_report == dialog)
            m_report.clear();
    });
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QDialog> dialogGuard(dialog);
    dialog->show();
    if (!guard || !dialogGuard)
        return;
    dialog->raise();
    if (!guard || !dialogGuard)
        return;
    dialog->activateWindow();
}

void DataFlashLogToolsController::updateProgress()
{
    const QPointer<DataFlashLogToolsController> guard(this);
    const auto state = m_job;
    const QPointer<QProgressDialog> progress(m_progress);
    if (!state || !progress || m_phase != Phase::Working)
        return;
    if (state->cancelled.load(std::memory_order_acquire)) {
        progress->setLabelText(tr("Cancelling; waiting for the current filesystem/parser step…"));
        return;
    }
    const qint64 total = state->total.load(std::memory_order_acquire);
    const qint64 completed = state->completed.load(std::memory_order_acquire);
    const bool blocked = progress->blockSignals(true);
    if (total > 0) {
        progress->setRange(0, 1000);
        if (!guard || !progress)
            return;
        const qint64 scaled = qBound<qint64>(0, (completed * 1000) / total, 1000);
        progress->setValue(static_cast<int>(scaled));
        if (!guard || !progress)
            return;
    } else {
        progress->setRange(0, 0);
        if (!guard || !progress)
            return;
    }
    if (progress)
        progress->blockSignals(blocked);
}

bool DataFlashLogToolsController::dismissPrompt()
{
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QDialog> dialog(m_prompt);
    m_prompt.clear();
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

bool DataFlashLogToolsController::dismissProgress()
{
    const QPointer<DataFlashLogToolsController> guard(this);
    const QPointer<QProgressDialog> dialog(m_progress);
    m_progress.clear();
    if (!dialog)
        return true;
    const bool blocked = dialog->blockSignals(true);
    dialog->hide();
    if (!guard)
        return false;
    if (dialog) {
        dialog->blockSignals(blocked);
        dialog->deleteLater();
    }
    return true;
}

void DataFlashLogToolsController::cancel()
{
    if (m_destroying || !busy())
        return;
    if (m_phase == Phase::Working && m_job) {
        m_job->cancelled.store(true, std::memory_order_release);
        if (!m_watcher) {
            finishFlow(m_flow, {tr("DataFlash Logs: cancelled before the worker started.")});
            return;
        }
        if (!m_cancelReported) {
            m_cancelReported = true;
            if (!publishLog(tr("DataFlash Logs: cancellation requested; waiting for the active parser/filesystem step.")))
                return;
        }
        const QPointer<QProgressDialog> progress(m_progress);
        if (progress)
            progress->setLabelText(tr("Cancelling; waiting for the current filesystem/parser step…"));
        return;
    }
    const quint64 flow = m_flow;
    finishFlow(flow, {tr("DataFlash Logs: cancelled before background work started.")});
}

void DataFlashLogToolsController::shutdown()
{
    if (m_destroying || m_shutdownPending)
        return;
    m_shutdownPending = true;
    if (!busy()) {
        emit shutdownReady();
        return;
    }
    cancel();
}

bool DataFlashLogToolsController::publishLog(const QString &message)
{
    const QPointer<DataFlashLogToolsController> guard(this);
    setWidgetStatus(message);
    if (!guard)
        return false;
    emit logMessage(message);
    return !guard.isNull();
}

void DataFlashLogToolsController::setWidgetStatus(const QString &message)
{
    if (m_widget)
        m_widget->setStatusText(message);
}

void DataFlashLogToolsController::finishFlow(
    quint64 flow, const QStringList &messages)
{
    if (m_destroying || flow != m_flow)
        return;
    const QPointer<DataFlashLogToolsController> guard(this);
    if (!dismissPrompt() || !guard || flow != m_flow)
        return;
    if (!dismissProgress() || !guard || flow != m_flow)
        return;
    if (m_progressTimer)
        m_progressTimer->stop();
    m_job.reset();
    m_watcher.clear();
    m_phase = Phase::Idle;
    m_intent = Intent::None;
    m_work = Work::None;
    m_outputPath.clear();
    m_mapOutputs.clear();
    m_mapParentCanonical.clear();
    m_organizerRoot.clear();
    m_organizerPlan = FlightLogOrganizer::Plan();
    m_matlabPlan.reset();
    m_matlabWarnings.clear();
    if (m_widget)
        m_widget->setOperationBusy(false);
    if (!guard)
        return;
    emit busyChanged(false);
    if (!guard || flow != m_flow || m_phase != Phase::Idle)
        return;
    for (const QString &message : messages) {
        if (!publishLog(message) || flow != m_flow || m_phase != Phase::Idle)
            return;
    }
    if (m_shutdownPending)
        emit shutdownReady();
}
