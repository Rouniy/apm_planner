#include "ui/BackstageView.h"
#include "ui/MainWindowHeader.h"

#include <QProgressBar>
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
}

void BackstageViewTest::exposesLoadingState()
{
    BackstageView view;
    auto *overlay = view.findChild<QWidget *>(QStringLiteral("parameterLoadingOverlay"));
    auto *progress = view.findChild<QProgressBar *>(QStringLiteral("parameterLoadingProgress"));
    QVERIFY(overlay);
    QVERIFY(progress);

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
