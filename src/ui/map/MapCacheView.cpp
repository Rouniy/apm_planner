#include "MapCacheView.h"

#include "MapTileSourceFactory.h"
#include "pureimagecache.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {
constexpr int PathRole = Qt::UserRole;
constexpr int SnapshotIndexRole = Qt::UserRole + 1;

const char kMapCacheStyle[] = R"(
MapCacheView {
    background: #1A201D;
    color: #D8D8D8;
}
MapCacheView QLabel {
    color: #E6EDE9;
}
MapCacheView QTreeWidget,
MapCacheView QComboBox {
    color: #D8D8D8;
    background: #161B18;
    alternate-background-color: #1D2420;
    border: 1px solid #2A322D;
    border-radius: 3px;
}
MapCacheView QHeaderView::section {
    padding: 6px;
    color: #E6EDE9;
    background: #252E29;
    border: 0;
    border-right: 1px solid #38423C;
}
MapCacheView QComboBox {
    min-height: 28px;
    padding: 2px 7px;
}
MapCacheView QPushButton {
    min-height: 28px;
    padding: 4px 10px;
    font-weight: 600;
    color: #06251A;
    background: #34D399;
    border: 0;
    border-radius: 3px;
}
MapCacheView QPushButton:hover {
    background: #10B981;
}
MapCacheView QPushButton:disabled {
    color: #737B77;
    background: #303733;
}
MapCacheView QPushButton#CancelImportButton {
    color: #F8E7E7;
    background: #9F3A38;
}
MapCacheView QLabel#Status {
    color: #99AADD;
}
)";

QString importSummary(const MapTileImportResult &result)
{
    if (!result.error.isEmpty()) {
        return result.error;
    }
    if (result.maintenanceBusy) {
        return QObject::tr("Map cache maintenance is already in progress.");
    }

    QString summary = QObject::tr(
        "Imported %1, skipped %2, failed %3.")
        .arg(result.imported)
        .arg(result.skipped)
        .arg(result.failed);
    if (result.canceled) {
        summary.prepend(QObject::tr("Import canceled. "));
    }
    return summary;
}

QString deleteSummary(const MapCacheDeleteResult &result)
{
    if (!result.error.isEmpty()) {
        return result.error;
    }
    if (result.maintenanceBusy) {
        return QObject::tr("Map cache maintenance is already in progress.");
    }
    return QObject::tr("Removed %1 files (%2); %3 failed.")
        .arg(result.removedFiles)
        .arg(MapCacheManager::formatBytes(result.freedBytes))
        .arg(result.failedFiles);
}

template<typename Callback>
void postToUi(const QPointer<MapCacheView> &view, Callback callback)
{
    QApplication *application = qobject_cast<QApplication *>(
        QCoreApplication::instance());
    if (!application) {
        return;
    }
    QMetaObject::invokeMethod(
        application,
        [view, callback]() mutable {
            if (view) {
                callback(view.data());
            }
        },
        Qt::QueuedConnection);
}
} // namespace

MapCacheView::MapCacheView(QWidget *parent, const QString &cacheRoot)
    : QDialog(parent),
      m_cacheRoot(cacheRoot)
{
    buildUi();
    startScan();
}

MapCacheView::~MapCacheView()
{
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }
}

MapCacheView *MapCacheView::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new MapCacheView(resolvedOwner);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void MapCacheView::RefreshCommand()
{
    if (!m_busy) {
        startScan(selectedPath());
    }
}

void MapCacheView::RemoveOldCommand()
{
    startDelete(true);
}

void MapCacheView::RemoveAllCommand()
{
    startDelete(false);
}

