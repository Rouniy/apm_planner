#include "SftpLogDownloadWindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

struct SftpLogDownloadWindow::ListingIdentity
{
    QString host;
    quint16 port = 22;
    QString username;
    QString remoteDirectory;

    bool operator==(const ListingIdentity &other) const
    {
        return host.compare(other.host, Qt::CaseInsensitive) == 0
            && port == other.port && username == other.username
            && remoteDirectory == other.remoteDirectory;
    }
};

struct SftpLogDownloadWindow::PendingAction
{
    SftpLogDownloadService::Operation operation =
        SftpLogDownloadService::Operation::None;
    SftpLogConnection connection;
    QString remoteDirectory;
    QVector<SftpLogEntry> entries;
    bool createKml = false;
    bool allRows = false;
    quint64 flow = 0;
    quint64 formRevision = 0;
    quint64 listingRevision = 0;
};

namespace
{
QString formatBytes(qint64 bytes)
{
    bytes = qMax<qint64>(0, bytes);
    if (bytes >= 1024LL * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / 1024.0 / 1024 / 1024, 0, 'f', 1);
    if (bytes >= 1024LL * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024, 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

QString entryKey(const SftpLogEntry &entry)
{
    return entry.remotePath() + QLatin1Char('\n') + QString::number(entry.length)
        + QLatin1Char('\n') + entry.lastWriteTimeUtc.toUTC().toString(Qt::ISODateWithMs);
}

QString boundedList(const QStringList &values, int maximum = 8)
{
    if (values.isEmpty())
        return {};
    const int shown = qMin(maximum, values.size());
    QString result = values.mid(0, shown).join(QStringLiteral(", "));
    if (shown < values.size())
        result += SftpLogDownloadWindow::tr(" (+%1 more)").arg(values.size() - shown);
    return result;
}

void scrubSecret(QString *password)
{
    if (!password)
        return;
    volatile ushort *data = reinterpret_cast<volatile ushort *>(password->data());
    for (int index = 0; index < password->size(); ++index)
        data[index] = 0;
    password->clear();
    password->squeeze();
}

void scrubPassword(SftpLogConnection *connection)
{
    if (!connection)
        return;
    scrubSecret(&connection->password);
}
} // namespace

SftpLogDownloadWindow::SftpLogDownloadWindow(
    QWidget *parent, SftpLogSessionFactory factory)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("SftpLogDownloadWindow"));
    setWindowTitle(tr("Download DataFlash Logs over SFTP"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(900, 680);
    setMinimumSize(760, 560);
    buildUi();
    loadSettings();

    m_service = new SftpLogDownloadService(std::move(factory), this);
    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout,
            this, &SftpLogDownloadWindow::updateProgress);
    m_timer->start();

    connect(m_refresh, &QPushButton::clicked,
            this, &SftpLogDownloadWindow::refreshList);
    connect(m_downloadSelected, &QPushButton::clicked,
            this, &SftpLogDownloadWindow::downloadSelected);
    connect(m_downloadAll, &QPushButton::clicked,
            this, &SftpLogDownloadWindow::downloadAll);
    connect(m_deleteSelected, &QPushButton::clicked, this, [this]() {
        const QVector<SftpLogEntry> entries = selectedEntries();
        if (entries.isEmpty()) {
            setStatus(tr("Select at least one listed remote log first."));
            return;
        }
        beginDelete(entries, false);
    });
    connect(m_deleteAll, &QPushButton::clicked, this, [this]() {
        if (m_entries.isEmpty()) {
            setStatus(tr("There are no listed remote logs to delete."));
            return;
        }
        beginDelete(m_entries, true);
    });
    connect(m_cancel, &QPushButton::clicked,
            this, &SftpLogDownloadWindow::cancel);

    for (QLineEdit *field : {m_host, m_port, m_username, m_remoteDirectory}) {
        connect(field, &QLineEdit::textChanged,
                this, &SftpLogDownloadWindow::invalidateListing);
    }
    connect(m_grid, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem *) { refreshControls(); });
    connect(m_service, &SftpLogDownloadService::stateChanged, this, [this]() {
        if (m_shuttingDown || QCoreApplication::closingDown())
            return;
        QPointer<SftpLogDownloadWindow> guard(this);
        setStatus(m_service->status());
        if (!guard)
            return;
        refreshControls();
        if (!guard)
            return;
        // operationFinished is emitted while the service deliberately keeps
        // its finishing fence raised.  The following terminal stateChanged is
        // the first point at which a confirmed cancel-and-close may proceed.
        resolveDeferredClose();
    });
    connect(m_service, &SftpLogDownloadService::hostKeyChallengeAvailable,
            this, &SftpLogDownloadWindow::showHostKeyChallenge);
    connect(m_service, &SftpLogDownloadService::operationFinished,
            this, &SftpLogDownloadWindow::handleOperationFinished);

    setStatus(m_service->status());
    refreshControls();
}

SftpLogDownloadWindow::~SftpLogDownloadWindow()
{
    shutdown();
}

bool SftpLogDownloadWindow::busy() const noexcept
{
    return (m_service && m_service->busy()) || bool(m_pending)
        || !m_directoryDialog.isNull() || !m_actionPrompt.isNull()
        || !m_hostKeyPrompt.isNull() || !m_closePrompt.isNull();
}

void SftpLogDownloadWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *connection = new QGridLayout;
    connection->addWidget(new QLabel(tr("SSH host"), this), 0, 0);
    connection->addWidget(new QLabel(tr("Port"), this), 0, 1);
    connection->addWidget(new QLabel(tr("Username"), this), 0, 2);
    connection->addWidget(new QLabel(tr("Password (never saved)"), this), 0, 3);
    m_host = new QLineEdit(this);
    m_host->setObjectName(QStringLiteral("SftpHost"));
    m_port = new QLineEdit(this);
    m_port->setObjectName(QStringLiteral("SftpPort"));
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("SftpUsername"));
    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("SftpPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    connection->addWidget(m_host, 1, 0);
    connection->addWidget(m_port, 1, 1);
    connection->addWidget(m_username, 1, 2);
    connection->addWidget(m_password, 1, 3);
    connection->addWidget(new QLabel(tr("Remote directory"), this), 2, 0);
    m_remoteDirectory = new QLineEdit(this);
    m_remoteDirectory->setObjectName(QStringLiteral("SftpRemoteDirectory"));
    connection->addWidget(m_remoteDirectory, 2, 1, 1, 3);
    connection->setColumnStretch(0, 2);
    connection->setColumnStretch(1, 0);
    connection->setColumnStretch(2, 2);
    connection->setColumnStretch(3, 2);
    root->addLayout(connection);

    auto *actions = new QHBoxLayout;
    m_refresh = new QPushButton(tr("Refresh List"), this);
    m_refresh->setObjectName(QStringLiteral("SftpRefreshButton"));
    actions->addWidget(m_refresh);
    m_downloadSelected = new QPushButton(tr("Download Selected…"), this);
    m_downloadSelected->setObjectName(QStringLiteral("SftpDownloadSelectedButton"));
    actions->addWidget(m_downloadSelected);
    m_downloadAll = new QPushButton(tr("Download All…"), this);
    m_downloadAll->setObjectName(QStringLiteral("SftpDownloadAllButton"));
    actions->addWidget(m_downloadAll);
    m_createKml = new QCheckBox(tr("Create KML"), this);
    m_createKml->setObjectName(QStringLiteral("SftpCreateKmlCheckBox"));
    actions->addWidget(m_createKml);
    m_deleteSelected = new QPushButton(tr("Delete Selected…"), this);
    m_deleteSelected->setObjectName(QStringLiteral("SftpDeleteSelectedButton"));
    actions->addWidget(m_deleteSelected);
    m_deleteAll = new QPushButton(tr("Delete All…"), this);
    m_deleteAll->setObjectName(QStringLiteral("SftpDeleteAllButton"));
    actions->addWidget(m_deleteAll);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("SftpCancelButton"));
    actions->addWidget(m_cancel);
    actions->addStretch(1);
    root->addLayout(actions);

    m_grid = new QTableWidget(this);
    m_grid->setObjectName(QStringLiteral("RemoteLogGrid"));
    m_grid->setColumnCount(4);
    m_grid->setHorizontalHeaderLabels({tr("Get"), tr("Remote filename"),
                                       tr("Modified"), tr("Size")});
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setAlternatingRowColors(true);
    m_grid->verticalHeader()->setVisible(false);
    m_grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_grid->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_grid->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_grid->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    root->addWidget(m_grid, 1);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("SftpProgressBar"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    root->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("SftpStatusLabel"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    m_activity = new QPlainTextEdit(this);
    m_activity->setObjectName(QStringLiteral("SftpActivityLog"));
    m_activity->setReadOnly(true);
    m_activity->setPlaceholderText(tr("SFTP activity"));
    QFont fixed = QFont(QStringLiteral("monospace"));
    fixed.setStyleHint(QFont::Monospace);
    m_activity->setFont(fixed);
    m_activity->setMinimumHeight(120);
    m_activity->setMaximumHeight(160);
    root->addWidget(m_activity);
}

void SftpLogDownloadWindow::loadSettings()
{
    QSettings settings;
    m_host->setText(settings.value(QStringLiteral("SftpLogHost"),
                                   QStringLiteral("10.0.1.128")).toString());
    m_port->setText(settings.value(QStringLiteral("SftpLogPort"), 22).toString());
    m_username->setText(settings.value(QStringLiteral("SftpLogUsername"),
                                       QStringLiteral("user")).toString());
    m_remoteDirectory->setText(settings.value(QStringLiteral("SftpLogDirectory"),
        QStringLiteral("/home/user/dflogger/dataflash/")).toString());
    m_createKml->setChecked(settings.value(QStringLiteral("SftpLogCreateKml"), true).toBool());
    const QSignalBlocker passwordSignals(m_password);
    m_password->setText(QString());
}

void SftpLogDownloadWindow::saveNonSecretSettings()
{
    if (!m_service || !m_service->connected())
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("SftpLogHost"), m_host->text());
    settings.setValue(QStringLiteral("SftpLogPort"), m_port->text());
    settings.setValue(QStringLiteral("SftpLogUsername"), m_username->text());
    settings.setValue(QStringLiteral("SftpLogDirectory"), m_remoteDirectory->text());
    settings.setValue(QStringLiteral("SftpLogCreateKml"), m_createKml->isChecked());
    settings.sync();
}

