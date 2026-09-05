#include "AnonLogWindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

QString normalizedAbsolutePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return QDir::cleanPath(canonical);
    }
    const QFileInfo parent(info.absolutePath());
    const QString canonicalParent = parent.canonicalFilePath();
    if (!canonicalParent.isEmpty()) {
        return QDir(canonicalParent).absoluteFilePath(info.fileName());
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

QString privacyReminder()
{
    return LogAnonymizer::privacyWarning();
}

} // namespace

AnonLogWindow::AnonLogWindow(
    LogAnonymizeService *service, QWidget *owner,
    ConfirmationCallback confirm)
    : QWidget(owner, Qt::Window)
    , m_service(service)
    , m_confirm(confirm ? std::move(confirm) : defaultConfirmation)
{
    setObjectName(QStringLiteral("AnonLogWindow"));
    setWindowTitle(tr("Anon Log (Beta)"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(760, 740);
    setMinimumSize(640, 640);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *title = new QLabel(tr("Anon Log"), this);
    title->setObjectName(QStringLiteral("AnonLogTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *warning = new QLabel(
        tr("<b>Beta / privacy warning:</b> %1")
            .arg(LogAnonymizer::privacyWarning().toHtmlEscaped()),
        this);
    warning->setObjectName(QStringLiteral("AnonLogPrivacyWarning"));
    warning->setWordWrap(true);
    warning->setTextFormat(Qt::RichText);
    warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(warning);

    auto *files = new QGroupBox(tr("Files"), this);
    auto *filesLayout = new QFormLayout(files);

    auto *inputRow = new QWidget(files);
    auto *inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    m_input = new QLineEdit(inputRow);
    m_input->setObjectName(QStringLiteral("AnonLogInputEdit"));
    m_input->setPlaceholderText(tr("Select a .bin, .log, or .tlog file"));
    m_browseInput = new QPushButton(tr("Browse…"), inputRow);
    m_browseInput->setObjectName(QStringLiteral("BrowseAnonLogInputButton"));
    inputLayout->addWidget(m_input, 1);
    inputLayout->addWidget(m_browseInput);
    filesLayout->addRow(tr("Input log:"), inputRow);

    auto *outputRow = new QWidget(files);
    auto *outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    m_output = new QLineEdit(outputRow);
    m_output->setObjectName(QStringLiteral("AnonLogOutputEdit"));
    m_output->setPlaceholderText(tr("Choose where to save the anonymized log"));
    m_browseOutput = new QPushButton(tr("Browse…"), outputRow);
    m_browseOutput->setObjectName(QStringLiteral("BrowseAnonLogOutputButton"));
    outputLayout->addWidget(m_output, 1);
    outputLayout->addWidget(m_browseOutput);
    filesLayout->addRow(tr("Output log:"), outputRow);
    root->addWidget(files);

    auto *offsets = new QGroupBox(tr("Coordinate offsets"), this);
    auto *offsetsLayout = new QFormLayout(offsets);
    m_latitude = new QLineEdit(offsets);
    m_latitude->setObjectName(QStringLiteral("AnonLogLatitudeOffsetEdit"));
    m_latitude->setPlaceholderText(tr("Blank = random"));
    m_longitude = new QLineEdit(offsets);
    m_longitude->setObjectName(QStringLiteral("AnonLogLongitudeOffsetEdit"));
    m_longitude->setPlaceholderText(tr("Blank = random"));
    offsetsLayout->addRow(tr("Latitude (degrees):"), m_latitude);
    offsetsLayout->addRow(tr("Longitude (degrees):"), m_longitude);
    auto *offsetHelp = new QLabel(
        tr("Offsets use '.' as the decimal separator. Leaving a value blank "
           "chooses a new random offset for this run."), offsets);
    offsetHelp->setObjectName(QStringLiteral("AnonLogOffsetHelp"));
    offsetHelp->setWordWrap(true);
    offsetsLayout->addRow(QString(), offsetHelp);
    root->addWidget(offsets);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("AnonLogStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("AnonLogProgress"));
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto *progressHelp = new QLabel(
        tr("Progress includes source verification before and after the "
           "transformation, so a complete run reads the input in multiple "
           "passes."), this);
    progressHelp->setObjectName(QStringLiteral("AnonLogProgressHelp"));
    progressHelp->setWordWrap(true);
    root->addWidget(progressHelp);

    m_result = new QPlainTextEdit(this);
    m_result->setObjectName(QStringLiteral("AnonLogResult"));
    m_result->setReadOnly(true);
    m_result->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_result->setPlaceholderText(
        tr("Result counts and format-specific warnings appear here."));
    m_result->setMinimumHeight(110);
    // The application input stylesheet sets a 28px minimum on text edits.
    // Override that token locally so the result cannot collapse to one line.
    m_result->setStyleSheet(QStringLiteral("QPlainTextEdit#AnonLogResult { min-height: 110px; }"));
    root->addWidget(m_result, 1);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_start = new QPushButton(tr("Start"), this);
    m_start->setObjectName(QStringLiteral("StartAnonLogButton"));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("CancelAnonLogButton"));
    m_close = new QPushButton(tr("Close"), this);
    m_close->setObjectName(QStringLiteral("CloseAnonLogButton"));
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_close);
    root->addLayout(buttons);

    connect(m_browseInput, &QPushButton::clicked,
            this, &AnonLogWindow::chooseInput);
    connect(m_browseOutput, &QPushButton::clicked,
            this, &AnonLogWindow::chooseOutput);
    connect(m_start, &QPushButton::clicked,
            this, &AnonLogWindow::requestStart);
    connect(m_cancel, &QPushButton::clicked,
            this, &AnonLogWindow::requestCancel);
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);

    if (m_service) {
        connect(m_service, &LogAnonymizeService::changed,
                this, &AnonLogWindow::syncFromService);
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
            m_localStatus = tr("Log anonymization service unavailable.");
            syncButtons();
            m_status->setText(m_localStatus);
        });
    }
    syncFromService();
}

