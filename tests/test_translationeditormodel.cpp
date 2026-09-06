#include "ui/TranslationEditorModel.h"

#include <QtTest>

#include <QSignalSpy>

namespace
{
ResxTranslationProject sampleProject()
{
    ResxTranslationProject project;
    project.sourceRoot = QStringLiteral("/translation/source");
    project.culture = QStringLiteral("fr-FR");
    project.resourceFiles = 2;
    project.warnings = QStringList{QStringLiteral("one unreadable resource")};
    project.entries = {
        {QStringLiteral("GCSViews/FlightData.resx"),
         QStringLiteral("save.Text"), QStringLiteral("Save"),
         QStringLiteral("Save"), QStringLiteral("Button label"), false},
        {QStringLiteral("GCSViews/FlightData.resx"),
         QStringLiteral("open.Text"), QStringLiteral("Open"),
         QStringLiteral("Ouvrir"), QString(), true},
        {QStringLiteral("Strings.resx"), QStringLiteral("Welcome"),
         QStringLiteral("Welcome"), QStringLiteral("Welcome"),
         QStringLiteral("Greeting"), true}};
    return project;
}
} // namespace

class TranslationEditorModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesFiveColumnsRolesAndOnlyTranslationIsEditable();
    void tracksAcceptedBaselineAndRevertsAllRows();
    void filtersWithoutDiscardingTheFullProject();
    void importsUniqueExactBasenameAndResourcesAliasesInSourceOrder();
    void rejectsAmbiguousAliasesAndKeepsExactKeysCaseSensitive();
    void replacesAndClearsProjectMetadata();
};

void TranslationEditorModelTest::
    exposesFiveColumnsRolesAndOnlyTranslationIsEditable()
{
    TranslationEditorModel model;
    model.loadProject(sampleProject());

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.columnCount(), 5);
    QCOMPARE(model.headerData(TranslationEditorModel::FileColumn,
                              Qt::Horizontal).toString(),
             QStringLiteral("File"));
    QCOMPARE(model.headerData(TranslationEditorModel::InternalKeyColumn,
                              Qt::Horizontal).toString(),
             QStringLiteral("Internal key"));
    QCOMPARE(model.headerData(TranslationEditorModel::SourceTextColumn,
                              Qt::Horizontal).toString(),
             QStringLiteral("English / neutral"));
    QCOMPARE(model.headerData(TranslationEditorModel::TranslationColumn,
                              Qt::Horizontal).toString(),
             QStringLiteral("Other language"));
    QCOMPARE(model.headerData(TranslationEditorModel::ExistingColumn,
                              Qt::Horizontal).toString(),
             QStringLiteral("Existing"));

    const QModelIndex file = model.index(0, TranslationEditorModel::FileColumn);
    const QModelIndex key = model.index(
        0, TranslationEditorModel::InternalKeyColumn);
    const QModelIndex source = model.index(
        0, TranslationEditorModel::SourceTextColumn);
    const QModelIndex translation = model.index(
        0, TranslationEditorModel::TranslationColumn);
    const QModelIndex existing = model.index(
        0, TranslationEditorModel::ExistingColumn);
    QCOMPARE(file.data().toString(), QStringLiteral("GCSViews/FlightData.resx"));
    QCOMPARE(key.data().toString(), QStringLiteral("save.Text"));
    QCOMPARE(source.data().toString(), QStringLiteral("Save"));
    QCOMPARE(translation.data().toString(), QStringLiteral("Save"));
    QCOMPARE(file.data(Qt::ToolTipRole).toString(),
             QStringLiteral("Button label"));
    QCOMPARE(existing.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    QCOMPARE(model.index(1, TranslationEditorModel::ExistingColumn)
                 .data(Qt::CheckStateRole).toInt(),
             int(Qt::Checked));
    QVERIFY(!(file.flags() & Qt::ItemIsEditable));
    QVERIFY(!(key.flags() & Qt::ItemIsEditable));
    QVERIFY(!(source.flags() & Qt::ItemIsEditable));
    QVERIFY(translation.flags() & Qt::ItemIsEditable);
    QVERIFY(!(existing.flags() & Qt::ItemIsUserCheckable));
    QVERIFY(!model.setData(source, QStringLiteral("Changed")));

    const auto roles = model.roleNames();
    QCOMPARE(roles.value(TranslationEditorModel::RelativePathRole),
             QByteArrayLiteral("relativePath"));
    QCOMPARE(roles.value(TranslationEditorModel::TranslationRole),
             QByteArrayLiteral("translation"));
    QCOMPARE(roles.value(TranslationEditorModel::MissingRole),
             QByteArrayLiteral("isMissing"));
    QCOMPARE(file.data(TranslationEditorModel::MissingRole).toBool(), true);
    QCOMPARE(file.data(TranslationEditorModel::WillExportRole).toBool(), false);
    QCOMPARE(file.data(TranslationEditorModel::ModifiedRole).toBool(), false);
}

