#include <QtTest>

#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterMetaDataRepository.h"
#include "core/parameters/ParameterStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>

class ParameterCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void duplicatePacketsDoNotFakeCompletion();
    void sameNameFromTwoComponentsDoesNotCollide();
    void newlyDiscoveredComponentReopensCompleteness();
    void targetChangeInvalidatesSnapshots();
    void cancellationKeepsPartialSnapshot();
    void codecPreservesBytewiseTypes();
    void integerAndFloatEqualityAreTypeAware();
    void pdefMetadataSelectsVehicleAndLibraries();
    void pdefMetadataHandlesEnumsRangesAndMalformedXml();
    void packagedPdefsParse_data();
    void packagedPdefsParse();
    void metadataRepositoryMapsPackagedFamilies();
    void metadataRepositoryUsesResourceFallback();
};

void ParameterCoreTest::duplicatePacketsDoNotFakeCompletion()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();

    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QCOMPARE(store.progress().received, 1);
    QCOMPARE(store.progress().reported, 2);
    QCOMPARE(store.state(), ParameterLoadState::Loading);

    QVERIFY(store.ingest(1, 2, 1, QStringLiteral("B"), 2, ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.specializedPagesReady());
}

void ParameterCoreTest::sameNameFromTwoComponentsDoesNotCollide()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("VERSION"), 10, ParameterType::Int32));
    QVERIFY(store.ingest(154, 1, 0, QStringLiteral("VERSION"), 20, ParameterType::Int32));

    const ParameterSnapshot snapshot = store.snapshot();
    QCOMPARE(snapshot.records().size(), 2);
    QCOMPARE(snapshot.value(1, QStringLiteral("VERSION")).value.toInt(), 10);
    QCOMPARE(snapshot.value(154, QStringLiteral("VERSION")).value.toInt(), 20);
}

void ParameterCoreTest::newlyDiscoveredComponentReopensCompleteness()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QCOMPARE(store.state(), ParameterLoadState::Complete);

    QVERIFY(store.ingest(154, 2, 0, QStringLiteral("B"), 2, ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.progress().reported, 3);
    QCOMPARE(store.state(), ParameterLoadState::Loading);
    QVERIFY(!store.specializedPagesReady());

    QVERIFY(store.ingest(154, 2, 1, QStringLiteral("C"), 3, ParameterType::Int32));
    QCOMPARE(store.state(), ParameterLoadState::Complete);
}

void ParameterCoreTest::targetChangeInvalidatesSnapshots()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("OLD"), 10, ParameterType::Int32));
    const ParameterSnapshot oldSnapshot = store.snapshot();

    store.selectTarget(7, 2, 1);
    const ParameterSnapshot current = store.snapshot();
    QVERIFY(oldSnapshot.contains(1, QStringLiteral("OLD")));
    QVERIFY(!current.contains(1, QStringLiteral("OLD")));
    QVERIFY(current.target().revision > oldSnapshot.target().revision);
}

void ParameterCoreTest::cancellationKeepsPartialSnapshot()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 3, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    store.cancelLoad();

    QCOMPARE(store.state(), ParameterLoadState::Cancelled);
    QCOMPARE(store.progress().received, 1);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("A")));
    QVERIFY(!store.specializedPagesReady());
}

void ParameterCoreTest::codecPreservesBytewiseTypes()
{
    const QList<QPair<ParameterType, QVariant>> values = {
        {ParameterType::UInt8, QVariant::fromValue<quint32>(255)},
        {ParameterType::Int8, QVariant::fromValue<qint32>(-128)},
        {ParameterType::UInt16, QVariant::fromValue<quint32>(65535)},
        {ParameterType::Int16, QVariant::fromValue<qint32>(-32768)},
        {ParameterType::UInt32, QVariant::fromValue<quint32>(4000000000U)},
        {ParameterType::Int32, QVariant::fromValue<qint32>(-2147483647)}
    };

    for (const auto &entry : values) {
        bool encoded = false;
        const float wire = ParameterCodec::encodeClassic(
            entry.second, entry.first, ParameterEncoding::Bytewise, &encoded);
        QVERIFY(encoded);
        bool decoded = false;
        const QVariant roundTrip = ParameterCodec::decodeClassic(
            wire, entry.first, ParameterEncoding::Bytewise, &decoded);
        QVERIFY(decoded);
        QVERIFY(ParameterCodec::valuesEqual(roundTrip, entry.second, entry.first));
    }
}

