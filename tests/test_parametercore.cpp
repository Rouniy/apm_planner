#include <QtTest>

#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterMetaDataRepository.h"
#include "core/parameters/ParameterMetaDataUpdater.h"
#include "core/parameters/ParameterStore.h"
#include "uas/APMFirmwareVersion.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

namespace {
QByteArray catalogXml(const QString &vehicleName, const QString &label)
{
    QByteArray xml = "<paramfile><vehicles><parameters name=\""
        + vehicleName.toUtf8() + "\">";
    for (int index = 0; index < 101; ++index) {
        const QString name = index == 0
            ? QStringLiteral("TEST_PARAM")
            : QStringLiteral("TEST_%1").arg(index);
        xml += QStringLiteral(
            "<param name=\"%1:%2\" humanName=\"%3\" user=\"Standard\"/>")
                   .arg(vehicleName, name, index == 0 ? label : name)
                   .toUtf8();
    }
    xml += "</parameters></vehicles></paramfile>";
    return xml;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size();
}

class MetadataHttpServer final : public QObject
{
public:
    explicit MetadataHttpServer(const QByteArray &latestPayload)
        : m_latestPayload(latestPayload)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket]() {
                    if (socket->property("handled").toBool()) {
                        socket->readAll();
                        return;
                    }
                    QByteArray request = socket->property(
                        "requestBytes").toByteArray();
                    request += socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("requestBytes", request);
                        return;
                    }
                    socket->setProperty("handled", true);
                    const QByteArray firstLine = request.left(
                        request.indexOf("\r\n"));
                    const QList<QByteArray> words = firstLine.split(' ');
                    const QString path = words.size() > 1
                        ? QString::fromLatin1(words.at(1)) : QString();
                    requests.append(path);
                    if (holdVersionedRequests
                        && path.contains(QStringLiteral("/versioned/"))) {
                        return;
                    }
                    QByteArray response;
                    if (redirectVersionedRequests
                        && path.contains(QStringLiteral("/versioned/"))) {
                        response = QByteArrayLiteral(
                            "HTTP/1.1 302 Found\r\n"
                            "Location: /Parameters/ArduCopter/apm.pdef.xml\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    } else if (path.contains(QStringLiteral("/versioned/"))) {
                        response = QByteArrayLiteral(
                            "HTTP/1.1 404 Not Found\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    } else {
                        response = QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
                            + QByteArrayLiteral("Content-Type: application/xml\r\n")
                            + QByteArrayLiteral("Content-Length: ")
                            + QByteArray::number(m_latestPayload.size())
                            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                            + m_latestPayload;
                    }
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1")
                        .arg(m_server.serverPort()));
    }

    QStringList requests;
    bool holdVersionedRequests = false;
    bool redirectVersionedRequests = false;

private:
    QTcpServer m_server;
    QByteArray m_latestPayload;
};
}

class ParameterCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void duplicatePacketsDoNotFakeCompletion();
    void sameNameFromTwoComponentsDoesNotCollide();
    void exactEndpointsWithSameMavIdsDoNotCollide();
    void targetSwitchKeepsEndpointSnapshots();
    void switchingDoesNotMixInFlightRefreshes();
    void cancellationPreservesCommittedSnapshot();
    void failureAndPartialFinishPreserveCommittedSnapshot();
    void sentinelIndexAndCountDoNotCorruptProgress();
    void codecPreservesBytewiseTypes();
    void integerAndFloatEqualityAreTypeAware();
    void pdefMetadataSelectsVehicleAndLibraries();
    void pdefMetadataHandlesEnumsRangesAndMalformedXml();
    void packagedPdefsParse_data();
    void packagedPdefsParse();
    void metadataRepositoryMapsPackagedFamilies();
    void metadataRepositoryUsesResourceFallback();
    void metadataCachePrioritizesExactAndPreservesFallback();
    void metadataFreshnessUsesValidatedProvenance();
    void metadataUpdaterBuildsSafeCandidateUrls();
    void metadataUpdaterFallsBackBacksOffAndCancels();
    void firmwareVersionTracksReleaseType();
};

