#include "LogIndexWindow.h"

#include "Loghandling/LogIndexFiles.h"
#include "Loghandling/LogIndexService.h"

#include <QtConcurrentRun>

#include <QAbstractItemView>
#include <QBuffer>
#include <QCache>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <utility>

namespace
{
enum Column {
    MapColumn,
    DateColumn,
    DirectoryColumn,
    FrameColumn,
    SystemIdColumn,
    DurationColumn,
    NameColumn,
    SizeColumn,
    HomeColumn,
    TimeInAirColumn,
    DistanceColumn,
    CameraColumn,
    WarningColumn,
    ColumnCount
};

constexpr int SortRole = Qt::UserRole + 1;
constexpr int EntryIndexRole = Qt::UserRole + 2;
constexpr int ThumbnailBytesRole = Qt::UserRole + 3;
constexpr int ThumbnailPathRole = Qt::UserRole + 4;
constexpr int ThumbnailKeyRole = Qt::UserRole + 5;
constexpr int ThumbnailSizeRole = Qt::UserRole + 6;
constexpr int ThumbnailModifiedRole = Qt::UserRole + 7;
constexpr qint64 MaximumThumbnailBytes = 20LL * 1024 * 1024;
constexpr qint64 MaximumThumbnailPixels = 4LL * 1024 * 1024;

QImage decodeBoundedThumbnail(QIODevice *device, qint64 size)
{
    if (!device || size <= 0 || size > MaximumThumbnailBytes)
        return {};
    QImageReader reader(device, "JPEG");
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.width() > 4096
        || dimensions.height() > 4096
        || qint64(dimensions.width()) * dimensions.height()
            > MaximumThumbnailPixels) {
        return {};
    }
    return reader.read();
}

QString durationText(double seconds)
{
    const qint64 total = qMax<qint64>(0, qRound64(seconds));
    const qint64 days = total / 86400;
    const qint64 hours = (total / 3600) % 24;
    const qint64 minutes = (total / 60) % 60;
    const qint64 remainder = total % 60;
    const QString clock = QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainder, 2, 10, QLatin1Char('0'));
    return days > 0 ? QStringLiteral("%1.%2").arg(days).arg(clock) : clock;
}

QString sizeText(qint64 bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB"};
    double value = qMax<qint64>(0, bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0
        ? QStringLiteral("%1 B").arg(qint64(value))
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 1)
              .arg(QString::fromLatin1(units[unit]));
}

QString distanceText(double meters)
{
    return meters >= 1000.0
        ? LogIndexWindow::tr("%1 km").arg(meters / 1000.0, 0, 'f', 2)
        : LogIndexWindow::tr("%1 m").arg(meters, 0, 'f', 0);
}

QString homeText(const LogIndex::Home &home)
{
    if (!home.valid)
        return QStringLiteral("—");
    return QStringLiteral("%1, %2, %3 m")
        .arg(home.latitude, 0, 'f', 6)
        .arg(home.longitude, 0, 'f', 6)
        .arg(home.altitudeMeters, 0, 'f', 1);
}

QString warningSummary(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return {};
    const int displayed = qMin(3, warnings.size());
    QString summary = warnings.mid(0, displayed).join(QStringLiteral(" "));
    if (warnings.size() > displayed) {
        summary += LogIndexWindow::tr(" (+%1 more warning(s))")
            .arg(warnings.size() - displayed);
    }
    return summary;
}

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

