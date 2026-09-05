#include <QtTest>
#include "ui/flightdata/QuickViewWidget.h"
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <limits>

class QuickViewWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void referenceDefaultsPersistenceAndLimits() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("quick.ini"), QSettings::IniFormat);
        QStringList fields{"alt", "groundspeed", "current", "airspeed", "verticalspeed", "DistToHome", "battery_voltage"};
        QuickViewWidget view(&settings, fields);
        QCOMPARE(view.objectName(), QString("QuickHost"));
        QCOMPARE(view.columns(), 2);
        QCOMPARE(view.selectedFields().size(), 6);
        QCOMPARE(view.selectedFields().first(), QString("alt"));
        QVERIFY(!view.setGridLayout(0, 6));
        QVERIFY(!view.setGridLayout(7, 6));
        QVERIFY(!view.setGridLayout(2, 13));
        QVERIFY(view.setGridLayout(3, 12));
        QVERIFY(view.setField(11, "battery_voltage"));
        QVERIFY(!view.setField(12, "alt"));
        QVERIFY(!view.setField(0, "unknown"));
        QuickViewWidget restored(&settings, fields);
        QCOMPARE(restored.columns(), 3);
        QCOMPARE(restored.selectedFields().at(11), QString("battery_voltage"));
        QCOMPARE(restored.findChildren<QLabel *>().size(), 24);
    }
    void colorsContrastResetAndMissingValues() {
        QuickViewWidget view(nullptr, {"alt", "current"});
        view.setUnits([](const QString &name) { return name == "alt" ? QString("m") : QString("A"); });
        auto *cell = view.findChild<QWidget *>("QuickCell_0");
        auto *number = view.findChild<QLabel *>("QuickNumber_0");
        auto *label = view.findChild<QLabel *>("QuickDescription_0");
        QVERIFY(cell && number && label);
        view.setValues({{"alt", 12.5}});
        view.setWarningColors({{"alt", "Yellow"}});
        QCOMPARE(cell->property("warningColor").toString(), QString("Yellow"));
        QCOMPARE(number->text(), QString("12.50"));
        QCOMPARE(label->text(), QString("alt (m)"));
        QVERIFY(number->styleSheet().contains("#000000"));
        view.setWarningColors({{"alt", "Maroon"}});
        QVERIFY(number->styleSheet().contains("#ffffff"));
        view.setValues({});
        QCOMPARE(cell->property("warningColor").toString(), QString("NoColor"));
        QCOMPARE(number->text(), QString::fromUtf8("—"));
        view.setValues({{"alt", std::numeric_limits<double>::quiet_NaN()}});
        QCOMPARE(number->text(), QString::fromUtf8("—"));
        view.setValues({{"alt", 20}, {"current", 3}});
        QVERIFY(view.setField(0, "current"));
        QCOMPARE(cell->property("warningColor").toString(), QString("NoColor"));
        QVERIFY(number->styleSheet().contains("#D197F8"));
        view.setWarningColors({{"current", "not-a-color"}});
        QCOMPARE(cell->property("warningColor").toString(), QString("NoColor"));
    }
};
QTEST_MAIN(QuickViewWidgetTest)
#include "test_quickviewwidget.moc"
