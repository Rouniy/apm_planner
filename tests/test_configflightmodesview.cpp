#include <QtTest>

#include "ui/configuration/ConfigFlightModesView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTimer>

#include <algorithm>

namespace {

template<typename T>
T *required(QObject *owner, const QString &name)
{
    T *result = owner->findChild<T *>(name);
    Q_ASSERT(result);
    return result;
}

QList<ParamOption> modeOptions()
{
    return {{0, QStringLiteral("Stabilize")},
            {3, QStringLiteral("Auto")},
            {5, QStringLiteral("Loiter")}};
}

QList<ConfigFriendlyParameterValue> copterSnapshot()
{
    return {{1, QStringLiteral("FLTMODE1"), 0},
            {1, QStringLiteral("FLTMODE2"), 3},
            {1, QStringLiteral("FLTMODE3"), 5},
            {1, QStringLiteral("FLTMODE4"), 0},
            {1, QStringLiteral("FLTMODE5"), 3},
            {1, QStringLiteral("FLTMODE6"), 5},
            {1, QStringLiteral("FLTMODE_CH"), 5},
            {1, QStringLiteral("SIMPLE"), 5},
            {1, QStringLiteral("SUPER_SIMPLE"), 2}};
}

QList<ConfigFriendlyParameterValue> planeSnapshot()
{
    return {{1, QStringLiteral("FLTMODE1"), 0},
            {1, QStringLiteral("FLTMODE2"), 3},
            {1, QStringLiteral("FLTMODE3"), 5},
            {1, QStringLiteral("FLTMODE4"), 0},
            {1, QStringLiteral("FLTMODE5"), 3},
            {1, QStringLiteral("FLTMODE6"), 5},
            {1, QStringLiteral("FLTMODE_CH"), 8}};
}

void prepareCopter(ConfigFlightModesView *view)
{
    view->setFamily(
        ConfigFlightModesViewModel::Family::Copter, modeOptions());
    view->setParameterSnapshot(copterSnapshot(), 1, true);
    view->setConnected(true);
    view->setHeartbeat(3, true, false);
}

} // namespace

class ConfigFlightModesViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void visibleSurfaceIsNonEmptyBeforeSnapshot();
    void rowsHaveExactLabelsAndPwmBands();
    void copterShowsSimpleColumnsAndHelp();
    void copterWithoutSimpleParametersShowsDisabledColumns();
    void nonCopterHidesSimpleColumnsAndHelp();
    void snapshotHydrationNeverWrites();
    void acceptedRcInputMarksOnlyTheActiveRow();
    void helpButtonEmitsUrlWithoutOpeningIt();
    void destructionNeverWrites();
};

void ConfigFlightModesViewTest::visibleSurfaceIsNonEmptyBeforeSnapshot()
{
    ConfigFlightModesView view;
    view.resize(800, 640);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigFlightModesView"));
    QVERIFY(required<QScrollArea>(
        &view, QStringLiteral("flightModesScroll")));
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("flightModesTitle"))->text(),
             QStringLiteral("Flight Modes"));
    QVERIFY(!required<QLabel>(
                 &view, QStringLiteral("flightModesIntro"))->text().isEmpty());
    QCOMPARE(required<QLabel>(
                 &view,
                 QStringLiteral("flightModesCurrentModeLabel"))->text(),
             QStringLiteral("Current Mode:"));
    QCOMPARE(required<QLabel>(
                 &view,
                 QStringLiteral("flightModesCurrentPwmLabel"))->text(),
             QStringLiteral("Current PWM:"));
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("flightModesSave"))->text(),
             QStringLiteral("Save Modes"));
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("flightModesRefresh"))->text(),
             QStringLiteral("Refresh Params"));
    QVERIFY(!required<QLabel>(
                 &view, QStringLiteral("flightModesStatus"))->text().isEmpty());

    QTimer *freshness = required<QTimer>(
        &view, QStringLiteral("flightModesRcFreshnessTimer"));
    QVERIFY(freshness->isSingleShot());
    QCOMPARE(freshness->interval(), 2000);
    QVERIFY(!freshness->isActive());

    for (int position = 1; position <= 6; ++position) {
        QVERIFY(required<QFrame>(
            &view, QStringLiteral("flightModeRow%1").arg(position)));
        QVERIFY(required<QComboBox>(
            &view, QStringLiteral("flightModeCombo%1").arg(position)));
    }
    QVERIFY(!required<QPushButton>(
                 &view,
                 QStringLiteral("flightModesSuperSimpleHelp"))->isVisible());
}