QString pathKey(const QString &path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

class ThumbnailDelegate final : public QStyledItemDelegate
{
public:
    explicit ThumbnailDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
        m_cache.setMaxCost(128);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem background(option);
        background.text.clear();
        QStyledItemDelegate::paint(painter, background, index);
        const QString key = index.data(ThumbnailKeyRole).toString();
        QPixmap *cached = key.isEmpty() ? nullptr : m_cache.object(key);
        if (!cached && !key.isEmpty()) {
            QImage image;
            const QByteArray bytes = index.data(ThumbnailBytesRole).toByteArray();
            if (!bytes.isEmpty() && bytes.size() <= MaximumThumbnailBytes) {
                QBuffer buffer;
                buffer.setData(bytes);
                if (buffer.open(QIODevice::ReadOnly))
                    image = decodeBoundedThumbnail(&buffer, bytes.size());
            }
            if (image.isNull()) {
                const QString path = index.data(ThumbnailPathRole).toString();
                const qint64 expectedSize = index.data(ThumbnailSizeRole).toLongLong();
                const QDateTime expectedModified =
                    index.data(ThumbnailModifiedRole).toDateTime();
                const QFileInfo before(path);
                if (!path.isEmpty() && before.exists() && before.isFile()
                    && !before.isSymLink() && before.size() == expectedSize
                    && before.lastModified().toUTC() == expectedModified
                    && samePath(QFileInfo(path).absoluteFilePath(),
                                before.canonicalFilePath())) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly)) {
                        const QByteArray sidecar =
                            file.read(MaximumThumbnailBytes + 1);
                        if (sidecar.size() == expectedSize
                            && file.error() == QFileDevice::NoError) {
                            QBuffer buffer;
                            buffer.setData(sidecar);
                            if (buffer.open(QIODevice::ReadOnly)) {
                                image = decodeBoundedThumbnail(
                                    &buffer, sidecar.size());
                            }
                        }
                        const QFileInfo after(path);
                        if (!after.exists() || after.isSymLink()
                            || after.size() != expectedSize
                            || after.lastModified().toUTC() != expectedModified
                            || !samePath(QFileInfo(path).absoluteFilePath(),
                                         after.canonicalFilePath())) {
                            image = QImage();
                        }
                    }
                }
            }
            if (!image.isNull()) {
                auto *preview = new QPixmap(QPixmap::fromImage(image).scaled(
                    120, 70, Qt::KeepAspectRatioByExpanding,
                    Qt::SmoothTransformation));
                m_cache.insert(key, preview, 1);
                cached = preview;
            }
        }
        if (!cached)
            return;
        const QRect target(option.rect.center().x() - 60,
                           option.rect.center().y() - 35, 120, 70);
        painter->save();
        painter->setClipRect(option.rect);
        painter->drawPixmap(target, *cached,
            QRect((cached->width() - 120) / 2,
                  (cached->height() - 70) / 2, 120, 70));
        painter->restore();
    }

private:
    mutable QCache<QString, QPixmap> m_cache;
};
} // namespace

struct LogIndexWindow::JobState
{
    enum class Kind { Scan, PrepareDelete, Delete };
    explicit JobState(Kind value) : kind(value) {}
    Kind kind;
    std::atomic_bool cancelled{false};
    std::atomic_int completed{0};
    std::atomic_int total{1};
    QMutex mutex;
    QString currentPath;
};

struct LogIndexWindow::DeleteContext
{
    QString root;
    QVector<LogIndex::Entry> entries;
    LogIndexFiles::DeletePlan plan;
    QStringList warnings;
};

LogIndexWindow::LogIndexWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("LogIndexWindow"));
    setWindowTitle(tr("Flight Log Index"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1380, 760);
    setMinimumSize(900, 520);
    buildUi();

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &LogIndexWindow::updateProgress);
    m_progressTimer->start();
    refreshControls();
}

LogIndexWindow::~LogIndexWindow()
{
    m_closing = true;
    ++m_revision;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_relaxed);
    if (m_directoryDialog) {
        m_directoryDialog->blockSignals(true);
        m_directoryDialog.clear();
    }
    if (m_deletePrompt) {
        m_deletePrompt->blockSignals(true);
        m_deletePrompt.clear();
    }
    if (m_closePrompt) {
        m_closePrompt->blockSignals(true);
        m_closePrompt.clear();
    }
}

void LogIndexWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(new QLabel(tr("Directory"), this));
    m_directory = new QLineEdit(this);
    m_directory->setObjectName(QStringLiteral("LogIndexDirectory"));
    m_directory->setReadOnly(true);
    directoryRow->addWidget(m_directory, 1);
    m_defaultDirectoryButton = new QPushButton(tr("Default Log Dir"), this);
    m_defaultDirectoryButton->setObjectName(QStringLiteral("DefaultDirectoryButton"));
    m_defaultTlogDirectoryButton = new QPushButton(tr("Default TLOG Dir"), this);
    m_defaultTlogDirectoryButton->setObjectName(QStringLiteral("DefaultTlogDirectoryButton"));
    m_chooseDirectoryButton = new QPushButton(tr("Custom Directory…"), this);
    m_chooseDirectoryButton->setObjectName(QStringLiteral("ChooseDirectoryButton"));
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("RefreshButton"));
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setObjectName(QStringLiteral("CancelButton"));
    directoryRow->addWidget(m_defaultDirectoryButton);
    directoryRow->addWidget(m_defaultTlogDirectoryButton);
    directoryRow->addWidget(m_chooseDirectoryButton);
    directoryRow->addWidget(m_refreshButton);
    directoryRow->addWidget(m_cancelButton);
    root->addLayout(directoryRow);

    auto *hint = new QLabel(
        tr("Double-click a row to open it in Log Browser. Ctrl/Shift select multiple rows."), this);
    hint->setTextFormat(Qt::PlainText);
    root->addWidget(hint);

    m_model = new QStandardItemModel(0, ColumnCount, this);
    m_model->setHorizontalHeaderLabels({
        tr("Map"), tr("Date"), tr("Directory"), tr("Frame"),
        tr("Aircraft / sysid"), tr("Duration"), tr("Name"), tr("Size"),
        tr("Home"), tr("Time in air"), tr("Distance"), tr("CAM"),
        tr("Read warning")});
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(SortRole);
    m_proxy->setDynamicSortFilter(true);
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("LogGrid"));
    m_table->setModel(m_proxy);
    m_table->setItemDelegateForColumn(MapColumn,
                                      new ThumbnailDelegate(m_table));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->setDefaultSectionSize(78);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionsMovable(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(MapColumn, 130);
    m_table->setColumnWidth(DateColumn, 145);
    m_table->setColumnWidth(DirectoryColumn, 220);
    m_table->setColumnWidth(FrameColumn, 115);
    m_table->setColumnWidth(SystemIdColumn, 105);
    m_table->setColumnWidth(DurationColumn, 85);
    m_table->setColumnWidth(NameColumn, 180);
    m_table->setColumnWidth(SizeColumn, 80);
    m_table->setColumnWidth(HomeColumn, 230);
    m_table->setColumnWidth(TimeInAirColumn, 90);
    m_table->setColumnWidth(DistanceColumn, 85);
    m_table->setColumnWidth(CameraColumn, 60);
    root->addWidget(m_table, 1);

    auto *selectionRow = new QHBoxLayout;
    m_selectionSummary = new QLabel(tr("No logs selected."), this);
    m_selectionSummary->setObjectName(QStringLiteral("LogIndexSelectedSummary"));
    m_selectionSummary->setTextFormat(Qt::PlainText);
    selectionRow->addWidget(m_selectionSummary, 1);
    m_deleteButton = new QPushButton(tr("Delete Selected…"), this);
    m_deleteButton->setObjectName(QStringLiteral("DeleteButton"));
    m_deleteButton->setStyleSheet(QStringLiteral("QPushButton { color: #ff8888; }"));
    m_deleteButton->setAutoDefault(false);
    selectionRow->addWidget(m_deleteButton);
    root->addLayout(selectionRow);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("LogIndexProgressBar"));
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    root->addWidget(m_progress);
    m_status = new QLabel(
        tr("Choose a log directory or refresh a default one."), this);
    m_status->setObjectName(QStringLiteral("LogIndexStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    connect(m_defaultDirectoryButton, &QPushButton::clicked, this, [this]() {
        scanDirectory(m_defaultDataflashDirectory);
    });
    connect(m_defaultTlogDirectoryButton, &QPushButton::clicked, this, [this]() {
        scanDirectory(m_defaultTlogDirectory);
    });
    connect(m_chooseDirectoryButton, &QPushButton::clicked,
            this, &LogIndexWindow::chooseDirectory);
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        scanDirectory(m_currentRoot);
    });
    connect(m_cancelButton, &QPushButton::clicked,
            this, &LogIndexWindow::cancel);
    connect(m_deleteButton, &QPushButton::clicked,
            this, &LogIndexWindow::prepareDeleteSelected);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &LogIndexWindow::updateSelectionSummary);
    connect(m_table, &QTableView::doubleClicked, this,
            [this](const QModelIndex &proxyIndex) {
        if (!proxyIndex.isValid())
            return;
        const QModelIndex source = m_proxy->mapToSource(proxyIndex);
        if (!source.isValid())
            return;
        const int entryIndex = source.sibling(source.row(), 0)
                                   .data(EntryIndexRole).toInt();
        if (entryIndex >= 0 && entryIndex < m_entries.size()) {
            const QString path = m_entries.at(entryIndex).fullPath;
            emit openLogRequested(path);
        }
    });
}

void LogIndexWindow::setDefaultDirectories(QString dataflash, QString tlog)
{
    if (m_closing)
        return;
    m_defaultDataflashDirectory = QFileInfo(dataflash.trimmed()).absoluteFilePath();
    m_defaultTlogDirectory = QFileInfo(tlog.trimmed()).absoluteFilePath();
    if (dataflash.trimmed().isEmpty())
        m_defaultDataflashDirectory.clear();
    if (tlog.trimmed().isEmpty())
        m_defaultTlogDirectory.clear();
    if (m_currentRoot.isEmpty()) {
        m_currentRoot = !m_defaultDataflashDirectory.isEmpty()
            ? m_defaultDataflashDirectory : m_defaultTlogDirectory;
        const QSignalBlocker blocker(m_directory);
        m_directory->setText(m_currentRoot);
        m_directory->setToolTip(m_currentRoot);
    }
    refreshControls();
}

