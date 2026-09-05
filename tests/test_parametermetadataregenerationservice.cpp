#include "core/parameters/ParameterMetaDataRegenerationService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>

namespace {
QByteArray sourceDocument(bool recursive = false)
{
    QByteArray result;
    for (int index = 0; index < 101; ++index) {
        result += "// @Param: P" + QByteArray::number(index) + "\n";
        result += "// @DisplayName: Parameter "
            + QByteArray::number(index) + "\n";
        result += "// @Description: Runtime generated fixture\n";
    }
    if (recursive) {
        result += "// @Group: LIB_\n";
        result += "// @Path: ../libraries/AP_Test/AP_Test_VarInfo.h\n";
        result += "// @Group: LIB2_\n";
        result += "// @Path: ../libraries/AP_Test/AP_Test_VarInfo.h\n";
        result += "AP_NESTEDGROUPINFO(AP_Nested, var_info)\n";
    }
    return result;
}

QByteArray sitlPdefDocument()
{
    QByteArray result =
        "<?xml version=\"1.0\"?><paramfile><vehicles>"
        "<parameters name=\"Blimp\"><param name=\"Blimp:FOREIGN\"/>"
        "</parameters></vehicles><libraries>"
        "<parameters name=\"SITL supplemental libraries\">";
    for (int index = 0; index < 101; ++index) {
        const QByteArray name = QByteArray::number(index);
        result += "<param name=\"SIM_P" + name
            + "\" humanName=\"Simulation " + name + "\">"
              "<field name=\"Documentation\">Fixture</field></param>";
    }
    result += "</parameters></libraries></paramfile>";
    return result;
}

QByteArray pdefDocument(const QString &vehicle)
{
    QByteArray result = "<?xml version=\"1.0\"?><paramfile><vehicles>";
    result += "<parameters name=\"" + vehicle.toUtf8() + "\">";
    for (int index = 0; index < 101; ++index) {
        const QByteArray name = QByteArray::number(index);
        result += "<param name=\"" + vehicle.toUtf8() + ":P" + name
            + "\" humanName=\"Parameter " + name + "\">"
              "<field name=\"Documentation\">Fixture</field></param>";
    }
    result += "</parameters></vehicles></paramfile>";
    return result;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class LoopbackMetadataServer final : public QObject
{
public:
    explicit LoopbackMetadataServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket]() { receiveRequest(socket); });
                connect(socket, &QTcpSocket::disconnected, socket,
                        [this, socket]() {
                    m_activeSockets.remove(socket);
                    m_requestBuffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    ~LoopbackMetadataServer() override
    {
        QObject::disconnect(&m_server, nullptr, this, nullptr);
        const auto sockets = m_server.findChildren<QTcpSocket *>();
        for (QTcpSocket *socket : sockets) {
            QObject::disconnect(socket, nullptr, nullptr, nullptr);
            socket->abort();
            delete socket;
        }
        m_server.close();
    }

    QUrl endpoint() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1")
                        .arg(m_server.serverPort()));
    }