void TranslationEditorModelTest::tracksAcceptedBaselineAndRevertsAllRows()
{
    TranslationEditorModel model;
    model.loadProject(sampleProject());
    QSignalSpy counts(&model, &TranslationEditorModel::countsChanged);

    QCOMPARE(model.totalCount(), 3);
    QCOMPARE(model.missingCount(), 1);
    QCOMPARE(model.translatedCount(), 1);
    QCOMPARE(model.modifiedCount(), 0);
    QVERIFY(!model.hasUnsavedChanges());

    QVERIFY(model.setData(
        model.index(0, TranslationEditorModel::TranslationColumn),
        QStringLiteral("Enregistrer")));
    QCOMPARE(model.missingCount(), 0);
    QCOMPARE(model.translatedCount(), 2);
    QCOMPARE(model.modifiedCount(), 1);
    QVERIFY(model.hasUnsavedChanges());
    QCOMPARE(counts.count(), 1);

    QVERIFY(model.acceptChanges());
    QCOMPARE(model.modifiedCount(), 0);
    QVERIFY(!model.hasUnsavedChanges());
    QVERIFY(!model.acceptChanges());

    QVERIFY(model.setData(
        model.index(0, TranslationEditorModel::TranslationColumn),
        QStringLiteral("Save")));
    QCOMPARE(model.missingCount(), 1);
    QCOMPARE(model.translatedCount(), 1);
    QCOMPARE(model.modifiedCount(), 1);
    QVERIFY(model.revertAll());
    QCOMPARE(model.entryAt(0).translation, QStringLiteral("Enregistrer"));
    QCOMPARE(model.missingCount(), 0);
    QCOMPARE(model.translatedCount(), 2);
    QCOMPARE(model.modifiedCount(), 0);
    QVERIFY(!model.revertAll());
}

void TranslationEditorModelTest::filtersWithoutDiscardingTheFullProject()
{
    TranslationEditorModel model;
    TranslationEditorFilterModel filter;
    filter.setSourceModel(&model);
    model.loadProject(sampleProject());

    QCOMPARE(filter.visibleCount(), 3);
    filter.setSearchText(QStringLiteral("FLIGHTdata"));
    QCOMPARE(filter.visibleCount(), 2);
    filter.setMissingOnly(true);
    QCOMPARE(filter.visibleCount(), 1);
    QCOMPARE(filter.index(0, TranslationEditorModel::InternalKeyColumn)
                 .data().toString(),
             QStringLiteral("save.Text"));

    QVERIFY(filter.setData(
        filter.index(0, TranslationEditorModel::TranslationColumn),
        QStringLiteral("Sauvegarder")));
    QCOMPARE(filter.visibleCount(), 0);
    QCOMPARE(model.totalCount(), 3);
    QCOMPARE(model.snapshot().size(), 3);
    QCOMPARE(model.entryAt(0).translation, QStringLiteral("Sauvegarder"));

    filter.setMissingOnly(false);
    filter.setSearchText(QString());
    filter.setExportOnly(true);
    QCOMPARE(filter.visibleCount(), 2);
    filter.setSearchText(QStringLiteral("welcome"));
    QCOMPARE(filter.visibleCount(), 0);
    filter.setExportOnly(false);
    QCOMPARE(filter.visibleCount(), 1);
}

void TranslationEditorModelTest::
    importsUniqueExactBasenameAndResourcesAliasesInSourceOrder()
{
    ResxTranslationProject project;
    project.sourceRoot = QStringLiteral("/source");
    project.culture = QStringLiteral("ru-RU");
    project.resourceFiles = 4;
    project.entries = {
        {QStringLiteral("GCSViews/FlightData.resx"),
         QStringLiteral("save.Text"), QStringLiteral("Save"),
         QStringLiteral("Save"), QString(), false},
        {QStringLiteral("Other/Unique.resx"), QStringLiteral("only.Text"),
         QStringLiteral("Only"), QStringLiteral("Only"), QString(), false},
        {QStringLiteral("Strings.resx"), QStringLiteral("Welcome"),
         QStringLiteral("Welcome"), QStringLiteral("Welcome"), QString(), false},
        {QStringLiteral("More/Exact.resx"), QStringLiteral("same.Text"),
         QStringLiteral("Exact"), QStringLiteral("Exact"), QString(), false}};
    TranslationEditorModel model;
    model.loadProject(project);

    const TranslationIdentity resource{
        QStringLiteral("MissionPlanner.GCSViews.FlightData.resources"),
        QStringLiteral("save.Text")};
    const TranslationIdentity base{QStringLiteral("Unique.resx"),
                                   QStringLiteral("only.Text")};
    const TranslationIdentity stringsResource{
        QStringLiteral("MissionPlanner.Strings.resources"),
        QStringLiteral("Welcome")};
    const TranslationIdentity stringsExact{QStringLiteral("Strings.resx"),
                                            QStringLiteral("Welcome")};
    const TranslationIdentity exact{QStringLiteral("More\\Exact.RESX"),
                                     QStringLiteral("same.Text")};
    QHash<TranslationIdentity, QString> values;
    values.insert(resource, QStringLiteral("Сохранить"));
    values.insert(base, QStringLiteral("Только"));
    values.insert(stringsResource, QStringLiteral("Первый"));
    values.insert(stringsExact, QStringLiteral("Второй"));
    values.insert(exact, QStringLiteral("Точно"));

    const int changed = model.importTranslations(
        values, {stringsResource, stringsExact, resource, base, exact});

    QCOMPARE(changed, 4);
    QCOMPARE(model.entryAt(0).translation, QStringLiteral("Сохранить"));
    QCOMPARE(model.entryAt(1).translation, QStringLiteral("Только"));
    QCOMPARE(model.entryAt(2).translation, QStringLiteral("Первый"));
    QCOMPARE(model.entryAt(3).translation, QStringLiteral("Точно"));
    QCOMPARE(model.modifiedCount(), 4);

    ResxTranslationService::ImportResult failed;
    failed.success = false;
    failed.values.insert({QStringLiteral("Strings.resx"),
                          QStringLiteral("Welcome")},
                         QStringLiteral("Не применять"));
    QCOMPARE(model.importTranslations(failed), 0);
    QCOMPARE(model.entryAt(2).translation, QStringLiteral("Первый"));
}

