#ifndef MAPCACHEVIEW_H
#define MAPCACHEVIEW_H

#include "MapCacheManager.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class MapCacheView final : public QDialog
{
    Q_OBJECT

public:
    explicit MapCacheView(QWidget *parent = nullptr,
                          const QString &cacheRoot = QString());
    ~MapCacheView() override;

    static MapCacheView *OpenWindow(QWidget *owner = nullptr);

private slots:
    void RefreshCommand();
    void RemoveOldCommand();
    void RemoveAllCommand();
    void ImportTilesButton();
    void CancelImportButton();
    void updateSelectionActions();

private:
    void buildUi();
    void startScan(const QString &selectedPath = QString(),
                   const QString &completionStatus = QString());
    void finishScan(const QList<MapCacheSnapshot> &entries,
                    const QString &selectedPath,
                    const QString &completionStatus);
    void startDelete(bool removeOld);
    void finishDelete(const MapCacheDeleteResult &result,
                      const MapCacheSnapshot &entry);
    void finishImport(const MapTileImportResult &result,
                      core::MapType::Types mapType);
    void setBusy(bool busy, bool importing = false);
    MapCacheSnapshot selectedSnapshot() const;
    QString selectedPath() const;
    void invalidateMapCaches(const MapCacheSnapshot &entry);

    QString m_cacheRoot;
    QList<MapCacheSnapshot> m_entries;
    QTreeWidget *m_entriesView = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_removeOld = nullptr;
    QPushButton *m_removeAll = nullptr;
    QComboBox *m_importMapType = nullptr;
    QPushButton *m_importTiles = nullptr;
    QPushButton *m_cancelImport = nullptr;
    QLabel *m_status = nullptr;
    bool m_busy = false;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
};

#endif // MAPCACHEVIEW_H
