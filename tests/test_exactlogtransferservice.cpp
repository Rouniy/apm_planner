#include "comm/ExactLinkTransmitter.h"
#include "comm/ExactLogTransferService.h"
#include "comm/MAVLinkFrameParser.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstring>
#include <functional>
#include <limits>
#include <utility>

namespace {

constexpr int LinkId = 73;
constexpr int VehicleSystemId = 42;
constexpr quint8 VehicleComponentId = MAV_COMP_ID_AUTOPILOT1;
constexpr quint8 LocalSystemId = 250;
constexpr quint8 LocalComponentId = MAV_COMP_ID_MISSIONPLANNER;

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId = LinkId,
                         int systemId = VehicleSystemId,
                         int componentId = VehicleComponentId)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = VehicleEndpoint::defaultComponentName(componentId);
    return value;
}

mavlink_message_t heartbeat(int systemId = VehicleSystemId,
                            quint8 componentId = VehicleComponentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        static_cast<quint8>(systemId), componentId, &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        0, 0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t logEntry(quint16 id, quint16 count, quint16 last,
                           quint32 timeUtc, quint32 size,
                           int systemId = VehicleSystemId,
                           quint8 componentId = VehicleComponentId)
{
    mavlink_message_t message{};
    mavlink_msg_log_entry_pack(
        static_cast<quint8>(systemId), componentId, &message,
        id, count, last, timeUtc, size);
    return message;
}

mavlink_message_t logData(quint16 id, quint32 offset,
                          const QByteArray &bytes,
                          int systemId = VehicleSystemId,
                          quint8 componentId = VehicleComponentId)
{
    quint8 data[LogDownloadTracker::PacketSize]{};
    const int count = qMin(
        bytes.size(), static_cast<int>(LogDownloadTracker::PacketSize));
    if (count > 0) {
        std::memcpy(data, bytes.constData(), static_cast<size_t>(count));
    }
    mavlink_message_t message{};
    mavlink_msg_log_data_pack(
        static_cast<quint8>(systemId), componentId, &message,
        id, offset, static_cast<quint8>(count), data);
    return message;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

int countFrames(const QVector<CapturedFrame> &frames, quint32 messageId)
{
    int count = 0;
    for (const CapturedFrame &frame : frames) {
        if (decodeFrame(frame.bytes).msgid == messageId) {
            ++count;
        }
    }
    return count;
}

mavlink_message_t lastFrame(const QVector<CapturedFrame> &frames,
                            quint32 messageId)
{
    for (auto iterator = frames.crbegin(); iterator != frames.crend();
         ++iterator) {
        const mavlink_message_t message = decodeFrame(iterator->bytes);
        if (message.msgid == messageId) {
            return message;
        }
    }
    return {};
}

class Fixture final
{
public:
    Fixture()
        : registry([this]() { return registryNowMs; })
        , transmitter([this](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            if (writeHook) {
                writeHook();
            }
            return writesSucceed;
        })
        , service(
              &registry, &transmitter,
              [this](const SwarmVehicleInstanceLease &, QString *error) {
                  if (error) {
                      error->clear();
                  }
                  return routeAllowed;
              },
              [this]() { return serviceNowMs; },
              100, 4, 100, 20, 300)
    {
        session = registry.beginLinkSession(
            LinkId, QStringLiteral("Link 73"));
        transmitter.setLinkSessionEpoch(LinkId, session);
        registry.observeMessage(LinkId, session, heartbeat());
        vehicle = registry.acquireVehicle(endpoint());
        service.setLocalIdentity(LocalSystemId, LocalComponentId);
        QObject::connect(
            &service, &ExactLogTransferService::transferFinished,
            &service, [this](ExactLogTransferResult result) {
                results.append(std::move(result));
            });
    }

    bool expire(int milliseconds)
    {
        serviceNowMs += milliseconds;
        return QMetaObject::invokeMethod(
            &service, "timeout", Qt::DirectConnection);
    }

    void observe(const mavlink_message_t &message,
                 int linkId = LinkId, quint64 epoch = 0)
    {
        service.observeMessage(
            linkId, epoch == 0 ? session : epoch, message);
    }

    qint64 registryNowMs = 100;
    qint64 serviceNowMs = 1000;
    bool writesSucceed = true;
    bool routeAllowed = true;
    std::function<void()> writeHook;
    QVector<CapturedFrame> frames;
    QVector<ExactLogTransferResult> results;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter;
    ExactLogTransferService service;
    quint64 session = 0;
    SwarmVehicleInstanceLease vehicle;
};

} // namespace

class ExactLogTransferServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void listUsesExactRouteAndRetriesBoundedly();
    void downloadIsAtomicAndAdvertisedSizeIsOnlyAnEstimate();
    void repairIsSingleBoundedAndChainsOnlyOnProgress();
    void ownerCancelAndEraseAreExplicitlyBounded();
    void routeChangeAtProgressPreservesExistingDestination();
    void realShortEndPrunesDeferredFarPacket();
    void synchronousListReplyFinishesExactlyOnce();
};

