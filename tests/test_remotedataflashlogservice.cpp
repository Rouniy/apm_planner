#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/RemoteDataFlashLogService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>

#include <cstring>
#include <functional>
#include <utility>

namespace
{
constexpr int LinkId = 87;
constexpr int SystemId = 63;
constexpr quint8 LocalSystemId = 252;
constexpr quint8 LocalComponentId = MAV_COMP_ID_MISSIONPLANNER;

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    return condition();
}

struct StatusFrame
{
    int linkId = -1;
    mavlink_message_t message{};
    mavlink_remote_log_block_status_t payload{};
};

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_message_t heartbeat(int systemId, bool armed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t remoteBlock(
    quint32 sequence, char fill, int systemId = SystemId,
    quint8 componentId = MAV_COMP_ID_LOG,
    quint8 targetSystem = LocalSystemId,
    quint8 targetComponent = LocalComponentId)
{
    quint8 data[MAVLINK_MSG_REMOTE_LOG_DATA_BLOCK_FIELD_DATA_LEN]{};
    std::memset(data, fill, sizeof(data));
    mavlink_message_t message{};
    mavlink_msg_remote_log_data_block_pack(
        static_cast<quint8>(systemId), componentId, &message,
        targetSystem, targetComponent, sequence, data);
    return message;
}

class Fixture final
{
public:
    Fixture()
        : registry([this] { return registryNowMs; })
        , transmitter([this](int linkId, const QByteArray &bytes) {
              const mavlink_message_t message = decodeFrame(bytes);
              if (message.msgid == MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS) {
                  StatusFrame captured;
                  captured.linkId = linkId;
                  captured.message = message;
                  mavlink_msg_remote_log_block_status_decode(
                      &message, &captured.payload);
                  frames.append(captured);
                  if (writeHook) writeHook(captured);
              }
              return writesSucceed;
          })
        , parameters(&targets, &transmitter)
        , service(&targets, &registry, &parameters, &transmitter,
                  LocalSystemId, LocalComponentId,
                  [this](const SwarmVehicleInstanceLease &lease,
                         QString *error) {
                      if (routeHook) routeHook();
                      if (!routeAllowed) {
                          if (error) *error = QStringLiteral("Unsafe route.");
                          return false;
                      }
                      return registry.validateLease(lease, 3000);
                  },
                  [this] { return serviceNowMs; },
                  40, 50, 500, 0)
    {
        linkEpoch = registry.beginLinkSession(
            LinkId, QStringLiteral("Remote log test link"));
        transmitter.setLinkSessionEpoch(LinkId, linkEpoch);
        setArmed(false);
        QObject::connect(
            &service, &RemoteDataFlashLogService::operationFinished,
            &service, [this](RemoteDataFlashLogService::Report report) {
                reports.append(std::move(report));
            });
    }

    void setArmed(bool armed)
    {
        QVERIFY(registry.observeMessage(
            LinkId, linkEpoch, heartbeat(SystemId, armed)));
        endpoint = registry.endpoints().constFirst();
        if (!targets.contains(endpoint.linkId, endpoint.systemId,
                              endpoint.componentId)) {
            QVERIFY(targets.observeEndpoint(endpoint, true));
        }
        targets.observeHeartbeat(endpoint, armed,
                                 MAV_AUTOPILOT_ARDUPILOTMEGA,
                                 MAV_TYPE_QUADROTOR);
    }

    void publishBackend(quint32 flags = 2,
                        ParameterType type = ParameterType::UInt32)
    {
        ParameterStore *store = parameters.store();
        store->beginLoad(endpoint);
        QVERIFY(store->ingest(
            endpoint, 1, 0, QStringLiteral("LOG_BACKEND_TYPE"),
            QVariant::fromValue(flags), type));
        QVERIFY(store->snapshot(endpoint).isComplete());
    }

    RemoteDataFlashLogService::Plan prepare(const QString &directory)
    {
        RemoteDataFlashLogService::Plan plan;
        QString error;
        if (!service.prepare(directory, &plan, &error)) {
            qWarning().noquote() << error;
        }
        return plan;
    }

    quint64 start(const RemoteDataFlashLogService::Plan &plan)
    {
        quint64 id = 0;
        QString error;
        const auto result = service.start(plan, &id, &error);
        if (result != RemoteDataFlashLogService::StartResult::Started) {
            qWarning().noquote() << error;
            return 0;
        }
        if (!waitFor([this] {
                return service.phase()
                    == RemoteDataFlashLogService::Phase::AwaitingSequenceZero;
            })) {
            qWarning() << "Remote writer did not reach AwaitingSequenceZero";
            return 0;
        }
        return id;
    }

    void observe(const mavlink_message_t &message,
                 int linkId = LinkId, quint64 epoch = 0)
    {
        service.observeMessage(
            linkId, epoch == 0 ? linkEpoch : epoch, message);
    }

    int countSequence(quint32 sequence) const
    {
        int count = 0;
        for (const StatusFrame &frame : frames) {
            if (frame.payload.seqno == sequence) ++count;
        }
        return count;
    }

    const StatusFrame *lastSequence(quint32 sequence) const
    {
        for (auto it = frames.crbegin(); it != frames.crend(); ++it) {
            if (it->payload.seqno == sequence) return &*it;
        }
        return nullptr;
    }

    qint64 registryNowMs = 100;
    qint64 serviceNowMs = 1000;
    bool routeAllowed = true;
    bool writesSucceed = true;
    std::function<void()> routeHook;
    std::function<void(const StatusFrame &)> writeHook;
    QVector<StatusFrame> frames;
    QVector<RemoteDataFlashLogService::Report> reports;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    RemoteDataFlashLogService service;
    quint64 linkEpoch = 0;
    VehicleEndpoint endpoint;
};
}

class RemoteDataFlashLogServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void prepareRequiresCompleteEnabledExactVehicle();
    void outOfOrderBlocksWaitForZeroAndPublishTruthfully();
    void explicitStopPublishesGapsOnlyAsPartial();
    void filtersPhysicalAndPayloadIdentity();
    void pendingRetransmissionsDoNotFillDiskQueue();
    void noSequenceZeroTimesOutWithoutStopAndPreservesRecovery();
    void silenceIsAdvisoryWhileHeartbeatIsFresh();
    void targetLossPreservesPartialWithoutSuccessorStop();
    void explicitCancelDiscardsAndAttemptsOneStop();
    void routeMutationRejectsBeforeStartTransmission();
    void startAttemptSurvivesReentrantCancellation();
    void delayedTimerCannotPermitLateAcknowledgement();
    void cancelValidatorMayDeleteService();
    void storedBlockTimeoutValidatorMayDeleteService();
};

void RemoteDataFlashLogServiceTest::prepareRequiresCompleteEnabledExactVehicle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Fixture missing;
    QString error;
    QVERIFY(!missing.service.canPrepare(&error));
    QVERIFY(error.contains(QStringLiteral("parameter"), Qt::CaseInsensitive));

    Fixture disabled;
    disabled.publishBackend(1);
    QVERIFY(!disabled.service.canPrepare(&error));
    QVERIFY(error.contains(QStringLiteral("bit (2)")));

    Fixture valid;
    valid.publishBackend(3);
    QVERIFY(valid.service.canPrepare(&error));
    const auto plan = valid.prepare(directory.path());
    QVERIFY(plan.isValid());
    QCOMPARE(plan.directoryPath(), QFileInfo(directory.path()).canonicalFilePath());
    QCOMPARE(plan.target().endpoint.linkId, LinkId);
    QCOMPARE(plan.vehicle().linkSessionEpoch, valid.linkEpoch);
    QCOMPARE(plan.localSystemId(), LocalSystemId);
    QCOMPARE(plan.localComponentId(), LocalComponentId);
    QVERIFY(plan.destinationDescription().contains(plan.fileStem()));

    valid.setArmed(true);
    QVERIFY(!valid.service.validate(plan, &error));
    valid.setArmed(false);
    const quint64 otherEpoch = valid.linkEpoch;
    QVERIFY(valid.registry.observeMessage(
        LinkId, otherEpoch, heartbeat(SystemId + 1, false)));
    QVERIFY(!valid.service.validate(plan, &error));
}