    void failPath(const QString &path) { m_failPaths.insert(path); }
    void overrideBody(const QString &path, const QByteArray &body)
    {
        m_overrides.insert(path, body);
    }
    void setStalled(bool stalled) { m_stalled = stalled; }
    int requestCount() const { return m_requestCount; }
    int maximumConcurrency() const { return m_maximumConcurrency; }

private:
    void receiveRequest(QTcpSocket *socket)
    {
        if (!socket || m_activeSockets.contains(socket)) {
            return;
        }
        QByteArray &request = m_requestBuffers[socket];
        request += socket->readAll();
        if (!request.contains("\r\n\r\n")) {
            return;
        }
        const int lineEnd = request.indexOf("\r\n");
        if (lineEnd < 0) {
            return;
        }
        const QList<QByteArray> parts = request.left(lineEnd).split(' ');
        if (parts.size() < 2) {
            socket->disconnectFromHost();
            return;
        }
        const QString path = QString::fromUtf8(parts.at(1));
        m_activeSockets.insert(socket);
        ++m_requestCount;
        m_maximumConcurrency = qMax(m_maximumConcurrency,
                                    m_activeSockets.size());
        if (m_stalled) {
            return;
        }

        int status = m_failPaths.contains(path) ? 404 : 200;
        QByteArray body = m_overrides.value(path);
        if (body.isNull() && status == 200) {
            if (path.startsWith(QLatin1String(
                    "/ArduPilot/ardupilot/"))) {
                body = sourceDocument(path == QLatin1String(
                    "/ArduPilot/ardupilot/master/ArduCopter/Parameters.cpp"));
            } else if (path.startsWith(QLatin1String("/Parameters/"))) {
                const QStringList segments = path.split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
                const QString product = segments.value(1);
                const QString vehicle = product == QLatin1String("Heli")
                    ? QStringLiteral("Helicopter")
                    : product == QLatin1String("APMrover2")
                        ? QStringLiteral("Rover") : product;
                body = product == QLatin1String("SITL")
                    ? sitlPdefDocument() : pdefDocument(vehicle);
            } else {
                status = 404;
            }
        }
        const QPointer<QTcpSocket> guard(socket);
        QTimer::singleShot(15, socket,
                           [this, guard, status, body]() {
            if (!guard) {
                return;
            }
            const QByteArray reason = status == 200 ? "OK" : "Not Found";
            QByteArray response = "HTTP/1.1 " + QByteArray::number(status)
                + " " + reason + "\r\nContent-Type: application/octet-stream\r\n"
                + "Content-Length: " + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            guard->write(response);
            m_activeSockets.remove(guard.data());
            guard->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QSet<QString> m_failPaths;
    QMap<QString, QByteArray> m_overrides;
    QSet<QTcpSocket *> m_activeSockets;
    QMap<QTcpSocket *, QByteArray> m_requestBuffers;
    int m_requestCount = 0;
    int m_maximumConcurrency = 0;
    bool m_stalled = false;
};

using Service = ParameterMetaDataRegenerationService;

Service::Result resultFromSpy(QSignalSpy *spy, int index = 0)
{
    return spy->at(index).at(0).value<Service::Result>();
}
}

class ParameterMetaDataRegenerationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void manifestsAreExactAndStorageIsExplicit();
    void publishesGeneratedGraphAndNineProducts();
    void partialFailurePreservesPreviousProduct();
    void sourceBoundPreservesPreviousGeneratedFile();
    void cancellationIsTerminalAndPreservesPreviousFiles();
    void queuedNotificationsCannotCorruptSuccessorRun();
};

void ParameterMetaDataRegenerationServiceTest::
    manifestsAreExactAndStorageIsExplicit()
{
    const QList<Service::SourceSpec> sources = Service::sourceManifest();
    QCOMPARE(sources.size(), 9);
    for (int index = 0; index < sources.size(); ++index) {
        const auto &source = sources.at(index);
        QCOMPARE(source.rank, index);
        QCOMPARE(source.url.scheme(), QStringLiteral("https"));
        QCOMPARE(source.url.host(), QStringLiteral("raw.githubusercontent.com"));
        QVERIFY(source.url.path().startsWith(
            QStringLiteral("/ArduPilot/ardupilot/")
                + source.refName + QLatin1Char('/')));
        QVERIFY(source.url.path().endsWith(QStringLiteral(".cpp")));
    }
    QCOMPARE(sources.at(0).refName, QStringLiteral("master"));
    QCOMPARE(sources.at(5).refName, QStringLiteral("ArduCopter-stable"));
    QCOMPARE(sources.at(7).refName, QStringLiteral("APMrover2-stable"));
    QCOMPARE(sources.at(7).url.path(), QStringLiteral(
        "/ArduPilot/ardupilot/APMrover2-stable/Rover/Parameters.cpp"));

    const QList<Service::ProductSpec> products = Service::productManifest();
    QCOMPARE(products.size(), 9);
    QStringList names;
    for (const auto &product : products) {
        names.append(product.product);
        QCOMPARE(product.url.scheme(), QStringLiteral("https"));
        QCOMPARE(product.url.host(), QStringLiteral("autotest.ardupilot.org"));
        QCOMPARE(product.fileName,
                 product.product + QStringLiteral(".pdef.xml"));
    }
    QCOMPARE(names, QStringList({
        QStringLiteral("SITL"), QStringLiteral("AP_Periph"),
        QStringLiteral("ArduSub"), QStringLiteral("Rover"),
        QStringLiteral("ArduCopter"), QStringLiteral("ArduPlane"),
        QStringLiteral("AntennaTracker"), QStringLiteral("Blimp"),
        QStringLiteral("Heli")}));
    QCOMPARE(products.at(3).url.path(),
             QStringLiteral("/Parameters/APMrover2/apm.pdef.xml"));
    QCOMPARE(products.at(3).vehicleName, QStringLiteral("Rover"));
    QVERIFY(!products.first().requireVehicleSection);
    for (int index = 1; index < products.size(); ++index) {
        QVERIFY(products.at(index).requireVehicleSection);
    }
    QCOMPARE(products.last().vehicleName, QStringLiteral("Helicopter"));

    Service service(QString{});
    Service::RunToken token;
    QString error;
    QCOMPARE(service.start(&token, &error),
             Service::StartResult::StorageUnavailable);
    QVERIFY(!token.isValid());
    QVERIFY(!error.isEmpty());
}

void ParameterMetaDataRegenerationServiceTest::
    publishesGeneratedGraphAndNineProducts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LoopbackMetadataServer server;
    server.failPath(QStringLiteral(
        "/ArduPilot/ardupilot/master/ArduCopter/AP_Nested.cpp"));
    Service service(directory.path(), server.endpoint());
    QSignalSpy finished(&service, &Service::finished);
    QSignalSpy publications(&service, &Service::publication);