void ExactLogTransferServiceTest::listUsesExactRouteAndRetriesBoundedly()
{
    Fixture fixture;
    QObject owner;
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.requestList(
                 &owner, fixture.vehicle, &token),
             ExactLogTransferService::StartResult::Started);
    QVERIFY(token.isValid());
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_LIST), 1);
    const mavlink_message_t requestMessage =
        lastFrame(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_LIST);
    QCOMPARE(requestMessage.sysid, LocalSystemId);
    QCOMPARE(requestMessage.compid, LocalComponentId);
    mavlink_log_request_list_t request{};
    mavlink_msg_log_request_list_decode(&requestMessage, &request);
    QCOMPARE(request.target_system, quint8(VehicleSystemId));
    QCOMPARE(request.target_component, VehicleComponentId);
    QCOMPARE(request.start, quint16(0));
    QCOMPARE(request.end, std::numeric_limits<quint16>::max());

    fixture.observe(logEntry(4, 2, 5, 1000, 40), LinkId + 1);
    fixture.observe(logEntry(4, 2, 5, 1000, 40),
                    LinkId, fixture.session + 1);
    fixture.observe(logEntry(4, 2, 5, 1000, 40,
                             VehicleSystemId + 1));
    fixture.observe(logEntry(4, 2, 5, 1000, 40,
                             VehicleSystemId, MAV_COMP_ID_CAMERA));
    QVERIFY(fixture.results.isEmpty());

    fixture.observe(logEntry(5, 2, 5, 2000, 0));
    QVERIFY(fixture.results.isEmpty());
    fixture.observe(logEntry(4, 2, 5, 1000, 40));
    QCOMPARE(fixture.results.size(), 1);
    const ExactLogTransferResult complete = fixture.results.constFirst();
    QCOMPARE(complete.operation, ExactLogTransferService::Operation::List);
    QCOMPARE(complete.outcome, ExactLogTransferService::Outcome::Completed);
    QCOMPARE(complete.entries.size(), 2);
    QCOMPARE(complete.entries.at(0).id, quint16(4));
    QCOMPARE(complete.entries.at(1).id, quint16(5));
    QCOMPARE(complete.entries.at(1).size, quint32(0));
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_END), 1);

    Fixture silent;
    QObject silentOwner;
    ExactLogTransferToken silentToken;
    QCOMPARE(silent.service.requestList(
                 &silentOwner, silent.vehicle, &silentToken),
             ExactLogTransferService::StartResult::Started);
    for (int retry = 0; retry < 4; ++retry) {
        QVERIFY(silent.expire(100));
        QVERIFY(silent.results.isEmpty());
    }
    QCOMPARE(countFrames(silent.frames, MAVLINK_MSG_ID_LOG_REQUEST_LIST), 5);
    QVERIFY(silent.expire(100));
    QCOMPARE(silent.results.size(), 1);
    QCOMPARE(silent.results.constFirst().outcome,
             ExactLogTransferService::Outcome::TimedOut);
    QCOMPARE(countFrames(silent.frames, MAVLINK_MSG_ID_LOG_REQUEST_END), 1);
}