void MapCacheView::ImportTilesButton()
{
    if (m_busy || m_importMapType->currentIndex() < 0) {
        return;
    }

    const QString source = QFileDialog::getExistingDirectory(
        this, tr("Import Z/row/column tiles"));
    if (source.isEmpty()) {
        return;
    }

    const core::MapType::Types mapType =
        static_cast<core::MapType::Types>(
            m_importMapType->currentData().toInt());
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelFlag = m_cancelFlag;
    const QString cacheRoot = m_cacheRoot;
    const QPointer<MapCacheView> view(this);

    setBusy(true, true);
    m_status->setText(tr("Discovering tiles…"));

    QThread *thread = QThread::create(
        [view, cancelFlag, source, mapType, cacheRoot]() {
            const MapTileImportResult result = MapTileImporter::importTiles(
                source, mapType, cacheRoot,
                [view](const MapTileImportProgress &progress) {
                    postToUi(view, [progress](MapCacheView *window) {
                        if (!window->m_busy) {
                            return;
                        }
                        window->m_status->setText(QObject::tr(
                            "Found %1; imported %2; skipped %3; failed %4.")
                            .arg(progress.discovered)
                            .arg(progress.imported)
                            .arg(progress.skipped)
                            .arg(progress.failed));
                    });
                },
                [cancelFlag]() { return cancelFlag->load(); });
            postToUi(view, [result, mapType](MapCacheView *window) {
                window->finishImport(result, mapType);
            });
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MapCacheView::CancelImportButton()
{
    if (!m_busy || !m_cancelFlag) {
        return;
    }
    m_cancelFlag->store(true);
    m_cancelImport->setEnabled(false);
    m_status->setText(tr("Canceling import…"));
}

void MapCacheView::updateSelectionActions()
{
    const bool canRemove = !m_busy && selectedSnapshot().path.size() > 0;
    m_removeOld->setEnabled(canRemove);
    m_removeAll->setEnabled(canRemove);
}

void MapCacheView::buildUi()
{
    setObjectName(QStringLiteral("MapCacheWindow"));
    setWindowTitle(tr("Map Tile Cache"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(800, 540);
    setMinimumSize(650, 420);
    setStyleSheet(QString::fromLatin1(kMapCacheStyle));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    m_entriesView = new QTreeWidget(this);
    m_entriesView->setObjectName(QStringLiteral("Entries"));
    m_entriesView->setColumnCount(5);
    m_entriesView->setHeaderLabels({tr("Provider cache"), tr("Size"),
                                     tr("Files"), tr("Last write"),
                                     tr("Directory")});
    m_entriesView->setRootIsDecorated(false);
    m_entriesView->setAlternatingRowColors(true);
    m_entriesView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entriesView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entriesView->setSortingEnabled(false);
    QHeaderView *header = m_entriesView->header();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(m_entriesView, 1);

    auto *maintenance = new QHBoxLayout;
    maintenance->setSpacing(8);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName(QStringLiteral("RefreshCommand"));
    m_removeOld = new QPushButton(tr("Remove 30+ days"), this);
    m_removeOld->setObjectName(QStringLiteral("RemoveOldCommand"));
    m_removeAll = new QPushButton(tr("Remove All"), this);
    m_removeAll->setObjectName(QStringLiteral("RemoveAllCommand"));
    maintenance->addWidget(m_refresh);
    maintenance->addStretch(1);
    maintenance->addWidget(m_removeOld);
    maintenance->addWidget(m_removeAll);
    layout->addLayout(maintenance);

    auto *importRow = new QHBoxLayout;
    importRow->setSpacing(8);
    auto *importLabel = new QLabel(tr("Import into"), this);
    importLabel->setObjectName(QStringLiteral("ImportIntoLabel"));
    m_importMapType = new QComboBox(this);
    m_importMapType->setObjectName(QStringLiteral("ImportMapTypePicker"));
    const QList<core::MapType::Types> mapTypes =
        MapTileSourceFactory::CacheableMapTypes();
    for (core::MapType::Types mapType : mapTypes) {
        m_importMapType->addItem(
            MapTileSourceFactory::SettingsName(mapType),
            static_cast<int>(mapType));
    }
    const int currentMapTypeIndex = m_importMapType->findData(
        static_cast<int>(MapTileSourceFactory::instance()->CurrentMapType()));
    m_importMapType->setCurrentIndex(
        currentMapTypeIndex >= 0 ? currentMapTypeIndex : 0);
    m_importMapType->setMinimumWidth(220);
    m_importTiles = new QPushButton(tr("Import Z/row/column…"), this);
    m_importTiles->setObjectName(QStringLiteral("ImportTilesButton"));
    m_cancelImport = new QPushButton(tr("Cancel Import"), this);
    m_cancelImport->setObjectName(QStringLiteral("CancelImportButton"));
    m_cancelImport->setVisible(false);
    importRow->addWidget(importLabel);
    importRow->addWidget(m_importMapType, 1);
    importRow->addWidget(m_importTiles);
    importRow->addWidget(m_cancelImport);
    layout->addLayout(importRow);

    m_status = new QLabel(tr("Ready."), this);
    m_status->setObjectName(QStringLiteral("Status"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    connect(m_refresh, &QPushButton::clicked,
            this, &MapCacheView::RefreshCommand);
    connect(m_removeOld, &QPushButton::clicked,
            this, &MapCacheView::RemoveOldCommand);
    connect(m_removeAll, &QPushButton::clicked,
            this, &MapCacheView::RemoveAllCommand);
    connect(m_importTiles, &QPushButton::clicked,
            this, &MapCacheView::ImportTilesButton);
    connect(m_cancelImport, &QPushButton::clicked,
            this, &MapCacheView::CancelImportButton);
    connect(m_entriesView, &QTreeWidget::itemSelectionChanged,
            this, &MapCacheView::updateSelectionActions);
}

void MapCacheView::startScan(const QString &selectedPath,
                             const QString &completionStatus)
{
    if (m_busy) {
        return;
    }
    setBusy(true);
    m_status->setText(tr("Refreshing map cache…"));

    const QString cacheRoot = m_cacheRoot;
    const QPointer<MapCacheView> view(this);
    QThread *thread = QThread::create(
        [view, cacheRoot, selectedPath, completionStatus]() {
            const QList<MapCacheSnapshot> entries =
                MapCacheManager::scan(cacheRoot);
            postToUi(view, [entries, selectedPath, completionStatus](
                               MapCacheView *window) {
                window->finishScan(entries, selectedPath, completionStatus);
            });
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MapCacheView::finishScan(const QList<MapCacheSnapshot> &entries,
                              const QString &selectedPath,
                              const QString &completionStatus)
{
    m_entries = entries;
    m_entriesView->clear();

    QTreeWidgetItem *selectedItem = nullptr;
    QTreeWidgetItem *totalItem = nullptr;
    for (int index = 0; index < m_entries.size(); ++index) {
        const MapCacheSnapshot &entry = m_entries.at(index);
        auto *item = new QTreeWidgetItem(m_entriesView);
        item->setText(0, entry.name);
        item->setText(1, MapCacheManager::formatBytes(entry.sizeBytes));
        item->setText(2, QString::number(entry.fileCount));
        item->setText(3, entry.lastWriteUtc.isValid()
            ? entry.lastWriteUtc.toUTC().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"))
            : QStringLiteral("—"));
        item->setText(4, entry.path);
        item->setData(0, PathRole, entry.path);
        item->setData(0, SnapshotIndexRole, index);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (entry.isTotal) {
            QFont font = item->font(0);
            font.setBold(true);
            for (int column = 0; column < 5; ++column) {
                item->setFont(column, font);
            }
            totalItem = item;
        }
        if (!selectedPath.isEmpty() && entry.path == selectedPath) {
            selectedItem = item;
        }
    }

    m_entriesView->setCurrentItem(selectedItem ? selectedItem : totalItem);
    setBusy(false);
    updateSelectionActions();
    if (!completionStatus.isEmpty()) {
        m_status->setText(completionStatus);
    } else {
        qint64 totalBytes = 0;
        qint64 totalFiles = 0;
        for (const MapCacheSnapshot &entry : m_entries) {
            if (entry.isTotal) {
                totalBytes = entry.sizeBytes;
                totalFiles = entry.fileCount;
                break;
            }
        }
        m_status->setText(tr("%1 files, %2 total.")
                              .arg(totalFiles)
                              .arg(MapCacheManager::formatBytes(totalBytes)));
    }
}

void MapCacheView::startDelete(bool removeOld)
{
    if (m_busy) {
        return;
    }
    const MapCacheSnapshot entry = selectedSnapshot();
    if (entry.path.isEmpty()) {
        return;
    }

    const QString question = removeOld
        ? tr("Remove cached tiles older than 30 days from %1?").arg(entry.name)
        : tr("Remove all cached tiles from %1?").arg(entry.name);
    if (QMessageBox::question(this, tr("Map Tile Cache"), question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    setBusy(true);
    m_status->setText(removeOld ? tr("Removing old tiles…")
                                : tr("Removing cached tiles…"));
    const QString cacheRoot = m_cacheRoot;
    const QPointer<MapCacheView> view(this);
    QThread *thread = QThread::create(
        [view, cacheRoot, entry, removeOld]() {
            const MapCacheDeleteResult result = removeOld
                ? MapCacheManager::deleteOlderThan(
                    entry, QDateTime::currentDateTimeUtc().addDays(-30),
                    cacheRoot)
                : MapCacheManager::deleteAll(entry, cacheRoot);
            postToUi(view, [result, entry](MapCacheView *window) {
                window->finishDelete(result, entry);
            });
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MapCacheView::finishDelete(const MapCacheDeleteResult &result,
                                const MapCacheSnapshot &entry)
{
    if (result.removedFiles > 0) {
        invalidateMapCaches(entry);
    }
    const QString summary = deleteSummary(result);
    setBusy(false);
    startScan(entry.path, summary);
}

void MapCacheView::finishImport(const MapTileImportResult &result,
                                core::MapType::Types mapType)
{
    if (result.imported > 0) {
        MapTileSourceFactory::instance()->InvalidateMapType(mapType);
    }
    m_cancelFlag.reset();
    const QString summary = importSummary(result);
    setBusy(false);
    startScan(selectedPath(), summary);
}

void MapCacheView::setBusy(bool busy, bool importing)
{
    m_busy = busy;
    m_entriesView->setEnabled(!busy);
    m_refresh->setEnabled(!busy);
    m_importMapType->setEnabled(!busy);
    m_importTiles->setEnabled(!busy);
    m_cancelImport->setVisible(busy && importing);
    m_cancelImport->setEnabled(busy && importing);
    if (busy) {
        m_removeOld->setEnabled(false);
        m_removeAll->setEnabled(false);
    } else {
        updateSelectionActions();
    }
}

MapCacheSnapshot MapCacheView::selectedSnapshot() const
{
    const QTreeWidgetItem *item = m_entriesView->currentItem();
    if (!item) {
        return MapCacheSnapshot();
    }
    const int index = item->data(0, SnapshotIndexRole).toInt();
    if (index < 0 || index >= m_entries.size()) {
        return MapCacheSnapshot();
    }
    return m_entries.at(index);
}

QString MapCacheView::selectedPath() const
{
    const QTreeWidgetItem *item = m_entriesView->currentItem();
    return item ? item->data(0, PathRole).toString() : QString();
}

void MapCacheView::invalidateMapCaches(const MapCacheSnapshot &entry)
{
    MapTileSourceFactory *factory = MapTileSourceFactory::instance();
    const QList<core::MapType::Types> mapTypes =
        MapTileSourceFactory::CacheableMapTypes();
    const QString providerDirectory = QFileInfo(entry.path).fileName();
    for (core::MapType::Types mapType : mapTypes) {
        if (entry.isTotal
            || providerDirectory
                == core::PureImageCache::providerCacheDirectory(mapType)) {
            factory->InvalidateMapType(mapType);
        }
    }
}
