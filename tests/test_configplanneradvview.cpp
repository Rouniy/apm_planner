#include <QtTest>

#include "ui/configuration/ConfigPlannerAdvView.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QDir>
#include <QHeaderView>
#include <QFile>
#include <QLabel>
#include <QLocale>
#include <QMap>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <memory>

namespace {
QStringList tableNames(const QTableWidget *table)
{
    QStringList result;
    for (int row = 0; row < table->rowCount(); ++row) {
        result.append(table->item(row, 0)->text());
    }
    return result;
}

int rowForName(const QTableWidget *table, const QString &name)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == name) {
            return row;
        }
    }
    return -1;
}

QMap<QString, QVariant> settingsSnapshot(QSettings &settings)
{
    QMap<QString, QVariant> result;
    for (const QString &key : settings.allKeys()) {
        result.insert(key, settings.value(key));
    }
    return result;
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}
}

class ConfigPlannerAdvViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void modelActivatesSortedReadOnlySnapshot();
    void activateReplacesRowsAndFormatsSafeValues();
    void viewMatchesMissionPlannerLayoutAndRefresh();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void ConfigPlannerAdvViewTest::initTestCase()
{
    m_settingsDirectory.reset(new QTemporaryDir);
    QVERIFY(m_settingsDirectory->isValid());
    const QString userSettings = QDir(m_settingsDirectory->path()).filePath(
        QStringLiteral("user"));
    const QString systemSettings = QDir(m_settingsDirectory->path()).filePath(
        QStringLiteral("system"));
    QVERIFY(QDir().mkpath(userSettings));
    QVERIFY(QDir().mkpath(systemSettings));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       userSettings);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       systemSettings);
    QCoreApplication::setOrganizationName(
        QStringLiteral("APMPlannerAdvancedTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ConfigPlannerAdvView"));
}

void ConfigPlannerAdvViewTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QSettings organizationDefaults(
        QSettings::IniFormat, QSettings::UserScope,
        QCoreApplication::organizationName());
    organizationDefaults.clear();
    organizationDefaults.sync();
    QSettings systemApplication(
        QSettings::IniFormat, QSettings::SystemScope,
        QCoreApplication::organizationName(),
        QCoreApplication::applicationName());
    systemApplication.clear();
    systemApplication.sync();
    QSettings systemOrganization(
        QSettings::IniFormat, QSettings::SystemScope,
        QCoreApplication::organizationName());
    systemOrganization.clear();
    systemOrganization.sync();
    QSettings legacyApplication(
        QSettings::IniFormat, QSettings::UserScope,
        QCoreApplication::organizationName(), QStringLiteral("LegacyApp"));
    legacyApplication.clear();
    legacyApplication.sync();
}

void ConfigPlannerAdvViewTest::modelActivatesSortedReadOnlySnapshot()
{
    QSettings settings;
    settings.setValue(QStringLiteral("zeta"), 42);
    settings.setValue(QStringLiteral("Alpha"), true);
    settings.setValue(QStringLiteral("beta"),
                      QStringList({QStringLiteral("one"),
                                   QStringLiteral("two")}));
    settings.setValue(QStringLiteral("blob"), QByteArray("abc", 3));
    settings.setValue(QStringLiteral("Group/name"),
                      QStringLiteral("nested"));
    settings.sync();
    QSettings organizationDefaults(
        QSettings::IniFormat, QSettings::UserScope,
        QCoreApplication::organizationName());
    organizationDefaults.setValue(
        QStringLiteral("organizationFallback"),
        QStringLiteral("must stay hidden"));
    organizationDefaults.sync();
    QSettings systemApplication(
        QSettings::IniFormat, QSettings::SystemScope,
        QCoreApplication::organizationName(),
        QCoreApplication::applicationName());
    systemApplication.setValue(
        QStringLiteral("systemApplicationFallback"), 1);
    systemApplication.sync();
    QSettings systemOrganization(
        QSettings::IniFormat, QSettings::SystemScope,
        QCoreApplication::organizationName());
    systemOrganization.setValue(
        QStringLiteral("systemOrganizationFallback"), 1);
    systemOrganization.sync();
    QSettings legacyApplication(
        QSettings::IniFormat, QSettings::UserScope,
        QCoreApplication::organizationName(), QStringLiteral("LegacyApp"));
    legacyApplication.setValue(QStringLiteral("legacyOnly"), 1);
    legacyApplication.sync();
    const QMap<QString, QVariant> before = settingsSnapshot(settings);

    ConfigPlannerAdvViewModel model;
    const QList<PlannerAdvancedSettingRow> rows = model.Params();
    QCOMPARE(rows.size(), 5);
    QStringList names;
    for (const PlannerAdvancedSettingRow &row : rows) {
        names.append(row.Name);
    }
    QCOMPARE(names, QStringList({QStringLiteral("Alpha"),
                                 QStringLiteral("beta"),
                                 QStringLiteral("blob"),
                                 QStringLiteral("Group/name"),
                                 QStringLiteral("zeta")}));
    QCOMPARE(rows.at(0).Value, QStringLiteral("true"));
    QCOMPARE(rows.at(1).Value, QStringLiteral("one, two"));
    QCOMPARE(rows.at(2).Value, QStringLiteral("<binary data: 3 bytes>"));
    QCOMPARE(rows.at(3).Value, QStringLiteral("nested"));
    QCOMPARE(rows.at(4).Value, QStringLiteral("42"));
    QCOMPARE(settingsSnapshot(settings), before);
    QVERIFY(!names.contains(QStringLiteral("organizationFallback")));
    QVERIFY(!names.contains(QStringLiteral("systemApplicationFallback")));
    QVERIFY(!names.contains(QStringLiteral("systemOrganizationFallback")));
    QVERIFY(!names.contains(QStringLiteral("legacyOnly")));
}

