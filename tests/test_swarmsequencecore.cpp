#include <QtTest>

#include "ui/tools/SwarmSequenceCore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

namespace
{
SwarmSequenceOffset offset(double east, double north, double altitude)
{
    SwarmSequenceOffset result;
    result.x = east;
    result.y = north;
    result.z = altitude;
    return result;
}

SwarmSequenceLayout layout(
    const QString &id, int delayStart = 0, int delayEnd = 0)
{
    SwarmSequenceLayout result;
    result.id = id;
    result.delayStart = delayStart;
    result.delayEnd = delayEnd;
    result.offsets.insert(1, offset(1.0, 0.0, 5.0));
    result.offsets.insert(7, offset(-2.5, 3.25, 10.0));
    return result;
}

SwarmSequenceDocument validDocument()
{
    SwarmSequenceDocument result;
    result.layouts.append(layout(QStringLiteral("Launch"), 2, 3));
    SwarmSequenceLayout cruise = layout(QStringLiteral("Cruise"), -4, 8);
    cruise.offsets[1] = offset(100.0, 200.0, 30.0);
    result.layouts.append(cruise);
    result.steps << QStringLiteral("Launch") << QStringLiteral("Cruise")
                 << QStringLiteral("Launch");
    return result;
}

QByteArray canonical(const SwarmSequenceDocument &document)
{
    const SwarmSequenceSerializeResult result =
        SwarmSequenceFile::serialize(document);
    if (!result.isValid()) {
        qFatal("test fixture is invalid: %s", qPrintable(result.issue.message));
    }
    return result.json;
}
}

class SwarmSequenceCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesCompatibleJsonAndPreservesExactIds();
    void writesCanonicalMemberNamesAndRoundTripsDelays();
    void rejectsAmbiguousCaseInsensitiveMembers();
    void exactCaseLayoutReferencesAreNotFolded();
    void validatesCountsAndDuplicateIds();
    void validatesSystemIdsOffsetsAndCommonSlotSet();
    void rejectsWrongJsonTypesAndMalformedComments();
    void enforcesEightMiBBoundBeforeParsing();
    void savesAtomicallyAndLoadsUnicodePath();
    void editorCreatesClonesRenamesAndPreservesDelays();
    void cloningEmptyLayoutCreatesUsableCommonSlot();
    void editorResizesEveryLayoutDeterministically();
    void editorAddsReplacesMovesAndRemovesSteps();
    void failedEditorOperationsDoNotMutateDocument();
    void projectsEastAndNorthOnWgs84();
    void projectionWrapsLongitudeAndRejectsUnsafeInput();
};

void SwarmSequenceCoreTest::parsesCompatibleJsonAndPreservesExactIds()
{
    const QByteArray json = R"json(
        {
          // MP10 accepts property names without matching their declared case.
          "layouts": [
            {
              "iD": "  Alpha//Formation  ",
              "delaystart": 12,
              "DELAYEND": -7,
              "OFFSET": {
                "1": { "X": 1.5, "y": -2, "Z": 7, },
                "255": { "x": 0, "Y": 3, "z": -4 }
              },
            },
            {
              "Id": "alpha//formation",
              "DelayStart": 0,
              "DelayEnd": 0,
              "Offset": {
                "1": { "x": 4, "y": 5, "z": 6 },
                "255": { "x": 7, "y": 8, "z": 9 },
              },
            },
          ],
          /* Block comments and trailing commas are tolerated. */
          "steps": ["Alpha//Formation", "alpha//formation",],
        }
    )json";

    const SwarmSequenceLoadResult result = SwarmSequenceFile::parse(json);
    QVERIFY2(result.isValid(), qPrintable(result.issue.message));
    QCOMPARE(result.document.layouts.size(), 2);
    QCOMPARE(result.document.layouts.at(0).id,
             QStringLiteral("Alpha//Formation"));
    QCOMPARE(result.document.layouts.at(0).delayStart, 12);
    QCOMPARE(result.document.layouts.at(0).delayEnd, -7);
    QCOMPARE(result.document.layouts.at(0).offsets.value(1).x, 1.5);
    QCOMPARE(result.document.layouts.at(0).offsets.value(1).y, -2.0);
    QCOMPARE(result.document.layouts.at(0).offsets.value(255).z, -4.0);
    QCOMPARE(result.document.steps,
             QStringList({QStringLiteral("Alpha//Formation"),
                          QStringLiteral("alpha//formation")}));
}

