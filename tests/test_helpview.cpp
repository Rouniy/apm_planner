#include "ui/HelpView.h"

#include <QLabel>
#include <QLayout>
#include <QColor>
#include <QImage>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTest>

class HelpViewTest : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerSurface();
    void exposesStableAndBetaUpdateRequests();
};

void HelpViewTest::matchesMissionPlannerSurface()
{
    HelpView view;
    QCOMPARE(view.objectName(), QStringLiteral("HelpView"));
    QVERIFY(view.findChild<QScrollArea *>(QStringLiteral("helpScroll")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("helpWelcomeLabel"))
                ->text().contains(QStringLiteral("APM Planner 3.0")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("helpVersionLabel"))
                ->text().startsWith(QStringLiteral("Version ")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("helpShortcut7")));
    QVERIFY(!view.findChild<QLabel *>(QStringLiteral("helpShortcut8")));
    auto *content = view.findChild<QWidget *>(QStringLiteral("helpContent"));
    auto *viewportHost = view.findChild<QWidget *>(QStringLiteral("helpViewportHost"));
    QVERIFY(content);
    QVERIFY(viewportHost);
    QCOMPARE(content->minimumWidth(), 900);
    QCOMPARE(content->maximumWidth(), 900);
    QCOMPARE(viewportHost->layout()->contentsMargins(), QMargins(40, 40, 40, 40));

    view.resize(1100, 640);
    view.show();
    QTest::qWait(1);
    const QImage image = view.grab().toImage();
    QCOMPARE(image.pixelColor(image.width() - 30, 100), QColor("#1a201d"));
    QVERIFY(image.pixelColor(image.width() - 10, 100).lightness() < 80);
}

void HelpViewTest::exposesStableAndBetaUpdateRequests()
{
    HelpView view;
    auto *stable = view.findChild<QPushButton *>(
        QStringLiteral("helpStableUpdateButton"));
    auto *beta = view.findChild<QPushButton *>(
        QStringLiteral("helpBetaUpdateButton"));
    QVERIFY(stable);
    QVERIFY(beta);

    QSignalSpy stableSpy(&view, &HelpView::checkForUpdatesRequested);
    QSignalSpy betaSpy(&view, &HelpView::checkForBetaUpdatesRequested);
    stable->click();
    QCOMPARE(stableSpy.count(), 1);
    QVERIFY(view.updateCheckInProgress());
    QVERIFY(!stable->isEnabled());
    QVERIFY(!beta->isEnabled());

    view.setUpdateCheckInProgress(false, QStringLiteral("No update available."));
    QCOMPARE(view.updateStatus(), QStringLiteral("No update available."));
    QVERIFY(stable->isEnabled());
    beta->click();
    QCOMPARE(betaSpy.count(), 1);
}

QTEST_MAIN(HelpViewTest)
#include "test_helpview.moc"