void LogIndexWindow::setTileReaderFactory(
    std::function<LogIndex::TileReader()> factory)
{
    if (!m_closing)
        m_tileReaderFactory = std::move(factory);
}

bool LogIndexWindow::busy() const noexcept
{
    return m_admitting || bool(m_job) || m_directoryDialog
        || m_deletePrompt || m_closePrompt;
}

void LogIndexWindow::scanDirectory(QString root)
{
    root = root.trimmed();
    if (m_closing || busy() || root.isEmpty())
        return;
    const QString requested = QFileInfo(root).absoluteFilePath();
    const quint64 revision = ++m_revision;
    m_admitting = true;
    refreshControls();
    const QPointer<LogIndexWindow> guard(this);
    LogIndex::TileReader tiles;
    std::function<LogIndex::TileReader()> factory;
    try {
        factory = m_tileReaderFactory;
        if (factory)
            tiles = factory();
    } catch (...) {
        if (guard && revision == m_revision) {
            m_admitting = false;
            m_status->setText(tr("Cannot create the cache-only map tile reader."));
            refreshControls();
        }
        return;
    }
    if (!guard || m_closing || revision != m_revision)
        return;
    m_admitting = false;
    startScan(requested, std::move(tiles), revision);
}

void LogIndexWindow::startScan(QString root, LogIndex::TileReader tiles,
                               quint64 revision)
{
    auto state = std::make_shared<JobState>(JobState::Kind::Scan);
    m_job = state;
    m_status->setText(tr("Discovering flight logs…"));
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
    }
    auto *watcher = new QFutureWatcher<LogIndex::ScanResult>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<LogIndex::ScanResult>::finished,
            this, [this, watcher, state, revision]() {
        const LogIndex::ScanResult result = watcher->result();
        watcher->deleteLater();
        finishScan(revision, state, result);
    });
    watcher->setFuture(QtConcurrent::run(
        [root = std::move(root), tiles = std::move(tiles), state]() {
        const auto cancel = [state]() {
            return state->cancelled.load(std::memory_order_relaxed);
        };
        const auto progress = [state](int completed, int total,
                                      const QString &path) {
            state->completed.store(completed, std::memory_order_relaxed);
            state->total.store(qMax(1, total), std::memory_order_relaxed);
            QMutexLocker locker(&state->mutex);
            state->currentPath = path;
        };
        return LogIndexService::scan(root, cancel, progress, tiles);
    }));
    refreshControls();
}

void LogIndexWindow::finishScan(
    quint64 revision, const std::shared_ptr<JobState> &state,
    const LogIndex::ScanResult &result)
{
    if (m_closing || revision != m_revision || m_job != state)
        return;
    completeJob(state);
    if (result.cancelled) {
        m_status->setText(tr("Log indexing cancelled."));
    } else if (!result.success) {
        m_status->setText(tr("Log indexing failed: %1").arg(result.error));
    } else {
        m_currentRoot = result.rootPath;
        {
            const QSignalBlocker blocker(m_directory);
            m_directory->setText(m_currentRoot);
            m_directory->setToolTip(m_currentRoot);
        }
        const int count = result.entries.size();
        const int errors = std::count_if(
            result.entries.cbegin(), result.entries.cend(),
            [](const LogIndex::Entry &entry) { return !entry.error.isEmpty(); });
        if (!replaceEntries(result.entries)
            || revision != m_revision || m_job) {
            return;
        }
        QString status = tr("Indexed %1 log(s).").arg(count);
        if (errors)
            status += tr(" %1 could only be read partially.").arg(errors);
        if (!result.warnings.isEmpty())
            status += QStringLiteral(" ") + warningSummary(result.warnings);
        m_status->setText(status);
        m_status->setToolTip(result.warnings.join(QLatin1Char('\n')));
        {
            const QSignalBlocker blocker(m_progress);
            m_progress->setRange(0, qMax(1, count));
            m_progress->setValue(count);
        }
    }
    refreshControls();
    closeAfterCancelledJob();
}

void LogIndexWindow::chooseDirectory()
{
    if (m_closing || busy())
        return;
    const quint64 revision = ++m_revision;
    auto *dialog = new QFileDialog(this, tr("Select directory containing flight logs"),
                                   m_currentRoot);
    dialog->setObjectName(QStringLiteral("LogIndexCustomDirectoryDialog"));
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_directoryDialog = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_closing || revision != m_revision
            || m_directoryDialog != dialog) {
            return;
        }
        m_directoryDialog.clear();
        const QString selected = result == QDialog::Accepted
            ? dialog->selectedFiles().value(0) : QString();
        refreshControls();
        if (!selected.isEmpty())
            scanDirectory(selected);
    });
    const QPointer<LogIndexWindow> guard(this);
    dialog->open();
    if (guard && !m_closing && revision == m_revision
        && m_directoryDialog == dialog) {
        refreshControls();
    }
}