void RemoteDataFlashLogServiceTest::outOfOrderBlocksWaitForZeroAndPublishTruthfully()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    QVERIFY(id != 0);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_START), 1);
    const StatusFrame *start = fixture.lastSequence(
        MAV_REMOTE_LOG_DATA_BLOCK_START);
    QVERIFY(start);
    QCOMPARE(start->linkId, LinkId);
    QCOMPARE(start->message.sysid, LocalSystemId);
    QCOMPARE(start->message.compid, LocalComponentId);
    QCOMPARE(start->payload.target_system, quint8(SystemId));
    QCOMPARE(start->payload.target_component,
             quint8(MAV_COMP_ID_AUTOPILOT1));

    fixture.observe(remoteBlock(1, 'B'));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(1), 2000);
    QCOMPARE(fixture.countSequence(1), 0);

    fixture.observe(remoteBlock(0, 'A'));
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.phase(),
        RemoteDataFlashLogService::Phase::Receiving, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.countSequence(0), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.countSequence(1), 1, 2000);
    fixture.observe(remoteBlock(2, '\0'));
    fixture.observe(remoteBlock(1, 'B'));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.countSequence(2), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.countSequence(1), 2, 2000);

    fixture.setArmed(true); // Stopping an admitted capture is allowed in flight.
    QVERIFY(fixture.service.stopAndSave(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 3000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome,
             RemoteDataFlashLogService::Outcome::SavedUnverified);
    QVERIFY(report.published());
    QCOMPARE(report.blocks, qint64(3));
    QCOMPARE(report.duplicateBlocks, qint64(1));
    QCOMPARE(report.missingBlocks, qint64(0));
    QCOMPARE(report.bytes, qint64(600));
    QVERIFY(report.sequenceZeroObserved);
    QVERIFY(report.startAttempted);
    QVERIFY(report.stopAttempted);
    QVERIFY(report.stopSubmitted);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_STOP), 1);
    const StatusFrame *stop = fixture.lastSequence(
        MAV_REMOTE_LOG_DATA_BLOCK_STOP);
    QVERIFY(stop);
    QCOMPARE(stop->payload.target_component,
             quint8(MAV_COMP_ID_AUTOPILOT1));

    QFile output(report.destinationPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray bytes = output.readAll();
    QCOMPARE(bytes.size(), 600);
    QCOMPARE(bytes.left(200), QByteArray(200, 'A'));
    QCOMPARE(bytes.mid(200, 200), QByteArray(200, 'B'));
    QCOMPARE(bytes.right(200), QByteArray(200, '\0'));
}

