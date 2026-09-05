#include "ui/BackstageView.h"
#include "ui/MainWindowHeader.h"
#include "ui/configuration/ConfigParamLoadingView.h"
#include "ui/configuration/ConfigParamLoadingViewModel.h"

#include <QAbstractButton>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
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
    void restoresPreferredSubPageAndExpandsItsGroup();
    void reenablingAutomaticSelectionRestoresConcreteFallback();
    void restoresSelectionWhenPagesReappear();
    void updatesPagePresentationWithoutReplacingItsIdentity();
    void gatesParameterLoadingLikeMissionPlanner();
    void modelsMissionPlannerParameterLoadingStates();
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
    QAbstractButton *const groupButton = view.findChild<QAbstractButton *>(
        QStringLiteral("mandatoryHardwareGroup"));
    QVERIFY(groupButton);
    QCOMPARE(groupButton->text(), QStringLiteral("<< Mandatory Hardware"));
    QCOMPARE(groupButton->toolTip(),
             QStringLiteral("Collapse Mandatory Hardware"));
    QCOMPARE(frameType->property("pageId").toString(), QStringLiteral("menuFrameType"));
    QVERIFY(view.isGroupExpanded(QStringLiteral("mandatoryHardwareGroup")));
    QVERIFY(view.setCurrentPage(QStringLiteral("menuFrameType")));

    QVERIFY(view.setGroupExpanded(QStringLiteral("mandatoryHardwareGroup"), false));
    QCOMPARE(groupButton->text(), QStringLiteral(">> Mandatory Hardware"));
    QCOMPARE(groupButton->toolTip(),
             QStringLiteral("Expand Mandatory Hardware"));
    QVERIFY(!view.isPageVisible(QStringLiteral("menuFrameType")));
    QCOMPARE(view.currentPageId(), QStringLiteral("menuInstallFirmware"));
    groupButton->click();
    QVERIFY(view.isPageVisible(QStringLiteral("menuFrameType")));
    QVERIFY(view.isGroupExpanded(QStringLiteral("mandatoryHardwareGroup")));
    QCOMPARE(groupButton->text(), QStringLiteral("<< Mandatory Hardware"));

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

void BackstageViewTest::restoresPreferredSubPageAndExpandsItsGroup()
{
    BackstageView view;
    view.setAutomaticSelectionEnabled(false);

    int fallbackCreations = 0;
    int subPageCreations = 0;
    BackstagePage fallback;
    fallback.id = QStringLiteral("fallbackPage");
    fallback.header = QStringLiteral("Fallback");
    fallback.factory = [&fallbackCreations](QWidget *parent) {
        ++fallbackCreations;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(fallback));

    const QString groupId = QStringLiteral("mandatoryHardwareGroup");
    view.addGroup(QStringLiteral(">> Mandatory Hardware"), groupId);
    BackstagePage subPage;
    subPage.id = QStringLiteral("menuFrameType");
    subPage.header = QStringLiteral("Frame Type");
    subPage.isSub = true;
    subPage.factory = [&subPageCreations](QWidget *parent) {
        ++subPageCreations;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(subPage));
    QVERIFY(view.setGroupExpanded(groupId, false));
    QVERIFY(!view.isPageVisible(subPage.id));

    QVERIFY(view.restoreInitialPage(subPage.header));
    QVERIFY(view.isGroupExpanded(groupId));
    QVERIFY(view.isPageVisible(subPage.id));
    QCOMPARE(view.currentPageId(), subPage.id);
    QCOMPARE(fallbackCreations, 0);
    QCOMPARE(subPageCreations, 1);
}