    Service::RunToken token;
    QString error;
    QCOMPARE(service.start(&token, &error), Service::StartResult::Started);
    QVERIFY(token.isValid());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);

    const Service::Result result = resultFromSpy(&finished);
    QVERIFY(result.token == token);
    QCOMPARE(result.outcome, Service::Outcome::Complete);
    QCOMPARE(result.artifacts.size(), 10);
    QCOMPARE(publications.size(), 10);
    for (const Service::ArtifactReport &artifact : result.artifacts) {
        QVERIFY2(artifact.published, qPrintable(artifact.error));
        QVERIFY(QFileInfo::exists(artifact.filePath));
        QVERIFY(artifact.byteCount > 0);
        QCOMPARE(artifact.sha256.size(), 64);
    }
    const QByteArray generated = readFile(QDir(service.outputDirectory())
        .filePath(QStringLiteral("generated-source.pdef.xml")));
    QVERIFY(generated.contains("LIB_P0"));
    QVERIFY(generated.contains("LIB2_P0"));
    QVERIFY(generated.contains("P100"));
    QCOMPARE(server.requestCount(), 20);
    QVERIFY(server.maximumConcurrency() >= 2);
    QVERIFY(server.maximumConcurrency() <= 3);
    QVERIFY(std::any_of(
        result.logLines.cbegin(), result.logLines.cend(),
        [](const QString &line) {
            return line.contains(QStringLiteral(
                "Skipped optional inferred sibling"));
        }));

    const Service::Snapshot snapshot = service.snapshot();
    QVERIFY(!snapshot.active);
    QVERIFY(snapshot.terminal);
    QCOMPARE(snapshot.outcome, Service::Outcome::Complete);
    QCOMPARE(snapshot.phase, Service::Phase::Finished);
}

void ParameterMetaDataRegenerationServiceTest::
    partialFailurePreservesPreviousProduct()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = QDir(directory.path()).filePath(
        QStringLiteral("regenerated"));
    QVERIFY(QDir().mkpath(output));
    const QString planePath = QDir(output).filePath(
        QStringLiteral("ArduPlane.pdef.xml"));
    QFile oldPlane(planePath);
    QVERIFY(oldPlane.open(QIODevice::WriteOnly));
    QCOMPARE(oldPlane.write("previous-plane"), qint64(14));
    oldPlane.close();

    LoopbackMetadataServer server;
    server.failPath(QStringLiteral(
        "/Parameters/ArduPlane/apm.pdef.xml"));
    Service service(directory.path(), server.endpoint());
    QSignalSpy finished(&service, &Service::finished);
    Service::RunToken token;
    QCOMPARE(service.start(&token), Service::StartResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);

    const Service::Result result = resultFromSpy(&finished);
    QCOMPARE(result.outcome, Service::Outcome::PartialFailure);
    QCOMPARE(readFile(planePath), QByteArray("previous-plane"));
    bool planeFailed = false;
    int published = 0;
    for (const Service::ArtifactReport &artifact : result.artifacts) {
        published += artifact.published ? 1 : 0;
        if (artifact.key == QLatin1String("ArduPlane")) {
            planeFailed = !artifact.published && !artifact.error.isEmpty();
        }
    }
    QVERIFY(planeFailed);
    QCOMPARE(published, 9);
}

