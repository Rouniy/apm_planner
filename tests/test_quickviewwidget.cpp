#include <QtTest>
#include "ui/flightdata/QuickViewWidget.h"
#include <QLabel>
#include <QFontMetrics>
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
    void textScalesWithCellAndCanShrinkAgain_data() {
        QTest::addColumn<bool>("themed");
        QTest::newRow("without-theme") << false;
        QTest::newRow("production-ancestor-font-rule") << true;
    }
    void textScalesWithCellAndCanShrinkAgain() {
        QFETCH(bool, themed);
        QuickViewWidget view(nullptr, {"alt", "groundspeed", "current", "airspeed",
                                        "verticalspeed", "DistToHome"});
        if (themed)
            view.setStyleSheet("QWidget { font-family: 'Bitstream Vera Sans'; font-size: 11px; }");
        view.setUnits([](const QString &) { return QString("m"); });
        view.setValues({{"alt", 12.5}});
        view.resize(320, 240);
        view.show();
        QCoreApplication::processEvents();
        auto *number = view.findChild<QLabel *>("QuickNumber_0");
        auto *description = view.findChild<QLabel *>("QuickDescription_0");
        QVERIFY(number && description);
        const int smallNumber = number->font().pixelSize();
        const int smallDescription = description->font().pixelSize();
        QVERIFY(smallNumber > 0);
        QVERIFY(smallDescription > 0);

        view.resize(900, 600);
        QCoreApplication::processEvents();
        const int largeNumber = number->font().pixelSize();
        const int largeDescription = description->font().pixelSize();
        QVERIFY(largeNumber > smallNumber * 1.5);
        QVERIFY(largeDescription > smallDescription * 1.5);
        QVERIFY(largeNumber > 24); // the former fixed size must not cap a large tile
        view.setWarningColors({{"alt", "Yellow"}});
        QCoreApplication::processEvents();
        QCOMPARE(number->font().pixelSize(), largeNumber);
        QCOMPARE(description->font().pixelSize(), largeDescription);
        QCOMPARE(description->text(), QString("alt (m)"));
        QVERIFY(number->styleSheet().contains("#000000"));
        view.setValues({{"alt", 12.6}}); // repeated refresh must retain the font-size rule
        QCoreApplication::processEvents();
        QCOMPARE(number->font().pixelSize(), largeNumber);
        QCOMPARE(description->font().pixelSize(), largeDescription);

        view.resize(320, 240);
        QCoreApplication::processEvents();
        QCOMPARE(view.size(), QSize(320, 240));
        QCOMPARE(number->font().pixelSize(), smallNumber);
        QCOMPARE(description->font().pixelSize(), smallDescription);
    }
    void fontsRefitAfterLayoutFieldUnitsAndValueChanges_data() {
        QTest::addColumn<bool>("themed");
        QTest::newRow("without-theme") << false;
        QTest::newRow("production-ancestor-font-rule") << true;
    }
    void fontsRefitAfterLayoutFieldUnitsAndValueChanges() {
        QFETCH(bool, themed);
        const QString longField = "gps2_horizontal_accuracy";
        QuickViewWidget view(nullptr, {"alt", "lat", longField});
        if (themed)
            view.setStyleSheet("QWidget { font-family: 'Bitstream Vera Sans'; font-size: 11px; }");
        QVERIFY(view.setGridLayout(1, 1));
        view.setValues({{"alt", 12.5}, {"lat", -179.1234567}, {longField, -123456789012.25}});
        view.resize(360, 210);
        view.show();
        QCoreApplication::processEvents();
        auto *number = view.findChild<QLabel *>("QuickNumber_0");
        auto *description = view.findChild<QLabel *>("QuickDescription_0");
        QVERIFY(number && description);
        const int shortNumberSize = number->font().pixelSize();
        QVERIFY(view.setField(0, "lat"));
        QCoreApplication::processEvents();
        QCOMPARE(number->text(), QString("-179.1234567"));
        QVERIFY(number->font().pixelSize() < shortNumberSize);
        QVERIFY(view.setField(0, longField));
        view.setUnits([](const QString &) { return QString("meters per second squared"); });
        QCoreApplication::processEvents();
        QCOMPARE(description->text(), longField + " (meters per second squared)");
        QCOMPARE(number->text(), QString("-123456789012.25"));
        for (QLabel *label : {description, number}) {
            const QFontMetrics metrics(label->font());
            QVERIFY(metrics.horizontalAdvance(label->text()) <= label->contentsRect().width());
            QVERIFY(metrics.height() <= label->contentsRect().height());
        }

        // The same host must resize its rebuilt tiles, not retain the old font
        // or its size hint when the operator switches to a dense twelve-cell grid.
        QVERIFY(view.setField(0, "alt"));
        view.setUnits({});
        view.resize(900, 600);
        QCoreApplication::processEvents();
        const int singleNumberSize = number->font().pixelSize();
        const int singleDescriptionSize = description->font().pixelSize();
        QVERIFY(view.setGridLayout(3, 12));
        QCoreApplication::processEvents();
        number = view.findChild<QLabel *>("QuickNumber_0");
        description = view.findChild<QLabel *>("QuickDescription_0");
        QVERIFY(number && description);
        QVERIFY(number->font().pixelSize() < singleNumberSize);
        QVERIFY(description->font().pixelSize() < singleDescriptionSize);
        QCOMPARE(view.findChildren<QLabel *>().size(), 24);
        for (QLabel *label : view.findChildren<QLabel *>()) {
            const QFontMetrics metrics(label->font());
            QVERIFY(metrics.horizontalAdvance(label->text()) <= label->contentsRect().width());
            QVERIFY(metrics.height() <= label->contentsRect().height());
        }
    }
};
QTEST_MAIN(QuickViewWidgetTest)
#include "test_quickviewwidget.moc"