void BackstageViewTest::reenablingAutomaticSelectionRestoresConcreteFallback()
{
    BackstageView view;
    view.setAutomaticSelectionEnabled(false);

    int firstCreations = 0;
    int selectedCreations = 0;
    BackstagePage first;
    first.id = QStringLiteral("firstPage");
    first.header = QStringLiteral("First Page");
    first.factory = [&firstCreations](QWidget *parent) {
        ++firstCreations;
        return new QWidget(parent);
    };
    BackstagePage selected;
    selected.id = QStringLiteral("selectedPage");
    selected.header = QStringLiteral("Selected Page");
    selected.factory = [&selectedCreations](QWidget *parent) {
        ++selectedCreations;
        return new QWidget(parent);
    };
    QVERIFY(view.addPage(first));
    QVERIFY(view.addPage(selected));

    // restoreInitialPage() must still create only the preferred lazy page.
    QVERIFY(view.restoreInitialPage(selected.id));
    QCOMPARE(firstCreations, 0);
    QCOMPARE(selectedCreations, 1);
    QCOMPARE(view.currentPageId(), selected.id);

    view.setAutomaticSelectionEnabled(false);
    QVERIFY(view.resetPage(selected.id));
    QVERIFY(view.currentPageId().isEmpty());
    QVERIFY(!view.page(selected.id));

    view.setAutomaticSelectionEnabled(true);
    QCOMPARE(view.currentPageId(), first.id);
    QCOMPARE(firstCreations, 1);
    QVERIFY(view.page(first.id));
    auto *stack = view.findChild<QStackedWidget *>(
        QStringLiteral("backstagePageStack"));
    QVERIFY(stack);
    QCOMPARE(stack->currentWidget(), view.page(first.id));
    QVERIFY(stack->currentIndex() >= 0);
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

void BackstageViewTest::updatesPagePresentationWithoutReplacingItsIdentity()
{
    BackstageView view;
    const QString id = QStringLiteral("ConfigExtendedTuningView");
    QVERIFY(view.addPage(id, QStringLiteral("Extended Tuning"),
                         new QWidget));
    QAbstractButton *button = view.findChild<QAbstractButton *>(id);
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("Extended Tuning"));

    QVERIFY(view.setPagePresentation(
        id, QStringLiteral("QP Extended Tuning")));
    QCOMPARE(view.currentPageId(), id);
    QCOMPARE(button->text(), QStringLiteral("QP Extended Tuning"));
    QCOMPARE(view.pageDefinition(id).header,
             QStringLiteral("QP Extended Tuning"));
    QVERIFY(view.pageDefinition(id).badge.isEmpty());

    QVERIFY(view.setPagePresentation(
        id, QStringLiteral("Extended Tuning"),
        QStringLiteral("Legacy")));
    QCOMPARE(button->text(), QStringLiteral("Extended Tuning"));
    QCOMPARE(view.pageDefinition(id).badge, QStringLiteral("Legacy"));
    QVERIFY(!view.setPagePresentation(
        QStringLiteral("missing"), QStringLiteral("Missing")));
    QVERIFY(!view.setPagePresentation(id, QString()));
}

void BackstageViewTest::exposesLoadingState()
{
    BackstageView view;
    auto *overlay = view.findChild<QWidget *>(QStringLiteral("parameterLoadingOverlay"));
    auto *loadingView = view.findChild<ConfigParamLoadingView *>();
    auto *progress = view.findChild<QProgressBar *>(QStringLiteral("parameterLoadingProgress"));
    auto *status = view.findChild<QLabel *>(QStringLiteral("parameterLoadingStatus"));
    auto *count = view.findChild<QLabel *>(QStringLiteral("parameterLoadingCount"));
    QVERIFY(overlay);
    QVERIFY(loadingView);
    QVERIFY(progress);
    QVERIFY(status);
    QVERIFY(count);
    QCOMPARE(loadingView->objectName(), QStringLiteral("ConfigParamLoadingView"));
    QCOMPARE(view.findChild<QWidget *>(QStringLiteral("parameterLoadingPanel"))->width(), 320);
    QCOMPARE(progress->height(), 20);
    QCOMPARE(progress->minimum(), 0);
    QCOMPARE(progress->maximum(), 100);
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("stopParameterLoadingButton")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("retryParameterLoadingButton")));
    QSignalSpy stopSpy(&view, &BackstageView::stopLoadingRequested);
    QSignalSpy retrySpy(&view, &BackstageView::retryLoadingRequested);
    view.findChild<QPushButton *>(QStringLiteral("stopParameterLoadingButton"))->click();
    view.findChild<QPushButton *>(QStringLiteral("retryParameterLoadingButton"))->click();
    QCOMPARE(stopSpy.count(), 1);
    QCOMPARE(retrySpy.count(), 1);

    view.setParameterLoadingState(true, 12, 24);
    QVERIFY(!overlay->isHidden());
    QCOMPARE(progress->minimum(), 0);
    QCOMPARE(progress->maximum(), 100);
    QCOMPARE(progress->value(), 50);
    QCOMPARE(count->text(), QStringLiteral("12 / 24"));
    QCOMPARE(status->text(), QStringLiteral("Loading parameters (12 / 24)…"));

    view.setParameterLoadingState(false, 12, 24);
    QVERIFY(overlay->isHidden());
}

