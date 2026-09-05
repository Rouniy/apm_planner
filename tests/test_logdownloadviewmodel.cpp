#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/ExactLogTransferService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "ui/LogDownloadViewModel.h"

#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstring>

namespace {

VehicleEndpoint endpoint(int linkId = 7, int systemId = 42)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.linkName = QStringLiteral("Telemetry %1").arg(linkId);
    value.componentName = QStringLiteral("AUTOPILOT1");
    return value;
}

mavlink_message_t heartbeat(int systemId = 42)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, MAV_COMP_ID_AUTOPILOT1, &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        0, 0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t logEntry(quint16 id, quint16 count,
                           quint16 last, quint32 timeUtc,
                           quint32 size, int systemId = 42)
{
    mavlink_message_t message{};
    mavlink_msg_log_entry_pack(
        systemId, MAV_COMP_ID_AUTOPILOT1, &message,
        id, count, last, timeUtc, size);
    return message;
}

mavlink_message_t logData(quint16 id, quint32 offset,
                          const QByteArray &bytes, int systemId = 42)
{
    quint8 data[90]{};
    const int count = qMin(bytes.size(), 90);
    if (count > 0) {
        std::memcpy(data, bytes.constData(), static_cast<size_t>(count));
    }
    mavlink_message_t message{};
    mavlink_msg_log_data_pack(
        systemId, MAV_COMP_ID_AUTOPILOT1, &message,
        id, offset, static_cast<quint8>(count), data);
    return message;
}

struct Fixture
{
    qint64 now = 100;
    int frameCount = 0;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter;
    ExactLogTransferService service;
    quint64 session = 0;
    SwarmVehicleInstanceLease selected;

    Fixture()
        : registry([this]() { return now; })
        , transmitter([this](int, const QByteArray &) {
              ++frameCount;
              return true;
          })
        , service(&registry, &transmitter,
                  [](const SwarmVehicleInstanceLease &, QString *) {
              return true;
          })
    {
        session = registry.beginLinkSession(7, QStringLiteral("Telemetry 7"));
        transmitter.setLinkSessionEpoch(7, session);
        registry.observeMessage(7, session, heartbeat());
        selected = registry.acquireVehicle(endpoint());
        service.setLocalIdentity(250, MAV_COMP_ID_MISSIONPLANNER);
    }

    void finishList(std::initializer_list<ExactLogEntry> entries)
    {
        const quint16 count = static_cast<quint16>(entries.size());
        quint16 last = 0;
        for (const ExactLogEntry &entry : entries) {
            last = qMax(last, entry.id);
        }
        for (const ExactLogEntry &entry : entries) {
            service.observeMessage(
                7, session,
                logEntry(entry.id, count, last,
                         entry.timeUtc, entry.size));
        }
    }
};

} // namespace

class LogDownloadViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void formatsReferenceRowsAndSafeNames();
    void listIsAtomicFilteredSortedAndPickerRevalidatesTarget();
    void listCannotCrossTargetBeforePicker();
    void downloadAllCollisionCheckIncludesKml();
    void downloadRunsKmlAndAggregatesProgress();
    void cancellationDuringKmlStopsBatch();
    void foreignWindowCannotCancelActiveTransfer();
    void eraseIsExplicitlyUnconfirmed();
};

void LogDownloadViewModelTest::formatsReferenceRowsAndSafeNames()
{
    LogDownloadRow unknown;
    unknown.id = 19;
    unknown.sizeBytes = 1536;
    QCOMPARE(unknown.timeText(), QString::fromUtf8("—"));
    QCOMPARE(unknown.sizeText(), QStringLiteral("1.5 KB"));
    QCOMPARE(LogDownloadViewModel::suggestedFileName(unknown),
             QStringLiteral("log_19.bin"));

    LogDownloadRow known;
    known.id = 7;
    known.sizeBytes = 2U * 1024U * 1024U;
    known.timeUtc = QDateTime::fromSecsSinceEpoch(1700000000, Qt::UTC);
    QCOMPARE(known.sizeText(), QStringLiteral("2.0 MB"));
    const QString knownName =
        LogDownloadViewModel::suggestedFileName(known);
    QVERIFY(knownName.endsWith(QStringLiteral("_7.bin")));
    QVERIFY(!knownName.contains(QLatin1Char(':')));

    const QString dottedUnicode =
        QString::fromUtf8("/tmp/полёт.v1/flight.7.bin");
    QCOMPARE(LogDownloadViewModel::kmlFileName(dottedUnicode),
             QString::fromUtf8("/tmp/полёт.v1/flight.7.kml"));
}