AnonLogWindow::~AnonLogWindow()
{
    if (m_service) {
        disconnect(m_service.data(), nullptr, this, nullptr);
    }
}

void AnonLogWindow::closeEvent(QCloseEvent *event)
{
    // The worker belongs to the application. Closing an observer is not an
    // implicit cancellation request.
    QWidget::closeEvent(event);
}

void AnonLogWindow::chooseInput()
{
    QPointer<AnonLogWindow> guardedThis(this);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select a log to anonymize"), m_input->text(), fileFilter());
    if (!guardedThis || path.isEmpty()) {
        return;
    }
    m_input->setText(QDir::toNativeSeparators(path));
    if (m_output->text().trimmed().isEmpty()) {
        m_output->setText(QDir::toNativeSeparators(suggestedOutputPath(path)));
    }
    m_localStatus.clear();
    syncButtons();
}

void AnonLogWindow::chooseOutput()
{
    const QString inputAtOpen = m_input->text();
    QString suggested = m_output->text();
    if (suggested.trimmed().isEmpty()) {
        suggested = suggestedOutputPath(inputAtOpen);
    }

    QPointer<AnonLogWindow> guardedThis(this);
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save anonymized log"), suggested, fileFilter());
    if (!guardedThis || path.isEmpty()) {
        return;
    }
    const QString inputSuffix = QFileInfo(inputAtOpen).suffix();
    if (QFileInfo(path).suffix().isEmpty() && !inputSuffix.isEmpty()) {
        path += QLatin1Char('.') + inputSuffix;
    }
    m_output->setText(QDir::toNativeSeparators(path));
    m_localStatus.clear();
    syncButtons();
}