bool SftpLogDownloadWindow::captureConnection(
    SftpLogConnection *connection, QString *directory, QString *error)
{
    if (!connection || !directory)
        return false;
    const QString hostInput = m_host->text();
    const QString portInput = m_port->text();
    const QString usernameInput = m_username->text();
    const QString directoryInput = m_remoteDirectory->text();
    QString password = m_password->text();
    {
        // QLineEdit::clear() retains an undo copy of the password.  setText()
        // resets that history.  Suppress textChanged while erasing because a
        // listener that synchronously deletes the line edit from its own
        // signal leaves Qt's line-control mutation on a freed widget.
        const QSignalBlocker passwordSignals(m_password);
        m_password->setText(QString());
    }
    bool portOk = false;
    const int defaultPort = portInput.toInt(&portOk);
    if (!portOk || defaultPort < 1 || defaultPort > 65535) {
        scrubSecret(&password);
        if (error)
            *error = tr("SSH port must be a number between 1 and 65535.");
        return false;
    }
    QString host;
    quint16 port = 0;
    if (!SftpLogSession::parseEndpoint(hostInput, defaultPort,
                                       &host, &port, error)) {
        scrubSecret(&password);
        return false;
    }
    const QString username = usernameInput.trimmed();
    if (username.isEmpty()) {
        scrubSecret(&password);
        if (error)
            *error = tr("Enter the SSH username.");
        return false;
    }
    const QString normalized = SftpLogSession::normalizeDirectory(
        directoryInput, error);
    if (normalized.isEmpty()) {
        scrubSecret(&password);
        return false;
    }
    connection->host = host;
    connection->port = port;
    connection->username = username;
    connection->password = std::move(password);
    *directory = normalized;
    if (error)
        error->clear();
    return true;
}

bool SftpLogDownloadWindow::currentIdentity(
    ListingIdentity *identity, QString *error) const
{
    if (!identity)
        return false;
    bool portOk = false;
    const int defaultPort = m_port->text().toInt(&portOk);
    if (!portOk || defaultPort < 1 || defaultPort > 65535) {
        if (error)
            *error = tr("SSH port must be a number between 1 and 65535.");
        return false;
    }
    QString host;
    quint16 port = 0;
    if (!SftpLogSession::parseEndpoint(m_host->text(), defaultPort,
                                       &host, &port, error)) {
        return false;
    }
    const QString username = m_username->text().trimmed();
    if (username.isEmpty()) {
        if (error)
            *error = tr("Enter the SSH username.");
        return false;
    }
    const QString directory = SftpLogSession::normalizeDirectory(
        m_remoteDirectory->text(), error);
    if (directory.isEmpty())
        return false;
    *identity = {host, port, username, directory};
    if (error)
        error->clear();
    return true;
}

bool SftpLogDownloadWindow::listedIdentityIsCurrent(QString *error) const
{
    if (!m_listingIdentity || m_entries.isEmpty()) {
        if (error)
            *error = tr("Refresh the remote log list first.");
        return false;
    }
    ListingIdentity current;
    if (!currentIdentity(&current, error))
        return false;
    if (!(current == *m_listingIdentity)) {
        if (error)
            *error = tr("Connection or remote-directory fields changed; refresh the list again.");
        return false;
    }
    return true;
}

void SftpLogDownloadWindow::invalidateListing()
{
    ++m_formRevision;
    if (!m_entries.isEmpty() || m_listingIdentity) {
        m_entries.clear();
        m_listingIdentity.reset();
        ++m_listingRevision;
        QPointer<SftpLogDownloadWindow> guard(this);
        populateRows({});
        if (!guard)
            return;
        setStatus(tr("Connection fields changed; the old remote list was cleared. Refresh again."));
    }
    refreshControls();
}

void SftpLogDownloadWindow::refreshList()
{
    if (busy() || m_closing || m_shuttingDown)
        return;
    SftpLogConnection connection;
    QString directory;
    QString error;
    QPointer<SftpLogDownloadWindow> guard(this);
    if (!captureConnection(&connection, &directory, &error)) {
        if (!guard)
            return;
        setStatus(error);
        return;
    }
    if (!guard)
        return;
    auto pending = std::make_shared<PendingAction>();
    pending->operation = SftpLogDownloadService::Operation::Refresh;
    pending->connection = std::move(connection);
    pending->remoteDirectory = directory;
    pending->flow = ++m_flow;
    pending->formRevision = m_formRevision;
    m_pending = pending;
    startServiceOperation(pending);
}

QVector<SftpLogEntry> SftpLogDownloadWindow::selectedEntries() const
{
    QVector<SftpLogEntry> selected;
    for (int row = 0; row < m_entries.size() && row < m_grid->rowCount(); ++row) {
        QTableWidgetItem *item = m_grid->item(row, 0);
        if (item && item->checkState() == Qt::Checked)
            selected.append(m_entries.at(row));
    }
    return selected;
}