void ConfigPlannerAdvViewTest::activateReplacesRowsAndFormatsSafeValues()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Old"), QStringLiteral("value"));
    settings.sync();
    ConfigPlannerAdvViewModel model;
    QCOMPARE(model.Params().size(), 1);
    QCOMPARE(model.Params().first().Name, QStringLiteral("Old"));
    QSignalSpy changed(&model, &ConfigPlannerAdvViewModel::paramsChanged);

    settings.remove(QStringLiteral("Old"));
    settings.setValue(QStringLiteral("New"), QString());
    settings.sync();
    QCOMPARE(model.Params().first().Name, QStringLiteral("Old"));
    const QByteArray beforeRefresh = fileContents(settings.fileName());
    model.Activate();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(model.Params().size(), 1);
    QCOMPARE(model.Params().first().Name, QStringLiteral("New"));
    QCOMPARE(model.Params().first().Value, QString());
    QCOMPARE(ConfigPlannerAdvViewModel::DisplayValue(QVariant()), QString());
    QCOMPARE(fileContents(settings.fileName()), beforeRefresh);

    const QLocale previousLocale = QLocale();
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    QCOMPARE(ConfigPlannerAdvViewModel::DisplayValue(1.5),
             QStringLiteral("1.5"));
    QCOMPARE(ConfigPlannerAdvViewModel::DisplayValue(
                 QStringLiteral("<b>текст</b>\nline")),
             QStringLiteral("<b>текст</b>\nline"));
    QLocale::setDefault(previousLocale);

    settings.clear();
    settings.sync();
    model.Activate();
    QVERIFY(model.Params().isEmpty());
}

void ConfigPlannerAdvViewTest::viewMatchesMissionPlannerLayoutAndRefresh()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Bravo"), QStringLiteral("2"));
    settings.setValue(QStringLiteral("alpha"), QStringLiteral("1"));
    settings.sync();

    ConfigPlannerAdvView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigPlannerAdvView"));
    auto *root = qobject_cast<QVBoxLayout *>(view.layout());
    QVERIFY(root);
    QCOMPARE(root->contentsMargins(), QMargins(16, 16, 16, 16));
    QCOMPARE(root->spacing(), 0);
    QCOMPARE(view.findChild<QLabel *>(
                 QStringLiteral("plannerAdvancedTitle"))->text(),
             QStringLiteral("Planner Advanced (config.xml)"));
    auto *refresh = view.findChild<QPushButton *>(
        QStringLiteral("refreshButton"));
    QCOMPARE(refresh->text(), QStringLiteral("Refresh"));

    auto *table = view.findChild<QTableWidget *>(
        QStringLiteral("settingsTable"));
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 2);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Name"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("Value"));
    QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(table->isSortingEnabled());
    QVERIFY(table->showGrid());
    QCOMPARE(tableNames(table), QStringList({QStringLiteral("alpha"),
                                             QStringLiteral("Bravo")}));

    view.resize(640, 480);
    view.show();
    QCoreApplication::processEvents();
    const int available = table->viewport()->width();
    QVERIFY(available > 0);
    QVERIFY(qAbs(table->columnWidth(0) - available * 2 / 5) <= 2);

    table->sortItems(1, Qt::DescendingOrder);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("2"));
    settings.remove(QStringLiteral("Bravo"));
    settings.setValue(QStringLiteral("charlie"), QStringLiteral("3"));
    settings.sync();
    refresh->click();
    QCOMPARE(tableNames(table), QStringList({QStringLiteral("charlie"),
                                             QStringLiteral("alpha")}));
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 1);
    QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(),
             Qt::DescendingOrder);
    const int charlie = rowForName(table, QStringLiteral("charlie"));
    QVERIFY(charlie >= 0);
    QCOMPARE(table->item(charlie, 1)->text(), QStringLiteral("3"));
    QCOMPARE(table->item(charlie, 1)->toolTip(), QStringLiteral("3"));
}

QTEST_MAIN(ConfigPlannerAdvViewTest)

#include "test_configplanneradvview.moc"
