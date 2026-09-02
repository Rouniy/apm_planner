#include "ui/map/MapCacheView.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

class MapCacheViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void windowMatchesReferenceContract();
    void refreshPreservesSelectionByPath();
};

void MapCacheViewTest::windowMatchesReferenceContract()
{
    QTemporaryDir cacheRoot;
    QVERIFY(cacheRoot.isValid());
    MapCacheView window(nullptr, cacheRoot.path());

    QCOMPARE(window.objectName(), QStringLiteral("MapCacheWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Map Tile Cache"));
    QCOMPARE(window.size(), QSize(800, 540));
    QCOMPARE(window.minimumSize(), QSize(650, 420));
    QVERIFY(!window.isModal());
    QVERIFY(window.testAttribute(Qt::WA_DeleteOnClose));

    auto *entries = window.findChild<QTreeWidget *>(
        QStringLiteral("Entries"));
    auto *refresh = window.findChild<QPushButton *>(
        QStringLiteral("RefreshCommand"));
    auto *removeOld = window.findChild<QPushButton *>(
        QStringLiteral("RemoveOldCommand"));
    auto *removeAll = window.findChild<QPushButton *>(
        QStringLiteral("RemoveAllCommand"));
    auto *picker = window.findChild<QComboBox *>(
        QStringLiteral("ImportMapTypePicker"));
    auto *importTiles = window.findChild<QPushButton *>(
        QStringLiteral("ImportTilesButton"));
    auto *cancelImport = window.findChild<QPushButton *>(
        QStringLiteral("CancelImportButton"));
    auto *importLabel = window.findChild<QLabel *>(
        QStringLiteral("ImportIntoLabel"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("Status"));
    QVERIFY(entries);
    QVERIFY(refresh);
    QVERIFY(removeOld);
    QVERIFY(removeAll);
    QVERIFY(picker);
    QVERIFY(importTiles);
    QVERIFY(cancelImport);
    QVERIFY(importLabel);
    QVERIFY(status);

    QCOMPARE(entries->columnCount(), 5);
    const QStringList headers{QStringLiteral("Provider cache"),
                              QStringLiteral("Size"),
                              QStringLiteral("Files"),
                              QStringLiteral("Last write"),
                              QStringLiteral("Directory")};
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(entries->headerItem()->text(column), headers.at(column));
    }
    QCOMPARE(refresh->text(), QStringLiteral("Refresh"));
    QCOMPARE(removeOld->text(), QStringLiteral("Remove 30+ days"));
    QCOMPARE(removeAll->text(), QStringLiteral("Remove All"));
    QCOMPARE(importLabel->text(), QStringLiteral("Import into"));
    QCOMPARE(importTiles->text(), QStringLiteral("Import Z/row/column…"));
    QCOMPARE(cancelImport->text(), QStringLiteral("Cancel Import"));
    QVERIFY(cancelImport->isHidden());

    const QStringList providers{QStringLiteral("GoogleSatelliteMap"),
                                QStringLiteral("GoogleHybridMap"),
                                QStringLiteral("BingSatelliteMap"),
                                QStringLiteral("OpenStreetMap"),
                                QStringLiteral("EsriWorldImagery")};
    const QList<int> providerTypes{
        static_cast<int>(core::MapType::GoogleSatellite),
        static_cast<int>(core::MapType::GoogleHybrid),
        static_cast<int>(core::MapType::BingSatellite),
        static_cast<int>(core::MapType::OpenStreetMap),
        static_cast<int>(core::MapType::ArcGIS_Satellite)};
    QCOMPARE(picker->count(), static_cast<int>(providers.size()));
    for (int index = 0; index < static_cast<int>(providers.size()); ++index) {
        QCOMPARE(picker->itemText(index), providers.at(index));
        QCOMPARE(picker->itemData(index).toInt(), providerTypes.at(index));
    }

    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QCOMPARE(entries->topLevelItemCount(), 1);
    QCOMPARE(entries->topLevelItem(0)->text(0), QStringLiteral("Total"));
    QCOMPARE(entries->currentItem(), entries->topLevelItem(0));
}

void MapCacheViewTest::refreshPreservesSelectionByPath()
{
    QTemporaryDir cacheRoot;
    QVERIFY(cacheRoot.isValid());
    QVERIFY(QDir().mkpath(cacheRoot.filePath(QStringLiteral("alpha"))));
    QVERIFY(QDir().mkpath(cacheRoot.filePath(QStringLiteral("beta"))));

    QFile tile(cacheRoot.filePath(QStringLiteral("beta/one.tile")));
    QVERIFY(tile.open(QIODevice::WriteOnly));
    QCOMPARE(tile.write("tile"), qint64(4));
    tile.close();

    MapCacheView window(nullptr, cacheRoot.path());
    auto *entries = window.findChild<QTreeWidget *>(
        QStringLiteral("Entries"));
    auto *refresh = window.findChild<QPushButton *>(
        QStringLiteral("RefreshCommand"));
    QVERIFY(entries);
    QVERIFY(refresh);
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QCOMPARE(entries->topLevelItemCount(), 3);

    QTreeWidgetItem *beta = nullptr;
    for (int index = 0; index < entries->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = entries->topLevelItem(index);
        if (item->text(0) == QStringLiteral("beta")) {
            beta = item;
            break;
        }
    }
    QVERIFY(beta);
    entries->setCurrentItem(beta);
    const QString path = beta->data(0, Qt::UserRole).toString();
    QVERIFY(!path.isEmpty());

    refresh->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(entries->currentItem());
    QCOMPARE(entries->currentItem()->data(0, Qt::UserRole).toString(), path);
}

QTEST_MAIN(MapCacheViewTest)
#include "test_mapcacheview.moc"
