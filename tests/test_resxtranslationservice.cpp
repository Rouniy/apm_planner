#include "services/ResxTranslationService.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>
#include <algorithm>

namespace {
using Service = ResxTranslationService;
bool put(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QByteArray simple(const QByteArray &key = "button.Text", const QByteArray &value = "Save") {
    return "<root><data name=\"" + key + "\"><value>" + value + "</value></data></root>";
}
ResxTranslationEntry entry(const QString &path = QStringLiteral("View.resx"),
                           const QString &translation = QStringLiteral("Speichern")) {
    return {path, QStringLiteral("button.Text"), QStringLiteral("Save"), translation, {}, false};
}
QMap<QString, QString> values(const QByteArray &bytes) {
    QXmlStreamReader xml(bytes); QMap<QString, QString> result; QString key;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == "data") key = xml.attributes().value("name").toString();
        else if (xml.isStartElement() && xml.name() == "value" && !key.isEmpty()) result.insert(key, xml.readElementText());
        else if (xml.isEndElement() && xml.name() == "data") key.clear();
    }
    return result;
}
QStringList fileNames(const QString &root) {
    QStringList result; QDirIterator iterator(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) result.append(QDir(root).relativeFilePath(iterator.next()));
    result.sort(); return result;
}
}

class ResxTranslationServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void cultureAndKeyContracts()
    {
        QString error; const auto cultures = Service::cultures(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QVERIFY(cultures.size() > 800);
        QVERIFY(std::any_of(cultures.cbegin(), cultures.cend(), [](const TranslationCulture &c) { return c.name == "ru-RU"; }));
        QCOMPARE(Service::localizedRelativePath("GCSViews\\FlightData.RESX", " RU-ru "), QString("GCSViews/FlightData.ru-RU.resx"));
        QCOMPARE(Service::localizedRelativePath("View.resx", "zh-Hans"), QString("View.zh-Hans.resx"));
        QVERIFY(Service::localizedRelativePath("View.resx", "not-a-real-culture", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(Service::isTranslatable("STRINGS.RESX", "anything"));
        for (const auto &key : {"$this.Text", "x.ToolTip", "xHeaderText", "xToolTipText"}) QVERIFY(Service::isTranslatable("View.resx", key));
        QVERIFY(!Service::isTranslatable("View.resx", "button.text"));
        QVERIFY(!Service::isTranslatable("View.resx", "button.Size"));
    }

    void loadMergesExactStringsCommentsAndOrder()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath("GCSViews/FlightData.resx"),
            "<root><data name=\"z.Text\"><value>old</value></data>"
            "<data name=\"z.Text\"><value>last</value><comment>source</comment></data>"
            "<data name=\"$this.Text\"><value>Flight Data</value></data>"
            "<data name=\"lower.text\"><value>ignored</value></data>"
            "<data name=\"binary.Text\" type=\"System.Drawing.Bitmap\"><value>blob</value></data>"
            "<data name=\"mime.Text\" mimetype=\"binary\"><value>blob</value></data>"
            "<data name=\"case.Text\" type=\"System.String, mscorlib\"><value>case</value></data></root>"));
        QVERIFY(put(directory.filePath("GCSViews/FlightData.RU-ru.RESX"),
            "<root><data name=\"z.Text\"><value>translated</value><comment></comment></data>"
            "<data name=\"CASE.Text\"><value>not same key</value></data></root>"));
        QVERIFY(put(directory.filePath("Strings.RESX"), simple("Arbitrary", "hello")));
        QVERIFY(put(directory.filePath("obj/Ignored.resx"), simple()));
        QVERIFY(put(directory.filePath("translation/Old.resx"), simple()));
        const auto result = Service::load(directory.path(), "ru-RU");
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.project.resourceFiles, 2); QCOMPARE(result.project.entries.size(), 4);
        QVERIFY(result.project.warnings.isEmpty());
        QCOMPARE(result.project.entries.at(0).key, QString("$this.Text"));
        QCOMPARE(result.project.entries.at(1).key, QString("case.Text"));
        QVERIFY(!result.project.entries.at(1).hasExistingTranslation);
        const auto translated = result.project.entries.at(2);
        QCOMPARE(translated.key, QString("z.Text")); QCOMPARE(translated.sourceText, QString("last"));
        QCOMPARE(translated.translation, QString("translated")); QVERIFY(translated.hasExistingTranslation);
        QVERIFY(translated.comment.isEmpty()); QVERIFY(!translated.comment.isNull());
        QCOMPARE(result.project.entries.last().key, QString("Arbitrary"));
    }

    void malformedDtdAndLinkedSourcesWarnWithoutLosingValidFiles()
    {
        QTemporaryDir directory, outside;
        QVERIFY(put(directory.filePath("Good.resx"), simple()));
        QVERIFY(put(directory.filePath("Bad.resx"), "<!DOCTYPE root [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><root><data name='x.Text'><value>&x;</value></data></root>"));
        QVERIFY(put(directory.filePath("Wrong.resx"), "<notroot/>"));
        QVERIFY(put(outside.filePath("outside.resx"), simple("outside.Text", "outside")));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(outside.filePath("outside.resx"), directory.filePath("Linked.resx")));