void RemoteDataFlashLogServiceTest::explicitStopPublishesGapsOnlyAsPartial()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    fixture.observe(remoteBlock(0, 'A'));
    fixture.observe(remoteBlock(2, 'C'));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(2), 2000);
    QVERIFY(fixture.service.stopAndSave(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 3000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome,
             RemoteDataFlashLogService::Outcome::SavedUnverified);
    QCOMPARE(report.blocks, qint64(2));
    QCOMPARE(report.missingBlocks, qint64(1));
    QCOMPARE(report.missingRanges.size(), 1);
    QCOMPARE(report.missingRanges.first().first, quint32(1));
    QCOMPARE(report.missingRanges.first().last, quint32(1));
    QVERIFY(report.destinationPath.endsWith(QStringLiteral(".partial.bin")));
    QFile output(report.destinationPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray bytes = output.readAll();
    QCOMPARE(bytes.size(), 600);
    QCOMPARE(bytes.left(200), QByteArray(200, 'A'));
    QCOMPARE(bytes.mid(200, 200), QByteArray(200, '\0'));
    QCOMPARE(bytes.right(200), QByteArray(200, 'C'));
}

void RemoteDataFlashLogServiceTest::filtersPhysicalAndPayloadIdentity()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    QVERIFY(id != 0);
    fixture.observe(remoteBlock(0, 'a'), LinkId + 1);
    fixture.observe(remoteBlock(0, 'b'), LinkId, fixture.linkEpoch + 1);
    fixture.observe(remoteBlock(0, 'c', SystemId + 1));
    fixture.observe(remoteBlock(0, 'd', SystemId, MAV_COMP_ID_AUTOPILOT1));
    fixture.observe(remoteBlock(0, 'e', SystemId, MAV_COMP_ID_LOG,
                                LocalSystemId - 1));
    fixture.observe(remoteBlock(0, 'f', SystemId, MAV_COMP_ID_LOG,
                                LocalSystemId, LocalComponentId - 1));
    QTest::qWait(30);
    QCOMPARE(fixture.service.blocksStored(), qint64(0));
    QCOMPARE(fixture.countSequence(0), 0);
    QVERIFY(fixture.service.cancel(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
}

void RemoteDataFlashLogServiceTest::pendingRetransmissionsDoNotFillDiskQueue()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    QVERIFY(id != 0);
    const mavlink_message_t retransmission = remoteBlock(1, 'R');
    for (int repeat = 0; repeat < 400; ++repeat) {
        fixture.observe(retransmission);
    }
    fixture.observe(remoteBlock(0, 'A'));
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.phase(),
        RemoteDataFlashLogService::Phase::Receiving, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(2), 3000);
    QCOMPARE(fixture.countSequence(0), 1);
    QCOMPARE(fixture.countSequence(1), 1);
    QVERIFY(fixture.service.stopAndSave(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 3000);
    QCOMPARE(fixture.reports.constFirst().outcome,
             RemoteDataFlashLogService::Outcome::SavedUnverified);
}

void RemoteDataFlashLogServiceTest::noSequenceZeroTimesOutWithoutStopAndPreservesRecovery()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    QVERIFY(fixture.start(fixture.prepare(directory.path())) != 0);
    fixture.observe(remoteBlock(1, 'x'));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(1), 2000);
    fixture.serviceNowMs += 41;
    QVERIFY(QMetaObject::invokeMethod(
        &fixture.service, "checkTimeouts", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome,
             RemoteDataFlashLogService::Outcome::StartUnconfirmed);
    QVERIFY(!report.published());
    QVERIFY(!report.sequenceZeroObserved);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_STOP), 0);
    QCOMPARE(fixture.countSequence(1), 0);
    QVERIFY(!report.destinationPath.isEmpty());
    QVERIFY(report.destinationPath.endsWith(QStringLiteral(".part")));
    QVERIFY(QFileInfo::exists(report.destinationPath));
}

void RemoteDataFlashLogServiceTest::silenceIsAdvisoryWhileHeartbeatIsFresh()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    fixture.observe(remoteBlock(0, 'a'));
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.phase(),
        RemoteDataFlashLogService::Phase::Receiving, 2000);
    fixture.serviceNowMs += 60;
    QVERIFY(QMetaObject::invokeMethod(
        &fixture.service, "checkTimeouts", Qt::DirectConnection));
    QVERIFY(fixture.service.busy());
    QVERIFY(fixture.reports.isEmpty());
    QVERIFY(fixture.service.status().contains(
        QStringLiteral("remains active"), Qt::CaseInsensitive));
    QVERIFY(fixture.service.stopAndSave(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
}

void RemoteDataFlashLogServiceTest::targetLossPreservesPartialWithoutSuccessorStop()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    QVERIFY(fixture.start(fixture.prepare(directory.path())) != 0);
    fixture.observe(remoteBlock(0, 'a'));
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.phase(),
        RemoteDataFlashLogService::Phase::Receiving, 2000);
    fixture.targets.clearTarget();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome,
             RemoteDataFlashLogService::Outcome::LeaseRetired);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_STOP), 0);
    QVERIFY(!report.destinationPath.isEmpty());
    QVERIFY(QFileInfo::exists(report.destinationPath));
}