void LogDownloadViewModelTest::
listIsAtomicFilteredSortedAndPickerRevalidatesTarget()
{
    Fixture fixture;
    SwarmVehicleInstanceLease current = fixture.selected;
    LogDownloadViewModel model(
        &fixture.service, [&current]() { return current; });
    model.setTargetSource(QStringLiteral("Telemetry 7 / sys 42 / comp 1"));

    QVERIFY(model.refresh());
    QVERIFY(model.logs().isEmpty());
    fixture.finishList({
        ExactLogEntry{5, 3, 5, 1700000000, 2048},
        ExactLogEntry{3, 3, 5, 0, 0},
        ExactLogEntry{4, 3, 5, 0, 1024}
    });
    QCOMPARE(model.logs().size(), 2);
    QCOMPARE(model.logs().at(0).id, quint16(4));
    QCOMPARE(model.logs().at(1).id, quint16(5));
    QCOMPARE(model.status(),
             QStringLiteral("2 log(s) on board. [Telemetry 7 / sys 42 / comp 1]"));

    model.setSelectedLogId(5);
    QString suggested;
    const quint64 preparation =
        model.prepareSelectedDownload(&suggested);
    QVERIFY(preparation != 0);
    QVERIFY(model.isBusy());
    QCOMPARE(suggested, QStringLiteral("log_5.bin"));
    model.setTargetSource(QStringLiteral("replacement"));
    QCOMPARE(model.targetSource(),
             QStringLiteral("Telemetry 7 / sys 42 / comp 1"));

    ++current.instanceEpoch;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination =
        QDir(temporary.path()).filePath(QStringLiteral("must-not-start.bin"));
    QVERIFY(!model.startPreparedSelectedDownload(
        preparation, destination));
    QVERIFY(model.status().contains(QStringLiteral("changed")));
    QVERIFY(!QFileInfo::exists(destination));
    QVERIFY(model.logs().isEmpty());
    QCOMPARE(model.targetSource(), QStringLiteral("replacement"));
}

void LogDownloadViewModelTest::listCannotCrossTargetBeforePicker()
{
    Fixture fixture;
    SwarmVehicleInstanceLease current = fixture.selected;
    LogDownloadViewModel model(
        &fixture.service, [&current]() { return current; });
    QVERIFY(model.refresh());
    fixture.finishList({ExactLogEntry{8, 1, 8, 0, 90}});
    QCOMPARE(model.logs().size(), 1);
    model.setSelectedLogId(8);

    ++current.instanceEpoch;
    QVERIFY(model.prepareSelectedDownload() == 0);
    QVERIFY(model.logs().isEmpty());
    QCOMPARE(model.selectedLogId(), -1);
    QVERIFY(model.status().contains(QStringLiteral("previous vehicle")));
    QVERIFY(!fixture.service.busy());
}

void LogDownloadViewModelTest::downloadAllCollisionCheckIncludesKml()
{
    Fixture fixture;
    LogDownloadViewModel model(
        &fixture.service, [&fixture]() { return fixture.selected; });
    QVERIFY(model.refresh());
    fixture.finishList({ExactLogEntry{21, 1, 21, 0, 3}});
    model.setCreateKmlAfterDownload(true);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString folder = QDir(temporary.path()).filePath(
        QString::fromUtf8("полёт.v1"));
    QVERIFY(QDir().mkpath(folder));
    const QString logPath = QDir(folder).filePath(
        QStringLiteral("log_21.bin"));
    const QString kmlPath = QDir(folder).filePath(
        QStringLiteral("log_21.kml"));
    QFile logFile(logPath);
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.write("old log");
    logFile.close();
    QFile kmlFile(kmlPath);
    QVERIFY(kmlFile.open(QIODevice::WriteOnly));
    kmlFile.write("old kml");
    kmlFile.close();

    const quint64 preparation = model.prepareDownloadAll();
    QVERIFY(preparation != 0);
    const QStringList collisions =
        model.existingDownloadAllFiles(preparation, folder);
    QCOMPARE(collisions.size(), 2);
    QVERIFY(collisions.contains(logPath));
    QVERIFY(collisions.contains(kmlPath));
    QVERIFY(!model.startPreparedDownloadAll(
        preparation, folder, false));
    QVERIFY(model.status().contains(QStringLiteral("not overwritten")));
}

