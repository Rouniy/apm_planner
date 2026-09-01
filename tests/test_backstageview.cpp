#include "ui/BackstageView.h"
#include "ui/MainWindowHeader.h"

#include <QAbstractButton>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTest>

class BackstageViewTest : public QObject
{
    Q_OBJECT

private slots:
    void hasMissionPlannerGeometry();
    void hasMissionPlannerHeaderGeometry();
    void selectsAndFallsBackToVisiblePages();
    void rejectsInvalidAndDuplicatePages();
    void collapsesPageGroups();
    void createsAndResetsLazyPages();
    void evaluatesDeclarativeVisibilityWithoutCreatingHiddenPages();
    void restoresPreferredPageWithoutCreatingEarlierFactories();
    void restoresSelectionWhenPagesReappear();
    void exposesLoadingState();
};

void BackstageViewTest::hasMissionPlannerGeometry()
{
    BackstageView view;
    view.resize(900, 600);
    view.show();
    QTest::qWait(1);

    auto *navigation = view.findChild<QScrollArea *>(QStringLiteral("backstageNavigation"));
    QVERIFY(navigation);
    QCOMPARE(navigation->width(), 210);
    QCOMPARE(navigation->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}

void BackstageViewTest::hasMissionPlannerHeaderGeometry()
{
    QCOMPARE(MainWindowHeader::headerHeightFor(false, false), 64);
    QCOMPARE(MainWindowHeader::headerHeightFor(true, true), 64);
    QCOMPARE(MainWindowHeader::headerHeightFor(true, false), 7);
}

void BackstageViewTest::selectsAndFallsBackToVisiblePages()
{
    BackstageView view;
    auto *first = new QWidget;
    auto *second = new QWidget;
    QVERIFY(view.addPage(QStringLiteral("menuInstallFirmware"),
                         QStringLiteral("Install Firmware"), first));
    view.addGroup(QStringLiteral(">> Mandatory Hardware"),
                  QStringLiteral("mandatoryHardwareGroup"));
    QVERIFY(view.addPage(QStringLiteral("menuFrameType"),
                         QStringLiteral("Frame Type"), second, true));
    QCOMPARE(view.pageIds(), QStringList({QStringLiteral("menuInstallFirmware"),
                                         QStringLiteral("menuFrameType")}));
    QCOMPARE(view.currentPageId(), QStringLiteral("menuInstallFirmware"));

    QSignalSpy spy(&view, &BackstageView::currentPageChanged);
    QVERIFY(view.setCurrentPage(QStringLiteral("menuFrameType")));
    QCOMPARE(view.currentPageId(), QStringLiteral("menuFrameType"));
    QCOMPARE(spy.count(), 1);

    QVERIFY(view.setPageVisible(QStringLiteral("menuFrameType"), false));
    QCOMPARE(view.currentPageId(), QStringLiteral("menuInstallFirmware"));
    QVERIFY(!view.setCurrentPage(QStringLiteral("menuFrameType")));
}

void BackstageViewTest::rejectsInvalidAndDuplicatePages()
{
    BackstageView view;
    QVERIFY(!view.addPage(QString(), QStringLiteral("Invalid"), new QWidget(&view)));
    QVERIFY(!view.addPage(QStringLiteral("missing"), QStringLiteral("Invalid"), nullptr));
    QVERIFY(view.addPage(QStringLiteral("menuPlanner"),
                         QStringLiteral("Planner"), new QWidget));
    auto *duplicate = new QWidget;
    QVERIFY(!view.addPage(QStringLiteral("menuPlanner"),
                          QStringLiteral("Planner Advanced"), duplicate));
    delete duplicate;
}

void BackstageViewTest::collapsesPageGroups()
{
    BackstageView view;
    QVERIFY(view.addPage(QStringLiteral("menuInstallFirmware"),
                         QStringLiteral("Install Firmware"), new QWidget));
    view.addGroup(QStringLiteral(">> Mandatory Hardware"),
                  QStringLiteral("mandatoryHardwareGroup"));
    auto *frameType = new QWidget;
    QVERIFY(view.addPage(QStringLiteral("menuFrameType"),
                         QStringLiteral("Frame Type"), frameType, true));
    QCOMPARE(frameType->property("pageId").toString(), QStringLiteral("menuFrameType"));
    QVERIFY(view.isGroupExpanded(QStringLiteral("mandatoryHardwareGroup")));
    QVERIFY(view.setCurrentPage(QStringLiteral("menuFrameType")));

    QVERIFY(view.setGroupExpanded(QStringLiteral("mandatoryHardwareGroup"), false));
    QVERIFY(!view.isPageVisible(QStringLiteral("menuFrameType")));
    QCOMPARE(view.currentPageId(), QStringLiteral("menuInstallFirmware"));
    QVERIFY(view.setGroupExpanded(QStringLiteral("mandatoryHardwareGroup"), true));
    QVERIFY(view.isPageVisible(QStringLiteral("menuFrameType")));

    QVERIFY(view.setGroupVisible(QStringLiteral("mandatoryHardwareGroup"), false));
    QVERIFY(!view.isGroupVisible(QStringLiteral("mandatoryHardwareGroup")));
    QVERIFY(!view.isPageVisible(QStringLiteral("menuFrameType")));
    QVERIFY(view.setGroupVisible(QStringLiteral("mandatoryHardwareGroup"), true));
    QVERIFY(view.isGroupVisible(QStringLiteral("mandatoryHardwareGroup")));
    QVERIFY(view.isPageVisible(QStringLiteral("menuFrameType")));
}

void BackstageViewTest::createsAndResetsLazyPages()
{
    BackstageView view;
    QVERIFY(view.addPage(QStringLiteral("menuInstallFirmware"),
                         QStringLiteral("Install Firmware"), new QWidget));

    int creationCount = 0;
    BackstagePage definition;
    definition.id = QStringLiteral("menuFrameType");
    definition.header = QStringLiteral("Frame Type");
    definition.requiresConnection = true;
    definition.factory = [&creationCount](QWidget *parent) {
        ++creationCount;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(definition));
    QVERIFY(view.pageDefinition(definition.id).requiresConnection);
    QVERIFY(!view.isPageCreated(definition.id));
    QCOMPARE(creationCount, 0);

    QVERIFY(view.setCurrentPage(definition.id));
    QVERIFY(view.isPageCreated(definition.id));
    QCOMPARE(creationCount, 1);
    QCOMPARE(view.page(definition.id)->property("pageId").toString(), definition.id);

    QVERIFY(view.setCurrentPage(QStringLiteral("menuInstallFirmware")));
    QVERIFY(view.resetPage(definition.id));
    QVERIFY(!view.isPageCreated(definition.id));
    QVERIFY(view.setCurrentPage(definition.id));
    QCOMPARE(creationCount, 2);
}

void BackstageViewTest::evaluatesDeclarativeVisibilityWithoutCreatingHiddenPages()
{
    BackstageView view;
    bool connected = false;
    int connectedPageCreations = 0;

    BackstagePage connectedPage;
    connectedPage.id = QStringLiteral("connectedPage");
    connectedPage.header = QStringLiteral("Connected Page");
    connectedPage.visibleWhen = [&connected]() { return connected; };
    connectedPage.factory = [&connectedPageCreations](QWidget *parent) {
        ++connectedPageCreations;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(connectedPage));
    QVERIFY(!view.isPageVisible(connectedPage.id));
    QVERIFY(!view.isPageCreated(connectedPage.id));
    QVERIFY(view.currentPageId().isEmpty());

    QVERIFY(view.addPage(QStringLiteral("offlinePage"),
                         QStringLiteral("Offline Page"), new QWidget));
    QCOMPARE(view.currentPageId(), QStringLiteral("offlinePage"));

    connected = true;
    view.refreshVisibility();
    QVERIFY(view.isPageVisible(connectedPage.id));
    QVERIFY(!view.isPageCreated(connectedPage.id));
    QVERIFY(view.setCurrentPage(connectedPage.id));
    QCOMPARE(connectedPageCreations, 1);

    connected = false;
    view.refreshVisibility();
    QCOMPARE(view.currentPageId(), QStringLiteral("offlinePage"));
}

void BackstageViewTest::restoresPreferredPageWithoutCreatingEarlierFactories()
{
    BackstageView view;
    view.setAutomaticSelectionEnabled(false);
    int firstCreations = 0;
    int plannerCreations = 0;

    BackstagePage first;
    first.id = QStringLiteral("ConfigFlightModesView");
    first.header = QStringLiteral("Flight Modes");
    first.factory = [&firstCreations](QWidget *parent) {
        ++firstCreations;
        return new QWidget(parent);
    };
    BackstagePage planner;
    planner.id = QStringLiteral("ConfigPlannerView");
    planner.header = QStringLiteral("Planner");
    planner.factory = [&plannerCreations](QWidget *parent) {
        ++plannerCreations;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(first));
    QVERIFY(view.addPage(planner));
    QVERIFY(view.currentPageId().isEmpty());
    QCOMPARE(firstCreations, 0);
    QCOMPARE(plannerCreations, 0);

    QVERIFY(view.restoreInitialPage(QStringLiteral("Planner")));
    QCOMPARE(view.currentPageId(), planner.id);
    QCOMPARE(firstCreations, 0);
    QCOMPARE(plannerCreations, 1);

    view.setAutomaticSelectionEnabled(false);
    QVERIFY(view.resetPage(planner.id));
    QVERIFY(view.currentPageId().isEmpty());
    QCOMPARE(firstCreations, 0);
    QCOMPARE(plannerCreations, 1);
    QVERIFY(view.restoreInitialPage(planner.id));
    QCOMPARE(firstCreations, 0);
    QCOMPARE(plannerCreations, 2);
}

void BackstageViewTest::restoresSelectionWhenPagesReappear()
{
    BackstageView view;
    QVERIFY(view.addPage(QStringLiteral("onlyPage"),
                         QStringLiteral("Only Page"), new QWidget));
    QSignalSpy spy(&view, &BackstageView::currentPageChanged);

    QVERIFY(view.setPageVisible(QStringLiteral("onlyPage"), false));
    QVERIFY(view.currentPageId().isEmpty());
    QVERIFY(!view.findChild<QAbstractButton *>(QStringLiteral("onlyPage"))->isChecked());
    QCOMPARE(spy.takeLast().at(0).toString(), QString());

    QVERIFY(view.setPageVisible(QStringLiteral("onlyPage"), true));
    QCOMPARE(view.currentPageId(), QStringLiteral("onlyPage"));
}

void BackstageViewTest::exposesLoadingState()
{
    BackstageView view;
    auto *overlay = view.findChild<QWidget *>(QStringLiteral("parameterLoadingOverlay"));
    auto *progress = view.findChild<QProgressBar *>(QStringLiteral("parameterLoadingProgress"));
    QVERIFY(overlay);
    QVERIFY(progress);
    QCOMPARE(progress->height(), 20);
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("stopParameterLoadingButton")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("retryParameterLoadingButton")));
    QSignalSpy stopSpy(&view, &BackstageView::stopLoadingRequested);
    QSignalSpy retrySpy(&view, &BackstageView::retryLoadingRequested);
    view.findChild<QPushButton *>(QStringLiteral("stopParameterLoadingButton"))->click();
    view.findChild<QPushButton *>(QStringLiteral("retryParameterLoadingButton"))->click();
    QCOMPARE(stopSpy.count(), 1);
    QCOMPARE(retrySpy.count(), 1);

    view.setLoading(true, QStringLiteral("Parameters 12/24"), 50);
    QVERIFY(!overlay->isHidden());
    QCOMPARE(progress->minimum(), 0);
    QCOMPARE(progress->maximum(), 100);
    QCOMPARE(progress->value(), 50);

    view.setLoading(false);
    QVERIFY(overlay->isHidden());
}

QTEST_MAIN(BackstageViewTest)
#include "test_backstageview.moc"
