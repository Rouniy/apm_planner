#include "MavFTPUIView.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <limits>

namespace {

QString canonicalRemotePath(QString path)
{
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

bool sameRemotePath(const QString &left, const QString &right)
{
    return canonicalRemotePath(left) == canonicalRemotePath(right);
}

void deleteChildren(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    while (item->childCount() > 0) {
        delete item->takeChild(0);
    }
}

} // namespace

MavFTPUIView::MavFTPUIView(MavFtpServiceInterface *service, QWidget *parent)
    : QWidget(parent)
    , m_service(service)
{
    setObjectName(QStringLiteral("MavFTPUIView"));
    buildUi();
    connectUi();
    if (m_service) {
        const VehicleTargetLease target = m_service->currentTargetLease();
        m_displayTargetGeneration = target.generation;
        m_displayTargetEndpoint = target.endpoint;
        m_displayTargetValid = target.isValid();
    }
    resetRoots();

    if (!m_service) {
        m_status->setText(tr("MAVFTP service is unavailable."));
    } else {
        m_status->setText(tr(
            "Connect over MAVLink, then Refresh to browse the remote filesystem."));
    }
    syncControls();
}

MavFTPUIView::~MavFTPUIView()
{
    m_closeInProgress = true;
    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
    }
    const quint64 operationId = m_pendingOperationId;
    QPointer<MavFtpServiceInterface> service = m_service;
    dismissPrompt(true);
    if (service && operationId
        && service->activeOperationId() == operationId) {
        service->cancelOperation(operationId);
    }
}

void MavFTPUIView::closeEvent(QCloseEvent *event)
{
    m_closeInProgress = true;
    QPointer<MavFTPUIView> guard(this);
    cancelOwnedFlow();
    if (guard) {
        QWidget::closeEvent(event);
    }
}

void MavFTPUIView::showEvent(QShowEvent *event)
{
    m_closeInProgress = false;
    QWidget::showEvent(event);
    syncControls();
}

bool MavFTPUIView::ShouldAddSystemRoot(
    const QStringList &rootDirectoryNames)
{
    for (const QString &name : rootDirectoryNames) {
        if (name.compare(QStringLiteral("@SYS"), Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

void MavFTPUIView::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(tr("MAVFTP — Remote Files"), this);
    title->setObjectName(QStringLiteral("MavFtpTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName(QStringLiteral("RefreshButton"));
    m_download = new QPushButton(tr("Download"), this);
    m_download->setObjectName(QStringLiteral("DownloadBtn"));
    m_upload = new QPushButton(tr("Upload"), this);
    m_upload->setObjectName(QStringLiteral("UploadBtn"));
    m_delete = new QPushButton(tr("Delete"), this);
    m_delete->setObjectName(QStringLiteral("DeleteButton"));
    m_newFolderName = new QLineEdit(this);
    m_newFolderName->setObjectName(QStringLiteral("NewFolderName"));
    m_newFolderName->setPlaceholderText(tr("New folder name"));
    m_newFolderName->setFixedWidth(160);
    m_mkdir = new QPushButton(tr("Mkdir"), this);
    m_mkdir->setObjectName(QStringLiteral("MkdirButton"));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("CancelButton"));
    toolbar->addWidget(m_refresh);
    toolbar->addWidget(m_download);
    toolbar->addWidget(m_upload);
    toolbar->addWidget(m_delete);
    toolbar->addWidget(m_newFolderName);
    toolbar->addWidget(m_mkdir);
    toolbar->addWidget(m_cancel);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("MavFtpBrowserSplitter"));
    splitter->setChildrenCollapsible(false);

    m_directories = new QTreeWidget(splitter);
    m_directories->setObjectName(QStringLiteral("DirectoryTree"));
    m_directories->setHeaderHidden(true);
    m_directories->setSelectionMode(QAbstractItemView::SingleSelection);
    m_directories->setMinimumWidth(180);

    m_entries = new QTableWidget(splitter);
    m_entries->setObjectName(QStringLiteral("EntriesGrid"));
    m_entries->setColumnCount(3);
    m_entries->setHorizontalHeaderLabels(
        {tr("Name"), tr("Type"), tr("Size")});
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entries->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entries->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entries->setAlternatingRowColors(true);
    m_entries->setShowGrid(true);
    m_entries->verticalHeader()->setVisible(false);
    m_entries->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_entries->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    m_entries->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Fixed);
    m_entries->setColumnWidth(1, 120);
    m_entries->setColumnWidth(2, 120);

    splitter->addWidget(m_directories);
    splitter->addWidget(m_entries);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 560});
    root->addWidget(splitter, 1);

    auto *footer = new QGridLayout;
    footer->setHorizontalSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("MavFtpStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("MavFtpProgress"));
    m_progress->setRange(0, 100);
    setProgressValue(0);
    m_progress->setFixedWidth(200);
    footer->addWidget(m_status, 0, 0);
    footer->addWidget(m_progress, 0, 1);
    footer->setColumnStretch(0, 1);
    root->addLayout(footer);
}

void MavFTPUIView::connectUi()
{
    connect(m_refresh, &QPushButton::clicked,
            this, &MavFTPUIView::refreshRoot);
    connect(m_download, &QPushButton::clicked,
            this, &MavFTPUIView::downloadSelected);
    connect(m_upload, &QPushButton::clicked,
            this, &MavFTPUIView::uploadFile);
    connect(m_delete, &QPushButton::clicked,
            this, &MavFTPUIView::deleteSelected);
    connect(m_mkdir, &QPushButton::clicked,
            this, &MavFTPUIView::makeDirectory);
    connect(m_cancel, &QPushButton::clicked,
            this, &MavFTPUIView::cancelOperation);
    connect(m_newFolderName, &QLineEdit::textChanged,
            this, [this]() { syncControls(); });
    connect(m_entries, &QTableWidget::itemSelectionChanged,
            this, &MavFTPUIView::syncControls);
    connect(m_entries, &QTableWidget::itemDoubleClicked,
            this, [this](QTableWidgetItem *) { openSelectedEntry(); });
    connect(m_directories, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        syncControls();
        if (!current || current->data(0, PlaceholderRole).toBool()
            || !m_service || m_service->isBusy() || m_pending) {
            return;
        }
        listDirectory(current, true);
    });
    connect(m_directories, &QTreeWidget::itemExpanded,
            this, [this](QTreeWidgetItem *item) {
        if (!item || item->data(0, PlaceholderRole).toBool()
            || item->data(0, LoadedRole).toBool()
            || !m_service || m_service->isBusy() || m_pending) {
            return;
        }
        listDirectory(item, item == m_directories->currentItem());
    });

    if (!m_service) {
        return;
    }
    connect(m_service, &MavFtpServiceInterface::stateChanged,
            this, &MavFTPUIView::syncControls);
    connect(m_service, &MavFtpServiceInterface::operationProgress,
            this, &MavFTPUIView::handleProgress);
    connect(m_service, &MavFtpServiceInterface::operationFinished,
            this, &MavFTPUIView::handleResult);
    connect(m_service, &MavFtpServiceInterface::targetChanged,
            this, &MavFTPUIView::handleTargetChanged);
    connect(m_service, &QObject::destroyed, this, [this]() {
        m_service = nullptr;
        QPointer<MavFTPUIView> guard(this);
        cancelOwnedFlow();
        if (!guard) return;
        resetRoots();
        m_status->setText(tr("MAVFTP service is unavailable."));
        syncControls();
    });
}