void SftpLogDownloadWindow::downloadSelected()
{
    const QVector<SftpLogEntry> entries = selectedEntries();
    if (entries.isEmpty()) {
        setStatus(tr("Select at least one listed remote log first."));
        return;
    }
    beginDownload(entries);
}

void SftpLogDownloadWindow::downloadAll()
{
    if (m_entries.isEmpty()) {
        setStatus(tr("Refresh the remote log list first."));
        return;
    }
    beginDownload(m_entries);
}

void SftpLogDownloadWindow::beginDownload(QVector<SftpLogEntry> entries)
{
    if (busy() || m_closing || m_shuttingDown)
        return;
    QString error;
    if (!listedIdentityIsCurrent(&error)) {
        setStatus(error);
        return;
    }
    SftpLogConnection connection;
    QString directory;
    QPointer<SftpLogDownloadWindow> guard(this);
    if (!captureConnection(&connection, &directory, &error)) {
        if (!guard)
            return;
        setStatus(error);
        return;
    }
    if (!guard)
        return;
    auto pending = std::make_shared<PendingAction>();
    pending->operation = SftpLogDownloadService::Operation::Download;
    pending->connection = std::move(connection);
    pending->remoteDirectory = directory;
    pending->entries = std::move(entries);
    pending->createKml = m_createKml->isChecked();
    pending->flow = ++m_flow;
    pending->formRevision = m_formRevision;
    pending->listingRevision = m_listingRevision;
    m_pending = pending;

    auto *dialog = new QFileDialog(this, tr("Select folder for downloaded DataFlash logs"));
    dialog->setObjectName(QStringLiteral("SftpDownloadDirectoryDialog"));
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    m_directoryDialog = dialog;
    connect(dialog, &QFileDialog::finished, this,
            [this, dialog, pending](int result) {
        if (m_directoryDialog != dialog || m_pending != pending
            || pending->flow != m_flow) {
            return;
        }
        const QStringList paths = dialog->selectedFiles();
        m_directoryDialog = nullptr;
        if (result != QDialog::Accepted || paths.isEmpty()) {
            setStatus(tr("SFTP download cancelled before any local file was created."));
            finishPendingUi();
            return;
        }
        startPendingDownload(paths.first(), pending);
    });
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void SftpLogDownloadWindow::startPendingDownload(
    const QString &destination, const std::shared_ptr<PendingAction> &pending)
{
    if (!pending || pending != m_pending || pending->flow != m_flow
        || pending->formRevision != m_formRevision
        || pending->listingRevision != m_listingRevision
        || m_closing || m_shuttingDown) {
        setStatus(tr("The connection or listed files changed while choosing a destination; download was not started."));
        finishPendingUi();
        return;
    }
    QString error;
    if (!listedIdentityIsCurrent(&error)) {
        setStatus(error);
        finishPendingUi();
        return;
    }
    startServiceOperation(pending, destination);
}

void SftpLogDownloadWindow::beginDelete(
    QVector<SftpLogEntry> entries, bool allRows)
{
    if (busy() || m_closing || m_shuttingDown)
        return;
    QString error;
    if (!listedIdentityIsCurrent(&error)) {
        setStatus(error);
        return;
    }
    SftpLogConnection connection;
    QString directory;
    QPointer<SftpLogDownloadWindow> guard(this);
    if (!captureConnection(&connection, &directory, &error)) {
        if (!guard)
            return;
        setStatus(error);
        return;
    }
    if (!guard)
        return;
    auto pending = std::make_shared<PendingAction>();
    pending->operation = SftpLogDownloadService::Operation::Delete;
    pending->connection = std::move(connection);
    pending->remoteDirectory = directory;
    pending->entries = std::move(entries);
    pending->allRows = allRows;
    pending->flow = ++m_flow;
    pending->formRevision = m_formRevision;
    pending->listingRevision = m_listingRevision;
    m_pending = pending;
    showDeleteConfirmation(pending);
}

void SftpLogDownloadWindow::showDeleteConfirmation(
    const std::shared_ptr<PendingAction> &pending)
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("SftpDeleteConfirmation"));
    dialog->setWindowTitle(pending->allRows
        ? tr("Delete all remote logs") : tr("Delete selected remote logs"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->resize(760, 480);
    auto *layout = new QVBoxLayout(dialog);
    auto *warning = new QLabel(
        tr("Permanently delete %1 DataFlash log(s) from %2 on %3@%4:%5?\n\n"
           "Each exact remote path, listed size and modification time is frozen "
           "below and rechecked by the backend. Changed files are refused. This "
           "cannot be undone.")
            .arg(QString::number(pending->entries.size()),
                 pending->remoteDirectory, pending->connection.username,
                 pending->connection.host,
                 QString::number(pending->connection.port)), dialog);
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    QStringList lines;
    lines.reserve(pending->entries.size());
    for (const SftpLogEntry &entry : pending->entries) {
        lines.append(QStringLiteral("%1\t%2\t%3 UTC")
            .arg(entry.remotePath(), formatBytes(entry.length),
                 entry.lastWriteTimeUtc.toUTC().toString(Qt::ISODateWithMs)));
    }
    auto *plan = new QPlainTextEdit(dialog);
    plan->setObjectName(QStringLiteral("SftpDeletePlan"));
    plan->setReadOnly(true);
    plan->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(plan, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::Cancel,
                                         dialog);
    QPushButton *confirm = buttons->button(QDialogButtonBox::Yes);
    confirm->setObjectName(QStringLiteral("SftpDeleteConfirmButton"));
    confirm->setText(pending->allRows ? tr("Delete all") : tr("Delete selected"));
    confirm->setAutoDefault(false);
    QPushButton *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    m_actionPrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, pending](int result) {
        if (m_actionPrompt != dialog || m_pending != pending
            || pending->flow != m_flow) {
            return;
        }
        m_actionPrompt = nullptr;
        if (result != QDialog::Accepted) {
            setStatus(tr("Remote deletion cancelled; no delete request was sent."));
            finishPendingUi();
            return;
        }
        if (pending->formRevision != m_formRevision
            || pending->listingRevision != m_listingRevision) {
            setStatus(tr("Connection fields or the remote list changed during confirmation; no delete request was sent."));
            finishPendingUi();
            return;
        }
        QString error;
        if (!listedIdentityIsCurrent(&error)) {
            setStatus(error);
            finishPendingUi();
            return;
        }
        startServiceOperation(pending);
    });
    QPointer<SftpLogDownloadWindow> guard(this);
    dialog->show();
    if (!guard)
        return;
    refreshControls();
}