#endif
        const auto result = Service::load(directory.path(), "de-DE");
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.project.entries.size(), 1);
        QVERIFY(result.project.warnings.join('\n').contains("DTD"));
        QVERIFY(result.project.warnings.join('\n').contains("Wrong.resx"));
        QVERIFY(!result.project.entries.first().sourceText.contains("outside"));
    }

    void caseCollisionsAndChineseAliasesAreNotExtraNeutralResources()
    {
        QTemporaryDir directory;
        QVERIFY(put(directory.filePath("View.resx"), simple("first.Text")));
#ifdef Q_OS_LINUX
        QVERIFY(put(directory.filePath("view.RESX"), simple("second.Text")));
#endif
        QVERIFY(put(directory.filePath("View.zh-TW.resx"), simple("first.Text", "traditional")));
        QVERIFY(put(directory.filePath("View.zh-CN.resx"), simple("first.Text", "simplified")));
        const auto result = Service::load(directory.path(), "ru-RU");
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.project.resourceFiles, 1);
        QCOMPARE(result.project.entries.size(), 1); QCOMPARE(result.project.entries.first().key, QString("first.Text"));
        QTemporaryDir output;
        const auto exported = Service::exportTranslations(output.path(), "ru-RU", result.project.entries);
        QVERIFY2(exported.success, qPrintable(exported.error));
        QCOMPARE(fileNames(output.path()), QStringList({"View.ru-RU.resx", "output.html"}));
    }

    void sparseResxHtmlCsvAndNewlineGolden()
    {
        QTemporaryDir output;
        const QString raw = QString::fromUtf8("Résumé 😀 & <tag>\r\nnext\rthird");
        QVector<ResxTranslationEntry> entries{
            {"Nested/View.resx", "first.Text", "One", raw, "line1\r\nline2\rline3", false},
            {"Nested/View.resx", "second.Text", "Same", "Same", {}, false},
            {"Strings.resx", "All", "English", QString::fromUtf8("Русский"), "sample", true}};
        const auto result = Service::exportTranslations(output.path(), "ru-RU", entries);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.resourceFiles, 2); QCOMPARE(result.translatedEntries, 2); QCOMPARE(result.overwrittenFiles, 0);
        QCOMPARE(result.publishedPaths.size(), 3); QVERIFY(result.backupPaths.isEmpty());
        const auto xml = get(output.filePath("Nested/View.ru-RU.resx"));
        QVERIFY(xml.startsWith("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root>\n  <resheader"));
        QVERIFY(xml.endsWith("</root>"));
        QCOMPARE(values(xml).size(), 1);
        QCOMPARE(values(xml).value("first.Text"), QString::fromUtf8("Résumé 😀 & <tag>\nnext\nthird"));
        QVERIFY(xml.contains("<comment>line1\nline2\nline3</comment>"));
        const auto html = get(result.resumeHtmlPath);
        QVERIFY(html.contains("R&#233;sum&#233; &#128512; &amp; &lt;tag&gt;\r\nnext\rthird"));
        const auto imported = Service::importResumeHtml(result.resumeHtmlPath);
        QVERIFY2(imported.success, qPrintable(imported.error));
        QCOMPARE(imported.values.value({"Nested/View.resx", "first.Text"}), raw);
        const auto csv = Service::buildCsv({{"View.resx", "button.Text", "Save, \"now\"", "line\n2", {}, false}});
        QVERIFY(csv.success);
        QCOMPARE(csv.text, QString("File,Key,English,Translation\r\n\"View.resx\",\"button.Text\",\"Save, \"\"now\"\"\",\"line\n2\"\r\n"));
    }

    void allExistingOutputsAreBackedUpBeforeFirstReplacement()
    {
        QTemporaryDir output;
        const QVector<ResxTranslationEntry> entries{entry("Nested/View.resx"), entry("Strings.resx")};
        const auto first = Service::exportTranslations(output.path(), "de-DE", entries); QVERIFY(first.success);
        QMap<QString, QByteArray> originals;
        for (const auto &path : first.publishedPaths) originals.insert(QDir(output.path()).relativeFilePath(path), get(path));
        auto replacement = entries; replacement[0].translation = "New";
        bool inspected = false, originalsIntact = false; int backupCount = 0;
        const auto second = Service::exportTranslations(output.path(), "de-DE", replacement, {}, [&](qint64 done, qint64 total) {
            if (done != total / 2) return;
            inspected = true; originalsIntact = true;
            for (auto i = originals.cbegin(); i != originals.cend(); ++i)
                originalsIntact &= get(output.filePath(i.key())) == i.value();
            for (const auto &path : fileNames(output.path())) if (path.startsWith(".backup/")) ++backupCount;
        });
        QVERIFY2(second.success, qPrintable(second.error)); QVERIFY(inspected); QVERIFY(originalsIntact);
        QCOMPARE(backupCount, 3); QCOMPARE(second.backupPaths.size(), 3); QCOMPARE(second.overwrittenFiles, 3);
        for (auto i = originals.cbegin(); i != originals.cend(); ++i)
            QCOMPARE(get(QDir(second.backupDirectory).filePath(i.key())), i.value());
        QCOMPARE(values(get(output.filePath("Nested/View.de-DE.resx"))).value("button.Text"), QString("New"));
    }

    void referenceAsciiPathOrderPlacesLettersBeforeUnderscores()
    {
        QTemporaryDir source, output;
        QVERIFY(put(source.filePath("ConfigAC_Fence.resx"), simple()));
        QVERIFY(put(source.filePath("ConfigAccelerometer.resx"), simple()));
        const auto loaded = Service::load(source.path(), "de-DE");
        QVERIFY2(loaded.success, qPrintable(loaded.error));
        QCOMPARE(loaded.project.entries.first().relativePath, QString("ConfigAccelerometer.resx"));
        auto reversed = loaded.project.entries;
        std::reverse(reversed.begin(), reversed.end());
        const auto result = Service::exportTranslations(output.path(), "de-DE", reversed);
        QVERIFY2(result.success, qPrintable(result.error));
        const auto html = get(result.resumeHtmlPath);
        QVERIFY(html.indexOf("ConfigAccelerometer.resx") < html.indexOf("ConfigAC_Fence.resx"));
    }

    void xmlDeclarationNormalizationDoesNotChangeTranslationContent()
    {
        QTemporaryDir output;
        const QString literal = QStringLiteral("encoding=\"UTF-8\"");
        const auto result = Service::exportTranslations(output.path(), "de-DE", {entry("View.resx", literal)});
        QVERIFY2(result.success, qPrintable(result.error));
        const auto bytes = get(output.filePath("View.de-DE.resx"));
        QVERIFY(bytes.startsWith("<?xml version=\"1.0\" encoding=\"utf-8\"?>"));
        QCOMPARE(values(bytes).value("button.Text"), literal);
    }

    void revertingTranslationsWritesEmptyResxAndEmptyProjectWritesHtml()
    {
        QTemporaryDir output;
        QVERIFY(Service::exportTranslations(output.path(), "de-DE", {entry()}).success);
        const auto result = Service::exportTranslations(output.path(), "de-DE", {entry("View.resx", "Save")});
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.resourceFiles, 1);
        QCOMPARE(result.translatedEntries, 0); QCOMPARE(values(get(output.filePath("View.de-DE.resx"))).size(), 0);
        QVERIFY(get(output.filePath("View.de-DE.resx")).contains("resheader"));
        QTemporaryDir empty;
        const auto noEntries = Service::exportTranslations(empty.path(), "de-DE", {});
        QVERIFY(noEntries.success); QCOMPARE(noEntries.resourceFiles, 0);
        QCOMPARE(fileNames(empty.path()), QStringList({"output.html"}));
    }

    void cancellingAfterBackupsLeavesAllOriginalsAndNoPublications()
    {
        QTemporaryDir output;
        const QVector<ResxTranslationEntry> entries{entry("A.resx"), entry("B.resx")};
        const auto first = Service::exportTranslations(output.path(), "de-DE", entries); QVERIFY(first.success);
        QMap<QString, QByteArray> originals;
        for (const auto &path : first.publishedPaths) originals.insert(path, get(path));
        bool stop = false;
        const auto cancelled = Service::exportTranslations(output.path(), "de-DE", entries,
            [&] { return stop; }, [&](qint64 done, qint64 total) { if (done == total / 2) stop = true; });
        QVERIFY(cancelled.cancelled); QVERIFY(!cancelled.success); QVERIFY(cancelled.publishedPaths.isEmpty());
        QCOMPARE(cancelled.backupPaths.size(), 3); QCOMPARE(cancelled.overwrittenFiles, 0);
        for (auto original = originals.cbegin(); original != originals.cend(); ++original) QCOMPARE(get(original.key()), original.value());
    }

    void changedExistingContentWithRestoredTimestampIsNotBackedUpOrOverwritten()
    {
        QTemporaryDir output;
        const auto first = Service::exportTranslations(output.path(), "de-DE", {entry()}); QVERIFY(first.success);
        const QString path = output.filePath("View.de-DE.resx"); const auto original = get(path);
        const auto modified = QFileInfo(path).lastModified(); bool mutated = false;
        const auto result = Service::exportTranslations(output.path(), "de-DE", {entry("View.resx", "new")}, {},
            [&](qint64 done, qint64) {
                if (done || mutated) return;
                QByteArray bytes = original; bytes[bytes.size() - 1] = '!';
                mutated = put(path, bytes);
                QFile file(path); if (file.open(QIODevice::ReadWrite)) file.setFileTime(modified, QFileDevice::FileModificationTime);
            });
        QVERIFY(mutated); QVERIFY(!result.success); QVERIFY(result.publishedPaths.isEmpty());
        QVERIFY(result.backupPaths.isEmpty()); QVERIFY(get(path).endsWith('!'));
    }

    void resumeOrderDuplicatesEntitiesAndLegacyIdentity()
    {
        QTemporaryDir directory; const auto path = directory.filePath("resume.html");
        QVERIFY(put(path, "<table><tr><td>File</td><td>Key</td><td>Translation</td></tr>"
            "<TR class='x'><TD>View.resx</TD><TD>x.Text</TD><TD>first</TD></TR>"
            "<tr><td>Other.resx</td><td>X.Text</td><td>Keep <angle> &copy; &alpha; &apos; &#x1F600; &#xD800; &unknown;</td></tr>"
            "<tr><td>VIEW.RESX</td><td>x.Text</td><td>last</td></tr>"
            "<tr><td>View.resx</td><td>X.Text</td><td>ordinal key</td></tr></table>"));
        const auto result = Service::importResumeHtml(path);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.order.size(), 3);
        QCOMPARE(result.order.first().relativePath, QString("View.resx"));
        QCOMPARE(result.values.value({"view.resx", "x.Text"}), QString("last"));
        QCOMPARE(result.values.value({"Other.resx", "X.Text"}), QString::fromUtf8("Keep <angle> © α ' 😀 &#xD800; &unknown;"));
        QCOMPARE(result.values.value({"View.resx", "X.Text"}), QString("ordinal key"));
        QVERIFY(Service::resumeFileMatches("MissionPlanner.GCSViews.FlightData.resources", "GCSViews/FlightData.resx"));
        QVERIFY(!Service::resumeFileMatches("MissionPlanner.Other.FlightData.resources", "GCSViews/FlightData.resx"));
        QVERIFY(Service::resumeFileMatches("FlightData.RESX", "GCSViews/FlightData.resx"));
    }

    void unsafePathsDuplicatesAndLinkedDestinationsFailWithoutOverwrite()
    {
        QTemporaryDir output, outside;
        for (const auto &path : {"../Escape.resx", "/absolute.resx", "nested//empty.resx", "C:/drive.resx", "View.resx/../x.resx", "CON.resx"})
            QVERIFY(!Service::exportTranslations(output.path(), "de-DE", {entry(path)}).success);
        QVERIFY(!Service::exportTranslations(output.path(), "de-DE", {entry("View.resx"), entry("view.RESX")}).success);
        QVERIFY(fileNames(output.path()).isEmpty());
#ifdef Q_OS_UNIX
        QVERIFY(put(outside.filePath("old"), "outside"));
        QVERIFY(QFile::link(outside.filePath("old"), output.filePath("View.de-DE.resx")));
        const auto linked = Service::exportTranslations(output.path(), "de-DE", {entry()});
        QVERIFY(!linked.success); QCOMPARE(get(outside.filePath("old")), QByteArray("outside"));
        QVERIFY(QFile::link(outside.path(), output.filePath("linked")));
        QVERIFY(!Service::exportTranslations(output.path(), "de-DE", {entry("linked/New.resx")}).success);
        QVERIFY(!QFileInfo::exists(outside.filePath("New.de-DE.resx")));
#endif
    }

    void cancellationAndDestinationMutationHaveExactPartialReceipts()
    {
        QTemporaryDir output;
        const QVector<ResxTranslationEntry> entries{entry("A.resx"), entry("B.resx")};
        bool stop = false;
        const auto partial = Service::exportTranslations(output.path(), "de-DE", entries, [&] { return stop; },
            [&](qint64 done, qint64 total) { if (done == total / 2 + 1) stop = true; });
        QVERIFY(!partial.success); QVERIFY(partial.cancelled);
        QCOMPARE(partial.publishedPaths, QStringList({output.filePath("A.de-DE.resx")}));
        QCOMPARE(partial.resourceFiles, 1); QCOMPARE(fileNames(output.path()), QStringList({"A.de-DE.resx"}));
        QTemporaryDir changed;
        bool inserted = false;
        const auto race = Service::exportTranslations(changed.path(), "de-DE", entries, {}, [&](qint64 done, qint64 total) {
            if (done == total / 2) inserted = put(changed.filePath("B.de-DE.resx"), "foreign");
        });
        QVERIFY(inserted); QVERIFY(!race.success); QVERIFY(!race.cancelled);
        QCOMPARE(race.publishedPaths, QStringList({changed.filePath("A.de-DE.resx")}));
        QCOMPARE(get(changed.filePath("B.de-DE.resx")), QByteArray("foreign"));
        QVERIFY(!QFileInfo::exists(changed.filePath("output.html")));
    }

    void preCancelledMissingMalformedAndSizeLimits()
    {
        QTemporaryDir directory;
        QVERIFY(put(directory.filePath("View.resx"), simple()));
        QVERIFY(Service::load(directory.path(), "de-DE", [] { return true; }).cancelled);
        QVERIFY(Service::exportTranslations(directory.filePath("not-created"), "de-DE", {entry()}, [] { return true; }).cancelled);
        QVERIFY(!QFileInfo::exists(directory.filePath("not-created")));
        QVERIFY(Service::buildCsv({entry()}, [] { return true; }).cancelled);
        QVERIFY(!Service::importResumeHtml(directory.filePath("missing.html")).success);
        QVERIFY(put(directory.filePath("empty.html"), "<html>no translation table</html>"));
        const auto empty = Service::importResumeHtml(directory.filePath("empty.html"));
        QVERIFY(empty.success); QVERIFY(empty.order.isEmpty());
        QFile huge(directory.filePath("huge.html")); QVERIFY(huge.open(QIODevice::WriteOnly));
        QVERIFY(huge.resize(Service::MaximumResumeBytes + 1)); huge.close();
        const auto large = Service::importResumeHtml(huge.fileName());
        QVERIFY(!large.success); QVERIFY(large.error.contains("limit"));
        auto invalid = entry(); invalid.translation = QString(QChar(1));
        QVERIFY(!Service::exportTranslations(directory.path(), "de-DE", {invalid}).success);
    }
};

QTEST_GUILESS_MAIN(ResxTranslationServiceTest)
#include "test_resxtranslationservice.moc"