void MavFTPUIView::resetRoots()
{
    const QSignalBlocker directoryBlocker(m_directories);
    const QSignalBlocker entryBlocker(m_entries);
    m_directories->clear();
    m_entries->setRowCount(0);
    auto *root = new QTreeWidgetItem(m_directories, {QStringLiteral("/")});
    root->setData(0, PathRole, QStringLiteral("/"));
    root->setData(0, LoadedRole, false);
    root->setData(0, PlaceholderRole, false);
    auto *placeholder = new QTreeWidgetItem(root, {QStringLiteral("…")});
    placeholder->setData(0, PlaceholderRole, true);
    m_directories->setCurrentItem(root);
    root->setExpanded(true);
}

void MavFTPUIView::refreshRoot()
{
    if (!m_service || m_service->isBusy() || m_pending) {
        return;
    }
    resetRoots();
    listDirectory(m_directories->topLevelItem(0), true, true);
}

void MavFTPUIView::refreshCurrentDirectory()
{
    if (!m_service || m_service->isBusy() || m_pending) {
        return;
    }
    QTreeWidgetItem *const current = m_directories->currentItem();
    if (!current || current->data(0, PlaceholderRole).toBool()) {
        return;
    }
    listDirectory(current, true);
}

void MavFTPUIView::listDirectory(QTreeWidgetItem *item, bool updateEntries,
                                 bool rootRefresh)
{
    if (!item || !m_service || m_service->isBusy() || m_pending) {
        return;
    }
    const QString path = item->data(0, PathRole).toString();
    if (path.isEmpty()) {
        m_status->setText(tr("The selected remote directory has no valid path."));
        return;
    }

    QString targetError;
    const VehicleTargetLease target = acquireTarget(&targetError);
    if (!target.isValid()) {
        m_status->setText(targetError);
        syncControls();
        return;
    }
    const QVariant listedValue = item->data(0, TargetLeaseRole);
    if (listedValue.isValid()) {
        const VehicleTargetLease listed = listedValue.value<VehicleTargetLease>();
        if (!listed.isValid() || listed.generation != target.generation
            || !listed.endpoint.sameIdentity(target.endpoint)) {
            m_status->setText(tr(
                "The displayed directory belongs to an older vehicle selection. Refresh first."));
            syncControls();
            return;
        }
    }

    if (!beginPending(MavFtpServiceInterface::Operation::ListDirectory, path,
                      target)) return;
    m_pendingDirectory = item;
    m_pendingParentPath = path;
    m_pendingListUpdatesEntries = updateEntries;
    m_pendingRootRefresh = rootRefresh;
    m_status->setText(tr("Listing %1").arg(path));
    admitPending();
}

void MavFTPUIView::downloadSelected()
{
    if (!m_service || m_service->isBusy() || m_pending
        || selectedEntryPath().isEmpty() || selectedEntryIsDirectory()) {
        m_status->setText(tr("Select a file to download."));
        return;
    }

    const QString fileName = selectedEntryName();
    const QString remotePath = selectedEntryPath();
    if (fileName.isEmpty() || fileName == QStringLiteral(".")
        || fileName == QStringLiteral("..")
        || QFileInfo(fileName).fileName() != fileName
        || fileName.contains(QLatin1Char('\\'))) {
        m_status->setText(tr("The remote file name is not safe for a local download."));
        return;
    }
    QString targetError;
    const VehicleTargetLease target = acquireTarget(&targetError);
    if (!target.isValid()) {
        m_status->setText(targetError);
        return;
    }
    if (!selectedContentMatchesTarget(target, true)) {
        m_status->setText(tr(
            "The displayed file belongs to an older vehicle selection. Refresh first."));
        return;
    }

    if (!beginPending(MavFtpServiceInterface::Operation::Download, remotePath,
                      target)) return;
    m_pendingDisplayName = fileName;
    m_status->setText(tr("Choose a local folder for %1.").arg(fileName));
    openDownloadDirectoryPrompt(m_pendingRevision);
}

