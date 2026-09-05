#include "ParameterMetaDataRegenerationWindow.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDialog>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <utility>

namespace {

QString refreshNotice()
{
    return QObject::tr(
        "Existing parameter pages keep their current metadata snapshots. "
        "Newly created pages use newly published metadata. To refresh "
        "already-created pages, save or apply pending edits and finish "
        "current operations before reconnecting or restarting the "
        "application.");
}

QString artifactKindText(
    ParameterMetaDataRegenerationService::ArtifactKind kind)
{
    switch (kind) {
    case ParameterMetaDataRegenerationService::ArtifactKind::GeneratedSource:
        return QObject::tr("Generated source metadata");
    case ParameterMetaDataRegenerationService::ArtifactKind::PreparedPdef:
        return QObject::tr("Prepared PDEF");
    }
    return QObject::tr("Metadata artifact");
}

} // namespace

ParameterMetaDataRegenerationWindow::ParameterMetaDataRegenerationWindow(
    ParameterMetaDataRegenerationService *service, QWidget *owner,
    ConfirmationCallback confirm)
    : QWidget(owner, Qt::Window)
    , m_service(service)
    , m_confirm(confirm ? std::move(confirm) : defaultConfirmation)
{
    setObjectName(QStringLiteral("ParameterMetaDataRegenerationWindow"));
    setWindowTitle(tr("Parameter Metadata Generator"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(760, 560);
    setMinimumSize(560, 400);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(9);

    auto *title = new QLabel(tr("Parameter Metadata Generator"), this);
    title->setObjectName(QStringLiteral("ParameterMetadataTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *intro = new QLabel(
        tr("Downloads the current master and stable ArduPilot parameter "
           "definitions, rebuilds generated source metadata, and refreshes "
           "prepared parameter catalogs. The operation does not write any "
           "vehicle parameters."),
        this);
    intro->setObjectName(QStringLiteral("ParameterMetadataIntroduction"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_phase = new QLabel(this);
    m_phase->setObjectName(QStringLiteral("ParameterMetadataPhase"));
    m_phase->setWordWrap(true);
    root->addWidget(m_phase);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("ParameterMetadataProgress"));
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    m_artifacts = new QTableWidget(this);
    m_artifacts->setObjectName(
        QStringLiteral("ParameterMetadataArtifacts"));
    m_artifacts->setColumnCount(4);
    m_artifacts->setHorizontalHeaderLabels(
        {tr("Artifact"), tr("Source"), tr("Status"), tr("Size")});
    m_artifacts->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_artifacts->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_artifacts->setSelectionMode(QAbstractItemView::SingleSelection);
    m_artifacts->setShowGrid(false);
    m_artifacts->verticalHeader()->hide();
    m_artifacts->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_artifacts->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_artifacts->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    m_artifacts->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);
    root->addWidget(m_artifacts, 1);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("ParameterMetadataLog"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_log->document()->setMaximumBlockCount(MaximumVisibleLogLines);
    m_log->setPlaceholderText(tr("Download and generation details appear here."));
    m_log->setMinimumHeight(110);
    root->addWidget(m_log, 1);

    m_result = new QLabel(this);
    m_result->setObjectName(QStringLiteral("ParameterMetadataResult"));
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_result);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_regenerate = new QPushButton(tr("Regenerate"), this);
    m_regenerate->setObjectName(
        QStringLiteral("RegenerateMetadataButton"));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("CancelMetadataButton"));
    m_close = new QPushButton(tr("Close"), this);
    m_close->setObjectName(QStringLiteral("CloseMetadataButton"));
    buttons->addWidget(m_regenerate);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_close);
    root->addLayout(buttons);

    connect(m_regenerate, &QPushButton::clicked,
            this, &ParameterMetaDataRegenerationWindow::requestStart);
    connect(m_cancel, &QPushButton::clicked,
            this, &ParameterMetaDataRegenerationWindow::requestCancel);
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);

    if (m_service) {
        connect(m_service,
                &ParameterMetaDataRegenerationService::progress,
                this,
                [this](
                    ParameterMetaDataRegenerationService::RunToken token,
                    ParameterMetaDataRegenerationService::Phase phase,
                    int completed, int total, const QString &label) {
            if (!m_service) {
                return;
            }
            if (token != m_observedToken) {
                const auto snapshot = m_service->snapshot();
                if (snapshot.token != token) {
                    return;
                }
                adoptToken(token, true);
            }
            showProgress(phase, completed, total, label);
            syncButtons();
        });
        connect(m_service,
                &ParameterMetaDataRegenerationService::logLine,
                this,
                [this](
                    ParameterMetaDataRegenerationService::RunToken token,
                    const QString &line) {
            if (!m_service) {
                return;
            }
            if (token != m_observedToken) {
                const auto snapshot = m_service->snapshot();
                if (snapshot.token != token) {
                    return;
                }
                adoptToken(token, true);
            }
            appendLogBounded(line);
        });
        connect(m_service,
                &ParameterMetaDataRegenerationService::publication,
                this,
                [this](
                    ParameterMetaDataRegenerationService::RunToken token,
                    const ParameterMetaDataRegenerationService::ArtifactReport
                        &artifact) {
            if (!m_service || token != m_observedToken) {
                return;
            }
            updateArtifact(artifact);
        });
        connect(m_service,
                &ParameterMetaDataRegenerationService::finished,
                this,
                [this](
                    const ParameterMetaDataRegenerationService::Result
                        &result) {
            if (!m_service || result.token != m_observedToken) {
                return;
            }
            showResult(result);
            syncButtons();
        });
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
            m_regenerate->setEnabled(false);
            m_cancel->setEnabled(false);
            m_phase->setText(tr("Parameter metadata service unavailable."));
        });
    }

    syncFromSnapshot();
}