void SwarmSequenceCoreTest::writesCanonicalMemberNamesAndRoundTripsDelays()
{
    const SwarmSequenceDocument document = validDocument();
    const SwarmSequenceSerializeResult encoded =
        SwarmSequenceFile::serialize(document);
    QVERIFY2(encoded.isValid(), qPrintable(encoded.issue.message));

    const QJsonDocument json = QJsonDocument::fromJson(encoded.json);
    QVERIFY(json.isObject());
    const QJsonObject root = json.object();
    QVERIFY(root.contains(QStringLiteral("Layouts")));
    QVERIFY(root.contains(QStringLiteral("Steps")));
    QVERIFY(!root.contains(QStringLiteral("layouts")));
    const QJsonObject first = root.value(QStringLiteral("Layouts"))
                                  .toArray().first().toObject();
    QVERIFY(first.contains(QStringLiteral("Id")));
    QVERIFY(first.contains(QStringLiteral("DelayStart")));
    QVERIFY(first.contains(QStringLiteral("DelayEnd")));
    QVERIFY(first.contains(QStringLiteral("Offset")));
    const QJsonObject firstOffset = first.value(QStringLiteral("Offset"))
                                        .toObject().value(QStringLiteral("1"))
                                        .toObject();
    QCOMPARE(firstOffset.keys(),
             QStringList({QStringLiteral("x"), QStringLiteral("y"),
                          QStringLiteral("z")}));

    const SwarmSequenceLoadResult decoded =
        SwarmSequenceFile::parse(encoded.json);
    QVERIFY2(decoded.isValid(), qPrintable(decoded.issue.message));
    QCOMPARE(canonical(decoded.document), encoded.json);
    QCOMPARE(decoded.document.layouts.at(0).delayStart, 2);
    QCOMPARE(decoded.document.layouts.at(0).delayEnd, 3);
    QCOMPARE(decoded.document.layouts.at(1).delayStart, -4);
    QCOMPARE(decoded.document.layouts.at(1).delayEnd, 8);
}

void SwarmSequenceCoreTest::rejectsAmbiguousCaseInsensitiveMembers()
{
    const SwarmSequenceLoadResult outer = SwarmSequenceFile::parse(
        QByteArrayLiteral("{\"Layouts\":[],\"layouts\":[],\"Steps\":[]}"));
    QVERIFY(!outer.isValid());
    QCOMPARE(outer.issue.code, SwarmSequenceError::AmbiguousMember);

    const SwarmSequenceLoadResult nested = SwarmSequenceFile::parse(
        QByteArrayLiteral(
            "{\"Layouts\":[{\"Id\":\"A\",\"id\":\"B\",\"Offset\":{}}],"
            "\"Steps\":[]}"));
    QVERIFY(!nested.isValid());
    QCOMPARE(nested.issue.code, SwarmSequenceError::AmbiguousMember);
}

void SwarmSequenceCoreTest::exactCaseLayoutReferencesAreNotFolded()
{
    SwarmSequenceDocument document;
    SwarmSequenceLayout upper = layout(QStringLiteral("Layout"));
    SwarmSequenceLayout lower = layout(QStringLiteral("layout"));
    document.layouts << upper << lower;
    document.steps << QStringLiteral("Layout") << QStringLiteral("layout");
    QVERIFY2(SwarmSequenceFile::validate(document),
             qPrintable(SwarmSequenceFile::validate(document).message));

    document.steps[1] = QStringLiteral("LAYOUT");
    const SwarmSequenceIssue invalid = SwarmSequenceFile::validate(document);
    QVERIFY(!invalid);
    QCOMPARE(invalid.code, SwarmSequenceError::MissingStepLayout);
}

