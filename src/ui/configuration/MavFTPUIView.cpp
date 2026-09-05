#include "MavFTPUIView.h"

#include <QAbstractItemView>
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
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

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
    resetRoots();

    if (!m_service) {
        m_status->setText(tr("MAVFTP service is unavailable."));
    } else {
        m_status->setText(tr(
            "Connect over MAVLink, then Refresh to browse the remote filesystem."));
    }
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
    m_progress->setValue(0);
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
    connect(m_service, &QObject::destroyed, this, [this]() {
        m_service = nullptr;
        clearPending();
        m_status->setText(tr("MAVFTP service is unavailable."));
        syncControls();
    });
}

void MavFTPUIView::resetRoots()
{
    const QSignalBlocker blocker(m_directories);
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

    beginPending(MavFtpServiceInterface::Operation::ListDirectory, path);
    m_pendingDirectory = item;
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
    if (fileName.isEmpty() || fileName == QStringLiteral(".")
        || fileName == QStringLiteral("..")
        || QFileInfo(fileName).fileName() != fileName
        || fileName.contains(QLatin1Char('\\'))) {
        m_status->setText(tr("The remote file name is not safe for a local download."));
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Select download folder"));
    if (directory.isEmpty()) {
        return;
    }

    const QString remotePath = selectedEntryPath();
    beginPending(MavFtpServiceInterface::Operation::Download, remotePath);
    m_pendingDisplayName = fileName;
    m_pendingLocalPath = uniqueDownloadPath(directory, fileName);
    m_status->setText(tr("Download %1").arg(fileName));
    admitPending();
}

void MavFTPUIView::uploadFile()
{
    const QString directory = selectedDirectoryPath();
    if (!m_service || m_service->isBusy() || m_pending || directory.isEmpty()) {
        m_status->setText(tr("Select a destination directory first."));
        return;
    }
    const QString localPath = QFileDialog::getOpenFileName(
        this, tr("Select file to upload"));
    if (localPath.isEmpty()) {
        return;
    }

    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) {
        m_status->setText(tr("The selected upload file is unavailable."));
        return;
    }
    if (info.size() > MaximumBufferedTransferBytes) {
        m_status->setText(tr(
            "This MAVFTP build buffers transfers and accepts files up to %1 MiB.")
                              .arg(MaximumBufferedTransferBytes / (1024 * 1024)));
        return;
    }
    if (info.size() > std::numeric_limits<quint32>::max()) {
        m_status->setText(tr("MAVFTP paths cannot transfer files larger than 4 GiB."));
        return;
    }
    if (QMessageBox::warning(
            this, tr("Upload remote file"),
            tr("Upload %1 to the connected vehicle?\n\n"
               "An existing remote file with the same name will be replaced. "
               "If the transfer is interrupted, a partial remote file may remain.")
                .arg(info.fileName()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_status->setText(tr("Could not open %1: %2")
                              .arg(info.fileName(), file.errorString()));
        return;
    }
    const QByteArray data = file.read(MaximumBufferedTransferBytes + 1);
    if (data.size() > MaximumBufferedTransferBytes) {
        m_status->setText(tr(
            "The upload file grew beyond this build's %1 MiB buffer limit.")
                              .arg(MaximumBufferedTransferBytes / (1024 * 1024)));
        return;
    }
    if (data.size() != info.size()) {
        m_status->setText(tr("Could not read all of %1: %2")
                              .arg(info.fileName(), file.errorString()));
        return;
    }

    const QString remotePath = combineRemotePath(directory, info.fileName());
    beginPending(MavFtpServiceInterface::Operation::Upload, remotePath);
    m_pendingDisplayName = info.fileName();
    m_status->setText(tr("Upload %1").arg(info.fileName()));
    admitPending(data);
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

    if (QMessageBox::warning(
            this, tr("Delete remote item"),
            tr("Delete %1 from the connected vehicle?\n\n%2").arg(name, path),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    const bool directory = selectedEntryIsDirectory();
    const auto operation = directory
        ? MavFtpServiceInterface::Operation::RemoveDirectory
        : MavFtpServiceInterface::Operation::RemoveFile;
    beginPending(operation, path);
    m_pendingDisplayName = name;
    m_status->setText(tr("Delete %1").arg(name));
    admitPending();
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
    beginPending(MavFtpServiceInterface::Operation::MakeDirectory, path);
    m_pendingDisplayName = name;
    m_status->setText(tr("Create %1").arg(path));
    admitPending();
}

void MavFTPUIView::cancelOperation()
{
    if (!m_service || !m_pending || !m_service->isBusy()
        || !m_pendingOperationId
        || m_service->activeOperationId() != m_pendingOperationId
        || m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    m_status->setText(tr("Cancelling MAVFTP operation…"));
    m_cancel->setEnabled(false);
    QPointer<MavFTPUIView> guard(this);
    m_service->cancelOperation(m_pendingOperationId);
    if (guard) syncControls();
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
    QTreeWidgetItem *child = findDirectoryChild(parent, path);
    if (!child && parent) {
        child = new QTreeWidgetItem(parent, {selectedEntryName()});
        child->setData(0, PathRole, path);
        child->setData(0, LoadedRole, false);
        child->setData(0, PlaceholderRole, false);
        auto *placeholder = new QTreeWidgetItem(child, {QStringLiteral("…")});
        placeholder->setData(0, PlaceholderRole, true);
    }
    if (child) {
        parent->setExpanded(true);
        m_directories->setCurrentItem(child);
        child->setExpanded(true);
    }
}

void MavFTPUIView::handleResult(
    const MavFtpServiceInterface::Result &result)
{
    if (!m_pending || !m_pendingOperationId || result.operationId != m_pendingOperationId
        || result.operation != m_pendingOperation
        || (!result.remotePath.isEmpty()
            && !sameRemotePath(result.remotePath, m_pendingRemotePath))
        || (m_pendingGeneration != 0
            && result.targetGeneration != m_pendingGeneration)) {
        syncControls();
        return;
    }

    const auto operation = m_pendingOperation;
    const QString remotePath = m_pendingRemotePath;
    const QString localPath = m_pendingLocalPath;
    const QString displayName = m_pendingDisplayName;
    QTreeWidgetItem *const directory = m_pendingDirectory;
    const bool updateEntries = m_pendingListUpdatesEntries;
    const bool rootRefresh = m_pendingRootRefresh;
    clearPending();

    m_progress->setRange(0, 100);
    if (result.cancelled) {
        m_progress->setValue(0);
        m_status->setText(result.error.isEmpty()
                              ? tr("MAVFTP operation cancelled.")
                              : result.error);
        syncControls();
        return;
    }
    if (!result.error.isEmpty()) {
        m_progress->setValue(0);
        m_status->setText(result.error);
        syncControls();
        return;
    }

    switch (operation) {
    case MavFtpServiceInterface::Operation::ListDirectory:
        populateDirectory(directory, result.entries, updateEntries, rootRefresh);
        m_status->setText(tr("Ready."));
        m_progress->setValue(100);
        break;
    case MavFtpServiceInterface::Operation::Download: {
        if (result.data.size() > MaximumBufferedTransferBytes) {
            m_status->setText(tr(
                "The downloaded file exceeds this build's %1 MiB buffer limit.")
                                  .arg(MaximumBufferedTransferBytes
                                       / (1024 * 1024)));
            m_progress->setValue(0);
            break;
        }
        m_pendingLocalPath = localPath;
        QString error;
        if (writeDownloadedFile(result.data, &error)) {
            m_status->setText(tr("Downloaded %1 to %2")
                                  .arg(displayName, m_pendingLocalPath));
            m_progress->setValue(100);
        } else {
            m_status->setText(error);
            m_progress->setValue(0);
        }
        m_pendingLocalPath.clear();
        break;
    }
    case MavFtpServiceInterface::Operation::Upload:
        m_status->setText(tr("Uploaded %1.").arg(displayName));
        m_progress->setValue(100);
        QTimer::singleShot(0, this, &MavFTPUIView::refreshCurrentDirectory);
        break;
    case MavFtpServiceInterface::Operation::MakeDirectory:
        m_newFolderName->clear();
        m_status->setText(tr("Created %1.").arg(remotePath));
        markDirectoryStale(m_directories->currentItem());
        m_progress->setValue(100);
        QTimer::singleShot(0, this, &MavFTPUIView::refreshCurrentDirectory);
        break;
    case MavFtpServiceInterface::Operation::RemoveFile:
    case MavFtpServiceInterface::Operation::RemoveDirectory:
        m_status->setText(tr("Deleted %1.").arg(displayName));
        markDirectoryStale(m_directories->currentItem());
        m_progress->setValue(100);
        QTimer::singleShot(0, this, &MavFTPUIView::refreshCurrentDirectory);
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
        || (m_pendingGeneration != 0
                       && generation != m_pendingGeneration)) {
        return;
    }
    if (m_pendingGeneration == 0) {
        m_pendingGeneration = generation;
    }
    if (total <= 0) {
        m_progress->setRange(0, 0);
        return;
    }
    m_progress->setRange(0, 100);
    const qint64 percent = qBound<qint64>(
        0, (qMax<qint64>(0, completed) * 100) / total, 100);
    m_progress->setValue(static_cast<int>(percent));
}

void MavFTPUIView::populateDirectory(
    QTreeWidgetItem *directory,
    const QVector<MavFtpProtocol::DirectoryEntry> &entries,
    bool updateEntries, bool rootRefresh)
{
    if (!directory) {
        return;
    }
    setDirectoryChildren(directory, entries);
    if (updateEntries && directory == m_directories->currentItem()) {
        setEntries(entries);
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
            auto *placeholder = new QTreeWidgetItem(
                systemRoot, {QStringLiteral("…")});
            placeholder->setData(0, PlaceholderRole, true);
        }
    }
}

void MavFTPUIView::setDirectoryChildren(
    QTreeWidgetItem *directory,
    const QVector<MavFtpProtocol::DirectoryEntry> &entries)
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
        auto *placeholder = new QTreeWidgetItem(child, {QStringLiteral("…")});
        placeholder->setData(0, PlaceholderRole, true);
    }
    directory->setData(0, LoadedRole, true);
}

void MavFTPUIView::setEntries(
    const QVector<MavFtpProtocol::DirectoryEntry> &entries)
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
    syncControls();
}

void MavFTPUIView::markDirectoryStale(QTreeWidgetItem *directory)
{
    if (!directory) {
        return;
    }
    directory->setData(0, LoadedRole, false);
}

void MavFTPUIView::beginPending(
    MavFtpServiceInterface::Operation operation, const QString &remotePath)
{
    m_pending = true;
    ++m_pendingRevision;
    m_pendingOperationId = 0;
    m_pendingOperation = operation;
    m_pendingRemotePath = remotePath;
    m_pendingLocalPath.clear();
    m_pendingDisplayName.clear();
    m_pendingDirectory = nullptr;
    m_pendingGeneration = 0;
    m_pendingListUpdatesEntries = false;
    m_pendingRootRefresh = false;
    m_cancelRequested = false;
    m_progress->setRange(0, 0);
    syncControls();
}

void MavFTPUIView::admitPending(const QByteArray &data)
{
    if (!m_service || !m_pending) return;
    const auto operation = m_pendingOperation;
    const QString path = m_pendingRemotePath;
    const quint64 revision = m_pendingRevision;
    QPointer<MavFTPUIView> guard(this);
    const auto result = m_service->startOperation(operation, path, data, &m_pendingOperationId);
    if (guard && m_pendingRevision == revision)
        finishStart(result, operation, path);
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
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_status->setText(startFailureText(result));
        syncControls();
    }
    return false;
}

void MavFTPUIView::clearPending()
{
    m_pending = false;
    m_pendingOperationId = 0;
    m_pendingOperation = MavFtpServiceInterface::Operation::None;
    m_pendingRemotePath.clear();
    m_pendingDirectory = nullptr;
    m_pendingGeneration = 0;
    m_pendingListUpdatesEntries = false;
    m_pendingRootRefresh = false;
    m_cancelRequested = false;
}

void MavFTPUIView::syncControls()
{
    const bool serviceAvailable = !m_service.isNull();
    const bool busy = m_pending || (m_service && m_service->isBusy());
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
    m_cancel->setEnabled(serviceAvailable && m_pending && m_pendingOperationId
                         && m_service->activeOperationId() == m_pendingOperationId
                         && !m_cancelRequested);
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
