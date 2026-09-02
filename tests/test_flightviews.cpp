#include "ui/FlightDataView.h"
#include "ui/FlightPlannerView.h"

#include <QApplication>
#include <QAction>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSplitter>
#include <QStackedWidget>
#include <QDockWidget>
#include <QMainWindow>
#include <QtTest/QTest>

namespace {
QLabel *makePaintedPanel(const QString &text, const QColor &color)
{
    auto *panel = new QLabel(text);
    panel->setAutoFillBackground(true);
    QPalette palette = panel->palette();
    palette.setColor(QPalette::Window, color);
    palette.setColor(QPalette::WindowText, Qt::white);
    panel->setPalette(palette);
    panel->setAlignment(Qt::AlignCenter);
    return panel;
}

QImage renderWidget(QWidget *widget)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
}

int matchingPixelCount(const QImage &image, const QColor &color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = QColor::fromRgba(line[x]);
            if (qAbs(pixel.red() - color.red()) <= 2
                && qAbs(pixel.green() - color.green()) <= 2
                && qAbs(pixel.blue() - color.blue()) <= 2) {
                ++count;
            }
        }
    }
    return count;
}

void verifyPaintedSurface(QWidget *widget, const QList<QColor> &colors)
{
    const QImage image = renderWidget(widget);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), widget->size());
    const int pixels = image.width() * image.height();
    QVERIFY(pixels > 0);
    for (const QColor &color : colors) {
        QVERIFY2(matchingPixelCount(image, color) > pixels / 100,
                 qPrintable(QStringLiteral("Panel color %1 was not painted")
                                .arg(color.name())));
    }
}

QByteArray withEveryPanelHidden(const QByteArray &layout)
{
    QJsonObject envelope = QJsonDocument::fromJson(layout).object();
    QJsonArray states = envelope.value(QStringLiteral("panelStates")).toArray();
    for (int index = 0; index < states.size(); ++index) {
        QJsonObject state = states.at(index).toObject();
        state.insert(QStringLiteral("open"), false);
        states.replace(index, state);
    }
    envelope.insert(QStringLiteral("panelStates"), states);
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}
}

class FlightViewsTest final : public QObject
{
    Q_OBJECT

private slots:
    void flightDataUsesStableMissionPlannerNames();
    void flightPlannerUsesStableMissionPlannerNames();
    void flightViewsRenderEveryDefaultPanel();
    void flightViewsRemainVisibleAfterStackSwitch();
};