QVector<LogIndex::Entry> LogIndexWindow::selectedEntries() const
{
    QVector<LogIndex::Entry> selected;
    QSet<int> seen;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &proxyIndex : rows) {
        const QModelIndex source = m_proxy->mapToSource(proxyIndex);
        const int index = source.sibling(source.row(), 0)
                              .data(EntryIndexRole).toInt();
        if (index >= 0 && index < m_entries.size() && !seen.contains(index)) {
            seen.insert(index);
            selected.append(m_entries.at(index));
        }
    }
    return selected;
}

void LogIndexWindow::prepareDeleteSelected()
{
    if (m_closing || busy() || m_currentRoot.isEmpty())
        return;
    const QVector<LogIndex::Entry> entries = selectedEntries();
    if (entries.isEmpty())
        return;
    const quint64 revision = ++m_revision;
    auto context = std::make_shared<DeleteContext>();
    context->root = m_currentRoot;
    context->entries = entries;
    m_deleteContext = context;
    auto state = std::make_shared<JobState>(JobState::Kind::PrepareDelete);
    m_job = state;
    m_status->setText(tr("Checking the exact selected log and companion files…"));
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, 0);
    }
    auto *watcher = new QFutureWatcher<LogIndexFiles::DeletePreparation>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<LogIndexFiles::DeletePreparation>::finished,
            this, [this, watcher, state, context, revision]() {
        const LogIndexFiles::DeletePreparation result = watcher->result();
        watcher->deleteLater();
        if (m_closing || revision != m_revision || m_job != state)
            return;
        context->plan = result.plan;
        context->warnings = result.warnings;
        completeJob(state);
        if (result.cancelled) {
            m_deleteContext.reset();
            m_status->setText(tr("Delete preparation cancelled; no files were removed."));
        } else if (!result.success || !result.plan.isValid()) {
            m_deleteContext.reset();
            m_status->setText(tr("Delete preparation failed: %1").arg(result.error));
        } else {
            const QPointer<LogIndexWindow> guard(this);
            finishDeletePreparation(revision, state, context);
            if (!guard)
                return;
        }
        refreshControls();
        closeAfterCancelledJob();
    });
    watcher->setFuture(QtConcurrent::run([context, state]() {
        return LogIndexFiles::prepareDelete(
            context->root, context->entries, [state]() {
                return state->cancelled.load(std::memory_order_relaxed);
            });
    }));
    refreshControls();
}

void LogIndexWindow::finishDeletePreparation(
    quint64 revision, const std::shared_ptr<JobState> &,
    const std::shared_ptr<DeleteContext> &context)
{
    if (m_closing || revision != m_revision
        || m_deleteContext != context || !context->plan.isValid()) {
        return;
    }
    showDeleteConfirmation(context, revision);
}

void LogIndexWindow::showDeleteConfirmation(
    const std::shared_ptr<DeleteContext> &context, quint64 revision)
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("LogIndexDeleteConfirmation"));
    dialog->setWindowTitle(tr("Delete indexed flight logs?"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(760, 480);
    auto *layout = new QVBoxLayout(dialog);
    auto *warning = new QLabel(tr(
        "Permanently delete %1 selected log(s) and every exact companion path "
        "listed below? The immutable plan will refuse changed files; deletion "
        "can still be partial if a later file fails or cancellation arrives. "
        "Stop and close active log writers first: size and modification time "
        "are checked, but these files are not locked.")
        .arg(context->entries.size()), dialog);
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto *paths = new QPlainTextEdit(dialog);
    paths->setObjectName(QStringLiteral("LogIndexDeletePaths"));
    paths->setReadOnly(true);
    paths->setPlainText(context->plan.paths().join(QLatin1Char('\n')));
    layout->addWidget(paths, 1);
    if (!context->warnings.isEmpty()) {
        auto *warnings = new QPlainTextEdit(dialog);
        warnings->setObjectName(QStringLiteral("LogIndexDeleteWarnings"));
        warnings->setReadOnly(true);
        warnings->setPlainText(context->warnings.join(QLatin1Char('\n')));
        warnings->setMaximumHeight(90);
        layout->addWidget(warnings);
    }
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *deleteButton = buttons->button(QDialogButtonBox::Ok);
    deleteButton->setObjectName(QStringLiteral("LogIndexDeleteConfirmButton"));
    deleteButton->setText(tr("Delete permanently"));
    deleteButton->setAutoDefault(false);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    m_deletePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, context, revision](int result) {
        if (m_closing || revision != m_revision
            || m_deletePrompt != dialog || m_deleteContext != context) {
            return;
        }
        m_deletePrompt.clear();
        if (result != QDialog::Accepted) {
            m_deleteContext.reset();
            m_status->setText(tr("Permanent deletion cancelled; no files were removed."));
            refreshControls();
            return;
        }
        executeDelete(context, revision);
    });
    const QPointer<LogIndexWindow> guard(this);
    dialog->open();
    if (guard && !m_closing && revision == m_revision
        && m_deletePrompt == dialog && m_deleteContext == context) {
        refreshControls();
    }
}