void ConfigFlightModesViewTest::rowsHaveExactLabelsAndPwmBands()
{
    ConfigFlightModesView view;
    view.resize(800, 640);
    view.show();
    QApplication::processEvents();

    const QStringList bands{
        QStringLiteral("PWM 0 - 1230"),
        QStringLiteral("PWM 1231 - 1360"),
        QStringLiteral("PWM 1361 - 1490"),
        QStringLiteral("PWM 1491 - 1620"),
        QStringLiteral("PWM 1621 - 1749"),
        QStringLiteral("PWM 1750 +")};
    for (int index = 0; index < 6; ++index) {
        const int position = index + 1;
        QCOMPARE(required<QLabel>(
                     &view,
                     QStringLiteral("flightModeLabel%1").arg(position))
                     ->text(),
                 QStringLiteral("Flight Mode %1").arg(position));
        QCOMPARE(required<QLabel>(
                     &view,
                     QStringLiteral("flightModePwm%1").arg(position))
                     ->text(),
                 bands.at(index));
    }
}

void ConfigFlightModesViewTest::copterShowsSimpleColumnsAndHelp()
{
    ConfigFlightModesView view;
    prepareCopter(&view);
    view.resize(800, 640);
    view.show();
    QApplication::processEvents();

    QVERIFY(required<QLabel>(
        &view, QStringLiteral("flightModesSimpleHeader"))->isVisible());
    QVERIFY(required<QLabel>(
        &view, QStringLiteral("flightModesSuperSimpleHeader"))->isVisible());
    QPushButton *help = required<QPushButton>(
        &view, QStringLiteral("flightModesSuperSimpleHelp"));
    QVERIFY(help->isVisible());
    QCOMPARE(help->text(), QStringLiteral("Super Simple Modes"));
    for (int position = 1; position <= 6; ++position) {
        QVERIFY(required<QCheckBox>(
            &view,
            QStringLiteral("flightModeSimple%1").arg(position))->isVisible());
        QVERIFY(required<QCheckBox>(
            &view,
            QStringLiteral("flightModeSuperSimple%1").arg(position))
                    ->isVisible());
    }
}

void ConfigFlightModesViewTest::copterWithoutSimpleParametersShowsDisabledColumns()
{
    ConfigFlightModesView view;
    view.setFamily(
        ConfigFlightModesViewModel::Family::Copter, modeOptions());
    QList<ConfigFriendlyParameterValue> parameters = copterSnapshot();
    parameters.erase(
        std::remove_if(parameters.begin(), parameters.end(),
                       [](const ConfigFriendlyParameterValue &parameter) {
            return parameter.name == QLatin1String("SIMPLE")
                || parameter.name == QLatin1String("SUPER_SIMPLE");
        }),
        parameters.end());
    view.setParameterSnapshot(parameters, 1, true);
    view.setConnected(true);
    view.setHeartbeat(3, true, false);
    view.resize(800, 640);
    view.show();
    QApplication::processEvents();

    QVERIFY(required<QLabel>(
        &view, QStringLiteral("flightModesSimpleHeader"))->isVisible());
    QVERIFY(required<QLabel>(
        &view, QStringLiteral("flightModesSuperSimpleHeader"))->isVisible());
    QVERIFY(required<QPushButton>(
        &view, QStringLiteral("flightModesSuperSimpleHelp"))->isVisible());
    for (int position = 1; position <= 6; ++position) {
        QCheckBox *simple = required<QCheckBox>(
            &view, QStringLiteral("flightModeSimple%1").arg(position));
        QCheckBox *superSimple = required<QCheckBox>(
            &view,
            QStringLiteral("flightModeSuperSimple%1").arg(position));
        QVERIFY(simple->isVisible());
        QVERIFY(superSimple->isVisible());
        QVERIFY(!simple->isEnabled());
        QVERIFY(!superSimple->isEnabled());
    }
}