void ExactLogTransferServiceTest::
downloadIsAtomicAndAdvertisedSizeIsOnlyAnEstimate()
{
    Fixture fixture;
    QObject owner;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString destination =
        QDir(directory.path()).filePath(QStringLiteral("log.bin"));
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.startDownload(
                 &owner, fixture.vehicle, 9, 3, destination, &token),
             ExactLogTransferService::StartResult::Started);

    const mavlink_message_t requestMessage =
        lastFrame(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA);
    mavlink_log_request_data_t request{};
    mavlink_msg_log_request_data_decode(&requestMessage, &request);
    QCOMPARE(request.id, quint16(9));
    QCOMPARE(request.ofs, quint32(0));
    QCOMPARE(request.count, std::numeric_limits<quint32>::max());
    QCOMPARE(request.target_system, quint8(VehicleSystemId));
    QCOMPARE(request.target_component, VehicleComponentId);

    const QByteArray first(90, 'a');
    const QByteArray last(20, 'b');
    fixture.observe(logData(9, 0, first));
    QVERIFY(fixture.results.isEmpty());
    QVERIFY(!QFileInfo::exists(destination));
    fixture.observe(logData(9, 90, last));
    QCOMPARE(fixture.results.size(), 1);
    const ExactLogTransferResult result = fixture.results.constFirst();
    QVERIFY(result.succeeded());
    QVERIFY(result.totalKnown);
    QCOMPARE(result.covered, quint64(110));
    QCOMPARE(result.total, quint64(110));
    QFile file(destination);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), first + last);
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_END), 1);
}

void ExactLogTransferServiceTest::
repairIsSingleBoundedAndChainsOnlyOnProgress()
{
    Fixture fixture;
    QObject owner;
    QTemporaryDir directory;
    const QString destination =
        QDir(directory.path()).filePath(QStringLiteral("gapped.bin"));
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.startDownload(
                 &owner, fixture.vehicle, 11, 380, destination, &token),
             ExactLogTransferService::StartResult::Started);

    fixture.observe(logData(11, 0, QByteArray(90, 'a')));
    fixture.observe(logData(11, 180, QByteArray(90, 'c')));
    fixture.observe(logData(11, 360, QByteArray(20, 'e')));
    QVERIFY(fixture.results.isEmpty());
    const int requestsBeforeRepair = countFrames(
        fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA);
    QVERIFY(fixture.expire(20));
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA),
             requestsBeforeRepair + 1);
    mavlink_log_request_data_t repair{};
    mavlink_message_t repairMessage =
        lastFrame(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA);
    mavlink_msg_log_request_data_decode(&repairMessage, &repair);
    QCOMPARE(repair.ofs, quint32(90));
    QCOMPARE(repair.count, quint32(90));
    QVERIFY(repair.count <= ExactLogTransferService::MaximumRepairRequest);

    fixture.observe(logData(11, 180, QByteArray(90, 'x')));
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA),
             requestsBeforeRepair + 1);
    fixture.observe(logData(11, 90, QByteArray(90, 'b')));
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA),
             requestsBeforeRepair + 2);
    repairMessage = lastFrame(
        fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_DATA);
    mavlink_msg_log_request_data_decode(&repairMessage, &repair);
    QCOMPARE(repair.ofs, quint32(270));
    QCOMPARE(repair.count, quint32(90));

    fixture.observe(logData(11, 270, QByteArray(90, 'd')));
    QCOMPARE(fixture.results.size(), 1);
    QVERIFY(fixture.results.constFirst().succeeded());
    QFile file(destination);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray(90, 'a') + QByteArray(90, 'b')
             + QByteArray(90, 'c') + QByteArray(90, 'd')
             + QByteArray(20, 'e'));
}

void ExactLogTransferServiceTest::
ownerCancelAndEraseAreExplicitlyBounded()
{
    Fixture fixture;
    QTemporaryDir directory;
    const QString destination =
        QDir(directory.path()).filePath(QStringLiteral("partial.bin"));
    auto *owner = new QObject;
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.startDownload(
                 owner, fixture.vehicle, 13, 200, destination, &token),
             ExactLogTransferService::StartResult::Started);
    fixture.observe(logData(13, 0, QByteArray(90, 'p')));

    QObject otherOwner;
    ExactLogTransferToken otherToken;
    QCOMPARE(fixture.service.requestList(
                 &otherOwner, fixture.vehicle, &otherToken),
             ExactLogTransferService::StartResult::Busy);
    QVERIFY(!otherToken.isValid());
    QVERIFY(!fixture.service.cancel({token.id + 1}));
    delete owner;
    QCOMPARE(fixture.results.size(), 1);
    QCOMPARE(fixture.results.constFirst().outcome,
             ExactLogTransferService::Outcome::Cancelled);
    QVERIFY(!QFileInfo::exists(destination));
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_END), 1);

    const int eraseBefore = countFrames(
        fixture.frames, MAVLINK_MSG_ID_LOG_ERASE);
    ExactLogTransferToken eraseToken;
    QCOMPARE(fixture.service.erase(
                 &otherOwner, fixture.vehicle, &eraseToken),
             ExactLogTransferService::StartResult::Started);
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_ERASE),
             eraseBefore + 2);
    QCOMPARE(fixture.results.size(), 2);
    const ExactLogTransferResult eraseResult = fixture.results.constLast();
    QCOMPARE(eraseResult.operation, ExactLogTransferService::Operation::Erase);
    QCOMPARE(eraseResult.outcome,
             ExactLogTransferService::Outcome::SubmittedUnconfirmed);
    QVERIFY(!eraseResult.succeeded());
}

