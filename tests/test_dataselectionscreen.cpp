#include "ui/dataselectionscreen.h"
#include "logging.h"

#include <QTreeWidget>
#include <QtTest/QTest>

Q_LOGGING_CATEGORY(apmGeneral, "apm.general.test")

class DataSelectionScreenTest final : public QObject
{
    Q_OBJECT

private slots:
    void usesExactSortedGroupsWithoutRecursiveSorting();
    void rebuildsGroupIndexAfterClear();
};

void DataSelectionScreenTest::usesExactSortedGroupsWithoutRecursiveSorting()
{
    DataSelectionScreen screen;
    auto *tree = screen.findChild<QTreeWidget *>(QStringLiteral("treeWidget"));
    QVERIFY(tree);

    screen.addItem(QStringLiteral("GPS2_RAW.lat"));
    screen.addItem(QStringLiteral("GPS.lon"));
    screen.addItem(QStringLiteral("ATTITUDE.pitch"));
    screen.addItem(QStringLiteral("GPS.lat"));

    QCOMPARE(tree->topLevelItemCount(), 3);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("ATTITUDE"));
    QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("GPS"));
    QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("GPS2_RAW"));

    QTreeWidgetItem *gps = tree->topLevelItem(1);
    QCOMPARE(gps->childCount(), 2);
    QCOMPARE(gps->child(0)->text(0), QStringLiteral("lon"));
    QCOMPARE(gps->child(1)->text(0), QStringLiteral("lat"));
    QCOMPARE(tree->topLevelItem(2)->childCount(), 1);
}

void DataSelectionScreenTest::rebuildsGroupIndexAfterClear()
{
    DataSelectionScreen screen;
    auto *tree = screen.findChild<QTreeWidget *>(QStringLiteral("treeWidget"));
    QVERIFY(tree);

    screen.addItem(QStringLiteral("VFR_HUD.airspeed"));
    screen.clear();
    screen.addItem(QStringLiteral("VFR_HUD.groundspeed"));

    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("VFR_HUD"));
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->child(0)->text(0),
             QStringLiteral("groundspeed"));
}

QTEST_MAIN(DataSelectionScreenTest)
#include "test_dataselectionscreen.moc"