void AnonLogWindow::requestStart()
{
    QPointer<AnonLogWindow> guardedThis(this);
    QPointer<LogAnonymizeService> guardedService(m_service);
    if (!guardedService) {
        showLocalError(tr("Log anonymization service unavailable."));
        return;
    }
    if (guardedService->busy()) {
        showLocalError(tr("A log anonymization job is already running."));
        return;
    }

    Request request;
    QString error;
    if (!collectRequest(&request, &error)) {
        showLocalError(error);
        return;
    }

    const QString title = tr("Confirm Anon Log (Beta)");
    const QString message = tr(
        "Anonymize:\n%1\n\nSave as:\n%2\n\nCoordinate offsets: "
        "%3° latitude, %4° longitude.\n\nIf the output already exists, "
        "it will be atomically replaced only after processing and verification "
        "succeed.\n\n%5\n\nContinue?")
            .arg(QDir::toNativeSeparators(request.inputPath),
                 QDir::toNativeSeparators(request.outputPath),
                 offsetText(request.options.latitudeOffset),
                 offsetText(request.options.longitudeOffset),
                 privacyReminder());
    const ConfirmationCallback confirm = m_confirm;
    if (!confirm || !confirm(this, title, message)
        || !guardedThis || !guardedService) {
        return;
    }

    // A confirmation callback is a nested public event boundary. Never start
    // different paths/options or join a job that appeared while it was open.
    if (guardedService != m_service || guardedService->busy()) {
        showLocalError(tr(
            "The anonymization service changed while confirmation was open. "
            "Review the current job before trying again."));
        return;
    }
    if (!fieldsStillMatch(request)) {
        showLocalError(tr(
            "The selected files or offsets changed while confirmation was "
            "open. Review them and press Start again."));
        return;
    }
    Request revalidated;
    if (!collectRequest(&revalidated, &error)
        || revalidated.inputPath != request.inputPath
        || revalidated.outputPath != request.outputPath
        || (!request.latitudeText.trimmed().isEmpty()
            && revalidated.options.latitudeOffset
                != request.options.latitudeOffset)
        || (!request.longitudeText.trimmed().isEmpty()
            && revalidated.options.longitudeOffset
                != request.options.longitudeOffset)) {
        showLocalError(error.isEmpty()
                           ? tr("The selected request is no longer valid.")
                           : error);
        return;
    }

    m_localStatus.clear();
    const bool started = guardedService->start(
        request.inputPath, request.outputPath, request.options, &error);
    if (!guardedThis || !guardedService) {
        return;
    }
    if (!started) {
        showLocalError(error.isEmpty()
                           ? tr("The log anonymization job could not start.")
                           : error);
        return;
    }

    m_observedToken = guardedService->token();
    syncFromService();
}

void AnonLogWindow::requestCancel()
{
    if (!m_service || !m_service->busy() || m_observedToken == 0
        || m_service->token() != m_observedToken) {
        syncButtons();
        return;
    }
    QPointer<AnonLogWindow> guardedThis(this);
    QPointer<LogAnonymizeService> guardedService(m_service);
    const quint64 token = m_observedToken;
    guardedService->cancel(token);
    // cancel() emits a public signal synchronously. A listener may close this
    // observer or destroy the application service at that boundary.
    if (!guardedThis || !guardedService) {
        return;
    }
    syncFromService();
}

