#include <QtTest>

#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterMetaDataParser.h"

#include <QBuffer>

class ParameterMetaDataParserTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesFieldsAndKeepsFirstDuplicate();
    void appliesVehicleConditionsWithoutLeakingOtherFrames();
    void exposesNormalizedGroupsAndNestedSources();
    void writesCatalogCompatiblePdef();
    void rejectsOversizedInput();
    void lastMatchingConditionalWinsAndBlocksStaySeparate();
    void preservesAnonymousGroupsAndEmptyFieldsWithoutEatingNextLine();
    void capsSerializedOutputAndPreservesExplicitEmptyAttributes();
};

void ParameterMetaDataParserTest::parsesFieldsAndKeepsFirstDuplicate()
{
    const QString source = QStringLiteral(R"cpp(
// @Param: TEST PARAM
// @DisplayName: Test parameter
// @Description: First definition
// @Units: m/s
// @FutureField: retained
// @Param: DUPLICATE
// @DisplayName: First wins
// @Param: DUPLICATE
// @DisplayName: Must not replace it
)cpp");

    const auto parsed = ParameterMetaDataParser::Parse(
        source, QStringLiteral("ArduCopter2"));
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QCOMPARE(parsed.parameters.size(), 2);
    QVERIFY(parsed.parameters.contains(QStringLiteral("TEST_PARAM")));
    QCOMPARE(parsed.parameters.value(QStringLiteral("TEST_PARAM"))
                 .value(QStringLiteral("FutureField")),
             QStringLiteral("retained"));
    QCOMPARE(parsed.parameters.value(QStringLiteral("DUPLICATE"))
                 .value(QStringLiteral("DisplayName")),
             QStringLiteral("First wins"));
}

void ParameterMetaDataParserTest::appliesVehicleConditionsWithoutLeakingOtherFrames()
{
    const QString source = QStringLiteral(R"cpp(
// @Param{Plane}: PLANE_ONLY
// @DisplayName: Plane only
// @Param{Copter, Blimp}: COPTER_ONLY
// @Description{Copter}: Copter override
// @Description: Generic fallback
// @Units{Plane}: knots
// @Range: 0 10
// @Param: COMMON
// @DisplayName{Plane}: Wrong frame
// @DisplayName{Copter}: Copter name
// @DisplayName: Generic name
// @User{Plane}: Advanced
)cpp");

    const auto copter = ParameterMetaDataParser::Parse(
        source, QStringLiteral("ArduCopter2"));
    QVERIFY2(copter.error.isEmpty(), qPrintable(copter.error));
    QVERIFY(!copter.parameters.contains(QStringLiteral("PLANE_ONLY")));
    QVERIFY(copter.parameters.contains(QStringLiteral("COPTER_ONLY")));
    QCOMPARE(copter.parameters.value(QStringLiteral("COPTER_ONLY"))
                 .value(QStringLiteral("Description")),
             QStringLiteral("Copter override"));
    QVERIFY(!copter.parameters.value(QStringLiteral("COPTER_ONLY"))
                 .contains(QStringLiteral("Units")));
    QCOMPARE(copter.parameters.value(QStringLiteral("COMMON"))
                 .value(QStringLiteral("DisplayName")),
             QStringLiteral("Copter name"));
    QVERIFY(!copter.parameters.value(QStringLiteral("COMMON"))
                 .contains(QStringLiteral("User")));

    const auto plane = ParameterMetaDataParser::Parse(
        source, QStringLiteral("ArduPlane"));
    QVERIFY(plane.parameters.contains(QStringLiteral("PLANE_ONLY")));
    QVERIFY(!plane.parameters.contains(QStringLiteral("COPTER_ONLY")));
    QCOMPARE(plane.parameters.value(QStringLiteral("COMMON"))
                 .value(QStringLiteral("DisplayName")),
             QStringLiteral("Wrong frame"));
    QCOMPARE(plane.parameters.value(QStringLiteral("COMMON"))
                 .value(QStringLiteral("User")),
             QStringLiteral("Advanced"));
}