ParameterMetaDataRegenerationWindow::~ParameterMetaDataRegenerationWindow()
{
    if (m_service) {
        disconnect(m_service.data(), nullptr, this, nullptr);
    }
}

ParameterMetaDataRegenerationService *
ParameterMetaDataRegenerationWindow::service() const
{
    return m_service.data();
}

void ParameterMetaDataRegenerationWindow::closeEvent(QCloseEvent *event)
{
    // The operation belongs to the application service.  Closing an observer
    // must not turn into an implicit cross-window cancellation.
    QWidget::closeEvent(event);
}

void ParameterMetaDataRegenerationWindow::requestStart()
{
    QPointer<ParameterMetaDataRegenerationWindow> guardedThis(this);
    QPointer<ParameterMetaDataRegenerationService> guardedService(m_service);
    if (!guardedService || guardedService->busy()) {
        syncButtons();
        return;
    }

    const ConfirmationCallback confirm = m_confirm;
    const QString title = tr("Regenerate Parameter Metadata");
    const QString message = tr(
        "Download current master/stable ArduPilot parameter definitions and "
        "rebuild the local metadata cache?");
    if (!confirm || !confirm(this, title, message)
        || !guardedThis || !guardedService) {
        return;
    }

    ParameterMetaDataRegenerationService::RunToken token;
    QString error;
    const auto startResult = guardedService->start(&token, &error);
    if (!guardedThis || !guardedService) {
        return;
    }
    if (startResult
        == ParameterMetaDataRegenerationService::StartResult::Started) {
        adoptToken(token, true);
        syncFromSnapshot();
        return;
    }

    if (error.isEmpty()) {
        error = startResult
                == ParameterMetaDataRegenerationService::StartResult::Busy
            ? tr("Parameter metadata regeneration is already running.")
            : tr("The parameter metadata cache directory is unavailable.");
    }
    m_result->setText(error);
    syncButtons();
}

void ParameterMetaDataRegenerationWindow::requestCancel()
{
    if (!m_service || !m_observedToken.isValid()) {
        syncButtons();
        return;
    }
    const auto snapshot = m_service->snapshot();
    if (!snapshot.active || snapshot.token != m_observedToken) {
        syncButtons();
        return;
    }
    if (m_service->cancel(m_observedToken)) {
        // cancel() retires the token synchronously, while public terminal
        // signals are deliberately queued so a handler cannot corrupt a
        // successor run. Reflect that immutable snapshot immediately.
        syncFromSnapshot();
        return;
    } else {
        m_result->setText(
            tr("The active metadata operation could not be cancelled."));
    }
    syncButtons();
}