void MavFTPUIView::uploadFile()
{
    const QString directory = selectedDirectoryPath();
    if (!m_service || m_service->isBusy() || m_pending || directory.isEmpty()) {
        m_status->setText(tr("Select a destination directory first."));
        return;
    }
    QString targetError;
    const VehicleTargetLease target = acquireTarget(&targetError);
    if (!target.isValid()) {
        m_status->setText(targetError);
        return;
    }
    if (!selectedContentMatchesTarget(target, false)) {
        m_status->setText(tr(
            "The displayed directory belongs to an older vehicle selection. Refresh first."));
        return;
    }
    if (!beginPending(MavFtpServiceInterface::Operation::Upload, directory,
                      target)) return;
    m_pendingParentPath = directory;
    m_status->setText(tr("Choose a local file to upload to %1.").arg(directory));
    openUploadFilePrompt(m_pendingRevision);
}

void MavFTPUIView::deleteSelected()
{
    const QString path = selectedEntryPath();
    const QString name = selectedEntryName();
    if (!m_service || m_service->isBusy() || m_pending || path.isEmpty()
        || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        m_status->setText(tr("Select an item to delete."));
        return;
    }

    const bool directory = selectedEntryIsDirectory();
    QString targetError;
    const VehicleTargetLease target = acquireTarget(&targetError);
    if (!target.isValid()) {
        m_status->setText(targetError);
        return;
    }
    if (!selectedContentMatchesTarget(target, true)) {
        m_status->setText(tr(
            "The displayed item belongs to an older vehicle selection. Refresh first."));
        return;
    }

    const auto operation = directory
        ? MavFtpServiceInterface::Operation::RemoveDirectory
        : MavFtpServiceInterface::Operation::RemoveFile;
    if (!beginPending(operation, path, target)) return;
    m_pendingDisplayName = name;
    m_pendingParentPath = selectedDirectoryPath();
    m_status->setText(tr("Confirm deletion of %1.").arg(name));
    openDeleteConfirmation(m_pendingRevision);
}

void MavFTPUIView::makeDirectory()
{
    const QString validationError = folderNameError();
    if (!validationError.isEmpty()) {
        m_status->setText(validationError);
        return;
    }
    const QString parentPath = selectedDirectoryPath();
    if (!m_service || m_service->isBusy() || m_pending
        || parentPath.isEmpty()) {
        m_status->setText(tr("Select a parent directory first."));
        return;
    }

    const QString name = m_newFolderName->text().trimmed();
    const QString path = combineRemotePath(parentPath, name);
    QString targetError;
    const VehicleTargetLease target = acquireTarget(&targetError);
    if (!target.isValid()) {
        m_status->setText(targetError);
        return;
    }
    if (!selectedContentMatchesTarget(target, false)) {
        m_status->setText(tr(
            "The displayed directory belongs to an older vehicle selection. Refresh first."));
        return;
    }
    if (!beginPending(MavFtpServiceInterface::Operation::MakeDirectory, path,
                      target)) return;
    m_pendingDisplayName = name;
    m_pendingParentPath = parentPath;
    m_status->setText(tr("Create %1").arg(path));
    admitPending();
}

void MavFTPUIView::openDownloadDirectoryPrompt(quint64 revision)
{
    if (!m_pending || revision != m_pendingRevision || m_prompt) return;
    auto *dialog = new QFileDialog(this, tr("Select download folder"));
    dialog->setObjectName(QStringLiteral("MavFtpDownloadDirectoryDialog"));
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    connect(dialog, &QDialog::accepted, this, [this, dialog, revision]() {
        if (m_prompt == dialog) m_prompt = nullptr;
        const QStringList selected = dialog->selectedFiles();
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        if (!pendingTargetIsCurrent()) {
            cancelOwnedFlow(tr("The active vehicle changed; choose the download again."));
            return;
        }
        const QString directory = selected.value(0);
        const QFileInfo target(directory);
        if (directory.isEmpty() || !target.exists() || !target.isDir()) {
            cancelOwnedFlow(tr("No valid local download folder was selected."));
            return;
        }
        m_pendingLocalPath = uniqueDownloadPath(
            target.absoluteFilePath(), m_pendingDisplayName);
        m_status->setText(tr("Download %1").arg(m_pendingDisplayName));
        admitPending();
    });
    connect(dialog, &QDialog::rejected, this, [this, dialog, revision]() {
        if (m_prompt == dialog) m_prompt = nullptr;
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        cancelOwnedFlow(tr("Download cancelled before it was sent."));
    });
    dialog->open();
}

void MavFTPUIView::openUploadFilePrompt(quint64 revision)
{
    if (!m_pending || revision != m_pendingRevision || m_prompt) return;
    auto *dialog = new QFileDialog(this, tr("Select file to upload"));
    dialog->setObjectName(QStringLiteral("MavFtpUploadFileDialog"));
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    connect(dialog, &QDialog::accepted, this, [this, dialog, revision]() {
        if (m_prompt == dialog) m_prompt = nullptr;
        const QStringList selected = dialog->selectedFiles();
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        if (!pendingTargetIsCurrent()) {
            cancelOwnedFlow(tr("The active vehicle changed; choose the upload again."));
            return;
        }
        const QString localPath = selected.value(0);
        const QFileInfo info(localPath);
        if (localPath.isEmpty() || !info.exists() || !info.isFile()) {
            cancelOwnedFlow(tr("The selected upload file is unavailable."));
            return;
        }
        if (info.size() > MaximumBufferedTransferBytes) {
            cancelOwnedFlow(tr(
                "This MAVFTP build buffers transfers and accepts files up to %1 MiB.")
                                  .arg(MaximumBufferedTransferBytes / (1024 * 1024)));
            return;
        }
        if (info.size() > std::numeric_limits<quint32>::max()) {
            cancelOwnedFlow(tr("MAVFTP paths cannot transfer files larger than 4 GiB."));
            return;
        }
        QFile file(localPath);
        if (!file.open(QIODevice::ReadOnly)) {
            cancelOwnedFlow(tr("Could not open %1: %2")
                                .arg(info.fileName(), file.errorString()));
            return;
        }
        const QByteArray data = file.read(MaximumBufferedTransferBytes + 1);
        if (data.size() > MaximumBufferedTransferBytes) {
            cancelOwnedFlow(tr(
                "The upload file grew beyond this build's %1 MiB buffer limit.")
                                  .arg(MaximumBufferedTransferBytes / (1024 * 1024)));
            return;
        }
        if (data.size() != info.size()) {
            cancelOwnedFlow(tr("Could not read all of %1: %2")
                                .arg(info.fileName(), file.errorString()));
            return;
        }
        m_pendingLocalPath = info.absoluteFilePath();
        m_pendingDisplayName = info.fileName();
        m_pendingRemotePath = combineRemotePath(
            m_pendingParentPath, m_pendingDisplayName);
        openUploadConfirmation(revision, data);
    });
    connect(dialog, &QDialog::rejected, this, [this, dialog, revision]() {
        if (m_prompt == dialog) m_prompt = nullptr;
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        cancelOwnedFlow(tr("Upload cancelled before it was sent."));
    });
    dialog->open();
}