void SwarmSequenceCoreTest::validatesCountsAndDuplicateIds()
{
    SwarmSequenceDocument tooManyLayouts;
    tooManyLayouts.layouts.resize(SwarmSequenceFile::MaximumLayouts + 1);
    QCOMPARE(SwarmSequenceFile::validate(tooManyLayouts).code,
             SwarmSequenceError::TooManyLayouts);

    SwarmSequenceDocument tooManySteps;
    tooManySteps.steps.reserve(SwarmSequenceFile::MaximumSteps + 1);
    for (int index = 0; index <= SwarmSequenceFile::MaximumSteps; ++index) {
        tooManySteps.steps.append(QStringLiteral("A"));
    }
    QCOMPARE(SwarmSequenceFile::validate(tooManySteps).code,
             SwarmSequenceError::TooManySteps);

    SwarmSequenceDocument duplicate;
    duplicate.layouts << layout(QStringLiteral(" Same "))
                      << layout(QStringLiteral("Same"));
    QCOMPARE(SwarmSequenceFile::validate(duplicate).code,
             SwarmSequenceError::DuplicateLayoutId);

    SwarmSequenceDocument blank;
    blank.layouts << layout(QStringLiteral(" \t "));
    QCOMPARE(SwarmSequenceFile::validate(blank).code,
             SwarmSequenceError::EmptyLayoutId);
}

void SwarmSequenceCoreTest::validatesSystemIdsOffsetsAndCommonSlotSet()
{
    SwarmSequenceDocument invalidId;
    invalidId.layouts << layout(QStringLiteral("A"));
    invalidId.layouts[0].offsets.insert(0, offset(0, 0, 0));
    QCOMPARE(SwarmSequenceFile::validate(invalidId).code,
             SwarmSequenceError::InvalidSystemId);

    SwarmSequenceDocument unsafe = validDocument();
    unsafe.layouts[0].offsets[1].x = 100000.0001;
    QCOMPARE(SwarmSequenceFile::validate(unsafe).code,
             SwarmSequenceError::UnsafeOffset);
    unsafe = validDocument();
    unsafe.layouts[0].offsets[1].y =
        std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(SwarmSequenceFile::validate(unsafe).code,
             SwarmSequenceError::UnsafeOffset);
    unsafe = validDocument();
    unsafe.layouts[0].offsets[1].z = -10000.0001;
    QCOMPARE(SwarmSequenceFile::validate(unsafe).code,
             SwarmSequenceError::UnsafeOffset);

    SwarmSequenceDocument inconsistent = validDocument();
    inconsistent.layouts[1].offsets.remove(7);
    inconsistent.layouts[1].offsets.insert(8, offset(0, 0, 0));
    QCOMPARE(SwarmSequenceFile::validate(inconsistent).code,
             SwarmSequenceError::InconsistentSlots);

    const SwarmSequenceLoadResult duplicateNumeric = SwarmSequenceFile::parse(
        QByteArrayLiteral(
            "{\"Layouts\":[{\"Id\":\"A\",\"Offset\":{"
            "\"1\":{\"x\":0},\"01\":{\"x\":0}}}],\"Steps\":[]}"));
    QVERIFY(!duplicateNumeric.isValid());
    QCOMPARE(duplicateNumeric.issue.code, SwarmSequenceError::InvalidSystemId);
}

void SwarmSequenceCoreTest::rejectsWrongJsonTypesAndMalformedComments()
{
    const SwarmSequenceLoadResult arrayRoot =
        SwarmSequenceFile::parse(QByteArrayLiteral("[]"));
    QCOMPARE(arrayRoot.issue.code, SwarmSequenceError::InvalidRoot);

    const SwarmSequenceLoadResult wrongLayouts =
        SwarmSequenceFile::parse(QByteArrayLiteral("{\"Layouts\":{},\"Steps\":[]}"));
    QCOMPARE(wrongLayouts.issue.code, SwarmSequenceError::InvalidMemberType);

    const SwarmSequenceLoadResult fractionalDelay = SwarmSequenceFile::parse(
        QByteArrayLiteral(
            "{\"Layouts\":[{\"Id\":\"A\",\"DelayStart\":1.5,"
            "\"Offset\":{}}],\"Steps\":[]}"));
    QCOMPARE(fractionalDelay.issue.code,
             SwarmSequenceError::InvalidMemberType);

    const SwarmSequenceLoadResult invalidStep = SwarmSequenceFile::parse(
        QByteArrayLiteral(
            "{\"Layouts\":[{\"Id\":\"A\",\"Offset\":{}}],"
            "\"Steps\":[1]}"));
    QCOMPARE(invalidStep.issue.code, SwarmSequenceError::InvalidMemberType);

    const SwarmSequenceLoadResult unterminated = SwarmSequenceFile::parse(
        QByteArrayLiteral("{/* never closed"));
    QCOMPARE(unterminated.issue.code, SwarmSequenceError::InvalidJson);
}

