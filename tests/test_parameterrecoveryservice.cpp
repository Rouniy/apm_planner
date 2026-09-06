#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "services/ParameterRecoveryService.h"

#include <QtTest>

#include <QCoreApplication>
#include <QEvent>
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
    const QVariant &value, ParameterType type)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, type, ParameterEncoding::Bytewise, &encoded);
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
              frames.append(bytes);
              const mavlink_message_t message = decodeFrame(bytes);
              if (onFrame) onFrame(message);
              if (autoRespond) respond(linkId, message);
              if (afterFrame) afterFrame(message);
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
            [](const SwarmVehicleInstanceLease &, QString *) {
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
            [](const SwarmVehicleInstanceLease &, QString *) {
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
                                   current.value, current.type));
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
            ParameterEncoding::Bytewise, &decoded);
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
            parameterValue(endpoint, name, value, type));
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
}

class ParameterRecoveryServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void enableFirstIdResetAndSourceOrderArePreserved();
    void missingAndIncompatibleParametersAreReportedAndContinue();
    void cancellationAfterWriteIsOutcomeUncertain();
    void queuedSuccessfulWriteReceiptSurvivesCancellation();
    void nestedForeignTerminalsCannotHideOwnReport();
    void armedAndSelectionAbaStopBeforeWrite();
    void deadlineAndInputBoundsFailClosed();
};

void ParameterRecoveryServiceTest::
enableFirstIdResetAndSourceOrderArePreserved()
{
    Fixture fixture;
    fixture.live.insert(QStringLiteral("COMPASS_DEV_ID"),
                        LiveParameter{quint32(101), ParameterType::UInt32});
    fixture.live.insert(QStringLiteral("RECOVERY_ENABLE"),
                        LiveParameter{0.0F, ParameterType::Real32});
    fixture.live.insert(QStringLiteral("UNCHANGED"),
                        LiveParameter{9.0F, ParameterType::Real32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory,
        "RECOVERY_GAIN,12.5\n"
        "COMPASS_DEV_ID,202\n"
        "RECOVERY_ENABLE,1\n"
        "UNCHANGED,9\n");
    QVERIFY(!path.isEmpty());

    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY(fixture.recovery.prepare(path, &plan, &error));
    QCOMPARE(plan.entries().size(), 4);
    QCOMPARE(plan.entries().at(0).name,
             QStringLiteral("RECOVERY_GAIN"));
    bool callerPlanReset = false;
    fixture.routeHook = [&fixture, &plan, &callerPlanReset]() {
        if (fixture.recovery.busy() && !callerPlanReset) {
            callerPlanReset = true;
            plan = {};
        }
    };
    quint64 operationId = 0;
    QSignalSpy finished(&fixture.recovery,
                        &ParameterRecoveryService::operationFinished);
    QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QVERIFY(operationId != 0);
    QVERIFY(callerPlanReset);
    QVERIFY(!plan.isValid());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);

    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.operationId, operationId);
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(report.setCount, 2);
    QCOMPARE(report.unchangedCount, 2);
    QCOMPARE(report.failedCount, 0);
    QCOMPARE(report.receipts.size(), 4);
    QCOMPARE(report.receipts.at(0).kind,
             ParameterRecoveryService::Receipt::Kind::EnableWrite);
    QCOMPARE(report.receipts.at(0).name,
             QStringLiteral("RECOVERY_ENABLE"));
    QCOMPARE(report.receipts.at(1).kind,
             ParameterRecoveryService::Receipt::Kind::ParameterWrite);
    QCOMPARE(report.receipts.at(1).name,
             QStringLiteral("RECOVERY_GAIN"));
    QCOMPARE(report.receipts.at(2).kind,
             ParameterRecoveryService::Receipt::Kind::IdentifierReset);
    QCOMPARE(report.receipts.at(2).name,
             QStringLiteral("COMPASS_DEV_ID"));
    QCOMPARE(report.receipts.at(3).kind,
             ParameterRecoveryService::Receipt::Kind::ParameterWrite);
    QCOMPARE(report.receipts.at(3).name,
             QStringLiteral("COMPASS_DEV_ID"));

    QCOMPARE(fixture.writes.size(), 4);
    QCOMPARE(fixture.writes.at(0).first,
             QStringLiteral("RECOVERY_ENABLE"));
    QCOMPARE(fixture.writes.at(1).first,
             QStringLiteral("RECOVERY_GAIN"));
    QCOMPARE(fixture.writes.at(2).first,
             QStringLiteral("COMPASS_DEV_ID"));
    QCOMPARE(fixture.writes.at(2).second.toUInt(), quint32(0));
    QCOMPARE(fixture.writes.at(3).second.toUInt(), quint32(202));
}