void MavFTPUIView::openUploadConfirmation(quint64 revision,
                                           const QByteArray &data)
{
    if (!m_pending || revision != m_pendingRevision || m_prompt) return;
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Upload remote file"),
        tr("Upload to %1?\n\n%2\n\n"
           "An existing remote file with this path will be replaced. "
           "If the transfer is interrupted, a partial remote file may remain.")
            .arg(m_pendingTarget.endpoint.displayName(), m_pendingRemotePath),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("MavFtpUploadConfirmDialog"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision, data](int result) {
        if (m_prompt == dialog) m_prompt = nullptr;
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        if (result != QMessageBox::Yes) {
            cancelOwnedFlow(tr("Upload cancelled before it was sent."));
            return;
        }
        if (!pendingTargetIsCurrent()) {
            cancelOwnedFlow(tr("The active vehicle changed; confirm the upload again."));
            return;
        }
        m_status->setText(tr("Upload %1").arg(m_pendingDisplayName));
        admitPending(data);
    });
    dialog->open();
}

void MavFTPUIView::openDeleteConfirmation(quint64 revision)
{
    if (!m_pending || revision != m_pendingRevision || m_prompt) return;
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Delete remote item"),
        tr("Delete from %1?\n\n%2")
            .arg(m_pendingTarget.endpoint.displayName(), m_pendingRemotePath),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("MavFtpDeleteConfirmDialog"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog, revision](int result) {
        if (m_prompt == dialog) m_prompt = nullptr;
        dialog->deleteLater();
        if (!m_pending || revision != m_pendingRevision) return;
        if (result != QMessageBox::Yes) {
            cancelOwnedFlow(tr("Delete cancelled before it was sent."));
            return;
        }
        if (!pendingTargetIsCurrent()) {
            cancelOwnedFlow(tr("The active vehicle changed; confirm the deletion again."));
            return;
        }
        m_status->setText(tr("Delete %1").arg(m_pendingDisplayName));
        admitPending();
    });
    dialog->open();
}

void MavFTPUIView::cancelOperation()
{
    cancelOwnedFlow(tr("MAVFTP operation cancelled."));
}

void MavFTPUIView::handleTargetChanged()
{
    const VehicleTargetLease target = m_service
        ? m_service->currentTargetLease() : VehicleTargetLease();
    const bool changed = target.generation != m_displayTargetGeneration
        || target.isValid() != m_displayTargetValid
        || (target.isValid() && m_displayTargetValid
            && !target.endpoint.sameIdentity(m_displayTargetEndpoint));
    if (!changed) return;

    m_displayTargetGeneration = target.generation;
    m_displayTargetEndpoint = target.endpoint;
    m_displayTargetValid = target.isValid();
    const quint64 operationId = m_pendingOperationId;
    QPointer<MavFtpServiceInterface> service = m_service;
    QPointer<MavFTPUIView> guard(this);
    clearPending();
    const quint64 retirementRevision = m_pendingRevision;
    resetRoots();
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, 100);
        setProgressValue(0);
    }
    m_status->setText(target.isValid()
        ? tr("The active vehicle changed. Select Refresh to browse it.")
        : tr("No settled exact vehicle target is available."));
    syncControls();
    if (!guard || retirementRevision != m_pendingRevision
        || target.generation != m_displayTargetGeneration
        || target.isValid() != m_displayTargetValid
        || (target.isValid()
            && !target.endpoint.sameIdentity(m_displayTargetEndpoint))) return;
    dismissPrompt();
    if (guard && retirementRevision == m_pendingRevision) syncControls();
    if (!guard || !service || !operationId) return;
    if (service->activeOperationId() == operationId) {
        service->cancelOperation(operationId);
    }
}

void MavFTPUIView::cancelOwnedFlow(const QString &statusText)
{
    if (!m_pending && !m_prompt) return;
    const quint64 operationId = m_pendingOperationId;
    QPointer<MavFtpServiceInterface> service = m_service;
    QPointer<MavFTPUIView> guard(this);
    clearPending();
    const quint64 retirementRevision = m_pendingRevision;
    if (!statusText.isEmpty()) m_status->setText(statusText);
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, 100);
        setProgressValue(0);
    }
    syncControls();
    if (!guard) return;
    dismissPrompt();
    if (guard && retirementRevision == m_pendingRevision) syncControls();
    if (!guard || !service || !operationId) return;
    if (service->activeOperationId() == operationId) {
        service->cancelOperation(operationId);
    }
}

void MavFTPUIView::dismissPrompt(bool disconnectAll)
{
    QPointer<QDialog> prompt = m_prompt;
    m_prompt = nullptr;
    if (!prompt) return;
    if (disconnectAll) {
        disconnect(prompt, nullptr, nullptr, nullptr);
    } else {
        disconnect(prompt, nullptr, this, nullptr);
    }
    prompt->reject();
    if (prompt) prompt->deleteLater();
}

