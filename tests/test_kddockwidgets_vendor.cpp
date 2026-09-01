#include <kddockwidgets/DockWidget.h>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/MainWindow.h>

#include <QLabel>
#include <QTest>

class KDDockWidgetsVendorTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsTabsAndRestoresRelativeLayout();
};

void KDDockWidgetsVendorTest::buildsTabsAndRestoresRelativeLayout()
{
    const QString affinity = QStringLiteral("vendor-smoke");
    KDDockWidgets::MainWindow window(QStringLiteral("VendorSmokeMainWindow"));
    window.setAffinities(QStringList{affinity});
    window.resize(900, 600);

    auto *mapDock = new KDDockWidgets::DockWidget(QStringLiteral("VendorMapDock"));
    mapDock->setAffinityName(affinity);
    mapDock->setWidget(new QLabel(QStringLiteral("Map")));
    auto *hudDock = new KDDockWidgets::DockWidget(QStringLiteral("VendorHudDock"));
    hudDock->setAffinityName(affinity);
    hudDock->setWidget(new QLabel(QStringLiteral("HUD")));
    auto *statusDock = new KDDockWidgets::DockWidget(QStringLiteral("VendorStatusDock"));
    statusDock->setAffinityName(affinity);
    statusDock->setWidget(new QLabel(QStringLiteral("Status")));

    window.addDockWidget(mapDock, KDDockWidgets::Location_OnLeft);
    window.addDockWidget(hudDock, KDDockWidgets::Location_OnRight, mapDock);
    hudDock->addDockWidgetAsTab(statusDock);

    KDDockWidgets::LayoutSaver saver(
        KDDockWidgets::RestoreOption_RelativeToMainWindow);
    saver.setAffinityNames(QStringList{affinity});
    const QByteArray layout = saver.serializeLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(saver.restoreLayout(layout));
}

QTEST_MAIN(KDDockWidgetsVendorTest)
#include "test_kddockwidgets_vendor.moc"