void ConfigFlightModesViewTest::nonCopterHidesSimpleColumnsAndHelp()
{
    ConfigFlightModesView view;
    view.setFamily(
        ConfigFlightModesViewModel::Family::Plane, modeOptions());
    view.setParameterSnapshot(planeSnapshot(), 1, true);
    view.setConnected(true);
    view.setHeartbeat(3, true, false);
    view.resize(800, 640);
    view.show();
    QApplication::processEvents();

    QVERIFY(!required<QLabel>(
        &view, QStringLiteral("flightModesSimpleHeader"))->isVisible());
    QVERIFY(!required<QLabel>(
        &view, QStringLiteral("flightModesSuperSimpleHeader"))->isVisible());
    QVERIFY(!required<QPushButton>(
        &view, QStringLiteral("flightModesSuperSimpleHelp"))->isVisible());
    for (int position = 1; position <= 6; ++position) {
        QVERIFY(!required<QCheckBox>(
            &view,
            QStringLiteral("flightModeSimple%1").arg(position))->isVisible());
        QVERIFY(!required<QCheckBox>(
            &view,
            QStringLiteral("flightModeSuperSimple%1").arg(position))
                     ->isVisible());
    }
}

void ConfigFlightModesViewTest::snapshotHydrationNeverWrites()
{
    ConfigFlightModesView view;
    QSignalSpy writes(&view, &ConfigFlightModesView::writeRequested);
    prepareCopter(&view);
    view.show();
    QApplication::processEvents();

    QCOMPARE(writes.count(), 0);
    const int expectedModes[] = {0, 3, 5, 0, 3, 5};
    for (int index = 0; index < 6; ++index) {
        QComboBox *combo = required<QComboBox>(
            &view, QStringLiteral("flightModeCombo%1").arg(index + 1));
        QCOMPARE(combo->currentData().toInt(), expectedModes[index]);
        QVERIFY(combo->isEnabled());
    }
    QVERIFY(required<QCheckBox>(
        &view, QStringLiteral("flightModeSimple1"))->isChecked());
    QVERIFY(!required<QCheckBox>(
        &view, QStringLiteral("flightModeSimple2"))->isChecked());
    QVERIFY(required<QCheckBox>(
        &view, QStringLiteral("flightModeSimple3"))->isChecked());
    QVERIFY(required<QCheckBox>(
        &view, QStringLiteral("flightModeSuperSimple2"))->isChecked());
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("flightModesCurrentMode"))->text(),
             QStringLiteral("Auto"));
    QVERIFY(!required<QPushButton>(
        &view, QStringLiteral("flightModesSave"))->isEnabled());
    QCOMPARE(writes.count(), 0);
}

void ConfigFlightModesViewTest::acceptedRcInputMarksOnlyTheActiveRow()
{
    ConfigFlightModesView view;
    prepareCopter(&view);
    QTimer *freshness = required<QTimer>(
        &view, QStringLiteral("flightModesRcFreshnessTimer"));

    QVERIFY(!view.setRcInput(6, 1500));
    QVERIFY(!freshness->isActive());
    QCOMPARE(view.viewModel()->ActiveRow(), -1);

    QVERIFY(view.setRcInput(5, 1500));
    QVERIFY(freshness->isActive());
    QCOMPARE(view.viewModel()->ActiveRow(), 3);
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("flightModesCurrentPwm"))->text(),
             QStringLiteral("5: 1500"));
    for (int index = 0; index < 6; ++index) {
        QFrame *row = required<QFrame>(
            &view, QStringLiteral("flightModeRow%1").arg(index + 1));
        QCOMPARE(row->property("flightModeActive").toBool(), index == 3);
    }
}

void ConfigFlightModesViewTest::helpButtonEmitsUrlWithoutOpeningIt()
{
    ConfigFlightModesView view;
    prepareCopter(&view);
    view.show();
    QApplication::processEvents();
    QSignalSpy help(&view, &ConfigFlightModesView::helpRequested);

    required<QPushButton>(
        &view, QStringLiteral("flightModesSuperSimpleHelp"))->click();

    QCOMPARE(help.count(), 1);
    QCOMPARE(help.at(0).at(0).toUrl(), QUrl(QStringLiteral(
        "https://ardupilot.org/copter/docs/simpleandsuper-simple-modes.html")));
}

void ConfigFlightModesViewTest::destructionNeverWrites()
{
    auto *view = new ConfigFlightModesView;
    prepareCopter(view);
    QSignalSpy writes(view, &ConfigFlightModesView::writeRequested);
    delete view;
    QCOMPARE(writes.count(), 0);
}

QTEST_MAIN(ConfigFlightModesViewTest)
#include "test_configflightmodesview.moc"