void ParameterCoreTest::duplicatePacketsDoNotFakeCompletion()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();

    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("COLLISION"), 99,
                         ParameterType::Int32));
    QCOMPARE(store.progress().received, 1);
    QCOMPARE(store.progress().reported, 2);
    QCOMPARE(store.state(), ParameterLoadState::Loading);
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("A")));

    QVERIFY(store.ingest(1, 2, 1, QStringLiteral("B"), 2, ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.specializedPagesReady());
    QCOMPARE(store.snapshot().value(1, QStringLiteral("A")).value.toInt(), 1);
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("COLLISION")));
}

void ParameterCoreTest::sameNameFromTwoComponentsDoesNotCollide()
{
    ParameterStore store;
    const VehicleEndpoint autopilot{7, 1, 1, QString(), QString()};
    const VehicleEndpoint gimbal{7, 1, 154, QString(), QString()};

    store.beginLoad(autopilot);
    QVERIFY(store.ingest(autopilot, 1, 0, QStringLiteral("VERSION"), 10,
                         ParameterType::Int32));
    store.beginLoad(gimbal);
    QVERIFY(store.ingest(gimbal, 1, 0, QStringLiteral("VERSION"), 20,
                         ParameterType::Int32));

    const ParameterSnapshot autopilotSnapshot = store.snapshot(autopilot);
    const ParameterSnapshot gimbalSnapshot = store.snapshot(gimbal);
    QCOMPARE(autopilotSnapshot.records().size(), 1);
    QCOMPARE(gimbalSnapshot.records().size(), 1);
    QCOMPARE(autopilotSnapshot.value(1, QStringLiteral("VERSION")).value.toInt(),
             10);
    QCOMPARE(gimbalSnapshot.value(154, QStringLiteral("VERSION")).value.toInt(),
             20);
}

void ParameterCoreTest::exactEndpointsWithSameMavIdsDoNotCollide()
{
    ParameterStore store;
    const VehicleEndpoint radio{7, 42, 1, QStringLiteral("Radio"),
                                QStringLiteral("Autopilot")};
    const VehicleEndpoint simulator{8, 42, 1, QStringLiteral("Simulator"),
                                    QStringLiteral("Autopilot")};

    store.beginLoad(radio);
    QVERIFY(store.ingest(radio, 1, 0, QStringLiteral("SYSID_THISMAV"), 42,
                         ParameterType::Int32));
    store.beginLoad(simulator);
    QVERIFY(store.ingest(simulator, 1, 0, QStringLiteral("SYSID_THISMAV"), 84,
                         ParameterType::Int32));

    QCOMPARE(store.snapshot(radio)
                 .value(1, QStringLiteral("SYSID_THISMAV")).value.toInt(), 42);
    QCOMPARE(store.snapshot(simulator)
                 .value(1, QStringLiteral("SYSID_THISMAV")).value.toInt(), 84);
    QCOMPARE(store.cachedEndpoints().size(), 2);
}

void ParameterCoreTest::targetSwitchKeepsEndpointSnapshots()
{
    ParameterStore store;
    const VehicleEndpoint first{7, 1, 1, QString(), QString()};
    const VehicleEndpoint second{7, 2, 1, QString(), QString()};
    store.selectEndpoint(first);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("OLD"), 10, ParameterType::Int32));
    const ParameterSnapshot firstVisit = store.snapshot();

    store.selectEndpoint(second);
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("OLD")));
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("NEW"), 20, ParameterType::Int32));

    store.selectEndpoint(first);
    const ParameterSnapshot secondVisit = store.snapshot();
    QVERIFY(secondVisit.contains(1, QStringLiteral("OLD")));
    QVERIFY(!secondVisit.contains(1, QStringLiteral("NEW")));
    QVERIFY(secondVisit.target().revision > firstVisit.target().revision);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.specializedPagesReady());
}

void ParameterCoreTest::switchingDoesNotMixInFlightRefreshes()
{
    ParameterStore store;
    const VehicleEndpoint first{21, 33, 1, QString(), QString()};
    const VehicleEndpoint second{22, 33, 1, QString(), QString()};

    store.selectEndpoint(first);
    store.beginLoad();
    QVERIFY(store.ingest(first, 2, 0, QStringLiteral("FIRST_A"), 1,
                         ParameterType::Int32));

    store.selectEndpoint(second);
    store.beginLoad();
    QVERIFY(store.ingest(second, 1, 0, QStringLiteral("SECOND"), 2,
                         ParameterType::Int32));
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("SECOND")));

    // The first endpoint's response can finish after selection has moved.
    QVERIFY(store.ingest(first, 2, 1, QStringLiteral("FIRST_B"), 3,
                         ParameterType::Int32));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("FIRST_A")));

    store.selectEndpoint(first);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("FIRST_A")));
    QVERIFY(store.snapshot().contains(1, QStringLiteral("FIRST_B")));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("SECOND")));
}