bool MavFTPUIView::targetIsCurrent(const VehicleTargetLease &target) const
{
    if (!m_service || !target.isValid()) return false;
    const VehicleTargetLease current = m_service->currentTargetLease();
    return current.isValid() && current.generation == target.generation
        && current.endpoint.sameIdentity(target.endpoint);
}

bool MavFTPUIView::pendingTargetIsCurrent() const
{
    return m_pending && targetIsCurrent(m_pendingTarget);
}

VehicleTargetLease MavFTPUIView::acquireTarget(QString *error) const
{
    const VehicleTargetLease target = !m_closeInProgress && m_service
        ? m_service->currentTargetLease() : VehicleTargetLease();
    if (!target.isValid() && error) {
        *error = tr("No settled exact vehicle target is available.");
    }
    return target;
}

bool MavFTPUIView::selectedContentMatchesTarget(
    const VehicleTargetLease &target, bool requireEntry) const
{
    QVariant stored;
    if (requireEntry) {
        const int row = m_entries->currentRow();
        QTableWidgetItem *const item = row >= 0
            ? m_entries->item(row, 0) : nullptr;
        if (!item) return false;
        stored = item->data(TargetLeaseRole);
    } else {
        QTreeWidgetItem *const item = m_directories->currentItem();
        if (!item) return false;
        stored = item->data(0, TargetLeaseRole);
        if (!stored.isValid()
            && item->data(0, PathRole).toString() == QStringLiteral("/")) {
            return target.isValid();
        }
    }
    if (!stored.isValid()) return false;
    const VehicleTargetLease listed = stored.value<VehicleTargetLease>();
    return listed.isValid() && target.isValid()
        && listed.generation == target.generation
        && listed.endpoint.sameIdentity(target.endpoint);
}

void MavFTPUIView::openSelectedEntry()
{
    if (m_pending || !m_service || m_service->isBusy()
        || !selectedEntryIsDirectory()) {
        return;
    }
    const QString path = selectedEntryPath();
    if (path.isEmpty()) {
        return;
    }
    QTreeWidgetItem *const parent = m_directories->currentItem();
    QTreeWidgetItem *child = nullptr;
    {
        const QSignalBlocker blocker(m_directories);
        child = findDirectoryChild(parent, path);
        if (!child && parent) {
            child = new QTreeWidgetItem(parent, {selectedEntryName()});
            child->setData(0, PathRole, path);
            child->setData(0, LoadedRole, false);
            child->setData(0, PlaceholderRole, false);
            const int row = m_entries->currentRow();
            if (row >= 0 && m_entries->item(row, 0)) {
                child->setData(0, TargetLeaseRole,
                               m_entries->item(row, 0)->data(TargetLeaseRole));
            }
            auto *placeholder = new QTreeWidgetItem(child, {QStringLiteral("…")});
            placeholder->setData(0, PlaceholderRole, true);
        }
        if (child) {
            parent->setExpanded(true);
            m_directories->setCurrentItem(child);
            child->setExpanded(true);
        }
    }
    if (child) listDirectory(child, true);
}

void MavFTPUIView::handleResult(
    const MavFtpServiceInterface::Result &result)
{
    if (!m_pending || !m_pendingOperationId || result.operationId != m_pendingOperationId
        || result.operation != m_pendingOperation
        || (!result.remotePath.isEmpty()
            && !sameRemotePath(result.remotePath, m_pendingRemotePath))
        || result.targetGeneration != m_pendingGeneration) {
        syncControls();
        return;
    }

    if (!pendingTargetIsCurrent()) {
        cancelOwnedFlow(tr(
            "The active vehicle changed; the stale MAVFTP result was ignored."));
        return;
    }

    const auto operation = m_pendingOperation;
    const QString remotePath = m_pendingRemotePath;
    const QString localPath = m_pendingLocalPath;
    const QString displayName = m_pendingDisplayName;
    const QString parentPath = m_pendingParentPath;
    const VehicleTargetLease target = m_pendingTarget;
    QTreeWidgetItem *const directory = m_pendingDirectory;
    const bool updateEntries = m_pendingListUpdatesEntries;
    const bool rootRefresh = m_pendingRootRefresh;
    clearPending();
    const quint64 terminalRevision = m_pendingRevision;
    QPointer<MavFTPUIView> guard(this);
    const auto terminalStateIsCurrent = [&]() {
        return guard && !m_pending && m_pendingRevision == terminalRevision;
    };

    m_progress->setRange(0, 100);
    if (!terminalStateIsCurrent()) return;
    if (result.cancelled) {
        m_status->setText(result.error.isEmpty()
                              ? tr("MAVFTP operation cancelled.")
                              : result.error);
        setProgressValue(0);
        if (!terminalStateIsCurrent()) return;
        syncControls();
        return;
    }
    if (!result.error.isEmpty()) {
        m_status->setText(result.error);
        setProgressValue(0);
        if (!terminalStateIsCurrent()) return;
        syncControls();
        return;
    }

    switch (operation) {
    case MavFtpServiceInterface::Operation::ListDirectory:
        populateDirectory(directory, result.entries, updateEntries, rootRefresh,
                          target);
        m_status->setText(tr("Ready."));
        setProgressValue(100);
        if (!terminalStateIsCurrent()) return;
        break;
    case MavFtpServiceInterface::Operation::Download: {
        if (result.data.size() > MaximumBufferedTransferBytes) {
            m_status->setText(tr(
                "The downloaded file exceeds this build's %1 MiB buffer limit.")
                                  .arg(MaximumBufferedTransferBytes
                                       / (1024 * 1024)));
            setProgressValue(0);
            if (!terminalStateIsCurrent()) return;
            break;
        }
        m_pendingLocalPath = localPath;
        QString error;
        if (writeDownloadedFile(result.data, &error)) {
            m_status->setText(tr("Downloaded %1 to %2")
                                  .arg(displayName, m_pendingLocalPath));
            setProgressValue(100);
        } else {
            m_status->setText(error);
            setProgressValue(0);
        }
        if (!terminalStateIsCurrent()) return;
        m_pendingLocalPath.clear();
        break;
    }
    case MavFtpServiceInterface::Operation::Upload:
        m_status->setText(tr("Uploaded %1.").arg(displayName));
        setProgressValue(100);
        if (!terminalStateIsCurrent()) return;
        scheduleDirectoryRefresh(parentPath, target);
        break;
    case MavFtpServiceInterface::Operation::MakeDirectory:
        {
            const QSignalBlocker blocker(m_newFolderName);
            m_newFolderName->clear();
        }
        m_status->setText(tr("Created %1.").arg(remotePath));
        {
            const QSignalBlocker blocker(m_directories);
            markDirectoryStale(findDirectoryByPath(parentPath));
        }
        setProgressValue(100);
        if (!terminalStateIsCurrent()) return;
        scheduleDirectoryRefresh(parentPath, target);
        break;
    case MavFtpServiceInterface::Operation::RemoveFile:
    case MavFtpServiceInterface::Operation::RemoveDirectory:
        m_status->setText(tr("Deleted %1.").arg(displayName));
        {
            const QSignalBlocker blocker(m_directories);
            markDirectoryStale(findDirectoryByPath(parentPath));
        }
        setProgressValue(100);
        if (!terminalStateIsCurrent()) return;
        scheduleDirectoryRefresh(parentPath, target);
        break;
    case MavFtpServiceInterface::Operation::None:
        break;
    }
    syncControls();
}