void SftpLogDownloadWindow::startServiceOperation(
    const std::shared_ptr<PendingAction> &pending, const QString &destination)
{
    if (!pending || pending != m_pending || pending->flow != m_flow
        || m_service->busy() || m_closing || m_shuttingDown) {
        return;
    }
    m_ownedOperation = pending->operation;
    m_ownedOperationId = 0;
    m_operationFormRevision = pending->formRevision;
    QString error;
    bool started = false;
    QPointer<SftpLogDownloadWindow> guard(this);
    switch (pending->operation) {
    case SftpLogDownloadService::Operation::Refresh:
        started = m_service->refresh(pending->connection,
            pending->remoteDirectory, &m_ownedOperationId, &error);
        break;
    case SftpLogDownloadService::Operation::Download:
        started = m_service->download(pending->connection, pending->entries,
            destination, pending->createKml, &m_ownedOperationId, &error);
        break;
    case SftpLogDownloadService::Operation::Delete:
        started = m_service->remove(pending->connection, pending->entries,
            &m_ownedOperationId, &error);
        break;
    case SftpLogDownloadService::Operation::None:
        error = tr("No SFTP operation was selected.");
        break;
    }
    // The service has synchronously copied the credential before publishing
    // its early operation ID. The prompt/list snapshot never retains a second
    // password copy after admission, even if a callback deletes this window.
    scrubPassword(&pending->connection);
    if (!guard)
        return;
    if (!started) {
        m_ownedOperationId = 0;
        m_ownedOperation = SftpLogDownloadService::Operation::None;
        setStatus(error.isEmpty() ? tr("SFTP operation could not be started.") : error);
        finishPendingUi();
        return;
    }
    appendActivity(tr("Started %1 for %2@%3:%4.")
        .arg(pending->operation == SftpLogDownloadService::Operation::Refresh
                 ? tr("remote listing")
                 : pending->operation == SftpLogDownloadService::Operation::Download
                     ? tr("download") : tr("remote deletion"),
             pending->connection.username, pending->connection.host,
             QString::number(pending->connection.port)));
    if (!guard)
        return;
    refreshControls();
}

void SftpLogDownloadWindow::showHostKeyChallenge(quint64 operationId)
{
    if (operationId == 0 || operationId != m_ownedOperationId
        || !m_service->awaitingHostKeyTrust() || m_hostKeyPrompt
        || m_closing || m_shuttingDown) {
        return;
    }
    const SshHostKeyChallenge challenge = m_service->hostKeyChallenge();
    QString text;
    if (challenge.isChanged()) {
        text = tr("The SSH host key for %1:%2 CHANGED. Connection stopped before "
                  "password authentication. Verify this change through a trusted "
                  "independent channel before replacing the pin.\n\n"
                  "Algorithm: %3 (%4 bits)\nExpected: %5\nPresented: %6")
            .arg(challenge.host, QString::number(challenge.port),
                 challenge.algorithm, QString::number(challenge.keyLength),
                 challenge.expectedFingerprint,
                 challenge.presentedFingerprint);
    } else {
        text = tr("The SSH host key for %1:%2 is not pinned. Connection stopped "
                  "before password authentication. Verify this fingerprint through "
                  "a trusted independent channel before continuing.\n\n"
                  "Algorithm: %3 (%4 bits)\nPresented: %5")
            .arg(challenge.host, QString::number(challenge.port),
                 challenge.algorithm, QString::number(challenge.keyLength),
                 challenge.presentedFingerprint);
    }
    auto *box = new QMessageBox(QMessageBox::Warning,
        challenge.isChanged() ? tr("SSH host key changed") : tr("Trust SSH host key?"),
        text, QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("SftpHostKeyConfirmation"));
    box->setTextFormat(Qt::PlainText);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *trust = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        trust->setObjectName(QStringLiteral("SftpTrustHostKeyButton"));
        trust->setText(challenge.isChanged() ? tr("Replace trusted key") : tr("Trust and reconnect"));
        trust->setAutoDefault(false);
    }
    m_hostKeyPrompt = box;
    connect(box, &QMessageBox::finished, this,
            [this, box, operationId](int result) {
        if (m_hostKeyPrompt != box || operationId != m_ownedOperationId)
            return;
        m_hostKeyPrompt = nullptr;
        QString error;
        QPointer<SftpLogDownloadWindow> guard(this);
        if (result == QMessageBox::Yes) {
            const bool trusted = m_service->trustHostKey(operationId, &error);
            if (!guard)
                return;
            if (!trusted && !error.isEmpty())
                setStatus(error);
        } else {
            m_service->rejectHostKey(operationId);
            if (!guard)
                return;
        }
        refreshControls();
    });
    QPointer<SftpLogDownloadWindow> guard(this);
    box->open();
    if (!guard)
        return;
    refreshControls();
}