void ExactLogTransferServiceTest::
routeChangeAtProgressPreservesExistingDestination()
{
    QString routeIdentity = QStringLiteral("udp:peer-a");
    Fixture fixture;
    fixture.service.setRouteIdentityProvider(
        [&routeIdentity](const SwarmVehicleInstanceLease &) {
            return routeIdentity;
        });
    QObject owner;
    QTemporaryDir directory;
    const QString destination =
        QDir(directory.path()).filePath(QStringLiteral("existing.bin"));
    QFile original(destination);
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write(QByteArrayLiteral("keep-old")), qint64(8));
    original.close();

    QObject::connect(
        &fixture.service, &ExactLogTransferService::progress,
        &owner, [&routeIdentity](ExactLogTransferToken, qulonglong,
                                qulonglong, bool) {
            routeIdentity = QStringLiteral("udp:peer-b");
        });
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.startDownload(
                 &owner, fixture.vehicle, 17, 3, destination, &token),
             ExactLogTransferService::StartResult::Started);
    fixture.observe(logData(17, 0, QByteArrayLiteral("new")));

    QCOMPARE(fixture.results.size(), 1);
    QCOMPARE(fixture.results.constFirst().outcome,
             ExactLogTransferService::Outcome::LeaseRetired);
    QFile preserved(destination);
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArrayLiteral("keep-old"));
}

void ExactLogTransferServiceTest::realShortEndPrunesDeferredFarPacket()
{
    Fixture fixture;
    QObject owner;
    QTemporaryDir directory;
    const QString destination =
        QDir(directory.path()).filePath(QStringLiteral("bounded.bin"));
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.startDownload(
                 &owner, fixture.vehicle, 19, 0, destination, &token),
             ExactLogTransferService::StartResult::Started);

    fixture.observe(logData(19, 10000, QByteArray(40, 'x')));
    QVERIFY(fixture.results.isEmpty());
    QVERIFY(!QFileInfo::exists(destination));
    fixture.observe(logData(19, 0, QByteArrayLiteral("eof")));

    QCOMPARE(fixture.results.size(), 1);
    const ExactLogTransferResult result = fixture.results.constFirst();
    QVERIFY(result.succeeded());
    QCOMPARE(result.covered, quint64(3));
    QCOMPARE(result.total, quint64(3));
    QFile file(destination);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("eof"));
}

void ExactLogTransferServiceTest::synchronousListReplyFinishesExactlyOnce()
{
    Fixture fixture;
    bool injectReply = true;
    fixture.writeHook = [&fixture, &injectReply]() {
        if (!injectReply || fixture.frames.isEmpty()
            || decodeFrame(fixture.frames.constLast().bytes).msgid
                != MAVLINK_MSG_ID_LOG_REQUEST_LIST) {
            return;
        }
        injectReply = false;
        fixture.observe(logEntry(0, 0, 0, 0, 0));
    };
    QObject owner;
    ExactLogTransferToken token;
    QCOMPARE(fixture.service.requestList(
                 &owner, fixture.vehicle, &token),
             ExactLogTransferService::StartResult::Started);

    QVERIFY(token.isValid());
    QCOMPARE(fixture.results.size(), 1);
    QCOMPARE(fixture.results.constFirst().token.id, token.id);
    QCOMPARE(fixture.results.constFirst().outcome,
             ExactLogTransferService::Outcome::Completed);
    QVERIFY(!fixture.service.busy());
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_LIST), 1);
    QCOMPARE(countFrames(fixture.frames, MAVLINK_MSG_ID_LOG_REQUEST_END), 1);
}

QTEST_MAIN(ExactLogTransferServiceTest)
#include "test_exactlogtransferservice.moc"
