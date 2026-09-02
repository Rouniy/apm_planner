#include "ui/configuration/ConfigMavCommandView.h"
#include "ui/configuration/ConfigMavCommandViewModel.h"
#include "ui/BackstageView.h"

#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>

class ConfigMavCommandViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void viewMatchesMissionPlannerContract();
    void stagedWorkflowSavesAndReloads();
};

void ConfigMavCommandViewTest::viewMatchesMissionPlannerContract()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("catalog.ini")),
                       QSettings::IniFormat);
    MissionCommandCatalog catalog(&settings);
    ConfigMavCommandViewModel model(&catalog);
    ConfigMavCommandView view(&model);

    QCOMPARE(view.objectName(), QStringLiteral("ConfigMavCommandView"));
    auto *layout = qobject_cast<QVBoxLayout *>(view.layout());
    QVERIFY(layout);
    QCOMPARE(layout->contentsMargins(), QMargins(16, 16, 16, 16));
    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
        return label->text() == QStringLiteral("Mission Command List");
    }));

    QTableView *table = view.commandTable();
    QCOMPARE(table->model(), &model);
    QCOMPARE(model.columnCount(), 9);
    const QStringList headers{
        QStringLiteral("ID"), QStringLiteral("Name"),
        QStringLiteral("P1"), QStringLiteral("P2"),
        QStringLiteral("P3"), QStringLiteral("P4"),
        QStringLiteral("P5 / X"), QStringLiteral("P6 / Y"),
        QStringLiteral("P7 / Z")};
    const int widths[] = {70, 190, 100, 100, 100, 100, 100, 100, 100};
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(model.headerData(column, Qt::Horizontal).toString(),
                 headers.at(column));
        QCOMPARE(table->columnWidth(column), widths[column]);
    }
    QStringList buttonTexts;
    for (QPushButton *button : view.findChildren<QPushButton *>()) {
        buttonTexts.append(button->text());
    }
    QCOMPARE(buttonTexts, QStringList({QStringLiteral("Add"),
                                      QStringLiteral("Remove"),
                                      QStringLiteral("Save"),
                                      QStringLiteral("Reload")}));

    const BackstagePage page = configMavCommandBackstagePage();
    QCOMPARE(page.id, QStringLiteral("ConfigMavCommandView"));
    QCOMPARE(page.header, QStringLiteral("Mission Command List"));
    QVERIFY(page.isSub);
    QVERIFY(page.isAdvanced);
    QVERIFY(!page.requiresConnection);
    QVERIFY(page.allowsPartialParameters);
    QVERIFY(bool(page.factory));
}

void ConfigMavCommandViewTest::stagedWorkflowSavesAndReloads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("catalog.ini")),
                       QSettings::IniFormat);
    MissionCommandCatalog catalog(&settings);
    ConfigMavCommandViewModel model(&catalog);
    QCOMPARE(model.rowCount(), 0);

    const int custom = model.AddCommand(61000, QStringLiteral("vendor_scan"));
    QCOMPARE(custom, 0);
    QCOMPARE(model.data(model.index(custom, 1)).toString(),
             QStringLiteral("VENDOR_SCAN"));
    QCOMPARE(model.data(model.index(custom, 6)).toString(),
             QStringLiteral("Lat"));
    QVERIFY(!settings.contains(QStringLiteral("PlannerExtraCommand")));
    QVERIFY(model.RemoveCommand(custom));
    QCOMPARE(model.rowCount(), 0);
    model.ReloadCommand();
    QCOMPARE(model.rowCount(), 0);

    const int waypoint = model.AddCommand(16);
    QCOMPARE(model.data(model.index(waypoint, 1)).toString(),
             QStringLiteral("WAYPOINT"));
    const int vendor = model.AddCommand(61000, QStringLiteral("vendor_scan"));
    QVERIFY(model.setData(model.index(vendor, 2), QStringLiteral("Rows")));
    QVERIFY(!model.setData(model.index(vendor, 0), 61000.5));
    QCOMPARE(model.data(model.index(vendor, 0)).toInt(), 61000);
    QVERIFY(model.SaveCommand());
    QVERIFY(model.Status().contains(QStringLiteral("Saved 2")));
    QCOMPARE(catalog.GetLabels(61000).at(0), QStringLiteral("Rows"));

    QVERIFY(model.RemoveCommand(waypoint));
    QCOMPARE(model.rowCount(), 1);
    model.ReloadCommand();
    QCOMPARE(model.rowCount(), 2);

    QVERIFY(model.setData(model.index(1, 0), 16));
    QVERIFY(!model.SaveCommand());
    QVERIFY(model.Status().startsWith(QStringLiteral("Cannot save:")));
    QCOMPARE(catalog.LoadDefinitions().size(), 2);
}

QTEST_MAIN(ConfigMavCommandViewTest)
#include "test_configmavcommandview.moc"