void BackstageViewTest::gatesParameterLoadingLikeMissionPlanner()
{
    QVERIFY(!BackstageView::shouldShowParameterLoading(false, false, false));
    QVERIFY(!BackstageView::shouldShowParameterLoading(true, true, false));
    QVERIFY(!BackstageView::shouldShowParameterLoading(true, false, true));
    QVERIFY(BackstageView::shouldShowParameterLoading(true, false, false));

    // RequiresConnection deliberately is not part of the reference truth table:
    // incomplete parameters cover Planner and Install Firmware as well.
    QVERIFY(BackstageView::shouldShowParameterLoading(true, false, false));
}

void BackstageViewTest::modelsMissionPlannerParameterLoadingStates()
{
    ConfigParamLoadingViewModel model;
    QCOMPARE(model.progressPercent(), 0);
    QCOMPARE(model.count(), QStringLiteral("0 / 0"));
    QCOMPARE(model.status(), QStringLiteral(
        "Waiting for the first parameter response. Select another device or retry; "
        "old-device values remain hidden."));
    QVERIFY(!model.parametersReady());
    QVERIFY(!ConfigParamLoadingViewModel::hasAllParameters(0, 0));
    QVERIFY(!ConfigParamLoadingViewModel::hasAllParameters(2, 3));
    QVERIFY(ConfigParamLoadingViewModel::hasAllParameters(3, 3));
    QVERIFY(ConfigParamLoadingViewModel::hasAllParameters(4, 3));

    model.setState(2, 3);
    QCOMPARE(model.progressPercent(), 66);
    QCOMPARE(model.count(), QStringLiteral("2 / 3"));
    QCOMPARE(model.status(), QStringLiteral("Loading parameters (2 / 3)…"));

    model.setState(2, 3, true);
    QVERIFY(model.loadingCancelled());
    QCOMPARE(model.status(), QStringLiteral(
        "Parameter loading stopped at 2 / 3. The connection remains active. "
        "Received values are available in Full Parameter List; select Retry Now "
        "for a complete list."));

    model.setRequesting();
    QVERIFY(!model.loadingCancelled());
    QCOMPARE(model.status(), QStringLiteral("Requesting parameters…"));
    model.setStopping();
    QCOMPARE(model.status(), QStringLiteral(
        "Stopping parameter loading; the connection remains active…"));

    model.setState(9, 4, true, QStringLiteral("ignored on completion"));
    QCOMPARE(model.progressPercent(), 100);
    QVERIFY(model.parametersReady());
    QVERIFY(!model.loadingCancelled());
    QCOMPARE(model.status(), QStringLiteral("All parameters loaded."));

    model.setState(-4, -8, true);
    QCOMPARE(model.received(), 0);
    QCOMPARE(model.reported(), 0);
    QCOMPARE(model.progressPercent(), 0);
    QCOMPARE(model.status(), QStringLiteral(
        "Parameter loading stopped at 0 / unknown. The connection remains active. "
        "Received values are available in Full Parameter List; select Retry Now "
        "for a complete list."));

    model.setState(1, 4, false, QStringLiteral("TIMEOUT"));
    QCOMPARE(model.failure(), QStringLiteral("TIMEOUT"));
    QCOMPARE(model.status(), QStringLiteral(
        "Parameter loading failed: TIMEOUT Received values are available in Full "
        "Parameter List; select Retry Now for a complete list."));
}

QTEST_MAIN(BackstageViewTest)
#include "test_backstageview.moc"