void RemoteDataFlashLogServiceTest::explicitCancelDiscardsAndAttemptsOneStop()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const quint64 id = fixture.start(fixture.prepare(directory.path()));
    fixture.observe(remoteBlock(0, 'a'));
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.phase(),
        RemoteDataFlashLogService::Phase::Receiving, 2000);
    QVERIFY(fixture.service.cancel(id));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome, RemoteDataFlashLogService::Outcome::Cancelled);
    QVERIFY(report.destinationPath.isEmpty());
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_STOP), 1);
    const QStringList files = QDir(directory.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(files.isEmpty());
}

void RemoteDataFlashLogServiceTest::routeMutationRejectsBeforeStartTransmission()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const auto plan = fixture.prepare(directory.path());
    QVERIFY(plan.isValid());
    bool mutated = false;
    fixture.routeHook = [&] {
        if (mutated) return;
        mutated = true;
        fixture.setArmed(true);
    };
    quint64 id = 0;
    QString error;
    QCOMPARE(fixture.service.start(plan, &id, &error),
             RemoteDataFlashLogService::StartResult::InvalidPlan);
    QVERIFY(id != 0); // Ownership is visible before the injected validator.
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_START), 0);
    QCOMPARE(fixture.reports.size(), 1);
    QCOMPARE(fixture.reports.constFirst().outcome,
             RemoteDataFlashLogService::Outcome::LeaseRetired);
}

void RemoteDataFlashLogServiceTest::startAttemptSurvivesReentrantCancellation()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    const auto plan = fixture.prepare(directory.path());
    quint64 id = 0;
    fixture.writeHook = [&](const StatusFrame &frame) {
        if (frame.payload.seqno == MAV_REMOTE_LOG_DATA_BLOCK_START) {
            QVERIFY(fixture.service.cancel(fixture.service.currentOperationId()));
        }
    };
    QCOMPARE(fixture.service.start(plan, &id),
             RemoteDataFlashLogService::StartResult::Started);
    QVERIFY(id != 0);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
    const auto report = fixture.reports.constFirst();
    QCOMPARE(report.outcome, RemoteDataFlashLogService::Outcome::Cancelled);
    QVERIFY(report.startAttempted);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_START), 1);
    QCOMPARE(fixture.countSequence(MAV_REMOTE_LOG_DATA_BLOCK_STOP), 0);
}

void RemoteDataFlashLogServiceTest::delayedTimerCannotPermitLateAcknowledgement()
{
    QTemporaryDir directory;
    Fixture fixture;
    fixture.publishBackend();
    QVERIFY(fixture.start(fixture.prepare(directory.path())) != 0);
    fixture.serviceNowMs += 600; // Maximum lifetime elapsed; timer not pumped.
    fixture.observe(remoteBlock(0, 'z'));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.reports.size(), 1, 2000);
    QCOMPARE(fixture.reports.constFirst().outcome,
             RemoteDataFlashLogService::Outcome::TimedOut);
    QCOMPARE(fixture.countSequence(0), 0);
}