void ParameterMetaDataRegenerationServiceTest::
    sourceBoundPreservesPreviousGeneratedFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = QDir(directory.path()).filePath(
        QStringLiteral("regenerated"));
    QVERIFY(QDir().mkpath(output));
    const QString generatedPath = QDir(output).filePath(
        QStringLiteral("generated-source.pdef.xml"));
    QFile oldGenerated(generatedPath);
    QVERIFY(oldGenerated.open(QIODevice::WriteOnly));
    QCOMPARE(oldGenerated.write("previous-generated"), qint64(18));
    oldGenerated.close();

    LoopbackMetadataServer server;
    server.overrideBody(
        QStringLiteral(
            "/ArduPilot/ardupilot/master/ArduCopter/Parameters.cpp"),
        QByteArray(8 * 1024 * 1024 + 1, 'x'));
    QByteArray untrusted = sourceDocument();
    untrusted += "// @Group: BAD_\n"
                 "// @Path: https://example.invalid/Parameters.cpp\n";
    server.overrideBody(
        QStringLiteral(
            "/ArduPilot/ardupilot/master/ArduPlane/Parameters.cpp"),
        untrusted);
    Service service(directory.path(), server.endpoint());
    QSignalSpy finished(&service, &Service::finished);
    Service::RunToken token;
    QCOMPARE(service.start(&token), Service::StartResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);

    const Service::Result result = resultFromSpy(&finished);
    QCOMPARE(result.outcome, Service::Outcome::PartialFailure);
    QCOMPARE(readFile(generatedPath), QByteArray("previous-generated"));
    const auto generated = std::find_if(
        result.artifacts.cbegin(), result.artifacts.cend(),
        [](const Service::ArtifactReport &artifact) {
            return artifact.kind == Service::ArtifactKind::GeneratedSource;
        });
    QVERIFY(generated != result.artifacts.cend());
    QVERIFY(!generated->published);
    QVERIFY(std::any_of(
        result.logLines.cbegin(), result.logLines.cend(),
        [](const QString &line) {
            return line.contains(QStringLiteral("Rejected recursive source"));
        }));
}

void ParameterMetaDataRegenerationServiceTest::
    cancellationIsTerminalAndPreservesPreviousFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = QDir(directory.path()).filePath(
        QStringLiteral("regenerated"));
    QVERIFY(QDir().mkpath(output));
    const QString generatedPath = QDir(output).filePath(
        QStringLiteral("generated-source.pdef.xml"));
    QFile oldGenerated(generatedPath);
    QVERIFY(oldGenerated.open(QIODevice::WriteOnly));
    oldGenerated.write("keep-me");
    oldGenerated.close();

    LoopbackMetadataServer server;
    server.setStalled(true);
    Service service(directory.path(), server.endpoint());
    QSignalSpy finished(&service, &Service::finished);
    Service::RunToken token;
    QCOMPARE(service.start(&token), Service::StartResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() > 0, 3000);
    QVERIFY(service.cancel(token));
    QVERIFY(!service.busy());
    QVERIFY(service.snapshot().terminal);
    QCOMPARE(service.snapshot().outcome, Service::Outcome::Cancelled);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(resultFromSpy(&finished).outcome, Service::Outcome::Cancelled);
    QCOMPARE(readFile(generatedPath), QByteArray("keep-me"));
    QVERIFY(!service.cancel(token));
    QVERIFY(server.maximumConcurrency() <= 3);
}

void ParameterMetaDataRegenerationServiceTest::
    queuedNotificationsCannotCorruptSuccessorRun()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LoopbackMetadataServer server;
    Service service(directory.path(), server.endpoint());
    QSignalSpy finished(&service, &Service::finished);
    Service::RunToken first;
    Service::RunToken second;
    bool restarted = false;
    connect(&service, &Service::logLine, &service,
            [&](Service::RunToken token, const QString &) {
        if (!restarted && token == first) {
            restarted = true;
            QVERIFY(service.cancel(first));
            QCOMPARE(service.start(&second), Service::StartResult::Started);
            QVERIFY(second.isValid());
            QVERIFY(second != first);
        }
    });

    QCOMPARE(service.start(&first), Service::StartResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(restarted, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 20000);
    bool firstCancelled = false;
    bool secondComplete = false;
    for (int index = 0; index < finished.size(); ++index) {
        const Service::Result result = resultFromSpy(&finished, index);
        if (result.token == first) {
            firstCancelled = result.outcome == Service::Outcome::Cancelled;
        } else if (result.token == second) {
            secondComplete = result.outcome == Service::Outcome::Complete;
        }
    }
    QVERIFY(firstCancelled);
    QVERIFY(secondComplete);
    QVERIFY(!service.cancel(first));
    QVERIFY(service.snapshot().token == second);
    QCOMPARE(service.snapshot().outcome, Service::Outcome::Complete);
}

QTEST_GUILESS_MAIN(ParameterMetaDataRegenerationServiceTest)

#include "test_parametermetadataregenerationservice.moc"
