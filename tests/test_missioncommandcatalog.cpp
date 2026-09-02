#include "ui/flightplanner/MissionCommandCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class MissionCommandCatalogTest final : public QObject
{
    Q_OBJECT

private slots:
    void mergesBuiltinsAndPersistsExactMissionPlannerSchema();
    void malformedObjectsFallBackIndependently();
    void validatesCompleteCandidateAtomically();
    void qmlMapsRequireIntegralIds();
};

void MissionCommandCatalogTest::mergesBuiltinsAndPersistsExactMissionPlannerSchema()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("catalog.ini")),
                       QSettings::IniFormat);
    MissionCommandCatalog catalog(&settings);

    QVERIFY(catalog.Names().size() > 100);
    QCOMPARE(catalog.GetName(16), QStringLiteral("WAYPOINT"));
    QCOMPARE(catalog.GetId(QStringLiteral("waypoint")), 16);

    QVector<MissionCommandDefinition> rows{
        {16, QStringLiteral("WAYPOINT"),
         {QStringLiteral("Hold"), QString(), QString(),
          QStringLiteral("Heading")}},
        {61000, QStringLiteral("VENDOR_SCAN"),
         {QStringLiteral("Rows"), QString(), QString(), QString(),
          QStringLiteral("North"), QStringLiteral("East"),
          QStringLiteral("Height")}}
    };
    QSignalSpy changed(&catalog, &MissionCommandCatalog::catalogChanged);
    QSignalSpy revision(&catalog, &MissionCommandCatalog::revisionChanged);
    QString error;
    QVERIFY2(catalog.Save(rows, &error), qPrintable(error));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(revision.count(), 1);
    QCOMPARE(catalog.Revision(), qulonglong(1));
    QVERIFY(catalog.Names().contains(QStringLiteral("VENDOR_SCAN")));
    QCOMPARE(catalog.GetId(QStringLiteral("vendor_scan")), 61000);
    QCOMPARE(catalog.GetName(61000), QStringLiteral("VENDOR_SCAN"));
    QCOMPARE(catalog.EffectiveLabels(61000).at(0), QStringLiteral("Rows"));
    QCOMPARE(catalog.EffectiveLabels(16).at(1), QStringLiteral("P2"));

    const QJsonObject labels = QJsonDocument::fromJson(
        settings.value(QStringLiteral("PlannerExtraCommand"))
            .toString().toUtf8()).object();
    const QJsonObject ids = QJsonDocument::fromJson(
        settings.value(QStringLiteral("PlannerExtraCommandIDs"))
            .toString().toUtf8()).object();
    QCOMPARE(labels.size(), 2);
    QCOMPARE(labels.value(QStringLiteral("WAYPOINT")).toArray().size(), 7);
    QVERIFY(!ids.contains(QStringLiteral("WAYPOINT")));
    QCOMPARE(ids.value(QStringLiteral("VENDOR_SCAN")).toInt(), 61000);

    MissionCommandCatalog reloaded(&settings);
    QCOMPARE(reloaded.LoadDefinitions().size(), 2);
    QCOMPARE(reloaded.GetLabels(61000).at(6), QStringLiteral("Height"));

    settings.setValue(QStringLiteral("PlannerExtraCommandIDs"),
                      QStringLiteral("{\"EXTERNAL_CMD\":62000}"));
    QCOMPARE(reloaded.GetId(QStringLiteral("EXTERNAL_CMD")), 62000);
    QVERIFY(reloaded.Names().contains(QStringLiteral("EXTERNAL_CMD")));
}