void SftpLogDownloadWindow::handleOperationFinished(quint64 operationId)
{
    if (operationId == 0 || operationId != m_ownedOperationId || m_shuttingDown)
        return;
    const SftpLogDownloadService::Operation completed = m_ownedOperation;
    QPointer<SftpLogDownloadWindow> guard(this);
    dismissDialog(m_hostKeyPrompt);
    if (!guard)
        return;

    if (completed == SftpLogDownloadService::Operation::Refresh) {
        const auto result = m_service->lastListResult();
        if (result.success && m_operationFormRevision == m_formRevision && m_pending) {
            m_entries = result.entries;
            ListingIdentity identity;
            identity.host = m_pending->connection.host;
            identity.port = m_pending->connection.port;
            identity.username = m_pending->connection.username;
            identity.remoteDirectory = m_pending->remoteDirectory;
            m_listingIdentity = std::make_unique<ListingIdentity>(identity);
            ++m_listingRevision;
            populateRows(m_entries);
            if (!guard)
                return;
            appendActivity(m_entries.isEmpty()
                ? tr("No DataFlash BIN logs were found in %1.").arg(identity.remoteDirectory)
                : tr("Found %1 DataFlash log(s) in %2.")
                    .arg(QString::number(m_entries.size()),
                         identity.remoteDirectory));
        } else if (result.success) {
            appendActivity(tr("A completed remote listing was discarded because the connection fields changed."));
        } else {
            appendActivity(result.cancelled
                ? tr("Remote listing cancelled.")
                : tr("Remote listing failed: %1").arg(result.error));
        }
    } else if (completed == SftpLogDownloadService::Operation::Download) {
        const auto result = m_service->lastDownloadResult();
        QString message = result.success
            ? tr("Downloaded %1 log(s).").arg(result.savedLogs)
            : result.cancelled
                ? tr("Download cancelled; completed output files are retained.")
                : tr("Download failed: %1").arg(result.error);
        if (!result.publishedPaths.isEmpty())
            message += tr(" Published %1 path(s): %2.")
                .arg(result.publishedPaths.size())
                .arg(boundedList(result.publishedPaths));
        if (!result.warnings.isEmpty())
            message += tr(" Warnings: %1").arg(boundedList(result.warnings));
        appendActivity(message);
    } else if (completed == SftpLogDownloadService::Operation::Delete) {
        const auto result = m_service->lastDeleteResult();
        removeDeletedRows(result.deletedEntries);
        if (!guard)
            return;
        QStringList deleted;
        for (const SftpLogEntry &entry : result.deletedEntries)
            deleted.append(entry.remotePath());
        QString message = result.cancelled
            ? tr("Remote deletion cancelled after %1/%2 file(s).")
                .arg(result.deletedEntries.size()).arg(result.requested)
            : tr("Deleted %1/%2 remote log(s); %3 failed.")
                .arg(result.deletedEntries.size()).arg(result.requested)
                .arg(result.failedPaths.size());
        if (!deleted.isEmpty())
            message += tr(" Deleted: %1.").arg(boundedList(deleted));
        if (!result.failedPaths.isEmpty())
            message += tr(" Failures: %1").arg(boundedList(result.failedPaths));
        appendActivity(message);
    }
    if (!guard)
        return;
    saveNonSecretSettings();
    m_ownedOperationId = 0;
    m_ownedOperation = SftpLogDownloadService::Operation::None;
    if (m_pending)
        scrubPassword(&m_pending->connection);
    m_pending.reset();
    setStatus(m_service->status());
    QPointer<SftpLogDownloadWindow> terminalGuard(this);
    refreshControls();
    if (!terminalGuard)
        return;
    resolveDeferredClose();
}