void MavFTPUIView::handleProgress(qulonglong operationId, qulonglong generation,
                                  qint64 completed, qint64 total)
{
    if (!m_pending || !m_pendingOperationId || operationId != m_pendingOperationId
        || generation != m_pendingGeneration) {
        return;
    }
    if (total <= 0) {
        m_progress->setRange(0, 0);
        return;
    }
    const quint64 revision = m_pendingRevision;
    QPointer<MavFTPUIView> guard(this);
    m_progress->setRange(0, 100);
    if (!guard || !m_pending || revision != m_pendingRevision
        || operationId != m_pendingOperationId) return;
    const qint64 percent = qBound<qint64>(
        0, (qMax<qint64>(0, completed) * 100) / total, 100);
    setProgressValue(static_cast<int>(percent));
}

void MavFTPUIView::populateDirectory(
    QTreeWidgetItem *directory,
    const QVector<MavFtpProtocol::DirectoryEntry> &entries,
    bool updateEntries, bool rootRefresh, const VehicleTargetLease &target)
{
    if (!directory) {
        return;
    }
    const QSignalBlocker directoryBlocker(m_directories);
    const QSignalBlocker entryBlocker(m_entries);
    directory->setData(0, TargetLeaseRole, QVariant::fromValue(target));
    setDirectoryChildren(directory, entries, target);
    if (updateEntries && directory == m_directories->currentItem()) {
        setEntries(entries, target);
    }

    if (rootRefresh
        && sameRemotePath(directory->data(0, PathRole).toString(),
                          QStringLiteral("/"))) {
        QStringList names;
        for (const auto &entry : entries) {
            if (entry.type == MavFtpProtocol::DirectoryEntryType::Directory) {
                names.append(entry.name);
            }
        }
        if (ShouldAddSystemRoot(names)) {
            auto *systemRoot = new QTreeWidgetItem(
                m_directories, {QStringLiteral("@SYS")});
            systemRoot->setData(0, PathRole, QStringLiteral("@SYS/"));
            systemRoot->setData(0, LoadedRole, false);
            systemRoot->setData(0, PlaceholderRole, false);
            systemRoot->setData(0, TargetLeaseRole,
                                QVariant::fromValue(target));
            auto *placeholder = new QTreeWidgetItem(
                systemRoot, {QStringLiteral("…")});
            placeholder->setData(0, PlaceholderRole, true);
        }
    }
}

void MavFTPUIView::setDirectoryChildren(
    QTreeWidgetItem *directory,
    const QVector<MavFtpProtocol::DirectoryEntry> &entries,
    const VehicleTargetLease &target)
{
    deleteChildren(directory);
    const QString parentPath = directory->data(0, PathRole).toString();
    QSet<QString> paths;
    for (const auto &entry : entries) {
        if (entry.type != MavFtpProtocol::DirectoryEntryType::Directory
            || entry.name == QStringLiteral(".")
            || entry.name == QStringLiteral("..")) {
            continue;
        }
        const QString path = combineRemotePath(parentPath, entry.name);
        if (paths.contains(path)) {
            continue;
        }
        paths.insert(path);
        auto *child = new QTreeWidgetItem(directory, {entry.name});
        child->setData(0, PathRole, path);
        child->setData(0, LoadedRole, false);
        child->setData(0, PlaceholderRole, false);
        child->setData(0, TargetLeaseRole, QVariant::fromValue(target));
        auto *placeholder = new QTreeWidgetItem(child, {QStringLiteral("…")});
        placeholder->setData(0, PlaceholderRole, true);
    }
    directory->setData(0, LoadedRole, true);
}

void MavFTPUIView::setEntries(
    const QVector<MavFtpProtocol::DirectoryEntry> &entries,
    const VehicleTargetLease &target)
{
    m_entries->setRowCount(0);
    const QString parentPath = selectedDirectoryPath();
    for (const auto &entry : entries) {
        if (entry.name == QStringLiteral(".")
            || entry.name == QStringLiteral("..")
            || entry.name.isEmpty()) {
            continue;
        }
        const int row = m_entries->rowCount();
        m_entries->insertRow(row);
        auto *name = new QTableWidgetItem(entry.name);
        name->setData(PathRole, combineRemotePath(parentPath, entry.name));
        name->setData(TargetLeaseRole, QVariant::fromValue(target));
        const bool isDirectory =
            entry.type == MavFtpProtocol::DirectoryEntryType::Directory;
        name->setData(LoadedRole, isDirectory);
        m_entries->setItem(row, 0, name);
        m_entries->setItem(row, 1, new QTableWidgetItem(
            isDirectory ? tr("Directory") : tr("File")));
        m_entries->setItem(row, 2, new QTableWidgetItem(
            isDirectory ? QString() : QString::number(entry.size)));
    }
    m_entries->clearSelection();
}