void ParameterMetaDataParserTest::exposesNormalizedGroupsAndNestedSources()
{
    const QString source = QStringLiteral(R"cpp(
// @Group: MOT (Front) 
// @Path: ../motors/AP_Motors.cpp, AP_MotorsAux.cpp
// @Group{Plane}: PLANE_
// @Path: Plane.cpp
// @Group{Copter}: COPTER_
// @Path{Plane}: Wrong.cpp
// @Path{Copter}: First.cpp, Second.cpp
AP_NESTEDGROUPINFO(AP_Arming, 0),
AP_NESTEDGROUPINFO(AP_Arming, 1),
AP_NESTEDGROUPINFO(AP_Tuning, 0),
)cpp");

    const auto parsed = ParameterMetaDataParser::Parse(
        source, QStringLiteral("Copter"));
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QCOMPARE(parsed.groups.size(), 2);
    QCOMPARE(parsed.groups.at(0).prefix, QStringLiteral("MOT__Front_"));
    QCOMPARE(parsed.groups.at(0).paths,
             QStringList({QStringLiteral("../motors/AP_Motors.cpp"),
                          QStringLiteral("AP_MotorsAux.cpp")}));
    QCOMPARE(parsed.groups.at(1).prefix, QStringLiteral("COPTER_"));
    QCOMPARE(parsed.groups.at(1).paths,
             QStringList({QStringLiteral("First.cpp"),
                          QStringLiteral("Second.cpp")}));
    QCOMPARE(parsed.nestedSources,
             QStringList({QStringLiteral("AP_Arming"),
                          QStringLiteral("AP_Tuning")}));
}

void ParameterMetaDataParserTest::writesCatalogCompatiblePdef()
{
    ParameterMetaDataParser::Fields fields;
    fields.insert(QStringLiteral("DisplayName"),
                  QStringLiteral("Gain < primary>"));
    fields.insert(QStringLiteral("Description"),
                  QStringLiteral("A & B"));
    fields.insert(QStringLiteral("User"), QStringLiteral("Advanced"));
    fields.insert(QStringLiteral("Values"),
                  QStringLiteral("0:Disabled, 1:Enabled, 2:Label:with colon"));
    fields.insert(QStringLiteral("Units"), QStringLiteral("m/s"));
    fields.insert(QStringLiteral("Range"), QStringLiteral("0 20"));
    fields.insert(QStringLiteral("ReadOnly"), QStringLiteral("true"));
    fields.insert(QStringLiteral("FutureField"), QStringLiteral("kept"));

    ParameterMetaDataParser::Parameters parameters;
    parameters.insert(QStringLiteral("TEST_GAIN"), fields);
    QMap<QString, ParameterMetaDataParser::Parameters> vehicles;
    vehicles.insert(QStringLiteral("ArduCopter2"), parameters);

    const QByteArray pdef = ParameterMetaDataParser::ToPdef(vehicles);
    QVERIFY(!pdef.isEmpty());
    QCOMPARE(pdef, ParameterMetaDataParser::ToPdef(vehicles));
    QBuffer buffer;
    buffer.setData(pdef);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    const ParameterMetaDataCatalog catalog =
        ParameterMetaDataCatalog::fromPdef(
            &buffer, QStringLiteral("ArduCopter"));
    QVERIFY2(catalog.isValid(), qPrintable(catalog.errorString()));
    QVERIFY(catalog.contains(QStringLiteral("TEST_GAIN")));
    const ParameterMetaData metadata = catalog.value(
        QStringLiteral("TEST_GAIN"));
    QCOMPARE(metadata.title, QStringLiteral("Gain < primary>"));
    QCOMPARE(metadata.description, QStringLiteral("A & B"));
    QCOMPARE(metadata.userLevel, ParameterUserLevel::Advanced);
    QCOMPARE(metadata.values.size(), 3);
    QCOMPARE(metadata.values.at(2).label, QStringLiteral("Label:with colon"));
    QCOMPARE(metadata.units, QStringLiteral("m/s"));
    QVERIFY(metadata.hasRange);
    QVERIFY(metadata.readOnly);
    QCOMPARE(metadata.fields.value(QStringLiteral("FutureField")),
             QStringLiteral("kept"));
}