void FlightViewsTest::flightDataUsesStableMissionPlannerNames()
{
    FlightDataView view;
    QCOMPARE(view.objectName(), QStringLiteral("FlightDataView"));

    auto *hud = new QLabel(QStringLiteral("hud"));
    auto *map = new QLabel(QStringLiteral("map"));
    auto *info = new QLabel(QStringLiteral("info"));
    QVERIFY(view.setHudWidget(hud));
    QVERIFY(view.setMapWidget(map));
    QVERIFY(view.setInfoView(info));
    auto *duplicate = new QLabel(QStringLiteral("duplicate"));
    QVERIFY(!view.setMapWidget(duplicate));
    delete duplicate;

    QCOMPARE(view.panelIds(),
             QStringList({FlightDataView::hudPanelId(),
                          FlightDataView::mapPanelId(),
                          FlightDataView::infoPanelId()}));
    QVERIFY(view.panelToggleAction(FlightDataView::infoPanelId()));
    QVERIFY(!view.setPanelVisible(FlightDataView::mapPanelId(), false));
    const QByteArray layout = view.saveLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(view.restoreLayout(layout));
    QJsonObject envelope = QJsonDocument::fromJson(layout).object();
    QCOMPARE(envelope.value(QStringLiteral("schema")).toString(),
             QStringLiteral("apmplanner-fixed-panel-layout"));
    QCOMPARE(envelope.value(QStringLiteral("version")).toInt(), 2);

    // Floating QDockWidget/KDDockWidgets layouts are intentionally obsolete.
    // They are rejected into the complete fixed MP10 surface.
    envelope.insert(QStringLiteral("schema"),
                    QStringLiteral("apmplanner-qt-dock-layout"));
    envelope.insert(QStringLiteral("version"), 1);
    QVERIFY(!view.restoreLayout(
        QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
    QVERIFY(view.isPanelDocked(FlightDataView::mapPanelId()));

    QVERIFY(!view.restoreLayout(withEveryPanelHidden(layout)));
    for (const QString &panelId : view.panelIds()) {
        QVERIFY(view.isPanelOpen(panelId));
    }
    QVERIFY(view.isPanelDocked(FlightDataView::mapPanelId()));
    QVERIFY(view.findChildren<QDockWidget *>().isEmpty());
    QVERIFY(view.findChildren<QMainWindow *>().isEmpty());
    auto *mainSplitter = view.findChild<QSplitter *>(
        QStringLiteral("MainFlightSplitter"));
    auto *verticalSplitter = view.findChild<QSplitter *>(
        QStringLiteral("VerticalDockSplitter"));
    QVERIFY(mainSplitter);
    QVERIFY(verticalSplitter);
    QCOMPARE(mainSplitter->handleWidth(), 6);
    QCOMPARE(verticalSplitter->handleWidth(), 4);
    QCOMPARE(hud->minimumWidth(), 240);
    QCOMPARE(map->minimumWidth(), 240);
}

void FlightViewsTest::flightPlannerUsesStableMissionPlannerNames()
{
    FlightPlannerView view;
    QCOMPARE(view.objectName(), QStringLiteral("FlightPlannerView"));

    auto *map = new QLabel(QStringLiteral("map"));
    auto *waypoints = new QLabel(QStringLiteral("waypoints"));
    auto *actions = new QLabel(QStringLiteral("actions"));
    QVERIFY(view.setMapWidget(map));
    QVERIFY(view.setWaypointPanel(waypoints));
    QVERIFY(view.setActionPanel(actions));

    QCOMPARE(view.panelIds(),
             QStringList({FlightPlannerView::mapPanelId(),
                          FlightPlannerView::waypointPanelId(),
                          FlightPlannerView::actionPanelId()}));
    QVERIFY(view.setPanelVisible(FlightPlannerView::actionPanelId(), false));
    QVERIFY(view.setPanelVisible(FlightPlannerView::waypointPanelId(), false));
    QVERIFY(!view.setPanelVisible(FlightPlannerView::mapPanelId(), false));
    const QByteArray layout = view.saveLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(view.restoreLayout(layout));

    QVERIFY(!view.restoreLayout(withEveryPanelHidden(layout)));
    for (const QString &panelId : view.panelIds()) {
        QVERIFY(view.isPanelOpen(panelId));
    }
    QVERIFY(view.isPanelDocked(FlightPlannerView::mapPanelId()));
    QVERIFY(view.findChildren<QDockWidget *>().isEmpty());
    QVERIFY(view.findChildren<QMainWindow *>().isEmpty());
    auto *horizontalSplitter = view.findChild<QSplitter *>(
        QStringLiteral("HorizontalDockSplitter"));
    auto *verticalSplitter = view.findChild<QSplitter *>(
        QStringLiteral("VerticalDockSplitter"));
    QVERIFY(horizontalSplitter);
    QVERIFY(verticalSplitter);
    QCOMPARE(horizontalSplitter->handleWidth(), 4);
    QCOMPARE(verticalSplitter->handleWidth(), 4);
    QCOMPARE(actions->minimumWidth(), 168);
    QCOMPARE(actions->maximumWidth(), 168);
    QCOMPARE(waypoints->minimumHeight(), 210);
    QCOMPARE(waypoints->maximumHeight(), 210);
}

void FlightViewsTest::flightViewsRenderEveryDefaultPanel()
{
    const QColor hudColor(25, 80, 150);
    const QColor dataMapColor(25, 145, 80);
    const QColor infoColor(145, 55, 125);
    FlightDataView dataView;
    dataView.resize(1280, 800);
    auto *dataMap = makePaintedPanel(QStringLiteral("map"), dataMapColor);
    auto *hud = makePaintedPanel(QStringLiteral("hud"), hudColor);
    auto *info = makePaintedPanel(QStringLiteral("info"), infoColor);
    QVERIFY(dataView.setHudWidget(hud));
    QVERIFY(dataView.setMapWidget(dataMap));
    QVERIFY(dataView.setInfoView(info));
    dataView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dataView));
    QCoreApplication::processEvents();

    for (QWidget *panel : {static_cast<QWidget *>(dataMap),
                           static_cast<QWidget *>(hud),
                           static_cast<QWidget *>(info)}) {
        QVERIFY2(panel->isVisibleTo(&dataView), panel->objectName().toUtf8());
        QVERIFY2(panel->width() > 0 && panel->height() > 0,
                 panel->objectName().toUtf8());
    }

    auto *dataMainSplitter = dataView.findChild<QSplitter *>(
        QStringLiteral("MainFlightSplitter"));
    auto *dataVerticalSplitter = dataView.findChild<QSplitter *>(
        QStringLiteral("VerticalDockSplitter"));
    QVERIFY(dataMainSplitter);
    QVERIFY(dataVerticalSplitter);
    const QList<int> dataColumnSizes = dataMainSplitter->sizes();
    const QList<int> dataRowSizes = dataVerticalSplitter->sizes();
    QCOMPARE(dataColumnSizes.size(), 2);
    QCOMPARE(dataRowSizes.size(), 2);
    QVERIFY2(qAbs(dataColumnSizes.at(0) * 3
                  - dataColumnSizes.at(1) * 2) < 40,
             qPrintable(QStringLiteral("DATA column sizes are %1:%2")
                            .arg(dataColumnSizes.at(0))
                            .arg(dataColumnSizes.at(1))));
    QVERIFY(qAbs(dataRowSizes.at(0) - dataRowSizes.at(1)) < 20);

    const QPoint hudCenter = hud->mapTo(&dataView, hud->rect().center());
    const QPoint infoCenter = info->mapTo(&dataView, info->rect().center());
    const QPoint mapCenter = dataMap->mapTo(&dataView, dataMap->rect().center());
    QVERIFY(hudCenter.x() < mapCenter.x());
    QVERIFY(infoCenter.x() < mapCenter.x());
    QVERIFY(hudCenter.y() < infoCenter.y());
    verifyPaintedSurface(&dataView, {hudColor, dataMapColor, infoColor});

    const QColor plannerMapColor(120, 75, 20);
    const QColor waypointColor(40, 115, 145);
    const QColor actionColor(125, 30, 45);
    FlightPlannerView plannerView;
    plannerView.resize(1280, 800);
    auto *plannerMap = makePaintedPanel(QStringLiteral("map"), plannerMapColor);
    auto *waypoints = makePaintedPanel(QStringLiteral("waypoints"), waypointColor);
    auto *actions = makePaintedPanel(QStringLiteral("actions"), actionColor);
    QVERIFY(plannerView.setMapWidget(plannerMap));
    QVERIFY(plannerView.setWaypointPanel(waypoints));
    QVERIFY(plannerView.setActionPanel(actions));
    plannerView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&plannerView));
    QCoreApplication::processEvents();

    for (QWidget *panel : {static_cast<QWidget *>(plannerMap),
                           static_cast<QWidget *>(waypoints),
                           static_cast<QWidget *>(actions)}) {
        QVERIFY2(panel->isVisibleTo(&plannerView), panel->objectName().toUtf8());
        QVERIFY2(panel->width() > 0 && panel->height() > 0,
                 panel->objectName().toUtf8());
    }
    QCOMPARE(actions->width(), 168);
    QCOMPARE(waypoints->height(), 210);
    verifyPaintedSurface(&plannerView,
                         {plannerMapColor, waypointColor, actionColor});

    QVERIFY(dataView.findChildren<QDockWidget *>().isEmpty());
    QVERIFY(plannerView.findChildren<QDockWidget *>().isEmpty());
    QVERIFY(dataView.findChildren<QMainWindow *>().isEmpty());
    QVERIFY(plannerView.findChildren<QMainWindow *>().isEmpty());
}