void ParameterMetaDataRegenerationWindow::syncFromSnapshot()
{
    if (!m_service) {
        m_regenerate->setEnabled(false);
        m_cancel->setEnabled(false);
        m_phase->setText(tr("Parameter metadata service unavailable."));
        m_result->setText(refreshNotice());
        return;
    }

    const auto snapshot = m_service->snapshot();
    const bool newToken = snapshot.token != m_observedToken;
    adoptToken(snapshot.token, newToken);
    replaceLog(snapshot.logLines);
    rebuildArtifacts(snapshot.artifacts);
    showProgress(snapshot.phase, snapshot.completedUnits,
                 snapshot.totalUnits, QString());
    if (snapshot.terminal) {
        ParameterMetaDataRegenerationService::Result result;
        result.token = snapshot.token;
        result.outcome = snapshot.outcome;
        result.artifacts = snapshot.artifacts;
        result.logLines = snapshot.logLines;
        showResult(result);
    } else if (!snapshot.active
        && snapshot.phase
            == ParameterMetaDataRegenerationService::Phase::Finished
        && m_result->text().isEmpty()) {
        m_result->setText(
            tr("The last metadata operation has finished. Review the "
               "published artifacts and log above.\n%1")
                .arg(refreshNotice()));
    } else if (!snapshot.active && m_result->text().isEmpty()) {
        m_result->setText(refreshNotice());
    }
    syncButtons();
}

void ParameterMetaDataRegenerationWindow::adoptToken(
    const ParameterMetaDataRegenerationService::RunToken &token,
    bool clearPresentation)
{
    m_observedToken = token;
    if (!clearPresentation) {
        return;
    }
    m_artifacts->setRowCount(0);
    m_log->clear();
    m_result->clear();
}

void ParameterMetaDataRegenerationWindow::showProgress(
    ParameterMetaDataRegenerationService::Phase phase,
    int completed, int total, const QString &label)
{
    if (total > 0) {
        m_progress->setRange(0, total);
        m_progress->setValue(std::max(0, std::min(completed, total)));
    } else if (m_service && m_service->busy()) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
    }
    QString text = phaseText(phase);
    if (!label.trimmed().isEmpty()) {
        text += QStringLiteral(" — ") + label.trimmed();
    }
    if (total > 0) {
        text += tr(" (%1 of %2)").arg(completed).arg(total);
    }
    m_phase->setText(text);
}

void ParameterMetaDataRegenerationWindow::rebuildArtifacts(
    const QList<ParameterMetaDataRegenerationService::ArtifactReport>
        &artifacts)
{
    m_artifacts->setRowCount(0);
    for (const auto &artifact : artifacts) {
        updateArtifact(artifact);
    }
}

void ParameterMetaDataRegenerationWindow::updateArtifact(
    const ParameterMetaDataRegenerationService::ArtifactReport &artifact)
{
    const QString key = artifactKey(artifact);
    int row = -1;
    for (int index = 0; index < m_artifacts->rowCount(); ++index) {
        QTableWidgetItem *const item = m_artifacts->item(index, 0);
        if (item && item->data(Qt::UserRole).toString() == key) {
            row = index;
            break;
        }
    }
    if (row < 0) {
        row = m_artifacts->rowCount();
        m_artifacts->insertRow(row);
    }

    auto *name = new QTableWidgetItem(
        tr("%1: %2").arg(artifactKindText(artifact.kind), artifact.key));
    name->setData(Qt::UserRole, key);
    auto *source = new QTableWidgetItem(artifact.sourceUrl.toString());
    auto *status = new QTableWidgetItem(
        artifact.published ? tr("Published")
                           : (artifact.error.isEmpty()
                                  ? tr("Not published")
                                  : tr("Failed: %1").arg(artifact.error)));
    auto *size = new QTableWidgetItem(
        artifact.byteCount > 0
            ? tr("%1 KiB").arg(
                  QString::number(artifact.byteCount / 1024.0, 'f', 1))
            : QStringLiteral("—"));
    size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_artifacts->setItem(row, 0, name);
    m_artifacts->setItem(row, 1, source);
    m_artifacts->setItem(row, 2, status);
    m_artifacts->setItem(row, 3, size);
}

void ParameterMetaDataRegenerationWindow::replaceLog(
    const QStringList &lines)
{
    const int first = std::max(0, lines.size() - MaximumVisibleLogLines);
    m_log->setPlainText(lines.mid(first).join(QLatin1Char('\n')));
}