void ParameterCoreTest::integerAndFloatEqualityAreTypeAware()
{
    QVERIFY(!ParameterCodec::valuesEqual(1, 2, ParameterType::Int32));
    QVERIFY(ParameterCodec::valuesEqual(1.0, 1.0 + 1.0e-8, ParameterType::Real32));
    QVERIFY(!ParameterCodec::valuesEqual(1.0, 1.01, ParameterType::Real32));
}

void ParameterCoreTest::pdefMetadataSelectsVehicleAndLibraries()
{
    QByteArray xml(R"xml(
        <paramfile>
          <libraries>
            <parameters name="AP_Arming">
              <param name="AP_Arming:RTL_ALT" humanName="Library fallback"
                     user="Advanced" />
              <param name="AP_Arming:ARMING_CHECK" humanName="Arming checks"
                     user="Advanced">
                <values><value code="0.6">Low</value><value code="1">All</value></values>
                <field name="Bitmask">0:First,2:Third</field>
              </param>
            </parameters>
          </libraries>
          <vehicles>
            <parameters name="ArduCopter">
              <param name="ArduCopter:RTL_ALT" humanName="RTL Altitude"
                     documentation="Return altitude" user="Standard">
                <field name="Units">cm</field>
                <field name="Range">200 300000</field>
                <field name="Increment">10</field>
              </param>
            </parameters>
            <parameters name="ArduPlane">
              <param name="ArduPlane:PLANE_ONLY" user="Advanced" />
            </parameters>
          </vehicles>
        </paramfile>)xml");
    QBuffer buffer(&xml);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const ParameterMetaDataCatalog catalog =
        ParameterMetaDataCatalog::fromPdef(&buffer, QStringLiteral("ArduCopter"));
    QVERIFY2(catalog.isValid(), qPrintable(catalog.errorString()));
    QVERIFY(catalog.contains(QStringLiteral("RTL_ALT")));
    QVERIFY(catalog.contains(QStringLiteral("ARMING_CHECK")));
    QVERIFY(!catalog.contains(QStringLiteral("PLANE_ONLY")));

    const ParameterMetaData rtl = catalog.value(QStringLiteral("rtl_alt"));
    QCOMPARE(rtl.title, QStringLiteral("RTL Altitude"));
    QCOMPARE(rtl.userLevel, ParameterUserLevel::Standard);
    QCOMPARE(rtl.units, QStringLiteral("cm"));
    QVERIFY(rtl.hasRange);
    QCOMPARE(rtl.minimum, 200.0);
    QCOMPARE(rtl.maximum, 300000.0);
    QCOMPARE(rtl.increment, 10.0);
    QVERIFY(rtl.hasIncrement);
    QCOMPARE(catalog.entriesForLevel(ParameterUserLevel::Standard).size(), 1);
    QCOMPARE(catalog.entriesForLevel(ParameterUserLevel::Advanced).size(), 1);
    const ParameterMetaData arming = catalog.value(QStringLiteral("ARMING_CHECK"));
    QCOMPARE(arming.values.first().rawCode, QStringLiteral("0.6"));
    QCOMPARE(arming.values.first().value.toDouble(), 0.6);
    QCOMPARE(arming.bitmaskValues.size(), 2);
    QCOMPARE(arming.group, QStringLiteral("AP_Arming"));
    QCOMPARE(arming.scope, ParameterMetaDataScope::Library);
}