void SwarmSequenceCoreTest::enforcesEightMiBBoundBeforeParsing()
{
    QByteArray tooLarge(int(SwarmSequenceFile::MaximumFileBytes + 1), ' ');
    const SwarmSequenceLoadResult result = SwarmSequenceFile::parse(tooLarge);
    QVERIFY(!result.isValid());
    QCOMPARE(result.issue.code, SwarmSequenceError::TooManyBytes);
}

void SwarmSequenceCoreTest::savesAtomicallyAndLoadsUnicodePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(
        QStringLiteral("nested/последовательность.json"));
    const SwarmSequenceDocument document = validDocument();

    const SwarmSequenceIssue saved = SwarmSequenceFile::save(path, document);
    QVERIFY2(saved, qPrintable(saved.message));
    QVERIFY(QFileInfo::exists(path));
    const QStringList entries = QDir(QFileInfo(path).absolutePath())
                                    .entryList(QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(entries, QStringList({QFileInfo(path).fileName()}));

    const SwarmSequenceLoadResult loaded =
        SwarmSequenceFile::load(QStringLiteral("  %1  ").arg(path));
    QVERIFY2(loaded.isValid(), qPrintable(loaded.issue.message));
    QCOMPARE(loaded.absolutePath,
             QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QCOMPARE(canonical(loaded.document), canonical(document));
}

void SwarmSequenceCoreTest::editorCreatesClonesRenamesAndPreservesDelays()
{
    SwarmSequenceEditor editor;
    QVERIFY(editor.createLayout(QStringLiteral("  First  ")));
    QCOMPARE(editor.document().layouts.size(), 1);
    QCOMPARE(editor.document().layouts.first().id, QStringLiteral("First"));
    QCOMPARE(editor.document().layouts.first().offsets.keys(), QList<int>({1}));
    QCOMPARE(editor.document().layouts.first().offsets.value(1).x, 1.0);

    QVERIFY(editor.setLayoutDelays(QStringLiteral("First"), 25, -9));
    QVERIFY(editor.setOffset(QStringLiteral("First"), 1,
                             offset(4.0, 5.0, 6.0)));
    QVERIFY(editor.cloneLayout(QStringLiteral("First"),
                               QStringLiteral(" Second ")));
    QCOMPARE(editor.document().layouts.at(1).id, QStringLiteral("Second"));
    QCOMPARE(editor.document().layouts.at(1).delayStart, 25);
    QCOMPARE(editor.document().layouts.at(1).delayEnd, -9);
    QCOMPARE(editor.document().layouts.at(1).offsets.value(1).y, 5.0);

    QVERIFY(editor.addStep(QStringLiteral("First")));
    QVERIFY(editor.renameLayout(QStringLiteral("First"),
                                QStringLiteral("Launch")));
    QCOMPARE(editor.document().layouts.first().id, QStringLiteral("Launch"));
    QCOMPARE(editor.document().steps.first(), QStringLiteral("Launch"));
}

void SwarmSequenceCoreTest::cloningEmptyLayoutCreatesUsableCommonSlot()
{
    SwarmSequenceDocument empty;
    SwarmSequenceLayout source;
    source.id = QStringLiteral("Empty");
    empty.layouts.append(source);
    empty.steps.append(source.id);

    SwarmSequenceEditor editor;
    QVERIFY(editor.replaceDocument(empty));
    QVERIFY(editor.cloneLayout(QStringLiteral("Empty"),
                               QStringLiteral("Usable")));
    QCOMPARE(editor.document().layouts.size(), 2);
    QCOMPARE(editor.document().layouts.at(0).offsets.keys(), QList<int>({1}));
    QCOMPARE(editor.document().layouts.at(1).offsets.keys(), QList<int>({1}));
    QCOMPARE(editor.document().layouts.at(1).offsets.value(1).x, 1.0);
    QCOMPARE(editor.document().steps, QStringList({QStringLiteral("Empty")}));
}

void SwarmSequenceCoreTest::editorResizesEveryLayoutDeterministically()
{
    SwarmSequenceEditor editor;
    QVERIFY(editor.replaceDocument(validDocument()));
    QVERIFY(editor.resizeSlots(4));
    const QList<int> four({1, 7, 8, 9});
    QCOMPARE(editor.document().layouts.at(0).offsets.keys(), four);
    QCOMPARE(editor.document().layouts.at(1).offsets.keys(), four);
    QCOMPARE(editor.document().layouts.at(0).offsets.value(8).x, 8.0);
    QCOMPARE(editor.document().layouts.at(1).offsets.value(9).x, 9.0);

    QVERIFY(editor.resizeSlots(2));
    const QList<int> two({1, 7});
    QCOMPARE(editor.document().layouts.at(0).offsets.keys(), two);
    QCOMPARE(editor.document().layouts.at(1).offsets.keys(), two);
    QCOMPARE(editor.document().layouts.at(0).offsets.value(1).x, 1.0);
    QCOMPARE(editor.document().layouts.at(1).offsets.value(1).x, 100.0);
}

void SwarmSequenceCoreTest::editorAddsReplacesMovesAndRemovesSteps()
{
    SwarmSequenceEditor editor;
    QVERIFY(editor.replaceDocument(validDocument()));
    QVERIFY(editor.addStep(QStringLiteral("Cruise"), 1));
    QCOMPARE(editor.document().steps,
             QStringList({QStringLiteral("Launch"), QStringLiteral("Cruise"),
                          QStringLiteral("Cruise"), QStringLiteral("Launch")}));
    QVERIFY(editor.replaceStep(0, QStringLiteral("Cruise")));
    QVERIFY(editor.moveStep(3, 1));
    QCOMPARE(editor.document().steps,
             QStringList({QStringLiteral("Cruise"), QStringLiteral("Launch"),
                          QStringLiteral("Cruise"), QStringLiteral("Cruise")}));
    QVERIFY(editor.removeStep(2));
    QCOMPARE(editor.document().steps,
             QStringList({QStringLiteral("Cruise"), QStringLiteral("Launch"),
                          QStringLiteral("Cruise")}));

    QVERIFY(!editor.removeLayout(QStringLiteral("Launch")));
    QVERIFY(editor.removeStep(1));
    QVERIFY(editor.removeLayout(QStringLiteral("Launch")));
    QCOMPARE(editor.document().layouts.size(), 1);
}

void SwarmSequenceCoreTest::failedEditorOperationsDoNotMutateDocument()
{
    SwarmSequenceEditor editor;
    QVERIFY(editor.replaceDocument(validDocument()));
    const QByteArray before = canonical(editor.document());

    SwarmSequenceIssue result = editor.cloneLayout(
        QStringLiteral("Missing"), QStringLiteral("Clone"));
    QCOMPARE(result.code, SwarmSequenceError::MissingLayout);
    QCOMPARE(canonical(editor.document()), before);

    result = editor.cloneLayout(QStringLiteral("Launch"),
                                QStringLiteral("Cruise"));
    QCOMPARE(result.code, SwarmSequenceError::DuplicateLayoutId);
    QCOMPARE(canonical(editor.document()), before);

    result = editor.setOffset(QStringLiteral("Launch"), 7,
                              offset(100001.0, 0.0, 0.0));
    QCOMPARE(result.code, SwarmSequenceError::UnsafeOffset);
    QCOMPARE(canonical(editor.document()), before);

    result = editor.resizeSlots(0);
    QCOMPARE(result.code, SwarmSequenceError::InvalidSlotCount);
    QCOMPARE(canonical(editor.document()), before);

    SwarmSequenceDocument exhausted = validDocument();
    for (SwarmSequenceLayout &layoutItem : exhausted.layouts) {
        const SwarmSequenceOffset last = layoutItem.offsets.take(7);
        layoutItem.offsets.insert(255, last);
    }
    SwarmSequenceEditor exhaustedEditor;
    QVERIFY(exhaustedEditor.replaceDocument(exhausted));
    const QByteArray exhaustedBefore = canonical(exhaustedEditor.document());
    result = exhaustedEditor.resizeSlots(3);
    QCOMPARE(result.code, SwarmSequenceError::InvalidSlotCount);
    QCOMPARE(canonical(exhaustedEditor.document()), exhaustedBefore);

    result = editor.addStep(QStringLiteral("launch"));
    QCOMPARE(result.code, SwarmSequenceError::MissingLayout);
    QCOMPARE(canonical(editor.document()), before);

    result = editor.moveStep(0, 99);
    QCOMPARE(result.code, SwarmSequenceError::InvalidIndex);
    QCOMPARE(canonical(editor.document()), before);

    SwarmSequenceDocument invalid = validDocument();
    invalid.layouts[1].offsets.remove(7);
    result = editor.replaceDocument(invalid);
    QCOMPARE(result.code, SwarmSequenceError::InconsistentSlots);
    QCOMPARE(canonical(editor.document()), before);
}

void SwarmSequenceCoreTest::projectsEastAndNorthOnWgs84()
{
    SwarmSequenceGeodeticPoint north;
    SwarmSequenceIssue result = SwarmSequenceGeometry::projectEastNorth(
        0.0, 0.0, offset(0.0, 1000.0, 42.0), &north);
    QVERIFY2(result, qPrintable(result.message));
    QVERIFY(std::abs(north.latitude - 0.00898315284) < 1e-10);
    QVERIFY(std::abs(north.longitude) < 1e-12);
    QCOMPARE(north.altitudeM, 42.0);

    SwarmSequenceGeodeticPoint east;
    result = SwarmSequenceGeometry::projectEastNorth(
        0.0, 0.0, offset(1000.0, 0.0, -5.0), &east);
    QVERIFY2(result, qPrintable(result.message));
    QVERIFY(std::abs(east.latitude) < 1e-12);
    QVERIFY(std::abs(east.longitude - 0.00898315284) < 1e-10);
    QCOMPARE(east.altitudeM, -5.0);

    SwarmSequenceGeodeticPoint diagonal;
    result = SwarmSequenceGeometry::projectEastNorth(
        34.0, 33.0, offset(12000.0, 9000.0, 100.0), &diagonal);
    QVERIFY2(result, qPrintable(result.message));
    QVERIFY(diagonal.latitude > 34.08 && diagonal.latitude < 34.082);
    QVERIFY(diagonal.longitude > 33.129 && diagonal.longitude < 33.131);
}

void SwarmSequenceCoreTest::projectionWrapsLongitudeAndRejectsUnsafeInput()
{
    SwarmSequenceGeodeticPoint projected;
    SwarmSequenceIssue result = SwarmSequenceGeometry::projectEastNorth(
        0.0, 179.999, offset(1000.0, 0.0, 0.0), &projected);
    QVERIFY2(result, qPrintable(result.message));
    QVERIFY(projected.longitude < -179.99 && projected.longitude >= -180.0);

    result = SwarmSequenceGeometry::projectEastNorth(
        91.0, 0.0, offset(0.0, 0.0, 0.0), &projected);
    QCOMPARE(result.code, SwarmSequenceError::InvalidCoordinate);
    result = SwarmSequenceGeometry::projectEastNorth(
        0.0, 0.0, offset(100000.1, 0.0, 0.0), &projected);
    QCOMPARE(result.code, SwarmSequenceError::InvalidCoordinate);
    result = SwarmSequenceGeometry::projectEastNorth(
        0.0, 0.0, offset(0.0, 0.0, 0.0), nullptr);
    QCOMPARE(result.code, SwarmSequenceError::InvalidCoordinate);
}

QTEST_APPLESS_MAIN(SwarmSequenceCoreTest)
#include "test_swarmsequencecore.moc"