void TranslationEditorModelTest::
    rejectsAmbiguousAliasesAndKeepsExactKeysCaseSensitive()
{
    ResxTranslationProject project;
    project.entries = {
        {QStringLiteral("First/View.resx"), QStringLiteral("button.Text"),
         QStringLiteral("First"), QStringLiteral("First"), QString(), false},
        {QStringLiteral("Second/View.resx"), QStringLiteral("button.Text"),
         QStringLiteral("Second"), QStringLiteral("Second"), QString(), false},
        {QStringLiteral("Third/Other.resx"), QStringLiteral("Welcome"),
         QStringLiteral("Welcome"), QStringLiteral("Welcome"), QString(), false}};
    TranslationEditorModel model;
    model.loadProject(project);

    QHash<TranslationIdentity, QString> values;
    const TranslationIdentity ambiguous{QStringLiteral("View.resx"),
                                        QStringLiteral("button.Text")};
    const TranslationIdentity wrongCase{QStringLiteral("Third/Other.resx"),
                                        QStringLiteral("welcome")};
    values.insert(ambiguous, QStringLiteral("Ambiguous"));
    values.insert(wrongCase, QStringLiteral("Wrong case"));
    QCOMPARE(model.importTranslations(values, {ambiguous, wrongCase}), 0);
    QCOMPARE(model.modifiedCount(), 0);

    // The index stores no more than ambiguity, so a large same-name set stays
    // bounded while still refusing to choose an arbitrary row.
    project.entries.clear();
    project.entries.reserve(2000);
    for (int row = 0; row < 2000; ++row) {
        project.entries.append(
            {QStringLiteral("Folder%1/View.resx").arg(row),
             QStringLiteral("button.Text"), QStringLiteral("Source"),
             QStringLiteral("Source"), QString(), false});
    }
    model.loadProject(project);
    QCOMPARE(model.importTranslations(values, {ambiguous}), 0);
    QCOMPARE(model.modifiedCount(), 0);
}

void TranslationEditorModelTest::replacesAndClearsProjectMetadata()
{
    TranslationEditorModel model;
    QSignalSpy projectChanged(&model,
                              &TranslationEditorModel::projectChanged);
    model.loadProject(sampleProject());
    QVERIFY(model.hasProject());
    QCOMPARE(model.sourceRoot(), QStringLiteral("/translation/source"));
    QCOMPARE(model.culture(), QStringLiteral("fr-FR"));
    QCOMPARE(model.resourceFileCount(), 2);
    QCOMPARE(model.warnings(),
             QStringList{QStringLiteral("one unreadable resource")});

    ResxTranslationProject replacement;
    replacement.sourceRoot = QStringLiteral("/new/source");
    replacement.culture = QStringLiteral("de-DE");
    model.loadProject(replacement);
    QVERIFY(model.hasProject());
    QCOMPARE(model.sourceRoot(), QStringLiteral("/new/source"));
    QCOMPARE(model.culture(), QStringLiteral("de-DE"));
    QCOMPARE(model.rowCount(), 0);

    model.clearProject();
    QVERIFY(!model.hasProject());
    QVERIFY(model.sourceRoot().isEmpty());
    QVERIFY(model.culture().isEmpty());
    QCOMPARE(model.totalCount(), 0);
    QCOMPARE(model.missingCount(), 0);
    QCOMPARE(projectChanged.count(), 3);
}

QTEST_GUILESS_MAIN(TranslationEditorModelTest)

#include "test_translationeditormodel.moc"