void LogIndexWindow::executeDelete(
    const std::shared_ptr<DeleteContext> &context, quint64 revision)
{
    if (m_closing || revision != m_revision
        || m_deleteContext != context || m_job
        || !context->plan.isValid()) {
        return;
    }
    auto state = std::make_shared<JobState>(JobState::Kind::Delete);
    m_job = state;
    m_status->setText(tr("Deleting only the confirmed exact file plan…"));
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, qMax(1, context->entries.size()));
        m_progress->setValue(0);
    }
    auto *watcher = new QFutureWatcher<LogIndex::DeleteResult>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<LogIndex::DeleteResult>::finished,
            this, [this, watcher, state, revision]() {
        const LogIndex::DeleteResult result = watcher->result();
        watcher->deleteLater();
        finishDelete(revision, state, result);
    });
    const LogIndexFiles::DeletePlan plan = context->plan;
    watcher->setFuture(QtConcurrent::run([plan, state]() {
        const auto progress = [state](int completed, int total,
                                      const QString &path) {
            state->completed.store(completed, std::memory_order_relaxed);
            state->total.store(qMax(1, total), std::memory_order_relaxed);
            QMutexLocker locker(&state->mutex);
            state->currentPath = path;
        };
        return LogIndexFiles::executeDelete(
            plan, [state]() {
                return state->cancelled.load(std::memory_order_relaxed);
            }, progress);
    }));
    refreshControls();
}

void LogIndexWindow::finishDelete(
    quint64 revision, const std::shared_ptr<JobState> &state,
    const LogIndex::DeleteResult &result)
{
    if (m_closing || revision != m_revision || m_job != state)
        return;
    completeJob(state);
    m_deleteContext.reset();
    if (!result.deletedLogs.isEmpty()) {
        QSet<QString> deletedPaths;
        deletedPaths.reserve(result.deletedLogs.size());
        for (const QString &path : result.deletedLogs)
            deletedPaths.insert(pathKey(path));
        QVector<LogIndex::Entry> remaining;
        remaining.reserve(m_entries.size());
        for (const LogIndex::Entry &entry : m_entries) {
            if (!deletedPaths.contains(pathKey(entry.fullPath)))
                remaining.append(entry);
        }
        if (!replaceEntries(std::move(remaining))
            || revision != m_revision || m_job) {
            return;
        }
    }
    QString status;
    if (result.cancelled) {
        status = tr("Deletion cancelled after %1 log(s); %2 planned log(s) remain.")
            .arg(result.deletedLogs.size()).arg(result.remaining);
    } else if (!result.success) {
        status = result.error.isEmpty()
            ? tr("Deletion completed only partially: %1 log(s) deleted; %2 remain.")
                  .arg(result.deletedLogs.size()).arg(result.remaining)
            : tr("Deletion stopped after %1 log(s): %2")
                  .arg(result.deletedLogs.size()).arg(result.error);
    } else {
        status = tr("Deleted %1 log(s).").arg(result.deletedLogs.size());
    }
    if (!result.warnings.isEmpty())
        status += QStringLiteral(" ") + warningSummary(result.warnings);
    const int total = qMax(1, state->total.load(std::memory_order_relaxed));
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, total);
        m_progress->setValue(std::clamp(
            state->completed.load(std::memory_order_relaxed), 0, total));
    }
    m_status->setText(status);
    m_status->setToolTip(result.warnings.join(QLatin1Char('\n')));
    refreshControls();
    closeAfterCancelledJob();
}