void ParameterMetaDataParserTest::lastMatchingConditionalWinsAndBlocksStaySeparate()
{
    const auto parsed = ParameterMetaDataParser::Parse(QStringLiteral(
        "// @Param: TEST\n// @Values: 0:Generic\n"
        "// @Values{Copter}: 1:First\n// @Values{Copter,Sub}: 2:Last\n"
        "// @Values: 3:Ignored\n// @Group: LIB_\n// @Path: Library.cpp\n"
        "// @Param: NEXT\n// @Description: Next field\n"), QStringLiteral("Copter"));
    QCOMPARE(parsed.parameters.value(QStringLiteral("TEST")).value(QStringLiteral("Values")),
             QStringLiteral("2:Last"));
    QVERIFY(!parsed.parameters.value(QStringLiteral("TEST")).contains(QStringLiteral("Group")));
    QVERIFY(!parsed.parameters.value(QStringLiteral("TEST")).contains(QStringLiteral("Path")));
    QCOMPARE(parsed.groups.size(), 1);
    QCOMPARE(parsed.groups.first().paths, QStringList{QStringLiteral("Library.cpp")});
}

void ParameterMetaDataParserTest::preservesAnonymousGroupsAndEmptyFieldsWithoutEatingNextLine()
{
    const auto parsed = ParameterMetaDataParser::Parse(QStringLiteral(
        "// @Param: EMPTY\n// @Description:\n// @Units: m\n"
        "// @Group:\n// @Path: Parameters.cpp\n"
        "// @Group:\n// @Path: ../libraries/AP_Vehicle/AP_Vehicle.cpp\n"
        "// @Group: RC\n// @Path: ../libraries/RC_Channel/RC_Channels_VarInfo.h\n"),
        QStringLiteral("Copter"));
    QVERIFY(parsed.parameters.value(QStringLiteral("EMPTY")).contains(QStringLiteral("Description")));
    QVERIFY(parsed.parameters.value(QStringLiteral("EMPTY")).value(QStringLiteral("Description")).isEmpty());
    QCOMPARE(parsed.parameters.value(QStringLiteral("EMPTY")).value(QStringLiteral("Units")), QStringLiteral("m"));
    QCOMPARE(parsed.groups.size(), 3);
    QVERIFY(parsed.groups.at(0).prefix.isEmpty());
    QVERIFY(parsed.groups.at(1).prefix.isEmpty());
    QCOMPARE(parsed.groups.at(0).paths, QStringList{QStringLiteral("Parameters.cpp")});
    QCOMPARE(parsed.groups.at(1).paths, QStringList{QStringLiteral("../libraries/AP_Vehicle/AP_Vehicle.cpp")});
    QVERIFY(parsed.groups.at(2).paths.first().endsWith(QStringLiteral(".h")));
}

void ParameterMetaDataParserTest::capsSerializedOutputAndPreservesExplicitEmptyAttributes()
{
    using Parser = ParameterMetaDataParser;
    Parser::Fields fields{{QStringLiteral("Description"), QString()},
                          {QStringLiteral("Values"), QString()}};
    const QByteArray xml = Parser::ToPdef({{QStringLiteral("Copter"), {{QStringLiteral("EMPTY"), fields}}}});
    QBuffer input;
    input.setData(xml);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto metadata = ParameterMetaDataCatalog::fromPdef(&input, QStringLiteral("ArduCopter"))
        .value(QStringLiteral("EMPTY"));
    QVERIFY(metadata.presentFields.contains(QStringLiteral("documentation")));
    QVERIFY(metadata.presentFields.contains(QStringLiteral("values")));
    fields[QStringLiteral("Description")] = QString(4 * 1024 * 1024, QLatin1Char('&'));
    // XML escaping expands input. The cap applies to emitted bytes, not chars.
    QVERIFY(Parser::ToPdef({{QStringLiteral("Copter"), {{QStringLiteral("BIG"), fields}}}}).isEmpty());
}

void ParameterMetaDataParserTest::rejectsOversizedInput()
{
    const QString oversized(8 * 1024 * 1024 + 1, QLatin1Char('x'));
    const auto parsed = ParameterMetaDataParser::Parse(
        oversized, QStringLiteral("Copter"));
    QVERIFY(!parsed.error.isEmpty());
    QVERIFY(parsed.parameters.isEmpty());
    QVERIFY(parsed.groups.isEmpty());
}

QTEST_APPLESS_MAIN(ParameterMetaDataParserTest)

#include "test_parametermetadataparser.moc"