void ParameterMetaDataRegenerationWindow::appendLogBounded(
    const QString &line)
{
    if (!line.isEmpty()) {
        m_log->appendPlainText(line);
    }
}

void ParameterMetaDataRegenerationWindow::showResult(
    const ParameterMetaDataRegenerationService::Result &result)
{
    rebuildArtifacts(result.artifacts);
    replaceLog(result.logLines);
    if (m_service) {
        const auto snapshot = m_service->snapshot();
        if (snapshot.token == result.token) {
            showProgress(snapshot.phase, snapshot.completedUnits,
                         snapshot.totalUnits, QString());
        }
    }

    int published = 0;
    int failures = 0;
    for (const auto &artifact : result.artifacts) {
        published += artifact.published ? 1 : 0;
        failures += !artifact.error.isEmpty() ? 1 : 0;
    }

    QString summary;
    switch (result.outcome) {
    case ParameterMetaDataRegenerationService::Outcome::Complete:
        summary = tr(
            "Parameter metadata regeneration completed. %1 of %2 artifacts "
            "were published.")
                      .arg(published)
                      .arg(result.artifacts.size());
        break;
    case ParameterMetaDataRegenerationService::Outcome::PartialFailure:
        summary = tr(
            "Parameter metadata regeneration completed with errors. %1 of "
            "%2 artifacts were published; %3 reported errors. Existing "
            "cache entries remain available for failed artifacts.")
                      .arg(published)
                      .arg(result.artifacts.size())
                      .arg(failures);
        break;
    case ParameterMetaDataRegenerationService::Outcome::Failed:
        summary = tr(
            "Parameter metadata regeneration failed. %1 artifacts were "
            "published before the failure; review the log for details.")
                      .arg(published);
        break;
    case ParameterMetaDataRegenerationService::Outcome::Cancelled:
        summary = tr(
            "Parameter metadata regeneration was cancelled. %1 artifacts "
            "were published before cancellation.")
                      .arg(published);
        break;
    }
    m_result->setText(summary + QStringLiteral("\n") + refreshNotice());
}

void ParameterMetaDataRegenerationWindow::syncButtons()
{
    const bool available = bool(m_service);
    const auto snapshot = available
        ? m_service->snapshot()
        : ParameterMetaDataRegenerationService::Snapshot{};
    m_regenerate->setEnabled(available && !snapshot.active);
    m_cancel->setEnabled(
        available && snapshot.active && m_observedToken.isValid()
        && snapshot.token == m_observedToken);
    m_close->setEnabled(true);
}

bool ParameterMetaDataRegenerationWindow::defaultConfirmation(
    QWidget *owner, const QString &title, const QString &message)
{
    auto *dialog = new QMessageBox(
        QMessageBox::Question, title, message,
        QMessageBox::Yes | QMessageBox::Cancel, owner);
    dialog->setObjectName(
        QStringLiteral("ParameterMetadataRegenerationConfirmation"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    QPointer<QMessageBox> guardedDialog(dialog);
    const int answer = dialog->exec();
    const bool accepted = bool(guardedDialog)
        && answer == QMessageBox::Yes;
    if (guardedDialog) {
        delete guardedDialog.data();
    }
    return accepted;
}

QString ParameterMetaDataRegenerationWindow::phaseText(
    ParameterMetaDataRegenerationService::Phase phase)
{
    switch (phase) {
    case ParameterMetaDataRegenerationService::Phase::Idle:
        return tr("Ready");
    case ParameterMetaDataRegenerationService::Phase::DownloadingSources:
        return tr("Downloading ArduPilot source definitions");
    case ParameterMetaDataRegenerationService::Phase::BuildingGeneratedSource:
        return tr("Building generated source metadata");
    case ParameterMetaDataRegenerationService::Phase::DownloadingPreparedProducts:
        return tr("Downloading prepared PDEF catalogs");
    case ParameterMetaDataRegenerationService::Phase::Finished:
        return tr("Finished");
    }
    return tr("Working");
}

QString ParameterMetaDataRegenerationWindow::artifactKey(
    const ParameterMetaDataRegenerationService::ArtifactReport &artifact)
{
    return QString::number(static_cast<int>(artifact.kind))
        + QLatin1Char(':') + artifact.key;
}