bool LogIndexWindow::replaceEntries(QVector<LogIndex::Entry> entries)
{
    {
        const QSignalBlocker proxyBlock(m_proxy);
        m_proxy->setDynamicSortFilter(false);
    }
    {
        const QSignalBlocker modelBlock(m_model);
        const QSignalBlocker selectionBlock(m_table->selectionModel());
        m_model->removeRows(0, m_model->rowCount());
        m_entries = std::move(entries);
        for (int index = 0; index < m_entries.size(); ++index) {
            const LogIndex::Entry &entry = m_entries.at(index);
            QList<QStandardItem *> row;
            row.reserve(ColumnCount);
            for (int column = 0; column < ColumnCount; ++column) {
                auto *item = new QStandardItem;
                item->setEditable(false);
                item->setData(index, EntryIndexRole);
                row.append(item);
            }
            const QDateTime date = entry.dateUtc.isValid()
                ? entry.dateUtc : entry.source.modifiedUtc;
            row[MapColumn]->setData(entry.thumbnailJpeg, ThumbnailBytesRole);
            if (entry.thumbnail.exists)
                row[MapColumn]->setData(entry.fullPath + QStringLiteral(".jpg"),
                                        ThumbnailPathRole);
            row[MapColumn]->setData(entry.thumbnail.sizeBytes,
                                    ThumbnailSizeRole);
            row[MapColumn]->setData(entry.thumbnail.modifiedUtc,
                                    ThumbnailModifiedRole);
            row[MapColumn]->setData(QStringLiteral("%1|%2|%3|%4|%5")
                .arg(entry.fullPath)
                .arg(entry.thumbnail.modifiedUtc.toMSecsSinceEpoch())
                .arg(entry.thumbnail.sizeBytes)
                .arg(entry.source.modifiedUtc.toMSecsSinceEpoch())
                .arg(entry.source.sizeBytes), ThumbnailKeyRole);
            row[MapColumn]->setData(entry.fullPath, SortRole);
            row[DateColumn]->setText(date.isValid()
                ? date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : QStringLiteral("—"));
            row[DateColumn]->setData(date, SortRole);
            row[DirectoryColumn]->setText(QFileInfo(entry.fullPath).absolutePath());
            row[FrameColumn]->setText(entry.frame);
            row[SystemIdColumn]->setText(entry.systemId
                ? QString::number(entry.systemId) : QStringLiteral("—"));
            row[SystemIdColumn]->setData(entry.systemId, SortRole);
            row[DurationColumn]->setText(durationText(entry.durationSeconds));
            row[DurationColumn]->setData(entry.durationSeconds, SortRole);
            row[NameColumn]->setText(QFileInfo(entry.fullPath).fileName());
            row[SizeColumn]->setText(sizeText(entry.source.sizeBytes));
            row[SizeColumn]->setData(entry.source.sizeBytes, SortRole);
            row[HomeColumn]->setText(homeText(entry.home));
            row[TimeInAirColumn]->setText(durationText(entry.timeInAirSeconds));
            row[TimeInAirColumn]->setData(entry.timeInAirSeconds, SortRole);
            row[DistanceColumn]->setText(distanceText(entry.distanceMeters));
            row[DistanceColumn]->setData(entry.distanceMeters, SortRole);
            row[CameraColumn]->setText(QString::number(entry.cameraMessages));
            row[CameraColumn]->setData(QVariant::fromValue(entry.cameraMessages), SortRole);
            row[WarningColumn]->setText(entry.error);
            for (QStandardItem *item : row) {
                if (!item->data(SortRole).isValid())
                    item->setData(item->text(), SortRole);
                item->setToolTip(item->text());
            }
            m_model->appendRow(row);
        }
    }
    m_selectionSummary->setText(tr("No logs selected."));
    {
        const QSignalBlocker proxyBlock(m_proxy);
        m_proxy->setDynamicSortFilter(true);
    }
    const QPointer<LogIndexWindow> guard(this);
    m_proxy->invalidate();
    if (!guard || m_closing || m_job)
        return false;
    m_table->sortByColumn(DateColumn, Qt::DescendingOrder);
    return guard && !m_closing;
}

void LogIndexWindow::updateSelectionSummary()
{
    if (m_closing)
        return;
    const QVector<LogIndex::Entry> selected = selectedEntries();
    if (selected.isEmpty()) {
        m_selectionSummary->setText(tr("No logs selected."));
    } else {
        double time = 0.0;
        double distance = 0.0;
        for (const LogIndex::Entry &entry : selected) {
            time += entry.timeInAirSeconds;
            distance += entry.distanceMeters;
        }
        m_selectionSummary->setText(
            tr("Selected: %1    Time in air: %2    Distance: %3")
                .arg(selected.size()).arg(durationText(time), distanceText(distance)));
    }
    refreshControls();
}

void LogIndexWindow::cancel()
{
    if (!m_job)
        return;
    m_job->cancelled.store(true, std::memory_order_relaxed);
    m_status->setText(tr("Cancellation requested; finishing the current bounded read or file mutation…"));
    refreshControls();
}