void MissionCommandCatalogTest::malformedObjectsFallBackIndependently()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("catalog.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("PlannerExtraCommand"),
                      QStringLiteral("not json"));
    settings.setValue(QStringLiteral("PlannerExtraCommandIDs"),
                      QStringLiteral("{\"ONLY_ID\":44000}"));
    MissionCommandCatalog catalog(&settings);
    QCOMPARE(catalog.LoadDefinitions().size(), 1);
    QCOMPARE(catalog.LoadDefinitions().at(0).Name,
             QStringLiteral("ONLY_ID"));
    QCOMPARE(catalog.GetLabels(44000), QStringList());

    settings.setValue(QStringLiteral("PlannerExtraCommand"),
                      QStringLiteral("{\"WAYPOINT\":null}"));
    settings.setValue(QStringLiteral("PlannerExtraCommandIDs"),
                      QStringLiteral("{broken"));
    const QVector<MissionCommandDefinition> labelsOnly =
        catalog.LoadDefinitions();
    QCOMPARE(labelsOnly.size(), 1);
    QCOMPARE(labelsOnly.at(0).Id, quint16(16));
    QCOMPARE(labelsOnly.at(0).ParameterLabels,
             QStringList({QString(), QString(), QString(), QString(),
                          QString(), QString(), QString()}));
    QVERIFY(!catalog.Names().contains(QStringLiteral("ONLY_ID")));

    settings.setValue(QStringLiteral("PlannerExtraCommand"),
                      QStringLiteral("{\"Foo\":[],\"FOO\":[]}"));
    QCOMPARE(catalog.LoadDefinitions().size(), 0);

    settings.setValue(QStringLiteral("PlannerExtraCommand"),
                      QStringLiteral("{\"MixedCase\":[]}"));
    settings.setValue(QStringLiteral("PlannerExtraCommandIDs"),
                      QStringLiteral("{\"MIXEDCASE\":61002}"));
    const QVector<MissionCommandDefinition> mixedCase =
        catalog.LoadDefinitions();
    QCOMPARE(mixedCase.size(), 1);
    QCOMPARE(mixedCase.at(0).Name, QStringLiteral("MixedCase"));
}

void MissionCommandCatalogTest::validatesCompleteCandidateAtomically()
{
    const auto rejected = [](QVector<MissionCommandDefinition> rows,
                             const QString &fragment) {
        QString error;
        QVERIFY(!MissionCommandCatalog::Validate(rows, &error));
        QVERIFY2(error.contains(fragment, Qt::CaseInsensitive),
                 qPrintable(error));
    };
    rejected({{61000, QStringLiteral("ONE"), {}},
              {61000, QStringLiteral("TWO"), {}}},
             QStringLiteral("ID 61000"));
    rejected({{61000, QStringLiteral("Same"), {}},
              {61001, QStringLiteral("same"), {}}},
             QStringLiteral("used more than once"));
    rejected({{61000, QString(), {}}}, QStringLiteral("no name"));
    rejected({{61000, QStringLiteral("BAD NAME"), {}}},
             QStringLiteral("letters"));
    rejected({{61000, QStringLiteral(" WAYPOINT "), {}}},
             QStringLiteral("letters"));
    rejected({{16, QStringLiteral("LAND"), {}}},
             QStringLiteral("retain"));
    rejected({{61000, QStringLiteral("WAYPOINT"), {}}},
             QStringLiteral("cannot reuse"));

    QString error;
    QVERIFY(MissionCommandCatalog::Validate(
        {{16, QStringLiteral("waypoint"), {}},
         {61000, QStringLiteral("КОМАНДА_1"), {}}}, &error));
    QCOMPARE(MissionCommandCatalog::NormalizeLabels(
                 {QStringLiteral("A"), QStringLiteral("B")}).size(), 7);
    QCOMPARE(MissionCommandCatalog::NormalizeLabels(
                 {QStringLiteral("1"), QStringLiteral("2"),
                  QStringLiteral("3"), QStringLiteral("4"),
                  QStringLiteral("5"), QStringLiteral("6"),
                  QStringLiteral("7"), QStringLiteral("8")}).last(),
             QStringLiteral("7"));
}

void MissionCommandCatalogTest::qmlMapsRequireIntegralIds()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("catalog.ini")),
                       QSettings::IniFormat);
    MissionCommandCatalog catalog(&settings);
    QVariantMap invalid{{QStringLiteral("id"), 62000.5},
                        {QStringLiteral("name"), QStringLiteral("FRACTION")}};
    QSignalSpy failed(&catalog, &MissionCommandCatalog::saveFailed);
    QVERIFY(!catalog.SaveDefinitionMaps({invalid}));
    QCOMPARE(failed.count(), 1);

    QVariantMap valid{{QStringLiteral("id"), 62000.0},
                      {QStringLiteral("name"), QStringLiteral("QML_CMD")},
                      {QStringLiteral("parameterLabels"),
                       QVariantList{QStringLiteral("First")}}};
    QVERIFY(catalog.SaveDefinitionMaps({valid}));
    QCOMPARE(catalog.DefinitionMaps().at(0).toMap()
                 .value(QStringLiteral("parameterLabels"))
                 .toStringList().size(), 7);
}

QTEST_APPLESS_MAIN(MissionCommandCatalogTest)
#include "test_missioncommandcatalog.moc"
