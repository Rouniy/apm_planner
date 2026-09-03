#include "MavlinkLogWindow.h"

#include "comm/TlogExportService.h"
#include "configuration.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QThread>

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
    const QString matlabReason = tr(
        "Matlab export is not ported yet because a verified cross-platform "
        "MAT-file writer is not available.");
    matlab->setEnabled(false);
    matlab->setToolTip(matlabReason);
    matlab->setStatusTip(matlabReason);
    matlab->setWhatsThis(matlabReason);
    matlab->setProperty("unavailableReason", matlabReason);
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
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tlogConvertStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: #99AADD;"));
    root->addWidget(m_status, 5, 0);

    connect(m_pick, &QPushButton::clicked, this, &MavlinkLogWindow::pickTlog);
}

void MavlinkLogWindow::setTlogPath(const QString &path)
{
    if (m_busy) {
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
    if (m_busy) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select telemetry log"), QGC::MAVLinkLogDirectory(),
        tr("Telemetry log (*.tlog)"));
    if (!path.isEmpty()) {
        setTlogPath(path);
    }
}

void MavlinkLogWindow::beginExport(Operation operation)
{
    if (m_busy || m_tlogPath.isEmpty()) {
        return;
    }
    const QString label = operationLabel(operation);
    if (!confirmSensitiveExport(label)) {
        m_status->setText(tr("%1 export cancelled.").arg(label));
        return;
    }
    const QString output = chooseOutput(operation);
    if (output.isEmpty()) {
        return;
    }
    if (comparablePath(output) == comparablePath(m_tlogPath)) {
        m_status->setText(tr("The export destination must not replace the selected .tlog."));
        return;
    }

    TlogExportFormat format = TlogExportFormat::Kml;
    switch (operation) {
    case Operation::Kml: format = TlogExportFormat::Kml; break;
    case Operation::Gpx: format = TlogExportFormat::Gpx; break;
    case Operation::Csv: format = TlogExportFormat::Csv; break;
    case Operation::Text: format = TlogExportFormat::Text; break;
    case Operation::Parameters: format = TlogExportFormat::Parameters; break;
    case Operation::Missions: format = TlogExportFormat::Missions; break;
    }

    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelFlag = m_cancelFlag;
    const QString input = m_tlogPath;
    setBusy(true);
    m_status->setText(
        (operation == Operation::Kml || operation == Operation::Gpx)
            ? tr("Converting to %1…").arg(label)
            : tr("Exporting %1…").arg(label));

    auto exporter = m_dependencies.exportLog;
    if (!exporter) {
        exporter = [](TlogExportFormat requestedFormat,
                      const QString &requestedInput,
                      const QString &requestedOutput,
                      const TlogExportService::CancelRequested &cancel) {
            return TlogExportService::Export(
                requestedFormat, requestedInput, requestedOutput, cancel);
        };
    }
    const auto result = std::make_shared<TlogExportResult>();
    QThread *thread = QThread::create(
        [cancelFlag, format, input, output, exporter, result]() {
            *result = exporter(
                format, input, output,
                [cancelFlag]() { return cancelFlag->load(); });
        });
    m_thread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, result, label]() {
                if (m_thread == thread) {
                    m_thread = nullptr;
                }
                finishExport(*result, label);
            });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MavlinkLogWindow::finishExport(const TlogExportResult &result,
                                    const QString &label)
{
    setBusy(false);
    m_cancelFlag.reset();
    if (result.cancelled) {
        m_status->setText(tr("%1 export cancelled.").arg(label));
        return;
    }
    if (!result.success) {
        const bool track = label == QStringLiteral("KML")
            || label == QStringLiteral("GPX");
        m_status->setText(
            track ? tr("%1 conversion failed: %2").arg(label, result.error)
                  : tr("%1 export failed: %2").arg(label, result.error));
        return;
    }

    m_status->setText(result.message);
}

void MavlinkLogWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_pick->setEnabled(!busy);
    const bool canExport = !busy && !m_tlogPath.isEmpty();
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

bool MavlinkLogWindow::confirmSensitiveExport(const QString &label)
{
    if (m_dependencies.confirmExport) {
        return m_dependencies.confirmExport(label);
    }
    QMessageBox box(
        QMessageBox::Warning, tr("Export %1").arg(label),
        tr("Exported vehicle data can contain precise GPS coordinates, vehicle "
           "identifiers, missions, network details and sensitive parameter "
           "values. Save the file only to a trusted location and review it "
           "before sharing. Cancel is the default action."),
        QMessageBox::NoButton, this);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    QPushButton *accept = box.addButton(tr("EXPORT FILE"),
                                        QMessageBox::AcceptRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == accept;
}

QString MavlinkLogWindow::chooseOutput(Operation operation)
{
    const QFileInfo input(m_tlogPath);
    const QString extension = operationExtension(operation);
    const QString suggested = input.dir().filePath(
        input.completeBaseName() + QLatin1Char('.') + extension);
    if (m_dependencies.chooseOutput) {
        return m_dependencies.chooseOutput(
            suggested, operationLabel(operation), extension);
    }
    QFileDialog dialog(
        this, tr("Save converted log"), suggested,
        tr("%1 files (*.%2)").arg(operationLabel(operation), extension));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setDefaultSuffix(extension);
    if (dialog.exec() != QDialog::Accepted) {
        return QString();
    }
    return dialog.selectedFiles().value(0);
}

QString MavlinkLogWindow::operationLabel(Operation operation) const
{
    switch (operation) {
    case Operation::Kml: return QStringLiteral("KML");
    case Operation::Gpx: return QStringLiteral("GPX");
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
    case Operation::Csv: return QStringLiteral("csv");
    case Operation::Text: return QStringLiteral("txt");
    case Operation::Parameters: return QStringLiteral("param");
    case Operation::Missions: return QStringLiteral("waypoints");
    }
    return QString();
}