void LogIndexWindow::updateProgress()
{
    const std::shared_ptr<JobState> state = m_job;
    if (!state || m_closing)
        return;
    if (state->kind == JobState::Kind::PrepareDelete) {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, 0);
        return;
    }
    const int total = qMax(1, state->total.load(std::memory_order_relaxed));
    const int completed = std::clamp(
        state->completed.load(std::memory_order_relaxed), 0, total);
    {
        const QSignalBlocker blocker(m_progress);
        m_progress->setRange(0, total);
        m_progress->setValue(completed);
    }
    QString path;
    {
        QMutexLocker locker(&state->mutex);
        path = state->currentPath;
    }
    if (!path.isEmpty()) {
        const QString operation = state->kind == JobState::Kind::Scan
            ? tr("Indexing") : state->kind == JobState::Kind::Delete
                ? tr("Deleting") : tr("Checking");
        m_status->setText(tr("%1 %2/%3: %4")
            .arg(operation).arg(completed).arg(total)
            .arg(QFileInfo(path).fileName()));
        m_status->setToolTip(path);
    }
}

void LogIndexWindow::completeJob(const std::shared_ptr<JobState> &state)
{
    if (m_job == state)
        m_job.reset();
    m_watcher.clear();
}

void LogIndexWindow::refreshControls()
{
    if (m_closing)
        return;
    const bool operation = bool(m_job) || m_admitting;
    const bool prompt = m_directoryDialog || m_deletePrompt || m_closePrompt;
    const bool idle = !operation && !prompt;
    m_defaultDirectoryButton->setEnabled(
        idle && !m_defaultDataflashDirectory.isEmpty());
    m_defaultTlogDirectoryButton->setEnabled(
        idle && !m_defaultTlogDirectory.isEmpty());
    m_chooseDirectoryButton->setEnabled(idle);
    m_refreshButton->setEnabled(idle && !m_currentRoot.isEmpty());
    m_cancelButton->setEnabled(operation);
    m_deleteButton->setEnabled(
        idle && !m_table->selectionModel()->selectedRows().isEmpty());
    m_table->setEnabled(idle);
}

void LogIndexWindow::dismissDialog(QPointer<QDialog> &dialog)
{
    const QPointer<QDialog> current(dialog);
    dialog.clear();
    if (current) {
        current->blockSignals(true);
        current->reject();
        if (current)
            current->deleteLater();
    }
}

void LogIndexWindow::requestCloseWhileBusy(QCloseEvent *event)
{
    event->ignore();
    if (m_closePrompt)
        return;
    const quint64 revision = m_revision;
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Cancel log-index operation?"),
        tr("A log scan or deletion is still running. Cancel it and close the "
           "index after the worker reaches its next safe boundary? A deletion "
           "already completed before cancellation remains permanent."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral("LogIndexBusyCloseConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->button(QMessageBox::Yes)->setText(tr("Cancel and close"));
    if (auto *yes = qobject_cast<QPushButton *>(dialog->button(QMessageBox::Yes)))
        yes->setAutoDefault(false);
    m_closePrompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision](int result) {
        if (m_closing || m_closePrompt != dialog || revision != m_revision)
            return;
        m_closePrompt.clear();
        if (result != QMessageBox::Yes) {
            refreshControls();
            return;
        }
        m_closeWhenIdle = true;
        if (m_job) {
            m_job->cancelled.store(true, std::memory_order_relaxed);
            m_status->setText(tr("Cancellation requested; the window will close after the worker stops."));
            refreshControls();
        } else {
            m_allowClose = true;
            close();
        }
    });
    const QPointer<LogIndexWindow> guard(this);
    dialog->open();
    if (guard && !m_closing && revision == m_revision
        && m_closePrompt == dialog) {
        refreshControls();
    }
}

void LogIndexWindow::closeAfterCancelledJob()
{
    if (!m_closeWhenIdle || m_job || m_closing)
        return;
    m_closeWhenIdle = false;
    m_allowClose = true;
    close();
}

void LogIndexWindow::closeEvent(QCloseEvent *event)
{
    if (m_allowClose) {
        m_closing = true;
        QWidget::closeEvent(event);
        return;
    }
    if (m_job) {
        requestCloseWhileBusy(event);
        return;
    }
    m_closing = true;
    ++m_revision;
    const QPointer<QFileDialog> directoryDialog(m_directoryDialog);
    m_directoryDialog.clear();
    if (directoryDialog) {
        directoryDialog->blockSignals(true);
        directoryDialog->reject();
        if (directoryDialog)
            directoryDialog->deleteLater();
    }
    dismissDialog(m_deletePrompt);
    dismissDialog(m_closePrompt);
    QWidget::closeEvent(event);
}

void LogIndexWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_closing || m_initialScanStarted)
        return;
    m_initialScanStarted = true;
    const QString root = !m_currentRoot.isEmpty() ? m_currentRoot
        : !m_defaultDataflashDirectory.isEmpty() ? m_defaultDataflashDirectory
        : m_defaultTlogDirectory;
    if (!root.isEmpty())
        scanDirectory(root);
}