void ParameterCoreTest::cancellationPreservesCommittedSnapshot()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("COMMITTED"), 1,
                         ParameterType::Int32));

    store.beginLoad();
    QVERIFY(store.ingest(1, 3, 0, QStringLiteral("STAGED"), 2,
                         ParameterType::Int32));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("STAGED")));
    store.cancelLoad();

    QCOMPARE(store.state(), ParameterLoadState::Cancelled);
    QCOMPARE(store.progress().received, 1);
    QCOMPARE(store.progress().reported, 1);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("COMMITTED")));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("STAGED")));
    QVERIFY(store.snapshot().isComplete());
    QVERIFY(store.specializedPagesReady());
}

void ParameterCoreTest::failureAndPartialFinishPreserveCommittedSnapshot()
{
    ParameterStore store;
    const VehicleEndpoint endpoint{9, 55, 1, QString(), QString()};
    store.selectEndpoint(endpoint);
    store.beginLoad();
    QVERIFY(store.ingest(endpoint, 1, 0, QStringLiteral("BASE"), 7,
                         ParameterType::Int32));

    store.beginLoad();
    QVERIFY(store.ingest(endpoint, 2, 0, QStringLiteral("FAILED"), 8,
                         ParameterType::Int32));
    store.failLoad();
    QCOMPARE(store.state(), ParameterLoadState::Failed);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("BASE")));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("FAILED")));
    QVERIFY(store.specializedPagesReady());

    store.beginLoad();
    QVERIFY(store.ingest(endpoint, 2, 0, QStringLiteral("PARTIAL"), 9,
                         ParameterType::Int32));
    store.finishLoad();
    QCOMPARE(store.state(), ParameterLoadState::Partial);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("BASE")));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("PARTIAL")));
    QVERIFY(store.specializedPagesReady());
}