void ParameterRecoveryServiceTest::
missingAndIncompatibleParametersAreReportedAndContinue()
{
    Fixture fixture;
    fixture.live.insert(QStringLiteral("GOOD"),
                        LiveParameter{0.0F, ParameterType::Real32});
    fixture.live.insert(QStringLiteral("WIDE"),
                        LiveParameter{QVariant::fromValue<qulonglong>(7),
                                      ParameterType::UInt64});
    QTemporaryDir directory;
    const QString path = writeFile(&directory,
        "MISSING,1\nWIDE,8\nGOOD,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY(fixture.recovery.prepare(path, &plan, &error));
    quint64 operationId = 0;
    QSignalSpy finished(&fixture.recovery,
                        &ParameterRecoveryService::operationFinished);
    QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(report.setCount, 1);
    QCOMPARE(report.failedCount, 2);
    QVERIFY(report.failedParameters.contains(QStringLiteral("MISSING")));
    QVERIFY(report.failedParameters.contains(QStringLiteral("WIDE")));
    QCOMPARE(fixture.writes.size(), 1);
    QCOMPARE(fixture.writes.constFirst().first, QStringLiteral("GOOD"));
}

void ParameterRecoveryServiceTest::
cancellationAfterWriteIsOutcomeUncertain()
{
    Fixture fixture;
    fixture.autoRespond = false;
    fixture.live.insert(QStringLiteral("VALUE"),
                        LiveParameter{0.0F, ParameterType::Real32});
    // Answer reads, but deliberately leave the first PARAM_SET without echo.
    fixture.onFrame = [&fixture](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_PARAM_REQUEST_READ) return;
        fixture.respond(Fixture::linkId, message);
    };
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY(fixture.recovery.prepare(path, &plan, &error));
    quint64 operationId = 0;
    QSignalSpy finished(&fixture.recovery,
                        &ParameterRecoveryService::operationFinished);
    QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.recovery.busy()
                             && !fixture.frames.isEmpty()
                             && decodeFrame(fixture.frames.constLast()).msgid
                                == MAVLINK_MSG_ID_PARAM_SET, 500);
    QVERIFY(fixture.recovery.cancel(operationId));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::OutcomeUncertain);
    QVERIFY(fixture.recovery.lastReport().receipts.isEmpty());
}

void ParameterRecoveryServiceTest::
queuedSuccessfulWriteReceiptSurvivesCancellation()
{
    Fixture fixture;
    fixture.live.insert(QStringLiteral("VALUE"),
                        LiveParameter{0.0F, ParameterType::Real32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY(fixture.recovery.prepare(path, &plan, &error));
    quint64 operationId = 0;
    bool cancellationIssued = false;
    fixture.afterFrame = [&fixture, &operationId,
                          &cancellationIssued](
        const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_SET
            && !cancellationIssued) {
            cancellationIssued = fixture.recovery.cancel(operationId);
        }
    };
    QSignalSpy finished(&fixture.recovery,
                        &ParameterRecoveryService::operationFinished);
    QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QVERIFY(cancellationIssued);
    const auto report = fixture.recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Cancelled);
    QCOMPARE(report.setCount, 1);
    QCOMPARE(report.receipts.size(), 1);
    QCOMPARE(report.receipts.constFirst().kind,
             ParameterRecoveryService::Receipt::Kind::ParameterWrite);
    QCOMPARE(report.completedEntries, 1);
    QCOMPARE(report.remainingEntries, 0);
}

