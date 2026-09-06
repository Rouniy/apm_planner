#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "services/ParameterRecoveryService.h"

#include <QtTest>

#include <QFile>
#include <QHash>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>
#include <functional>

namespace
{
struct LiveParameter
{
    QVariant value;
    ParameterType type = ParameterType::Unknown;
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

QString parameterName(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') ++length;
    return QString::fromLatin1(id, length);
}

void copyParameterName(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    std::memset(id, 0, 16);
    std::memcpy(id, bytes.constData(),
                static_cast<size_t>(qMin(16, bytes.size())));
}

mavlink_message_t heartbeat(int systemId, bool armed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_STANDBY);
    return message;
}

mavlink_message_t parameterValue(
    const VehicleEndpoint &source, const QString &name,
    const QVariant &value, ParameterType type,
    ParameterEncoding encoding = ParameterEncoding::Bytewise)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, type, encoding, &encoded);
    if (!encoded) {
        payload.param_value = value.toFloat();
    }
    copyParameterName(name, payload.param_id);
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = 1;
    payload.param_index = 0;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId),
        &message, &payload);
    return message;
}

QString writeFile(QTemporaryDir *directory, const QByteArray &contents,
                  const QString &name = QStringLiteral("recovery.param"))
{
    if (!directory || !directory->isValid()) return {};
    const QString path = directory->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size()) {
        return {};
    }
    file.close();
    return path;
}

class Fixture
{
public:
    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              inWriter = true;
              frames.append(bytes);
              const mavlink_message_t message = decodeFrame(bytes);
              if (onFrame) onFrame(message);
              if (autoRespond) respond(linkId, message);
              if (afterFrame) afterFrame(message);
              inWriter = false;
              return true;
          })
        , parameters(&targets, &transmitter)
        , commands(&targets, &transmitter)
        , recovery(&targets, &registry, &parameters, &commands,
                   [this](const SwarmVehicleInstanceLease &lease,
                          QString *error) {
                       if (routeHook) routeHook();
                       if (!routeAllowed) {
                           if (error) *error = QStringLiteral("Route disabled.");
                           return false;
                       }
                       return registry.validateLease(lease, 3000);
                   })
    {
        linkEpoch = registry.beginLinkSession(linkId,
                                              QStringLiteral("Recovery link"));
        setArmed(false);
        QVERIFY(parameters.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &lease) {
                return registry.validateLease(lease, 3000);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QVERIFY(parameters.configureSingleVehicleExactRoute(
            [this](const SwarmVehicleInstanceLease &, QString *) {
                if (parameterRouteHook) parameterRouteHook();
                return true;
            }));
        QVERIFY(commands.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &lease) {
                return registry.validateLease(lease, 3000);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QVERIFY(commands.configureSingleVehicleExactRoute(
            [this](const SwarmVehicleInstanceLease &, QString *) {
                if (commandRouteHook) commandRouteHook();
                return true;
            }));
        parameters.setExactRetryPolicyForTesting(5, 0, 5, 0, 40, 20);
    }

    void setArmed(bool armed)
    {
        QVERIFY(registry.observeMessage(
            linkId, linkEpoch, heartbeat(systemId, armed)));
        endpoint = registry.endpoints().constFirst();
        if (!targets.contains(endpoint.linkId, endpoint.systemId,
                              endpoint.componentId)) {
            QVERIFY(targets.observeEndpoint(endpoint, true));
        }
        targets.observeHeartbeat(endpoint, armed,
                                 MAV_AUTOPILOT_ARDUPILOTMEGA,
                                 MAV_TYPE_QUADROTOR);
    }

    void respond(int frameLinkId, const mavlink_message_t &message)
    {
        if (frameLinkId != linkId) return;
        if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
            mavlink_param_request_read_t request{};
            mavlink_msg_param_request_read_decode(&message, &request);
            const QString name = parameterName(request.param_id);
            if (live.contains(name)) {
                const LiveParameter current = live.value(name);
                parameters.observePhysicalMessage(
                    linkId, linkEpoch,
                    parameterValue(endpoint, name,
                                   current.value, current.type, encoding));
            }
            return;
        }
        if (message.msgid != MAVLINK_MSG_ID_PARAM_SET) return;
        mavlink_param_set_t request{};
        mavlink_msg_param_set_decode(&message, &request);
        const QString name = parameterName(request.param_id);
        const ParameterType type =
            static_cast<ParameterType>(request.param_type);
        bool decoded = false;
        const QVariant value = ParameterCodec::decodeClassic(
            request.param_value, type,
            encoding, &decoded);
        if (!decoded) return;
        writes.append(qMakePair(name, value));
        live.insert(name, LiveParameter{value, type});
        if (name == QStringLiteral("RECOVERY_ENABLE")
            && value.toInt() == 1 && exposeAfterEnable) {
            live.insert(QStringLiteral("RECOVERY_GAIN"),
                        LiveParameter{0.0F, ParameterType::Real32});
        }
        parameters.observePhysicalMessage(
            linkId, linkEpoch,
            parameterValue(endpoint, name, value, type, encoding));
    }

    static constexpr int linkId = 31;
    static constexpr int systemId = 61;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QVector<QByteArray> frames;
    QVector<QPair<QString, QVariant>> writes;
    QHash<QString, LiveParameter> live;
    std::function<void(const mavlink_message_t &)> onFrame;
    std::function<void(const mavlink_message_t &)> afterFrame;
    std::function<void()> routeHook;
    std::function<void()> commandRouteHook;
    std::function<void()> parameterRouteHook;
    bool inWriter = false;
    ParameterEncoding encoding = ParameterEncoding::Bytewise;
    bool routeAllowed = true;
    bool autoRespond = true;
    bool exposeAfterEnable = true;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    VehicleCommandService commands;
    ParameterRecoveryService recovery;
    quint64 linkEpoch = 0;
    VehicleEndpoint endpoint;
};