void MavFTPUIView::markDirectoryStale(QTreeWidgetItem *directory)
{
    if (!directory) {
        return;
    }
    directory->setData(0, LoadedRole, false);
}

QTreeWidgetItem *MavFTPUIView::findDirectoryByPath(const QString &path) const
{
    QList<QTreeWidgetItem *> pending;
    for (int index = 0; index < m_directories->topLevelItemCount(); ++index) {
        pending.append(m_directories->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem *const item = pending.takeLast();
        if (item && sameRemotePath(item->data(0, PathRole).toString(), path)) {
            return item;
        }
        if (!item) continue;
        for (int index = 0; index < item->childCount(); ++index) {
            if (!item->child(index)->data(0, PlaceholderRole).toBool()) {
                pending.append(item->child(index));
            }
        }
    }
    return nullptr;
}

void MavFTPUIView::scheduleDirectoryRefresh(
    const QString &path, const VehicleTargetLease &target)
{
    QTimer::singleShot(0, this, [this, path, target]() {
        if (!targetIsCurrent(target) || !m_service || m_service->isBusy()
            || m_pending) {
            return;
        }
        QTreeWidgetItem *const directory = findDirectoryByPath(path);
        if (!directory) return;
        {
            const QSignalBlocker blocker(m_directories);
            markDirectoryStale(directory);
        }
        listDirectory(directory, directory == m_directories->currentItem());
    });
}

bool MavFTPUIView::beginPending(
    MavFtpServiceInterface::Operation operation, const QString &remotePath,
    const VehicleTargetLease &target)
{
    m_pending = true;
    ++m_pendingRevision;
    m_pendingOperationId = 0;
    m_pendingOperation = operation;
    m_pendingRemotePath = remotePath;
    m_pendingLocalPath.clear();
    m_pendingDisplayName.clear();
    m_pendingParentPath.clear();
    m_pendingTarget = target;
    m_pendingDirectory = nullptr;
    m_pendingGeneration = target.generation;
    m_pendingListUpdatesEntries = false;
    m_pendingRootRefresh = false;
    const quint64 revision = m_pendingRevision;
    QPointer<MavFTPUIView> guard(this);
    m_progress->setRange(0, 0);
    if (!guard || !m_pending || m_pendingRevision != revision) return false;
    syncControls();
    return guard && m_pending && m_pendingRevision == revision;
}

void MavFTPUIView::admitPending(const QByteArray &data)
{
    if (!m_service || !m_pending) return;
    const auto operation = m_pendingOperation;
    const QString path = m_pendingRemotePath;
    const VehicleTargetLease target = m_pendingTarget;
    const quint64 revision = m_pendingRevision;
    if (!targetIsCurrent(target)) {
        cancelOwnedFlow(tr(
            "The active vehicle changed before the MAVFTP request was sent."));
        return;
    }
    QPointer<MavFTPUIView> guard(this);
    const auto result = m_service->startOperationForTarget(
        operation, path, data, target, &m_pendingOperationId);
    if (!guard || m_pendingRevision != revision) return;
    if (!finishStart(result, operation, path)) return;
    if (!pendingTargetIsCurrent()) {
        cancelOwnedFlow(tr(
            "The active vehicle changed while the MAVFTP request was admitted."));
    }
}

bool MavFTPUIView::finishStart(
    MavFtpServiceInterface::StartResult result,
    MavFtpServiceInterface::Operation operation, const QString &remotePath)
{
    if (result == MavFtpServiceInterface::StartResult::Started) {
        return true;
    }
    // A test transport or loopback can complete synchronously inside start*().
    // Do not overwrite that terminal state with a late start result.
    if (m_pending && m_pendingOperation == operation
        && sameRemotePath(m_pendingRemotePath, remotePath)) {
        clearPending();
        m_status->setText(startFailureText(result));
        {
            const QSignalBlocker blocker(m_progress);
            m_progress->setRange(0, 100);
            setProgressValue(0);
        }
        syncControls();
    }
    return false;
}

void MavFTPUIView::clearPending()
{
    ++m_pendingRevision;
    m_pending = false;
    m_pendingOperationId = 0;
    m_pendingOperation = MavFtpServiceInterface::Operation::None;
    m_pendingRemotePath.clear();
    m_pendingLocalPath.clear();
    m_pendingDisplayName.clear();
    m_pendingParentPath.clear();
    m_pendingTarget = VehicleTargetLease();
    m_pendingDirectory = nullptr;
    m_pendingGeneration = 0;
    m_pendingListUpdatesEntries = false;
    m_pendingRootRefresh = false;
}

void MavFTPUIView::setProgressValue(int value)
{
    // Qt5's setValue() repaints after emitting valueChanged. A consumer may
    // delete this page from that signal, so publish only after Qt has returned.
    QProgressBar *const progress = m_progress;
    const int previous = progress->value();
    const bool notify = !progress->signalsBlocked();
    {
        const QSignalBlocker blocker(progress);
        progress->setValue(value);
    }
    if (notify && progress->value() != previous)
        emit progress->valueChanged(progress->value());
}

void MavFTPUIView::syncControls()
{
    const bool serviceAvailable = !m_closeInProgress && !m_service.isNull();
    const bool busy = m_pending || m_prompt
        || (m_service && m_service->isBusy());
    const bool hasDirectory = !selectedDirectoryPath().isEmpty();
    const bool hasEntry = !selectedEntryPath().isEmpty();
    const bool directoryEntry = hasEntry && selectedEntryIsDirectory();

    m_refresh->setEnabled(serviceAvailable && !busy);
    m_download->setEnabled(serviceAvailable && !busy && hasEntry
                           && !directoryEntry);
    m_upload->setEnabled(serviceAvailable && !busy && hasDirectory);
    m_delete->setEnabled(serviceAvailable && !busy && hasEntry);
    m_newFolderName->setEnabled(serviceAvailable && !busy && hasDirectory);
    m_mkdir->setEnabled(serviceAvailable && !busy && hasDirectory
                        && folderNameError().isEmpty());
    const bool ownsActiveOperation = m_pendingOperationId && m_service
        && m_service->activeOperationId() == m_pendingOperationId;
    m_cancel->setEnabled(serviceAvailable && m_pending
                         && (m_prompt || ownsActiveOperation));
    m_directories->setEnabled(!busy);
    m_entries->setEnabled(!busy);
}

QString MavFTPUIView::folderNameError() const
{
    const QString name = m_newFolderName->text().trimmed();
    if (name.isEmpty()) {
        return tr("Enter a new folder name.");
    }
    if (name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))
        || name.contains(QChar::Null)) {
        return tr("A folder name cannot be '.', '..', or contain path separators.");
    }
    const QString path = combineRemotePath(selectedDirectoryPath(), name);
    if (path.toUtf8().size() > MavFtpProtocol::MaximumPathBytes) {
        return tr("The UTF-8 remote path is too long for MAVFTP.");
    }
    return QString();
}