void LogDownloadViewModelTest::downloadRunsKmlAndAggregatesProgress()
{
    Fixture fixture;
    std::atomic_int exportCalls{0};
    LogDownloadViewModel model(
        &fixture.service, [&fixture]() { return fixture.selected; },
        [&exportCalls](
            const QString &, const QString &,
            const DataFlashKmlExporter::CancellationCheck &) {
            ++exportCalls;
            DataFlashKmlExporter::Result result;
            result.succeeded = true;
            result.pointCount = 3;
            return result;
        });
    QVERIFY(model.refresh());
    fixture.finishList({ExactLogEntry{9, 1, 9, 0, 3}});
    model.setSelectedLogId(9);
    model.setCreateKmlAfterDownload(true);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = QDir(temporary.path()).filePath(
        QString::fromUtf8("тест.папка/log.9.bin"));
    QVERIFY(QDir().mkpath(QFileInfo(destination).absolutePath()));
    QFile priorKml(LogDownloadViewModel::kmlFileName(destination));
    QVERIFY(priorKml.open(QIODevice::WriteOnly));
    priorKml.write("old kml");
    priorKml.close();
    QString suggested;
    quint64 preparation = model.prepareSelectedDownload(&suggested);
    QVERIFY(preparation != 0);
    QVERIFY(!model.startPreparedSelectedDownload(
        preparation, destination, false));
    QVERIFY(model.status().contains(QStringLiteral("existing KML")));

    preparation = model.prepareSelectedDownload(&suggested);
    QVERIFY(preparation != 0);
    QVERIFY(model.startPreparedSelectedDownload(
        preparation, destination, true));
    QVERIFY(model.isDownloading());
    fixture.service.observeMessage(
        7, fixture.session, logData(9, 0, QByteArrayLiteral("abc")));

    QTRY_COMPARE(exportCalls.load(), 1);
    QTRY_VERIFY(!model.isDownloading());
    QCOMPARE(model.progress(), 100.0);
    QVERIFY(model.status().contains(QStringLiteral("KML track(s) created")));
    QFile downloaded(destination);
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), QByteArrayLiteral("abc"));
}

void LogDownloadViewModelTest::cancellationDuringKmlStopsBatch()
{
    Fixture fixture;
    std::atomic_bool exporterStarted{false};
    LogDownloadViewModel model(
        &fixture.service, [&fixture]() { return fixture.selected; },
        [&exporterStarted](
            const QString &, const QString &,
            const DataFlashKmlExporter::CancellationCheck &cancelled) {
            exporterStarted = true;
            QElapsedTimer timeout;
            timeout.start();
            while (!cancelled() && timeout.elapsed() < 3000) {
                QThread::yieldCurrentThread();
            }
            DataFlashKmlExporter::Result result;
            result.cancelled = cancelled();
            result.error = result.cancelled
                ? QStringLiteral("canceled")
                : QStringLiteral("test timeout");
            return result;
        });
    QVERIFY(model.refresh());
    fixture.finishList({ExactLogEntry{11, 1, 11, 0, 3}});
    model.setSelectedLogId(11);
    model.setCreateKmlAfterDownload(true);

    QTemporaryDir temporary;
    const quint64 preparation = model.prepareSelectedDownload();
    QVERIFY(preparation != 0);
    QVERIFY(model.startPreparedSelectedDownload(
        preparation, QDir(temporary.path()).filePath(QStringLiteral("11.bin"))));
    fixture.service.observeMessage(
        7, fixture.session, logData(11, 0, QByteArrayLiteral("xyz")));
    QTRY_VERIFY(exporterStarted.load());
    model.cancelOwnDownload();
    QTRY_VERIFY(!model.isDownloading());
    QVERIFY(model.status().contains(QStringLiteral("canceled"),
                                    Qt::CaseInsensitive));
}

void LogDownloadViewModelTest::foreignWindowCannotCancelActiveTransfer()
{
    Fixture fixture;
    LogDownloadViewModel first(
        &fixture.service, [&fixture]() { return fixture.selected; });
    LogDownloadViewModel second(
        &fixture.service, [&fixture]() { return fixture.selected; });
    QVERIFY(first.refresh());
    fixture.finishList({ExactLogEntry{13, 1, 13, 0, 90}});
    first.setSelectedLogId(13);
    QTemporaryDir temporary;
    const quint64 preparation = first.prepareSelectedDownload();
    QVERIFY(first.startPreparedSelectedDownload(
        preparation, QDir(temporary.path()).filePath(QStringLiteral("13.bin"))));
    QVERIFY(fixture.service.busy());
    QVERIFY(second.isBusy());

    second.cancelOwnDownload();
    QVERIFY(fixture.service.busy());
    QVERIFY(first.isDownloading());
    first.cancelOwnDownload();
    QVERIFY(!fixture.service.busy());
    QVERIFY(!first.isDownloading());
}

void LogDownloadViewModelTest::eraseIsExplicitlyUnconfirmed()
{
    Fixture fixture;
    LogDownloadViewModel model(
        &fixture.service, [&fixture]() { return fixture.selected; });
    QVERIFY(model.refresh());
    fixture.finishList({ExactLogEntry{15, 1, 15, 0, 90}});
    QCOMPARE(model.logs().size(), 1);

    const int framesBeforeErase = fixture.frameCount;
    const quint64 preparation = model.prepareErase();
    QVERIFY(preparation != 0);
    QVERIFY(model.completePreparedErase(preparation, true));
    QCOMPARE(fixture.frameCount - framesBeforeErase, 2);
    QVERIFY(model.logs().isEmpty());
    QVERIFY(model.status().contains(QStringLiteral("does not confirm")));
    QVERIFY(model.status().contains(QStringLiteral("Refresh List")));
    QVERIFY(!model.status().contains(QStringLiteral("were erased")));
}

QTEST_MAIN(LogDownloadViewModelTest)
#include "test_logdownloadviewmodel.moc"