void SftpLogDownloadWindow::populateRows(const QVector<SftpLogEntry> &entries)
{
    QPointer<SftpLogDownloadWindow> guard(this);
    const bool gridSignals = m_grid->blockSignals(true);
    m_grid->setSortingEnabled(false);
    m_grid->setRowCount(entries.size());
    if (!guard)
        return;
    for (int row = 0; row < entries.size(); ++row) {
        const SftpLogEntry &entry = entries.at(row);
        auto *check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        check->setCheckState(Qt::Unchecked);
        m_grid->setItem(row, 0, check);
        if (!guard)
            return;
        m_grid->setItem(row, 1, new QTableWidgetItem(entry.name));
        if (!guard)
            return;
        m_grid->setItem(row, 2, new QTableWidgetItem(
            entry.lastWriteTimeUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        if (!guard)
            return;
        auto *size = new QTableWidgetItem(formatBytes(entry.length));
        size->setData(Qt::UserRole, entry.length);
        m_grid->setItem(row, 3, size);
        if (!guard)
            return;
    }
    m_grid->setSortingEnabled(false);
    if (!guard)
        return;
    m_grid->blockSignals(gridSignals);
}

void SftpLogDownloadWindow::removeDeletedRows(
    const QVector<SftpLogEntry> &deleted)
{
    QSet<QString> removed;
    for (const SftpLogEntry &entry : deleted)
        removed.insert(entryKey(entry));
    if (removed.isEmpty())
        return;
    QVector<SftpLogEntry> remaining;
    remaining.reserve(m_entries.size());
    for (const SftpLogEntry &entry : m_entries) {
        if (!removed.contains(entryKey(entry)))
            remaining.append(entry);
    }
    m_entries = std::move(remaining);
    ++m_listingRevision;
    populateRows(m_entries);
}

void SftpLogDownloadWindow::updateProgress()
{
    if (m_shuttingDown || !m_service)
        return;
    const qint64 done = qMax<qint64>(0, m_service->progressCompleted());
    const qint64 total = qMax<qint64>(0, m_service->progressTotal());
    const bool blocked = m_progress->blockSignals(true);
    m_progress->setRange(0, m_service->busy() && total <= 0 ? 0 : 1000);
    if (total > 0)
        m_progress->setValue(int(qMin<qint64>(1000, done * 1000 / total)));
    else if (!m_service->busy())
        m_progress->setValue(0);
    m_progress->blockSignals(blocked);
    if (m_service->busy())
        setStatus(m_service->status());
}

void SftpLogDownloadWindow::refreshControls()
{
    if (m_shuttingDown)
        return;
    const bool blocked = busy() || m_closing;
    QString listingError;
    const bool hasCurrentList = listedIdentityIsCurrent(&listingError);
    const bool selected = !selectedEntries().isEmpty();
    for (QLineEdit *field : {m_host, m_port, m_username, m_password, m_remoteDirectory})
        field->setEnabled(!blocked);
    m_createKml->setEnabled(!blocked);
    m_refresh->setEnabled(!blocked);
    m_downloadSelected->setEnabled(!blocked && hasCurrentList && selected);
    m_downloadAll->setEnabled(!blocked && hasCurrentList);
    m_deleteSelected->setEnabled(!blocked && hasCurrentList && selected);
    m_deleteAll->setEnabled(!blocked && hasCurrentList);
    m_cancel->setEnabled((m_service && m_service->busy()) || bool(m_pending)
                         || m_directoryDialog || m_actionPrompt || m_hostKeyPrompt);
}

void SftpLogDownloadWindow::appendActivity(const QString &text)
{
    if (text.isEmpty() || m_shuttingDown)
        return;
    m_activityText += (m_activityText.isEmpty() ? QString() : QStringLiteral("\n")) + text;
    if (m_activityText.size() > 16000)
        m_activityText = m_activityText.right(16000);
    QPointer<SftpLogDownloadWindow> guard(this);
    m_activity->setPlainText(m_activityText);
    if (!guard)
        return;
    m_activity->verticalScrollBar()->setValue(m_activity->verticalScrollBar()->maximum());
}

void SftpLogDownloadWindow::setStatus(const QString &text)
{
    if (!m_shuttingDown && m_status)
        m_status->setText(text);
}

void SftpLogDownloadWindow::dismissDialog(QPointer<QDialog> &dialog)
{
    QPointer<QDialog> current = dialog;
    dialog = nullptr;
    if (!current)
        return;
    const bool blocked = current->blockSignals(true);
    current->reject();
    if (!current)
        return;
    current->blockSignals(blocked);
    current->deleteLater();
}

void SftpLogDownloadWindow::dismissPicker()
{
    QPointer<QFileDialog> picker = m_directoryDialog;
    m_directoryDialog = nullptr;
    if (!picker)
        return;
    const bool blocked = picker->blockSignals(true);
    picker->reject();
    if (!picker)
        return;
    picker->blockSignals(blocked);
    picker->deleteLater();
}

void SftpLogDownloadWindow::finishPendingUi()
{
    if (m_service && m_service->busy())
        return;
    if (m_pending)
        scrubPassword(&m_pending->connection);
    m_pending.reset();
    m_ownedOperationId = 0;
    m_ownedOperation = SftpLogDownloadService::Operation::None;
    refreshControls();
}

void SftpLogDownloadWindow::cancel()
{
    if (m_shuttingDown)
        return;
    if (m_service && m_ownedOperationId != 0
        && m_service->operationId() == m_ownedOperationId) {
        QPointer<SftpLogDownloadWindow> guard(this);
        m_service->cancel(m_ownedOperationId);
        if (!guard)
            return;
        setStatus(tr("Cancelling SFTP operation; waiting for the worker to stop safely…"));
        return;
    }
    ++m_flow;
    QPointer<SftpLogDownloadWindow> guard(this);
    dismissPicker();
    if (!guard)
        return;
    dismissDialog(m_actionPrompt);
    if (!guard)
        return;
    dismissDialog(m_hostKeyPrompt);
    if (!guard)
        return;
    if (m_pending)
        scrubPassword(&m_pending->connection);
    m_pending.reset();
    setStatus(tr("SFTP action cancelled before an operation was admitted."));
    refreshControls();
}

void SftpLogDownloadWindow::closeEvent(QCloseEvent *event)
{
    if (m_shuttingDown || m_allowClose) {
        m_closing = true;
        event->accept();
        if (!m_shuttingDown && !m_closeResolutionEmitted) {
            m_closeResolutionEmitted = true;
            emit closeResolved(true);
        }
        return;
    }
    event->ignore();
    if (m_closePrompt) {
        m_closePrompt->raise();
        m_closePrompt->activateWindow();
        return;
    }
    if (!busy()) {
        m_closing = true;
        m_allowClose = true;
        event->accept();
        if (!m_closeResolutionEmitted) {
            m_closeResolutionEmitted = true;
            emit closeResolved(true);
        }
        return;
    }

    m_closing = true;
    auto *box = new QMessageBox(QMessageBox::Warning,
        tr("Cancel SFTP operation?"),
        tr("A remote log operation or confirmation is still active. Cancel it, "
           "remove any unpublished partial local file, and close only after the "
           "dedicated SFTP worker has stopped? Completed output files and remote "
           "deletions cannot be undone."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("SftpCloseConfirmation"));
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *yes = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        yes->setText(tr("Cancel and close"));
        yes->setAutoDefault(false);
    }
    m_closePrompt = box;
    connect(box, &QMessageBox::finished, this, [this, box](int result) {
        if (m_closePrompt != box)
            return;
        m_closePrompt = nullptr;
        if (result != QMessageBox::Yes) {
            m_closing = false;
            QPointer<SftpLogDownloadWindow> guard(this);
            refreshControls();
            if (!guard)
                return;
            emit closeResolved(false);
            if (!guard || m_closing || m_shuttingDown)
                return;
            // A worker can deliver an unknown/changed host key while the close
            // confirmation owns the UI. The first notification is suppressed
            // by m_closing, so restore the exact outstanding consent after the
            // user elects to keep this window and operation alive.
            if (m_service && m_ownedOperationId != 0
                && m_service->operationId() == m_ownedOperationId
                && m_service->awaitingHostKeyTrust() && !m_hostKeyPrompt) {
                showHostKeyChallenge(m_ownedOperationId);
            }
            return;
        }
        m_closeWhenIdle = true;
        ++m_flow;
        QPointer<SftpLogDownloadWindow> guard(this);
        dismissPicker();
        if (!guard)
            return;
        dismissDialog(m_actionPrompt);
        if (!guard)
            return;
        dismissDialog(m_hostKeyPrompt);
        if (!guard)
            return;
        if (m_service && m_ownedOperationId != 0
            && m_service->operationId() == m_ownedOperationId) {
            const quint64 operationId = m_ownedOperationId;
            m_service->cancel(m_ownedOperationId);
            if (!guard || (m_ownedOperationId != 0
                           && m_ownedOperationId != operationId))
                return;
        } else {
            if (m_pending)
                scrubPassword(&m_pending->connection);
            m_pending.reset();
            resolveDeferredClose();
        }
        refreshControls();
    });
    QPointer<SftpLogDownloadWindow> guard(this);
    box->open();
    if (!guard)
        return;
    refreshControls();
}

void SftpLogDownloadWindow::resolveDeferredClose()
{
    if (!m_closeWhenIdle || (m_service && m_service->busy())
        || m_shuttingDown) {
        return;
    }
    m_closeWhenIdle = false;
    m_allowClose = true;
    QTimer::singleShot(0, this, &QWidget::close);
}

void SftpLogDownloadWindow::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_closing = true;
    ++m_flow;
    if (m_timer)
        m_timer->stop();
    if (m_service) {
        // This is terminal for the page-owned service. Suppress every outward
        // callback before cancellation: an external service observer is
        // otherwise allowed to delete this window synchronously while
        // shutdown() is still draining its dedicated worker.
        m_service->blockSignals(true);
        disconnect(m_service, nullptr, nullptr, nullptr);
        if (m_ownedOperationId != 0
            && m_service->operationId() == m_ownedOperationId) {
            m_service->cancel(m_ownedOperationId);
        }
        m_service->shutdown();
    }
    m_ownedOperationId = 0;
    if (m_pending)
        scrubPassword(&m_pending->connection);
    m_pending.reset();
    if (QCoreApplication::closingDown()) {
        m_directoryDialog = nullptr;
        m_actionPrompt = nullptr;
        m_hostKeyPrompt = nullptr;
        m_closePrompt = nullptr;
        return;
    }
    QPointer<SftpLogDownloadWindow> guard(this);
    dismissPicker();
    if (!guard)
        return;
    dismissDialog(m_actionPrompt);
    if (!guard)
        return;
    dismissDialog(m_hostKeyPrompt);
    if (!guard)
        return;
    dismissDialog(m_closePrompt);
}