void AnonLogWindow::syncFromService()
{
    if (!m_service) {
        syncButtons();
        m_status->setText(tr("Log anonymization service unavailable."));
        return;
    }

    const quint64 token = m_service->token();
    if (token != 0 && token != m_observedToken) {
        adoptCurrentJob(token);
    }

    const bool active = m_service->busy();
    const qint64 processed = std::max<qint64>(0, m_service->bytesProcessed());
    const qint64 total = std::max<qint64>(0, m_service->bytesTotal());
    if (active && total > 0) {
        m_progress->setRange(0, 1000);
        const int scaled = processed >= total
            ? 1000
            : static_cast<int>(
                (static_cast<double>(processed)
                 / static_cast<double>(total)) * 1000.0);
        m_progress->setValue(std::max(0, std::min(scaled, 1000)));
    } else if (active) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 1000);
        const LogAnonymizeResult result = m_service->result();
        m_progress->setValue(result.success ? 1000 : 0);
    }

    if (!m_localStatus.isEmpty()) {
        m_status->setText(m_localStatus);
    } else if (active) {
        m_status->setText(m_service->cancellationRequested()
            ? tr("Cancellation requested — finishing the current safe step…")
            : (total > 0
                ? tr("Processing / verification… %1 of %2")
                      .arg(formatBytes(processed), formatBytes(total))
                : tr("Processing / verification…")));
    } else if (token == 0) {
        m_status->setText(tr("Ready"));
    } else {
        const LogAnonymizeResult result = m_service->result();
        m_status->setText(result.success
            ? tr("Completed")
            : (result.cancelled ? tr("Cancelled") : tr("Failed")));
    }

    if (token != 0) {
        const LogAnonymizeResult result = m_service->result();
        QStringList details;
        details << tr("Input: %1").arg(
                       QDir::toNativeSeparators(m_service->inputPath()))
                << tr("Output: %1").arg(
                       QDir::toNativeSeparators(m_service->outputPath()))
                << tr("Offsets: %1° latitude, %2° longitude")
                       .arg(offsetText(m_service->options().latitudeOffset),
                            offsetText(m_service->options().longitudeOffset));
        if (!active) {
            if (!result.error.trimmed().isEmpty()) {
                details << tr("Error: %1").arg(result.error.trimmed());
            }
            details << tr("Input bytes: %1").arg(result.inputBytes)
                    << tr("Output bytes: %1").arg(result.outputBytes)
                    << tr("Records: %1").arg(result.records)
                    << tr("Coordinate fields: %1").arg(result.coordinateFields)
                    << tr("Patched values: %1").arg(result.patchedValues)
                    << tr("Stripped MAVLink signatures: %1")
                           .arg(result.strippedSignatures);
            if (!result.warnings.isEmpty()) {
                details << QString() << tr("Warnings:");
                for (const QString &warning : result.warnings) {
                    details << QStringLiteral("• ") + warning;
                }
            }
            details << QString() << privacyReminder();
        }
        m_result->setPlainText(details.join(QLatin1Char('\n')));
    }

    syncButtons();
}

void AnonLogWindow::syncButtons()
{
    const bool available = bool(m_service);
    const bool active = available && m_service->busy();
    const bool ownsActive = active && m_observedToken != 0
        && m_service->token() == m_observedToken;
    m_input->setEnabled(available && !active);
    m_output->setEnabled(available && !active);
    m_latitude->setEnabled(available && !active);
    m_longitude->setEnabled(available && !active);
    m_browseInput->setEnabled(available && !active);
    m_browseOutput->setEnabled(available && !active);
    m_start->setEnabled(available && !active);
    m_cancel->setEnabled(available && ownsActive);
    m_close->setEnabled(true);
}

void AnonLogWindow::adoptCurrentJob(quint64 token)
{
    if (!m_service || token == 0 || m_service->token() != token) {
        return;
    }
    m_observedToken = token;
    m_localStatus.clear();
    m_input->setText(QDir::toNativeSeparators(m_service->inputPath()));
    m_output->setText(QDir::toNativeSeparators(m_service->outputPath()));
    m_latitude->setText(offsetText(m_service->options().latitudeOffset));
    m_longitude->setText(offsetText(m_service->options().longitudeOffset));
    m_result->clear();
}