void ParameterCoreTest::sentinelIndexAndCountDoNotCorruptProgress()
{
    ParameterStore store;
    const VehicleEndpoint endpoint{11, 70, 1, QString(), QString()};
    store.selectEndpoint(endpoint);
    store.beginLoad();

    QVERIFY(store.ingest(endpoint, 65535, 65535,
                         QStringLiteral("_HASH_CHECK"), 100,
                         ParameterType::UInt32));
    QCOMPARE(store.progress().received, 0);
    QCOMPARE(store.progress().reported, 0);
    QCOMPARE(store.state(), ParameterLoadState::Loading);

    QVERIFY(store.ingest(endpoint, 2, 0, QStringLiteral("A"), 1,
                         ParameterType::Int32));
    QCOMPARE(store.progress().received, 1);
    QCOMPARE(store.progress().reported, 2);

    // An ordinary duplicate index is an already-received slot, not a new
    // parameter and not additional progress.
    QVERIFY(store.ingest(endpoint, 2, 0, QStringLiteral("COLLISION"), 99,
                         ParameterType::Int32));
    QCOMPARE(store.progress().received, 1);

    // A sentinel count carries no replacement total. The known total of two
    // remains authoritative and index one completes the staged transaction.
    QVERIFY(store.ingest(endpoint, 65535, 1, QStringLiteral("B"), 2,
                         ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.progress().reported, 2);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("_HASH_CHECK")));
    QVERIFY(store.snapshot().contains(1, QStringLiteral("A")));
    QVERIFY(store.snapshot().contains(1, QStringLiteral("B")));
    QVERIFY(!store.snapshot().contains(1, QStringLiteral("COLLISION")));
    QCOMPARE(store.snapshot().value(1, QStringLiteral("B")).reportedCount,
             65535);

    // Outside a list refresh the same PARAM_VALUE index is a normal live
    // update (and commonly the acknowledgement for PARAM_SET).
    QVERIFY(store.ingest(endpoint, 2, 0, QStringLiteral("A"), 5,
                         ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.snapshot().value(1, QStringLiteral("A")).value.toInt(), 5);
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

void ParameterCoreTest::metadataCachePrioritizesExactAndPreservesFallback()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString packagedDirectory = QDir(temporaryDirectory.path())
        .filePath(QStringLiteral("packaged"));
    const QString cacheDirectory = QDir(temporaryDirectory.path())
        .filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(packagedDirectory));
    QVERIFY(writeBytes(QDir(packagedDirectory).filePath(
                           QStringLiteral("arducopter.pdef.xml")),
                       catalogXml(QStringLiteral("ArduCopter"),
                                  QStringLiteral("Packaged"))));

    ParameterMetaDataRepository repository(packagedDirectory, cacheDirectory);
    ParameterMetaDataCacheInfo latestInfo;
    latestInfo.sourceUrl = QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/ArduCopter/apm.pdef.xml");
    latestInfo.fetchedAtUtc = QDateTime::currentDateTimeUtc();
    ParameterMetaDataCacheInfo exactInfo;
    exactInfo.sourceUrl = QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/versioned/Copter/"
        "stable-4.6.3/apm.pdef.xml");
    exactInfo.fetchedAtUtc = latestInfo.fetchedAtUtc;
    QString error;
    QVERIFY2(repository.installCatalog(
                 ParameterFirmwareFamily::ArduCopter, QString(),
                 catalogXml(QStringLiteral("ArduCopter"),
                            QStringLiteral("Latest")),
                 latestInfo, &error),
             qPrintable(error));
    QVERIFY2(repository.installCatalog(
                 ParameterFirmwareFamily::ArduCopter,
                 QStringLiteral("4.6.3"),
                 catalogXml(QStringLiteral("ArduCopter"),
                            QStringLiteral("Exact")),
                 exactInfo, &error),
             qPrintable(error));

    const ParameterMetaDataCatalog exact = repository.catalog(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3"));
    QCOMPARE(exact.value(QStringLiteral("TEST_PARAM")).title,
             QStringLiteral("Exact"));
    QVERIFY(repository.catalogMatchesFirmwareVersion(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3")));

    const ParameterMetaDataCatalog latest = repository.catalog(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.4"));
    QCOMPARE(latest.value(QStringLiteral("TEST_PARAM")).title,
             QStringLiteral("Latest"));
    QVERIFY(!repository.catalogMatchesFirmwareVersion(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.4")));

    const QString exactPath = repository.cacheFilePath(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3"));
    QFile beforeFile(exactPath);
    QVERIFY(beforeFile.open(QIODevice::ReadOnly));
    const QByteArray before = beforeFile.readAll();
    beforeFile.close();
    QVERIFY(!repository.installCatalog(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3"),
        QByteArrayLiteral("<broken>"), exactInfo, &error));
    QFile afterFile(exactPath);
    QVERIFY(afterFile.open(QIODevice::ReadOnly));
    QCOMPARE(afterFile.readAll(), before);

    QVERIFY(writeBytes(exactPath + QStringLiteral(".json"),
                       QByteArrayLiteral("not json")));
    repository.clear(ParameterFirmwareFamily::ArduCopter);
    const ParameterMetaDataCatalog advisorySidecar = repository.catalog(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3"));
    QCOMPARE(advisorySidecar.value(QStringLiteral("TEST_PARAM")).title,
             QStringLiteral("Exact"));
    QVERIFY(!repository.catalogMatchesFirmwareVersion(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3")));
}

void ParameterCoreTest::metadataFreshnessUsesValidatedProvenance()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ParameterMetaDataRepository repository(
        temporaryDirectory.path(),
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("cache")));
    ParameterMetaDataCacheInfo cacheInfo;
    cacheInfo.sourceUrl = QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/ArduCopter/apm.pdef.xml");
    cacheInfo.fetchedAtUtc = QDateTime::currentDateTimeUtc().addDays(-8);
    QString error;
    QVERIFY2(repository.installCatalog(
                 ParameterFirmwareFamily::ArduCopter, QString(),
                 catalogXml(QStringLiteral("ArduCopter"),
                            QStringLiteral("Latest")),
                 cacheInfo, &error),
             qPrintable(error));
    QVERIFY(!repository.cachedCatalogIsFresh(
        ParameterFirmwareFamily::ArduCopter, QString(), 7 * 24 * 60 * 60));

    const QString path = repository.cacheFilePath(
        ParameterFirmwareFamily::ArduCopter, QString());
    QJsonObject forged;
    forged.insert(QStringLiteral("format"), 1);
    forged.insert(QStringLiteral("family"), QStringLiteral("ArduCopter"));
    forged.insert(QStringLiteral("firmwareVersion"), QStringLiteral("latest"));
    forged.insert(QStringLiteral("sha256"), QStringLiteral("wrong"));
    forged.insert(QStringLiteral("fetchedAtUtc"),
                  QDateTime::currentDateTimeUtc().addDays(30)
                      .toString(Qt::ISODate));
    QVERIFY(writeBytes(path + QStringLiteral(".json"),
                       QJsonDocument(forged).toJson()));
    QFile catalogFile(path);
    QVERIFY(catalogFile.open(QIODevice::ReadOnly));
    QVERIFY(catalogFile.setFileTime(
        QDateTime::currentDateTimeUtc().addDays(-8),
        QFileDevice::FileModificationTime));
    catalogFile.close();
    QVERIFY(!repository.cachedCatalogIsFresh(
        ParameterFirmwareFamily::ArduCopter, QString(), 7 * 24 * 60 * 60));
}

void ParameterCoreTest::metadataUpdaterBuildsSafeCandidateUrls()
{
    QVERIFY(ParameterMetaDataRepository::normalizedVersion(
        QString(300, QLatin1Char('9')) + QStringLiteral(".1.1")).isEmpty());

    const QList<QUrl> stable = ParameterMetaDataUpdater::candidateUrls(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.6.3"), false);
    QCOMPARE(stable.size(), 2);
    QVERIFY(stable.first().path().contains(
        QStringLiteral("/versioned/Copter/stable-4.6.3/")));
    QCOMPARE(stable.last().toString(), QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/ArduCopter/apm.pdef.xml"));

    const QList<QUrl> development = ParameterMetaDataUpdater::candidateUrls(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("4.8.0"), true);
    QCOMPARE(development.size(), 1);
    QVERIFY(!development.first().path().contains(QStringLiteral("versioned")));
    QVERIFY(ParameterMetaDataUpdater::candidateUrls(
        ParameterFirmwareFamily::ArduCopter,
        QStringLiteral("4.8.0-dev"), false).isEmpty());
}

void ParameterCoreTest::metadataUpdaterFallsBackBacksOffAndCancels()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    MetadataHttpServer server(catalogXml(
        QStringLiteral("ArduCopter"), QStringLiteral("Downloaded")));
    QVERIFY(server.listen());
    ParameterMetaDataRepository repository(
        temporaryDirectory.path(),
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("cache")));
    ParameterMetaDataUpdater updater(&repository, server.baseUrl());
    int updated = 0;
    int failed = 0;
    QString installedVersion;
    connect(&updater, &ParameterMetaDataUpdater::catalogUpdated,
            this, [&](ParameterFirmwareFamily, const QString &version) {
        ++updated;
        installedVersion = version;
    });
    connect(&updater, &ParameterMetaDataUpdater::updateFailed,
            this, [&](ParameterFirmwareFamily, const QString &) {
        ++failed;
    });

    updater.requestUpdate(ParameterFirmwareFamily::ArduCopter,
                          QStringLiteral("9.9.9"), false);
    QTRY_COMPARE_WITH_TIMEOUT(updated, 1, 3000);
    QCOMPARE(failed, 0);
    QVERIFY(installedVersion.isEmpty());
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests.at(0).contains(QStringLiteral("/versioned/")));
    QVERIFY(server.requests.at(1).endsWith(
        QStringLiteral("/Parameters/ArduCopter/apm.pdef.xml")));
    QVERIFY(repository.cachedCatalogIsFresh(
        ParameterFirmwareFamily::ArduCopter, QString(), 60));

    // The missing exact patch is backed off, while the freshly installed
    // latest catalog prevents another network request.
    updater.requestUpdate(ParameterFirmwareFamily::ArduCopter,
                          QStringLiteral("9.9.9"), false);
    QTest::qWait(100);
    QCOMPARE(server.requests.size(), 2);

    // A superseding/disconnect cancellation must not surface as a failure.
    server.holdVersionedRequests = true;
    updater.requestUpdate(ParameterFirmwareFamily::ArduCopter,
                          QStringLiteral("9.9.8"), false);
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 3000);
    updater.cancel();
    QTest::qWait(100);
    QCOMPARE(updated, 1);
    QCOMPARE(failed, 0);

    // A redirect from an exact URL to latest must not acquire exact
    // provenance. It is rejected and then fetched as an explicit fallback.
    QTemporaryDir redirectDirectory;
    QVERIFY(redirectDirectory.isValid());
    MetadataHttpServer redirectServer(catalogXml(
        QStringLiteral("ArduCopter"), QStringLiteral("Redirected")));
    redirectServer.redirectVersionedRequests = true;
    QVERIFY(redirectServer.listen());
    ParameterMetaDataRepository redirectRepository(
        redirectDirectory.path(),
        QDir(redirectDirectory.path()).filePath(QStringLiteral("cache")));
    ParameterMetaDataUpdater redirectUpdater(
        &redirectRepository, redirectServer.baseUrl());
    QString redirectInstalledVersion = QStringLiteral("not updated");
    connect(&redirectUpdater, &ParameterMetaDataUpdater::catalogUpdated,
            this, [&](ParameterFirmwareFamily, const QString &version) {
        redirectInstalledVersion = version;
    });
    redirectUpdater.requestUpdate(ParameterFirmwareFamily::ArduCopter,
                                  QStringLiteral("9.9.7"), false);
    QTRY_COMPARE_WITH_TIMEOUT(redirectInstalledVersion, QString(), 3000);
    QCOMPARE(redirectServer.requests.size(), 3);
    QVERIFY(!redirectRepository.catalogMatchesFirmwareVersion(
        ParameterFirmwareFamily::ArduCopter, QStringLiteral("9.9.7")));

    // Destruction with a held reply must disconnect callbacks before QNAM
    // destroys its children.
    redirectServer.redirectVersionedRequests = false;
    redirectServer.holdVersionedRequests = true;
    auto *destructingUpdater = new ParameterMetaDataUpdater(
        &redirectRepository, redirectServer.baseUrl());
    destructingUpdater->requestUpdate(ParameterFirmwareFamily::ArduCopter,
                                      QStringLiteral("9.9.6"), false);
    QTRY_COMPARE_WITH_TIMEOUT(redirectServer.requests.size(), 4, 3000);
    delete destructingUpdater;
    QCoreApplication::processEvents();
}

void ParameterCoreTest::firmwareVersionTracksReleaseType()
{
    APMFirmwareVersion development(QStringLiteral("ArduCopter V4.8.0-dev"));
    QVERIFY(development.isValid());
    QVERIFY(development.isDev());
    QVERIFY(!development.isOfficial());
    APMFirmwareVersion textStable(QStringLiteral("ArduCopter V4.6.3"));
    QVERIFY(textStable.isValid());
    QVERIFY(!textStable.isOfficial());
    APMFirmwareVersion overflow(
        QStringLiteral("ArduCopter V999999999999999999.1.1"));
    QVERIFY(!overflow.isValid());

    APMFirmwareVersion firmware;
    firmware.parseFlightSwVersion(
        (quint32(4) << 24) | (quint32(6) << 16)
        | (quint32(3) << 8) | quint32(255), QByteArray(),
        QStringLiteral("ArduCopter"));
    QCOMPARE(firmware.versionString(), QStringLiteral("4.6.3"));
    QVERIFY(firmware.isOfficial());
    QCOMPARE(firmware.releaseType(), 255);
    QCOMPARE(firmware.vehicleType(), QStringLiteral("ArduCopter"));

    firmware.parseVersion(QStringLiteral("ArduCopter V9.9.9-custom"));
    QCOMPARE(firmware.versionString(), QStringLiteral("4.6.3"));
    QCOMPARE(firmware.vehicleType(), QStringLiteral("ArduCopter"));
    QVERIFY(firmware.isOfficial());

    firmware.parseFlightSwVersion(
        (quint32(4) << 24) | (quint32(7) << 16)
        | (quint32(0) << 8) | quint32(192));
    QVERIFY(firmware.isBeta());
    QVERIFY(!firmware.isOfficial());
}

QTEST_GUILESS_MAIN(ParameterCoreTest)

#include "test_parametercore.moc"