void FlightViewsTest::flightViewsRemainVisibleAfterStackSwitch()
{
    const QColor hudColor(25, 80, 150);
    const QColor dataMapColor(25, 145, 80);
    const QColor infoColor(145, 55, 125);
    const QColor plannerMapColor(120, 75, 20);
    const QColor waypointColor(40, 115, 145);
    const QColor actionColor(125, 30, 45);
    QStackedWidget stack;
    stack.resize(1280, 800);

    auto *dataView = new FlightDataView;
    auto *hud = makePaintedPanel(QStringLiteral("hud"), hudColor);
    auto *dataMap = makePaintedPanel(QStringLiteral("map"), dataMapColor);
    auto *info = makePaintedPanel(QStringLiteral("info"), infoColor);
    QVERIFY(dataView->setHudWidget(hud));
    QVERIFY(dataView->setMapWidget(dataMap));
    QVERIFY(dataView->setInfoView(info));
    stack.addWidget(dataView);

    auto *plannerView = new FlightPlannerView;
    auto *plannerMap = makePaintedPanel(QStringLiteral("map"), plannerMapColor);
    auto *waypoints = makePaintedPanel(QStringLiteral("waypoints"), waypointColor);
    auto *actions = makePaintedPanel(QStringLiteral("actions"), actionColor);
    QVERIFY(plannerView->setMapWidget(plannerMap));
    QVERIFY(plannerView->setWaypointPanel(waypoints));
    QVERIFY(plannerView->setActionPanel(actions));
    stack.addWidget(plannerView);

    stack.setCurrentWidget(dataView);
    stack.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stack));
    QCoreApplication::processEvents();

    const auto verifySurface = [&stack](QWidget *page,
                                        const QList<QWidget *> &panels) {
        QCOMPARE(stack.currentWidget(), page);
        for (QWidget *panel : panels) {
            QVERIFY2(panel->isVisibleTo(&stack), panel->objectName().toUtf8());
            QVERIFY2(panel->width() > 0 && panel->height() > 0,
                     panel->objectName().toUtf8());
        }
    };

    verifySurface(dataView, {hud, dataMap, info});
    verifyPaintedSurface(&stack, {hudColor, dataMapColor, infoColor});
    stack.setCurrentWidget(plannerView);
    QCoreApplication::processEvents();
    verifySurface(plannerView, {plannerMap, waypoints, actions});
    verifyPaintedSurface(&stack,
                         {plannerMapColor, waypointColor, actionColor});
    stack.setCurrentWidget(dataView);
    QCoreApplication::processEvents();
    verifySurface(dataView, {hud, dataMap, info});
    verifyPaintedSurface(&stack, {hudColor, dataMapColor, infoColor});
}

QTEST_MAIN(FlightViewsTest)
#include "test_flightviews.moc"