bool AnonLogWindow::collectRequest(Request *request, QString *error) const
{
    if (!request) {
        return false;
    }
    request->inputPath = QDir::fromNativeSeparators(m_input->text());
    request->outputPath = QDir::fromNativeSeparators(m_output->text());
    request->latitudeText = m_latitude->text();
    request->longitudeText = m_longitude->text();

    if (request->inputPath.trimmed().isEmpty()) {
        if (error) {
            *error = tr("Select an input log first.");
        }
        return false;
    }
    const QFileInfo inputInfo(request->inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile() || !inputInfo.isReadable()) {
        if (error) {
            *error = tr("The selected input log is not a readable file.");
        }
        return false;
    }
    if (request->outputPath.trimmed().isEmpty()) {
        if (error) {
            *error = tr("Choose an output log first.");
        }
        return false;
    }

    LogAnonymizer::Format inputFormat;
    LogAnonymizer::Format outputFormat;
    if (!LogAnonymizer::formatForPath(request->inputPath, &inputFormat)) {
        if (error) {
            *error = tr("Input must be a .bin, .log, or .tlog file.");
        }
        return false;
    }
    if (!LogAnonymizer::formatForPath(request->outputPath, &outputFormat)
        || inputFormat != outputFormat) {
        if (error) {
            *error = tr(
                "Output must keep the input log format (.bin, .log, or .tlog).");
        }
        return false;
    }
    if (normalizedAbsolutePath(request->inputPath)
        == normalizedAbsolutePath(request->outputPath)) {
        if (error) {
            *error = tr("Input and output must be different files.");
        }
        return false;
    }

    if (!LogAnonymizer::parseOffset(
            request->latitudeText, &request->options.latitudeOffset)
        || !LogAnonymizer::parseOffset(
            request->longitudeText, &request->options.longitudeOffset)
        || !std::isfinite(request->options.latitudeOffset)
        || !std::isfinite(request->options.longitudeOffset)) {
        if (error) {
            *error = tr(
                "Offsets must be blank or finite numbers using '.' as the "
                "decimal separator.");
        }
        return false;
    }
    return true;
}

bool AnonLogWindow::fieldsStillMatch(const Request &request) const
{
    return QDir::fromNativeSeparators(m_input->text()) == request.inputPath
        && QDir::fromNativeSeparators(m_output->text()) == request.outputPath
        && m_latitude->text() == request.latitudeText
        && m_longitude->text() == request.longitudeText;
}

void AnonLogWindow::showLocalError(const QString &error)
{
    m_localStatus = error;
    m_status->setText(error);
    syncButtons();
}

bool AnonLogWindow::defaultConfirmation(
    QWidget *owner, const QString &title, const QString &message)
{
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, title, message,
        QMessageBox::Yes | QMessageBox::Cancel, owner);
    dialog->setObjectName(QStringLiteral("AnonLogConfirmation"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    QPointer<QMessageBox> guardedDialog(dialog);
    const int answer = dialog->exec();
    const bool accepted = guardedDialog && answer == QMessageBox::Yes;
    if (guardedDialog) {
        delete guardedDialog.data();
    }
    return accepted;
}

QString AnonLogWindow::fileFilter()
{
    return tr("Flight logs (*.bin *.log *.tlog)");
}

QString AnonLogWindow::suggestedOutputPath(const QString &inputPath)
{
    const QFileInfo input(inputPath);
    if (input.fileName().isEmpty()) {
        return QString();
    }
    const QString suffix = input.completeSuffix();
    QString name = input.completeBaseName() + QStringLiteral("-anon");
    if (!suffix.isEmpty()) {
        name += QLatin1Char('.') + suffix;
    }
    return input.dir().absoluteFilePath(name);
}

QString AnonLogWindow::formatBytes(qint64 bytes)
{
    if (bytes < 1024) {
        return tr("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return tr("%1 KiB").arg(QString::number(bytes / 1024.0, 'f', 1));
    }
    return tr("%1 MiB").arg(
        QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
}

QString AnonLogWindow::offsetText(double value)
{
    return QString::number(value, 'f', 6);
}