void RemoteDataFlashLogServiceTest::cancelValidatorMayDeleteService()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService parameters(&targets, &transmitter);
    const quint64 epoch = registry.beginLinkSession(
        LinkId, QStringLiteral("Delete callback link"));
    transmitter.setLinkSessionEpoch(LinkId, epoch);
    QVERIFY(registry.observeMessage(
        LinkId, epoch, heartbeat(SystemId, false)));
    const VehicleEndpoint endpoint = registry.endpoints().constFirst();
    QVERIFY(targets.observeEndpoint(endpoint, true));
    targets.observeHeartbeat(endpoint, false,
                             MAV_AUTOPILOT_ARDUPILOTMEGA,
                             MAV_TYPE_QUADROTOR);
    parameters.store()->beginLoad(endpoint);
    QVERIFY(parameters.store()->ingest(
        endpoint, 1, 0, QStringLiteral("LOG_BACKEND_TYPE"),
        quint32(2), ParameterType::UInt32));

    QPointer<RemoteDataFlashLogService> service;
    bool deleteOnRoute = false;
    service = new RemoteDataFlashLogService(
        &targets, &registry, &parameters, &transmitter,
        LocalSystemId, LocalComponentId,
        [&](const SwarmVehicleInstanceLease &, QString *) {
            if (deleteOnRoute && service) {
                deleteOnRoute = false;
                delete service.data();
                return false;
            }
            return true;
        });
    RemoteDataFlashLogService::Plan plan;
    QString error;
    QVERIFY2(service->prepare(directory.path(), &plan, &error),
             qPrintable(error));
    quint64 operationId = 0;
    QCOMPARE(service->start(plan, &operationId, &error),
             RemoteDataFlashLogService::StartResult::Started);
    QVERIFY(waitFor([&] {
        return service
            && service->phase()
                == RemoteDataFlashLogService::Phase::AwaitingSequenceZero;
    }));
    service->observeMessage(
        LinkId, epoch, remoteBlock(0, 'D'));
    QVERIFY(waitFor([&] {
        return service
            && service->phase()
                == RemoteDataFlashLogService::Phase::Receiving;
    }));

    deleteOnRoute = true;
    RemoteDataFlashLogService *const raw = service.data();
    raw->cancel(operationId, &error);
    QVERIFY(service.isNull());
}

void RemoteDataFlashLogServiceTest::storedBlockTimeoutValidatorMayDeleteService()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService parameters(&targets, &transmitter);
    const quint64 epoch = registry.beginLinkSession(
        LinkId, QStringLiteral("Stored-block callback link"));
    transmitter.setLinkSessionEpoch(LinkId, epoch);
    QVERIFY(registry.observeMessage(
        LinkId, epoch, heartbeat(SystemId, false)));
    const VehicleEndpoint endpoint = registry.endpoints().constFirst();
    QVERIFY(targets.observeEndpoint(endpoint, true));
    targets.observeHeartbeat(endpoint, false,
                             MAV_AUTOPILOT_ARDUPILOTMEGA,
                             MAV_TYPE_QUADROTOR);
    parameters.store()->beginLoad(endpoint);
    QVERIFY(parameters.store()->ingest(
        endpoint, 1, 0, QStringLiteral("LOG_BACKEND_TYPE"),
        quint32(2), ParameterType::UInt32));

    QPointer<RemoteDataFlashLogService> service;
    int routeCallsUntilDelete = -1;
    service = new RemoteDataFlashLogService(
        &targets, &registry, &parameters, &transmitter,
        LocalSystemId, LocalComponentId,
        [&](const SwarmVehicleInstanceLease &, QString *) {
            if (routeCallsUntilDelete > 0
                && --routeCallsUntilDelete == 0 && service) {
                delete service.data();
                return false;
            }
            return true;
        });
    RemoteDataFlashLogService::Plan plan;
    QString error;
    QVERIFY2(service->prepare(directory.path(), &plan, &error),
             qPrintable(error));
    quint64 operationId = 0;
    QCOMPARE(service->start(plan, &operationId, &error),
             RemoteDataFlashLogService::StartResult::Started);
    QVERIFY(waitFor([&] {
        return service
            && service->phase()
                == RemoteDataFlashLogService::Phase::AwaitingSequenceZero;
    }));

    // observeMessage validates once before queueing the write. The following
    // validation is checkTimeouts() inside handleBlockStored().
    routeCallsUntilDelete = 2;
    service->observeMessage(LinkId, epoch, remoteBlock(0, 'T'));
    QVERIFY(waitFor([&] { return service.isNull(); }));
}

QTEST_GUILESS_MAIN(RemoteDataFlashLogServiceTest)
#include "test_remotedataflashlogservice.moc"