void ParameterRecoveryServiceTest::
nestedForeignTerminalsCannotHideOwnReport()
{
    Fixture fixture;
    fixture.live.insert(QStringLiteral("VALUE"),
                        LiveParameter{0.0F, ParameterType::Real32});
    QTemporaryDir directory;
    const QString path = writeFile(&directory, "VALUE,2\n");
    ParameterRecoveryService::Plan plan;
    QString error;
    QVERIFY(fixture.recovery.prepare(path, &plan, &error));

    bool injected = false;
    fixture.onFrame = [&fixture, &plan, &injected](
        const mavlink_message_t &message) {
        if (injected
            || message.msgid != MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
            return;
        }
        injected = true;
        for (quint64 index = 0; index < 6; ++index) {
            ParameterService::ExactOperationReport foreign;
            foreign.token.operationId = 10000 + index;
            foreign.token.reservationId = 20000 + index;
            foreign.token.lease = plan.vehicle();
            foreign.token.kind =
                ParameterService::ExactOperationKind::Read;
            foreign.token.name = QStringLiteral("VALUE");
            foreign.terminalResult =
                ParameterService::ExactTerminalResult::ReadSucceeded;
            foreign.value = 99.0F;
            foreign.type = ParameterType::Real32;
            fixture.parameters.exactOperationFinished(foreign);
        }
        // Deliver the foreign queued notifications while submitExactRead is
        // still inside its frame writer.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    };
    fixture.afterFrame = [](const mavlink_message_t &) {
        // The simulated vehicle response has now published the real terminal
        // report. Deliver it before submitExactRead returns as well.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    };

    quint64 operationId = 0;
    QSignalSpy finished(&fixture.recovery,
                        &ParameterRecoveryService::operationFinished);
    QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(injected, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(fixture.recovery.lastReport().operationId, operationId);
    QCOMPARE(fixture.recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(fixture.recovery.lastReport().setCount, 1);
    QCOMPARE(fixture.writes.size(), 1);
}

void ParameterRecoveryServiceTest::armedAndSelectionAbaStopBeforeWrite()
{
    {
        Fixture fixture;
        fixture.live.insert(QStringLiteral("VALUE"),
                            LiveParameter{0.0F, ParameterType::Real32});
        int busyValidations = 0;
        fixture.routeHook = [&fixture, &busyValidations]() {
            if (fixture.recovery.busy()
                && ++busyValidations == 4) {
                fixture.setArmed(true);
            }
        };
        QTemporaryDir directory;
        const QString path = writeFile(&directory, "VALUE,2\n");
        ParameterRecoveryService::Plan plan;
        QString error;
        QVERIFY(fixture.recovery.prepare(path, &plan, &error));
        quint64 operationId = 0;
        QSignalSpy finished(&fixture.recovery,
                            &ParameterRecoveryService::operationFinished);
        QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
                 ParameterRecoveryService::SubmitResult::Started);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QVERIFY(fixture.writes.isEmpty());
        QVERIFY(fixture.recovery.lastReport().outcome
                != ParameterRecoveryService::Outcome::Completed);
    }


    {
        Fixture fixture;
        fixture.live.insert(QStringLiteral("VALUE"),
                            LiveParameter{0.0F, ParameterType::Real32});
        QTemporaryDir directory;
        const QString path = writeFile(&directory, "VALUE,2\n");
        ParameterRecoveryService::Plan plan;
        QString error;
        QVERIFY(fixture.recovery.prepare(path, &plan, &error));
        quint64 operationId = 0;
        int busyValidations = 0;
        bool cancelledAtWriteGate = false;
        fixture.routeHook = [&fixture, &operationId,
                             &busyValidations,
                             &cancelledAtWriteGate]() {
            if (fixture.recovery.busy()
                && ++busyValidations == 7) {
                cancelledAtWriteGate =
                    fixture.recovery.cancel(operationId);
            }
        };
        QSignalSpy finished(&fixture.recovery,
                            &ParameterRecoveryService::operationFinished);
        QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
                 ParameterRecoveryService::SubmitResult::Started);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QVERIFY(cancelledAtWriteGate);
        QVERIFY(fixture.writes.isEmpty());
        QCOMPARE(fixture.recovery.lastReport().outcome,
                 ParameterRecoveryService::Outcome::Cancelled);
    }

    {
        Fixture fixture;
        QTemporaryDir directory;
        const QString path = writeFile(&directory, "VALUE,2\n");
        ParameterRecoveryService::Plan plan;
        QString error;
        QVERIFY(fixture.recovery.prepare(path, &plan, &error));
        VehicleEndpoint alternate;
        alternate.linkId = Fixture::linkId + 1;
        alternate.systemId = Fixture::systemId + 1;
        alternate.componentId = MAV_COMP_ID_AUTOPILOT1;
        QVERIFY(fixture.targets.observeEndpoint(alternate));
        QVERIFY(fixture.targets.selectTarget(
            alternate.linkId, alternate.systemId,
            alternate.componentId));
        QVERIFY(fixture.targets.selectTarget(
            fixture.endpoint.linkId, fixture.endpoint.systemId,
            fixture.endpoint.componentId));
        fixture.targets.observeHeartbeat(
            fixture.endpoint, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        quint64 operationId = 0;
        QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
                 ParameterRecoveryService::SubmitResult::Unavailable);
        QVERIFY(operationId != 0);
        QCOMPARE(fixture.recovery.lastReport().operationId, operationId);
        QCOMPARE(fixture.recovery.lastReport().outcome,
                 ParameterRecoveryService::Outcome::Rejected);
        QVERIFY(fixture.frames.isEmpty());
    }
}

void ParameterRecoveryServiceTest::deadlineAndInputBoundsFailClosed()
{
    {
        Fixture fixture;
        fixture.autoRespond = false;
        fixture.parameters.setExactRetryPolicyForTesting(
            100, 3, 5, 0, 40, 20);
        fixture.recovery.setOverallDeadlineForTesting(20);
        QTemporaryDir directory;
        const QString path = writeFile(&directory, "VALUE,2\n");
        ParameterRecoveryService::Plan plan;
        QString error;
        QVERIFY(fixture.recovery.prepare(path, &plan, &error));
        quint64 operationId = 0;
        QSignalSpy finished(&fixture.recovery,
                            &ParameterRecoveryService::operationFinished);
        QCOMPARE(fixture.recovery.execute(plan, &operationId, &error),
                 ParameterRecoveryService::SubmitResult::Started);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.recovery.lastReport().outcome,
                 ParameterRecoveryService::Outcome::Rejected);
        QVERIFY(fixture.writes.isEmpty());
    }

    Fixture fixture;
    QTemporaryDir directory;
    ParameterRecoveryService::Plan plan;
    QString error;
    const QString invalidName = writeFile(
        &directory, "PARAMETER_NAME_LONGER_THAN_16,1\n",
        QStringLiteral("invalid.param"));
    QVERIFY(!fixture.recovery.prepare(invalidName, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("16-byte")));
    QVERIFY(!plan.isValid());

    QByteArray oversized(
        static_cast<int>(ParameterRecoveryService::MaximumSourceBytes + 1),
        'x');
    const QString oversizedPath = writeFile(
        &directory, oversized, QStringLiteral("oversized.param"));
    QVERIFY(!fixture.recovery.prepare(oversizedPath, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("1 MiB")));
}

QTEST_GUILESS_MAIN(ParameterRecoveryServiceTest)

#include "test_parameterrecoveryservice.moc"