void ParameterCoreTest::pdefMetadataHandlesEnumsRangesAndMalformedXml()
{
    QVERIFY(!ParameterMetaDataCatalog().isValid());
    QByteArray xml(R"xml(
        <paramfile><vehicles><parameters name="ArduCopter">
          <param name="ArduCopter:MODE" user="Standard">
            <values><value code="-1">Disabled</value><value code="4">Auto</value></values>
            <field name="Bitmask">0:Disabled,2:Auto</field>
          </param>
          <param name="ArduCopter:GAIN" user="Advanced">
            <field name="Range">-2.5--0.5</field>
            <field name="ReadOnly">true</field>
            <field name="RebootRequired">1</field>
            <field name="FutureField">kept</field>
          </param>
        </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    const ParameterMetaDataCatalog catalog =
        ParameterMetaDataCatalog::fromPdef(&buffer, QStringLiteral("ArduCopter"));
    QVERIFY(catalog.isValid());
    const ParameterMetaData mode = catalog.value(QStringLiteral("MODE"));
    QVERIFY(mode.isEnum());
    QCOMPARE(mode.values.size(), 2);
    QCOMPARE(mode.values.first().value.toLongLong(), qlonglong(-1));
    const ParameterMetaData gain = catalog.value(QStringLiteral("GAIN"));
    QVERIFY(gain.hasRange);
    QCOMPARE(gain.minimum, -2.5);
    QCOMPARE(gain.maximum, -0.5);
    QVERIFY(gain.readOnly);
    QVERIFY(gain.rebootRequired);
    QVERIFY(!gain.hasIncrement);
    QCOMPARE(gain.fields.value(QStringLiteral("FutureField")),
             QStringLiteral("kept"));

    QByteArray malformed("<paramfile><vehicles>");
    QBuffer malformedBuffer(&malformed);
    QVERIFY(malformedBuffer.open(QIODevice::ReadOnly));
    const ParameterMetaDataCatalog invalid =
        ParameterMetaDataCatalog::fromPdef(
            &malformedBuffer, QStringLiteral("ArduCopter"));
    QVERIFY(!invalid.isValid());
    QVERIFY(!invalid.errorString().isEmpty());
}

void ParameterCoreTest::packagedPdefsParse_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("vehicleName");
    QTest::addColumn<QString>("knownParameter");

    QTest::newRow("copter") << QStringLiteral("arducopter.pdef.xml")
                             << QStringLiteral("ArduCopter")
                             << QStringLiteral("FRAME_CLASS");
    QTest::newRow("plane") << QStringLiteral("arduplane.pdef.xml")
                            << QStringLiteral("ArduPlane")
                            << QStringLiteral("ARSPD_FBW_MIN");
    QTest::newRow("rover") << QStringLiteral("ardurover.pdef.xml")
                            << QStringLiteral("Rover")
                            << QStringLiteral("CRUISE_SPEED");
}

void ParameterCoreTest::packagedPdefsParse()
{
    QFETCH(QString, fileName);
    QFETCH(QString, vehicleName);
    QFETCH(QString, knownParameter);

    const QString path = QDir(QStringLiteral(APM_TEST_SOURCE_DIR)).filePath(
        QStringLiteral("files/ardupilotmega/") + fileName);
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const ParameterMetaDataCatalog catalog =
        ParameterMetaDataCatalog::fromPdef(&file, vehicleName);
    QVERIFY2(catalog.isValid(), qPrintable(catalog.errorString()));
    QVERIFY(catalog.contains(knownParameter));
    QVERIFY(catalog.entries().size() > 100);

    if (vehicleName == QLatin1String("Rover")) {
        const ParameterMetaData failsafe = catalog.value(
            QStringLiteral("FS_EKF_THRESH"));
        QVERIFY(failsafe.isEnum());
        QCOMPARE(failsafe.values.first().rawCode, QStringLiteral("0.6"));
        QCOMPARE(failsafe.values.first().value.toDouble(), 0.6);
    }
}

void ParameterCoreTest::metadataRepositoryMapsPackagedFamilies()
{
    const QString directory = QDir(QStringLiteral(APM_TEST_SOURCE_DIR)).filePath(
        QStringLiteral("files/ardupilotmega"));
    ParameterMetaDataRepository repository(directory);

    const ParameterMetaDataSource rover =
        ParameterMetaDataRepository::sourceForFamily(
            ParameterFirmwareFamily::Rover);
    QCOMPARE(rover.fileName, QStringLiteral("ardurover.pdef.xml"));
    QCOMPARE(rover.vehicleName, QStringLiteral("Rover"));

    const ParameterMetaDataCatalog catalog = repository.catalog(
        ParameterFirmwareFamily::Rover);
    QVERIFY2(catalog.isValid(),
             qPrintable(repository.errorString(ParameterFirmwareFamily::Rover)));
    QVERIFY(catalog.contains(QStringLiteral("CRUISE_SPEED")));

    QVERIFY(!repository.catalog(ParameterFirmwareFamily::Unknown).isValid());
    QVERIFY(!repository.errorString(ParameterFirmwareFamily::Unknown).isEmpty());
}

void ParameterCoreTest::metadataRepositoryUsesResourceFallback()
{
    ParameterMetaDataRepository repository(
        QStringLiteral("/path/that/does/not/contain/metadata"));
    const ParameterMetaDataCatalog catalog = repository.catalog(
        ParameterFirmwareFamily::Rover);
    QVERIFY2(catalog.isValid(),
             qPrintable(repository.errorString(ParameterFirmwareFamily::Rover)));
    QVERIFY(catalog.contains(QStringLiteral("CRUISE_SPEED")));
    QCOMPARE(catalog.value(QStringLiteral("CRUISE_SPEED")).title,
             QStringLiteral("Cruise Speed"));
}

QTEST_APPLESS_MAIN(ParameterCoreTest)

#include "test_parametercore.moc"