QString MavFTPUIView::selectedDirectoryPath() const
{
    QTreeWidgetItem *const current = m_directories->currentItem();
    return current && !current->data(0, PlaceholderRole).toBool()
        ? current->data(0, PathRole).toString() : QString();
}

QString MavFTPUIView::selectedEntryPath() const
{
    const int row = m_entries->currentRow();
    QTableWidgetItem *const item = row >= 0 ? m_entries->item(row, 0) : nullptr;
    return item ? item->data(PathRole).toString() : QString();
}

QString MavFTPUIView::selectedEntryName() const
{
    const int row = m_entries->currentRow();
    QTableWidgetItem *const item = row >= 0 ? m_entries->item(row, 0) : nullptr;
    return item ? item->text() : QString();
}

bool MavFTPUIView::selectedEntryIsDirectory() const
{
    const int row = m_entries->currentRow();
    QTableWidgetItem *const item = row >= 0 ? m_entries->item(row, 0) : nullptr;
    return item && item->data(LoadedRole).toBool();
}

QTreeWidgetItem *MavFTPUIView::findDirectoryChild(
    QTreeWidgetItem *parent, const QString &path) const
{
    if (!parent) {
        return nullptr;
    }
    for (int index = 0; index < parent->childCount(); ++index) {
        QTreeWidgetItem *const child = parent->child(index);
        if (child && sameRemotePath(child->data(0, PathRole).toString(), path)) {
            return child;
        }
    }
    return nullptr;
}

QString MavFTPUIView::combineRemotePath(const QString &directory,
                                        const QString &name)
{
    if (directory.isEmpty() || directory == QStringLiteral("/")) {
        return QStringLiteral("/") + name;
    }
    return directory.endsWith(QLatin1Char('/'))
        ? directory + name : directory + QLatin1Char('/') + name;
}

QString MavFTPUIView::uniqueDownloadPath(const QString &directory,
                                         const QString &fileName)
{
    QDir target(directory);
    QString candidate = target.filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QFileInfo original(fileName);
    const QString suffix = original.completeSuffix();
    const QString base = suffix.isEmpty()
        ? fileName : fileName.left(fileName.size() - suffix.size() - 1);
    for (int copy = 1; copy < std::numeric_limits<int>::max(); ++copy) {
        const QString numbered = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(copy)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(copy).arg(suffix);
        candidate = target.filePath(numbered);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return target.filePath(fileName + QStringLiteral(".download"));
}

QString MavFTPUIView::startFailureText(
    MavFtpServiceInterface::StartResult result)
{
    switch (result) {
    case MavFtpServiceInterface::StartResult::Started:
        return QString();
    case MavFtpServiceInterface::StartResult::Busy:
        return tr("Another MAVFTP operation is already active.");
    case MavFtpServiceInterface::StartResult::NoTarget:
        return tr("No current exact vehicle target is available.");
    case MavFtpServiceInterface::StartResult::StaleTarget:
        return tr("The active vehicle changed; start the MAVFTP operation again.");
    case MavFtpServiceInterface::StartResult::InvalidPath:
        return tr("The remote MAVFTP path is invalid or too long.");
    case MavFtpServiceInterface::StartResult::InvalidData:
        return tr("The local file data cannot be transferred over MAVFTP.");
    case MavFtpServiceInterface::StartResult::TransportUnavailable:
        return tr("The selected MAVLink transport is unavailable.");
    case MavFtpServiceInterface::StartResult::ShuttingDown:
        return tr("MAVFTP is shutting down.");
    }
    return tr("MAVFTP could not start the operation.");
}

bool MavFTPUIView::writeDownloadedFile(const QByteArray &data, QString *error)
{
    if (m_pendingLocalPath.isEmpty()) {
        if (error) {
            *error = tr("No local download destination was selected.");
        }
        return false;
    }

    // Re-check immediately before committing so a file created while the
    // transfer was active is never intentionally overwritten.
    const QFileInfo requested(m_pendingLocalPath);
    if (requested.exists()) {
        m_pendingLocalPath = uniqueDownloadPath(
            requested.absolutePath(), requested.fileName());
    }
    QSaveFile file(m_pendingLocalPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = tr("Could not create %1: %2")
                         .arg(m_pendingLocalPath, file.errorString());
        }
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error) {
            *error = tr("Could not write %1: %2")
                         .arg(m_pendingLocalPath, file.errorString());
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = tr("Could not commit %1: %2")
                         .arg(m_pendingLocalPath, file.errorString());
        }
        return false;
    }
    return true;
}