int frameCount(const Fixture &fixture, quint32 messageId)
{
    int count = 0;
    for (const auto &frame : fixture.frames)
        if (decodeFrame(frame).msgid == messageId) ++count;
    return count;
}
}

class ParameterRecoveryAdmissionTest final : public QObject
{
    Q_OBJECT
private slots:
    void callerPlanResetCannotChangeAdmittedSnapshot();
    void firstValidationSeesCancellableOwnedId();
    void interruptedReservationsReleaseBothLanes_data();
    void interruptedReservationsReleaseBothLanes();
    void finalWriteGateCancellationSendsNoWrite();
    void acknowledgedIdentifierResetSurvivesImmediateCancellation();
    void cancellationInsideWriterIsUncertain();
    void elapsedDeadlineStopsBeforeTimerDispatch();
    void identifierValueMustFitCurrentEncodingBeforeReset_data();
    void identifierValueMustFitCurrentEncodingBeforeReset();
};

void ParameterRecoveryAdmissionTest::callerPlanResetCannotChangeAdmittedSnapshot()
{
    Fixture fixture;
    fixture.live.insert("VALUE", {0.0F, ParameterType::Real32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    bool reset = false;
    fixture.routeHook = [&]() {
        if (!reset) { reset = true; plan = {}; }
    };
    quint64 id = 0;
    QCOMPARE(fixture.recovery.execute(plan, &id, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QVERIFY(reset);
    QVERIFY(!plan.isValid());
    QVERIFY(id != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 1000);
    QCOMPARE(fixture.recovery.lastReport().operationId, id);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(fixture.recovery.lastReport().setCount, 1);
    QCOMPARE(frameCount(fixture, MAVLINK_MSG_ID_PARAM_SET), 1);
    QCOMPARE(fixture.live.value("VALUE").value.toFloat(), 2.0F);
}

void ParameterRecoveryAdmissionTest::firstValidationSeesCancellableOwnedId()
{
    Fixture fixture;
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    quint64 id = 0;
    bool called = false;
    fixture.routeHook = [&]() {
        if (called) return;
        called = true;
        QVERIFY(fixture.recovery.busy());
        QVERIFY(id != 0);
        QCOMPARE(fixture.recovery.currentOperationId(), id);
        QVERIFY(fixture.recovery.cancel(id));
    };
    fixture.recovery.execute(plan, &id, &error);
    QVERIFY(called);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 500);
    QVERIFY(fixture.frames.isEmpty());
    QCOMPARE(fixture.recovery.lastReport().operationId, id);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Cancelled);
    QCOMPARE(fixture.recovery.lastReport().completedEntries, 0);
    QCOMPARE(fixture.recovery.lastReport().remainingEntries, 1);
}

void ParameterRecoveryAdmissionTest::interruptedReservationsReleaseBothLanes_data()
{
    QTest::addColumn<bool>("parameterLane");
    QTest::addColumn<bool>("shutdown");
    QTest::newRow("command-cancel") << false << false;
    QTest::newRow("command-shutdown") << false << true;
    QTest::newRow("parameter-cancel") << true << false;
    QTest::newRow("parameter-shutdown") << true << true;
}

void ParameterRecoveryAdmissionTest::interruptedReservationsReleaseBothLanes()
{
    QFETCH(bool, parameterLane);
    QFETCH(bool, shutdown);
    Fixture fixture;
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    const auto lease = plan.vehicle();
    quint64 id = 0;
    bool interrupted = false;
    const auto stop = [&]() {
        if (interrupted) return;
        interrupted = true;
        QVERIFY(id != 0);
        QVERIFY(fixture.recovery.busy());
        if (shutdown) fixture.recovery.shutdown();
        else QVERIFY(fixture.recovery.cancel(id));
    };
    if (parameterLane) fixture.parameterRouteHook = stop;
    else fixture.commandRouteHook = stop;
    fixture.recovery.execute(plan, &id, &error);
    QVERIFY(interrupted);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 500);
    QVERIFY(fixture.frames.isEmpty());
    fixture.parameterRouteHook = {};
    fixture.commandRouteHook = {};

    // Probe with another owner: neither lane may remain reserved by the
    // interrupted admission, including a reservation returned after shutdown.
    QObject owner;
    VehicleCommandService::ExactReservationToken commandReservation;
    QCOMPARE(fixture.commands.reserveSingleVehicleEndpoint(
                 &owner, fixture.targets.acquireTarget(), lease,
                 &commandReservation, &error),
             VehicleCommandService::ExactReservationResult::Reserved);
    ParameterService::ExactReservationToken parameterReservation;
    QCOMPARE(fixture.parameters.reserveSingleVehicleEndpoint(
                 &owner, fixture.targets.acquireTarget(), lease,
                 &parameterReservation, &error),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(fixture.parameters.releaseExactReservation(parameterReservation));
    QVERIFY(fixture.commands.releaseExactReservation(commandReservation));
}

void ParameterRecoveryAdmissionTest::finalWriteGateCancellationSendsNoWrite()
{
    Fixture fixture;
    fixture.live.insert("VALUE", {0.0F, ParameterType::Real32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    quint64 id = 0;
    bool writeAdmissionReached = false;
    bool cancelled = false;
    // After prefetch + main reads, the next parameter route admission outside
    // a response callback belongs to the first write. The recovery safety
    // callback that follows it is the final validateBeforeWrite boundary.
    fixture.parameterRouteHook = [&]() {
        if (!fixture.inWriter
            && frameCount(fixture, MAVLINK_MSG_ID_PARAM_REQUEST_READ) == 2)
            writeAdmissionReached = true;
    };
    fixture.routeHook = [&]() {
        if (writeAdmissionReached && !cancelled)
            cancelled = fixture.recovery.cancel(id);
    };
    QCOMPARE(fixture.recovery.execute(plan, &id, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 1000);
    QVERIFY(writeAdmissionReached);
    QVERIFY(cancelled);
    QCOMPARE(frameCount(fixture, MAVLINK_MSG_ID_PARAM_SET), 0);
    QCOMPARE(fixture.live.value("VALUE").value.toFloat(), 0.0F);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Cancelled);
    QVERIFY(fixture.recovery.lastReport().receipts.isEmpty());
}

void ParameterRecoveryAdmissionTest::acknowledgedIdentifierResetSurvivesImmediateCancellation()
{
    Fixture fixture;
    fixture.live.insert("DEVICE_ID", {quint32(101), ParameterType::UInt32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "DEVICE_ID,202\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    quint64 id = 0;
    bool cancelled = false;
    fixture.afterFrame = [&](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_PARAM_SET || cancelled) return;
        // respond() has already emitted successful exact reset evidence, but
        // its deferred recovery handler has not run while this writer is active.
        QCOMPARE(fixture.live.value("DEVICE_ID").value.toUInt(), quint32(0));
        cancelled = fixture.recovery.cancel(id);
    };
    QCOMPARE(fixture.recovery.execute(plan, &id, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 1000);
    QVERIFY(cancelled);
    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Cancelled);
    QCOMPARE(report.setCount, 0);
    QCOMPARE(report.completedEntries, 0);
    QCOMPARE(report.remainingEntries, 1);
    QCOMPARE(report.receipts.size(), 1);
    QCOMPARE(report.receipts.constFirst().kind,
             ParameterRecoveryService::Receipt::Kind::IdentifierReset);
    QCOMPARE(report.receipts.constFirst().name, QStringLiteral("DEVICE_ID"));
    QCOMPARE(report.receipts.constFirst().value.toUInt(), quint32(0));
    QCOMPARE(frameCount(fixture, MAVLINK_MSG_ID_PARAM_SET), 1);
    QCOMPARE(fixture.live.value("DEVICE_ID").value.toUInt(), quint32(0));
}

void ParameterRecoveryAdmissionTest::cancellationInsideWriterIsUncertain()
{
    Fixture fixture;
    fixture.live.insert("VALUE", {0.0F, ParameterType::Real32});
    fixture.autoRespond = false;
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    quint64 id = 0;
    bool cancelled = false;
    fixture.onFrame = [&](const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ)
            fixture.respond(Fixture::linkId, message);
        else if (message.msgid == MAVLINK_MSG_ID_PARAM_SET && !cancelled)
            cancelled = fixture.recovery.cancel(id);
    };
    QCOMPARE(fixture.recovery.execute(plan, &id, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 1000);
    QVERIFY(cancelled);
    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::OutcomeUncertain);
    QVERIFY(report.receipts.isEmpty());
    QCOMPARE(report.setCount, 0);
    QCOMPARE(report.completedEntries, 0);
    QCOMPARE(report.remainingEntries, 1);
    QCOMPARE(frameCount(fixture, MAVLINK_MSG_ID_PARAM_SET), 1);
}

void ParameterRecoveryAdmissionTest::elapsedDeadlineStopsBeforeTimerDispatch()
{
    Fixture fixture;
    fixture.recovery.setOverallDeadlineForTesting(10);
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    bool elapsed = false;
    fixture.routeHook = [&]() {
        if (elapsed) return;
        elapsed = true;
        QVERIFY(fixture.recovery.busy());
        QTest::qSleep(30); // Deliberately no event processing / timer delivery.
    };
    quint64 id = 0;
    fixture.recovery.execute(plan, &id, &error);
    QVERIFY(elapsed);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 500);
    QVERIFY(fixture.frames.isEmpty());
    QCOMPARE(fixture.recovery.lastReport().operationId, id);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Rejected);
    QCOMPARE(fixture.recovery.lastReport().completedEntries, 0);
    QCOMPARE(fixture.recovery.lastReport().remainingEntries, 1);
}

void ParameterRecoveryAdmissionTest::identifierValueMustFitCurrentEncodingBeforeReset_data()
{
    QTest::addColumn<bool>("bytewise");
    QTest::newRow("C-style-unrepresentable") << false;
    QTest::newRow("bytewise-exact") << true;
}

void ParameterRecoveryAdmissionTest::identifierValueMustFitCurrentEncodingBeforeReset()
{
    QFETCH(bool, bytewise);
    Fixture fixture;
    fixture.encoding = bytewise ? ParameterEncoding::Bytewise
                               : ParameterEncoding::CStyleCast;
    fixture.parameters.setEncoding(fixture.endpoint, fixture.encoding);
    fixture.live.insert("DEVICE_ID", {quint32(42), ParameterType::UInt32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "DEVICE_ID,16777217\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY2(fixture.recovery.prepare(path, &plan, &error), qPrintable(error));
    quint64 id = 0;
    QCOMPARE(fixture.recovery.execute(plan, &id, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.recovery.busy(), 1000);
    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(report.setCount, bytewise ? 1 : 0);
    QCOMPARE(report.failedCount, bytewise ? 0 : 1);
    QCOMPARE(frameCount(fixture, MAVLINK_MSG_ID_PARAM_SET), bytewise ? 2 : 0);
    QCOMPARE(report.receipts.size(), bytewise ? 2 : 0);
    QCOMPARE(fixture.live.value("DEVICE_ID").value.toUInt(),
             bytewise ? quint32(16777217) : quint32(42));
    if (bytewise) {
        QCOMPARE(fixture.writes.at(0).second.toUInt(), quint32(0));
        QCOMPARE(fixture.writes.at(1).second.toUInt(), quint32(16777217));
    } else {
        QCOMPARE(report.failedParameters, QStringList({"DEVICE_ID"}));
    }
}

QTEST_GUILESS_MAIN(ParameterRecoveryAdmissionTest)
#include "test_parameterrecovery_admission.moc"
