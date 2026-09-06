#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "services/CameraProbeService.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSignalSpy>
#include <QVector>
#include <QtTest/QTest>

#include <cstring>
#include <functional>
#include <memory>

namespace {

constexpr int LinkId = 7;
constexpr quint64 LinkEpoch = 9;
constexpr quint8 LocalSystem = 250;
constexpr quint8 LocalComponent = 190;
constexpr quint8 CameraSystem = 42;
constexpr quint8 CameraComponent = MAV_COMP_ID_CAMERA;

struct CapturedFrame {
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId, int componentId,
                         const QString &componentName = QString())
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Camera test link %1").arg(linkId);
    result.componentName = componentName.isEmpty()
        ? QStringLiteral("Component %1").arg(componentId)
        : componentName;
    return result;
}

mavlink_message_t heartbeat(quint8 systemId, quint8 componentId,
                            quint8 type = MAV_TYPE_CAMERA,
                            quint8 autopilot = MAV_AUTOPILOT_INVALID)
{
    mavlink_heartbeat_t payload{};
    payload.type = type;
    payload.autopilot = autopilot;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(systemId, componentId, &message, &payload);
    return message;
}

mavlink_message_t commandAck(
    quint8 sourceSystem, quint8 sourceComponent, MAV_CMD command,
    MAV_RESULT result, quint8 targetSystem = LocalSystem,
    quint8 targetComponent = LocalComponent, quint8 progress = 255)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.progress = progress;
    payload.target_system = targetSystem;
    payload.target_component = targetComponent;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        sourceSystem, sourceComponent, &message, &payload);
    return message;
}

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

quint32 floatBits(float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float wire size");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

CameraProbeService::Report reportAt(const QSignalSpy &spy, int index)
{
    return qvariant_cast<CameraProbeService::Report>(spy.at(index).at(0));
}

class Fixture final
{
public:
    Fixture()
        : components(nullptr, [this]() { return componentClock; })
        , transmitter([this](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            if (writerHook) {
                writerHook();
            }
            return writerSucceeds;
        })
        , commands(&targets, &transmitter)
    {
        transmitter.setLinkSessionEpoch(LinkId, LinkEpoch);
        commands.setLocalIdentity(LocalSystem, LocalComponent);
        const bool configured = commands.configureComponentExactTransactions(
            [this](const MavlinkComponentInstanceLease &lease) {
                if (componentLeaseHook) {
                    componentLeaseHook();
                }
                return components.validateLease(lease);
            },
            [this](const MavlinkComponentInstanceLease &lease,
                   QString *error) {
                if (componentRouteHook) {
                    componentRouteHook();
                }
                if (!componentRouteAllowed
                    || !components.validateLease(lease)) {
                    if (error) {
                        *error = QStringLiteral(
                            "The component test route is unavailable.");
                    }
                    return false;
                }
                return true;
            });
        Q_ASSERT(configured);

        QObject::connect(
            &components, &MavlinkComponentRegistry::componentRetired,
            &commands, &VehicleCommandService::retireComponent);

        const VehicleEndpoint autopilot = endpoint(
            LinkId, 1, MAV_COMP_ID_AUTOPILOT1,
            QStringLiteral("Selected autopilot"));
        targets.observeEndpoint(autopilot, true);
        targets.observeHeartbeat(
            autopilot, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        components.beginLinkSession(LinkId, LinkEpoch);
        addCamera(CameraSystem);

        service.reset(new CameraProbeService(
            &targets, &components, &commands,
            [this](const MavlinkComponentInstanceLease &lease,
                   QString *error) {
                if (serviceRouteHook) {
                    serviceRouteHook();
                }
                if (!serviceRouteAllowed
                    || !components.validateLease(lease)) {
                    if (error) {
                        *error = QStringLiteral(
                            "The camera probe route is unavailable.");
                    }
                    return false;
                }
                return true;
            }));
        service->setTimeoutsForTesting(10, 60);
        commands.setExactQuarantineForTesting(1);
    }

    void addCamera(
        quint8 systemId, quint8 type = MAV_TYPE_CAMERA,
        quint8 autopilot = MAV_AUTOPILOT_INVALID)
    {
        components.observeMessage(
            LinkId, LinkEpoch,
            heartbeat(systemId, CameraComponent, type, autopilot));
    }

    void acknowledge(
        MAV_CMD command, MAV_RESULT result = MAV_RESULT_ACCEPTED,
        quint8 sourceSystem = CameraSystem,
        quint8 sourceComponent = CameraComponent,
        quint8 targetSystem = LocalSystem,
        quint8 targetComponent = LocalComponent,
        quint64 epoch = LinkEpoch)
    {
        commands.observeComponentMessage(
            LinkId, epoch,
            commandAck(sourceSystem, sourceComponent, command, result,
                       targetSystem, targetComponent));
    }

    mavlink_command_long_t commandPayload(int index) const
    {
        const mavlink_message_t message = decodeFrame(frames.at(index).bytes);
        mavlink_command_long_t payload{};
        if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
            mavlink_msg_command_long_decode(&message, &payload);
        }
        return payload;
    }

    bool prepare(CameraProbeService::Plan *plan, QString *error = nullptr)
    {
        return service->prepare(plan, error);
    }

    void acceptRemaining(int firstCommandIndex = 0)
    {
        const QList<MAV_CMD> expected = CameraProbeService::Commands();
        for (int index = firstCommandIndex; index < expected.size(); ++index) {
            QTRY_COMPARE_WITH_TIMEOUT(frames.size(), index + 1, 500);
            acknowledge(expected.at(index));
        }
    }

    qint64 componentClock = 0;
    VehicleTargetManager targets;
    MavlinkComponentRegistry components;
    QVector<CapturedFrame> frames;
    bool writerSucceeds = true;
    bool componentRouteAllowed = true;
    bool serviceRouteAllowed = true;
    std::function<void()> writerHook;
    std::function<void()> componentLeaseHook;
    std::function<void()> componentRouteHook;
    std::function<void()> serviceRouteHook;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    std::unique_ptr<CameraProbeService> service;
};

} // namespace

class CameraProbeServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void sixCommandsPreserveReferenceWireAndSelectedAutopilot();
    void rejectedAcknowledgementContinuesTheSequence();
    void timeoutRetriesFourTimesThenStopsRemainingCommands();
    void acknowledgementsRequireExactCameraEnvelope();
    void preparedIdentityRouteAndSelectionAreImmutable();
    void firstObservedCameraIsFrozenRatherThanLowestSystemId();
    void cancellationIsEarlyTokenedScopedAndStopsRetries();
    void sourceRetirementAndWriterCancellationAreTruthful();
    void terminalCleanupCannotReplaceTheRunBeforeItsReport();
};

void CameraProbeServiceTest::initTestCase()
{
    qRegisterMetaType<CameraProbeService::Report>();
    qRegisterMetaType<VehicleCommandService::ExactCommandReport>();
}

void CameraProbeServiceTest::
sixCommandsPreserveReferenceWireAndSelectedAutopilot()
{
    Fixture fixture;
    const VehicleTargetLease originalSelection =
        fixture.targets.acquireTarget();
    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QVERIFY(plan.isValid());
    QCOMPARE(plan.selection.generation, originalSelection.generation);
    QVERIFY(plan.selection.endpoint.sameIdentity(
        originalSelection.endpoint));
    QCOMPARE(plan.camera.endpoint.linkId, LinkId);
    QCOMPARE(plan.camera.endpoint.systemId, int(CameraSystem));
    QCOMPARE(plan.camera.endpoint.componentId, int(CameraComponent));
    const QString confirmation = CameraProbeService::ConfirmationText(plan);
    QVERIFY(confirmation.contains(QStringLiteral("42:100")));
    QVERIFY(confirmation.contains(QString::number(LinkId)));

    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    quint64 operationId = 0;
    QVERIFY2(fixture.service->execute(plan, &operationId, &error),
             qPrintable(error));
    QVERIFY(operationId != 0);

    const QList<MAV_CMD> expected{
        MAV_CMD_REQUEST_CAMERA_INFORMATION,
        MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION,
        MAV_CMD_REQUEST_CAMERA_SETTINGS,
        MAV_CMD_SET_CAMERA_MODE,
        MAV_CMD_REQUEST_STORAGE_INFORMATION,
        MAV_CMD_VIDEO_START_STREAMING
    };
    const QList<MAV_CMD> actualCommands = CameraProbeService::Commands();
    QCOMPARE(actualCommands.size(), expected.size());
    for (int index = 0; index < expected.size(); ++index) {
        QCOMPARE(static_cast<int>(actualCommands.at(index)),
                 static_cast<int>(expected.at(index)));
    }
    for (int index = 0; index < expected.size(); ++index) {
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), index + 1, 500);
        QCOMPARE(fixture.frames.at(index).linkId, LinkId);
        const mavlink_message_t message =
            decodeFrame(fixture.frames.at(index).bytes);
        QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
        QCOMPARE(message.sysid, LocalSystem);
        QCOMPARE(message.compid, LocalComponent);
        const mavlink_command_long_t payload =
            fixture.commandPayload(index);
        QCOMPARE(payload.target_system, CameraSystem);
        QCOMPARE(payload.target_component, CameraComponent);
        QCOMPARE(payload.command,
                 static_cast<quint16>(expected.at(index)));
        QCOMPARE(payload.confirmation, quint8(0));
        QCOMPARE(floatBits(payload.param1), quint32(0));
        QCOMPARE(floatBits(payload.param2), quint32(0));
        QCOMPARE(floatBits(payload.param3), quint32(0));
        QCOMPARE(floatBits(payload.param4), quint32(0));
        QCOMPARE(floatBits(payload.param5), quint32(0));
        QCOMPARE(floatBits(payload.param6), quint32(0));
        QCOMPARE(floatBits(payload.param7), quint32(0));
        if (index == 0) {
            // MAVLink 1 and older camera firmware leave the MAVLink 2 ACK
            // destination extensions at zero; that is still the same ACK.
            fixture.acknowledge(
                expected.at(index), MAV_RESULT_ACCEPTED,
                CameraSystem, CameraComponent, 0, 0);
        } else {
            fixture.acknowledge(expected.at(index));
        }
    }

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    const CameraProbeService::Report report = reportAt(finished, 0);
    QCOMPARE(report.operationId, operationId);
    QVERIFY(!report.cancelled);
    QCOMPARE(report.steps.size(), expected.size());
    for (int index = 0; index < report.steps.size(); ++index) {
        QCOMPARE(report.steps.at(index).command, expected.at(index));
        QCOMPARE(report.steps.at(index).outcome,
                 CameraProbeService::StepOutcome::Accepted);
        QCOMPARE(report.steps.at(index).attempts, 1);
        QCOMPARE(report.steps.at(index).mavResult,
                 int(MAV_RESULT_ACCEPTED));
    }
    const VehicleTargetLease finalSelection =
        fixture.targets.acquireTarget();
    QCOMPARE(finalSelection.generation, originalSelection.generation);
    QVERIFY(finalSelection.endpoint.sameIdentity(
        originalSelection.endpoint));
    QCOMPARE(finalSelection.endpoint.componentId,
             int(MAV_COMP_ID_AUTOPILOT1));
}

void CameraProbeServiceTest::
rejectedAcknowledgementContinuesTheSequence()
{
    Fixture fixture;
    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    QVERIFY(fixture.service->execute(plan, &error));
    const QList<MAV_CMD> commands = CameraProbeService::Commands();
    for (int index = 0; index < commands.size(); ++index) {
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), index + 1, 500);
        fixture.acknowledge(
            commands.at(index), index == 1
                ? MAV_RESULT_DENIED : MAV_RESULT_ACCEPTED);
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.steps.size(), commands.size());
    QCOMPARE(report.steps.at(1).outcome,
             CameraProbeService::StepOutcome::Rejected);
    QCOMPARE(report.steps.at(1).mavResult, int(MAV_RESULT_DENIED));
    QCOMPARE(report.steps.at(2).outcome,
             CameraProbeService::StepOutcome::Accepted);
    QCOMPARE(fixture.frames.size(), commands.size());
}

void CameraProbeServiceTest::
timeoutRetriesFourTimesThenStopsRemainingCommands()
{
    Fixture fixture;
    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    QVERIFY(fixture.service->execute(plan, &error));

    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 4, 500);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QCOMPARE(fixture.frames.size(), 4);
    const MAV_CMD first = CameraProbeService::Commands().constFirst();
    for (int attempt = 0; attempt < fixture.frames.size(); ++attempt) {
        const mavlink_command_long_t payload =
            fixture.commandPayload(attempt);
        QCOMPARE(payload.command, static_cast<quint16>(first));
        QCOMPARE(payload.confirmation, static_cast<quint8>(attempt));
    }
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.steps.size(), CameraProbeService::Commands().size());
    QCOMPARE(report.steps.constFirst().outcome,
             CameraProbeService::StepOutcome::OutcomeUncertain);
    QCOMPARE(report.steps.constFirst().attempts, 4);
    for (int index = 1; index < report.steps.size(); ++index) {
        QCOMPARE(report.steps.at(index).outcome,
                 CameraProbeService::StepOutcome::NotSent);
        QCOMPARE(report.steps.at(index).attempts, 0);
    }
}

void CameraProbeServiceTest::
acknowledgementsRequireExactCameraEnvelope()
{
    Fixture fixture;
    fixture.service->setTimeoutsForTesting(100, 500);
    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    QVERIFY(fixture.service->execute(plan, &error));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 1, 500);
    const MAV_CMD first = CameraProbeService::Commands().constFirst();

    fixture.acknowledge(first, MAV_RESULT_ACCEPTED,
                        CameraSystem, CameraComponent,
                        LocalSystem, LocalComponent, LinkEpoch + 1);
    fixture.acknowledge(first, MAV_RESULT_ACCEPTED,
                        CameraSystem + 1, CameraComponent);
    fixture.acknowledge(first, MAV_RESULT_ACCEPTED,
                        CameraSystem, MAV_COMP_ID_AUTOPILOT1);
    fixture.acknowledge(MAV_CMD_REQUEST_CAMERA_SETTINGS,
                        MAV_RESULT_ACCEPTED);
    fixture.acknowledge(first, MAV_RESULT_ACCEPTED,
                        CameraSystem, CameraComponent, 1, 1);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(finished.count(), 0);

    fixture.acknowledge(first);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 2, 500);
    fixture.acceptRemaining(1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QVERIFY(!reportAt(finished, 0).cancelled);
}

void CameraProbeServiceTest::
preparedIdentityRouteAndSelectionAreImmutable()
{
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));

        CameraProbeService::Plan changed = plan;
        ++changed.planId;
        QVERIFY(!fixture.service->validate(changed, &error));
        changed = plan;
        ++changed.camera.instanceEpoch;
        QVERIFY(!fixture.service->validate(changed, &error));
        changed = plan;
        ++changed.selection.generation;
        QVERIFY(!fixture.service->validate(changed, &error));
        quint64 operationId = 99;
        QVERIFY(!fixture.service->execute(changed, &operationId, &error));
        QCOMPARE(operationId, quint64(0));
        QCOMPARE(fixture.frames.size(), 0);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        fixture.serviceRouteAllowed = false;
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        quint64 operationId = 0;
        QVERIFY(!fixture.service->execute(plan, &operationId, &error));
        QVERIFY(operationId != 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 0);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        fixture.targets.clearTarget();
        QVERIFY(fixture.targets.selectTarget(
            LinkId, 1, MAV_COMP_ID_AUTOPILOT1));
        QVERIFY(!fixture.service->validate(plan, &error));
        QVERIFY(!fixture.service->execute(plan, &error));
        QCOMPARE(fixture.frames.size(), 0);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        fixture.components.endLinkSession(LinkId, LinkEpoch);
        fixture.components.beginLinkSession(LinkId, LinkEpoch + 1);
        fixture.transmitter.setLinkSessionEpoch(LinkId, LinkEpoch + 1);
        fixture.components.observeMessage(
            LinkId, LinkEpoch + 1,
            heartbeat(CameraSystem, CameraComponent));
        QVERIFY(!fixture.service->validate(plan, &error));
        QVERIFY(!fixture.service->execute(plan, &error));
        QCOMPARE(fixture.frames.size(), 0);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        QVERIFY(fixture.service->execute(plan, &error));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 1, 500);
        fixture.serviceRouteAllowed = false;
        fixture.acknowledge(CameraProbeService::Commands().constFirst());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 1);
        const auto report = reportAt(finished, 0);
        QCOMPARE(report.steps.constFirst().outcome,
                 CameraProbeService::StepOutcome::Accepted);
        QCOMPARE(report.steps.at(1).outcome,
                 CameraProbeService::StepOutcome::NotSent);
    }
}

void CameraProbeServiceTest::
firstObservedCameraIsFrozenRatherThanLowestSystemId()
{
    Fixture fixture;
    fixture.components.endLinkSession(LinkId, LinkEpoch);
    fixture.components.beginLinkSession(LinkId, LinkEpoch + 1);
    fixture.transmitter.setLinkSessionEpoch(LinkId, LinkEpoch + 1);
    fixture.components.observeMessage(
        LinkId, LinkEpoch + 1,
        heartbeat(99, CameraComponent));
    fixture.components.observeMessage(
        LinkId, LinkEpoch + 1,
        heartbeat(10, CameraComponent));

    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QCOMPARE(plan.camera.endpoint.systemId, 99);
    const QList<MavlinkComponentInstanceLease> sorted =
        fixture.components.components();
    QCOMPARE(sorted.constFirst().endpoint.systemId, 10);
    QVERIFY(CameraProbeService::ConfirmationText(plan).contains(
        QStringLiteral("99:100")));
}

void CameraProbeServiceTest::
cancellationIsEarlyTokenedScopedAndStopsRetries()
{
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        quint64 operationId = 0;
        QVERIFY(fixture.service->execute(plan, &operationId, &error));
        QVERIFY(operationId != 0);
        fixture.service->cancel(operationId);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 0);
        QVERIFY(reportAt(finished, 0).cancelled);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        quint64 operationId = 0;
        bool sawPublishedId = false;
        fixture.serviceRouteHook = [&]() {
            if (!sawPublishedId && operationId != 0) {
                sawPublishedId = fixture.service->busy()
                    && fixture.service->currentOperationId() == operationId;
                fixture.service->cancel(operationId);
            }
        };
        fixture.service->execute(plan, &operationId, &error);
        QVERIFY(operationId != 0);
        QVERIFY(sawPublishedId);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 0);
        QVERIFY(reportAt(finished, 0).cancelled);
    }
    {
        Fixture fixture;
        CameraProbeService::Plan firstPlan;
        QString error;
        QVERIFY2(fixture.prepare(&firstPlan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        quint64 firstId = 0;
        QVERIFY(fixture.service->execute(firstPlan, &firstId, &error));
        fixture.acceptRemaining();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);

        CameraProbeService::Plan secondPlan;
        QVERIFY2(fixture.prepare(&secondPlan, &error), qPrintable(error));
        quint64 secondId = 0;
        QVERIFY(fixture.service->execute(secondPlan, &secondId, &error));
        QVERIFY(secondId != firstId);
        fixture.service->cancel(firstId);
        QVERIFY(fixture.service->busy());
        for (int index = 0;
             index < CameraProbeService::Commands().size(); ++index) {
            QTRY_COMPARE_WITH_TIMEOUT(
                fixture.frames.size(),
                CameraProbeService::Commands().size() + index + 1, 500);
            fixture.acknowledge(CameraProbeService::Commands().at(index));
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 500);
        QVERIFY(!reportAt(finished, 1).cancelled);
    }
}

void CameraProbeServiceTest::
sourceRetirementAndWriterCancellationAreTruthful()
{
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(fixture.service.get(), &CameraProbeService::operationFinished);
        QVERIFY(fixture.service->execute(plan, &error));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 1, 500);
        fixture.targets.clearTarget();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 1);
        QVERIFY(reportAt(finished, 0).description.contains(QStringLiteral("vehicle or camera changed")));
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        QVERIFY(fixture.service->execute(plan, &error));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 1, 500);
        fixture.components.observeMessage(
            LinkId, LinkEpoch,
            heartbeat(CameraSystem, CameraComponent,
                      MAV_TYPE_GENERIC, MAV_AUTOPILOT_GENERIC));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 1);
        const auto report = reportAt(finished, 0);
        QCOMPARE(report.steps.constFirst().outcome,
                 CameraProbeService::StepOutcome::OutcomeUncertain);
        QVERIFY(!fixture.components.validateLease(plan.camera));
    }
    {
        Fixture fixture;
        CameraProbeService::Plan plan;
        QString error;
        QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
        QSignalSpy finished(
            fixture.service.get(), &CameraProbeService::operationFinished);
        bool cancelledInWriter = false;
        fixture.writerHook = [&]() {
            if (!cancelledInWriter) {
                cancelledInWriter = true;
                fixture.service->cancel(
                    fixture.service->currentOperationId());
            }
        };
        QVERIFY(fixture.service->execute(plan, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
        QCOMPARE(fixture.frames.size(), 1);
        const auto report = reportAt(finished, 0);
        QVERIFY(report.cancelled);
        QCOMPARE(report.steps.constFirst().outcome,
                 CameraProbeService::StepOutcome::OutcomeUncertain);
        QCOMPARE(report.steps.constFirst().attempts, 1);
    }
}

void CameraProbeServiceTest::
terminalCleanupCannotReplaceTheRunBeforeItsReport()
{
    Fixture fixture;
    CameraProbeService::Plan plan;
    QString error;
    QVERIFY2(fixture.prepare(&plan, &error), qPrintable(error));
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    bool terminalCleanupObserved = false;
    bool successorPrepared = false;
    bool successorStarted = false;
    bool armCallback = false;
    QObject::connect(
        &fixture.commands,
        &VehicleCommandService::exactReservationReleased,
        fixture.service.get(),
        [&](qulonglong) {
            if (!armCallback || terminalCleanupObserved) {
                return;
            }
            terminalCleanupObserved = true;
            CameraProbeService::Plan successor;
            QString successorError;
            successorPrepared = fixture.service->prepare(
                &successor, &successorError);
            if (successorPrepared) {
                successorStarted = fixture.service->execute(
                    successor, &successorError);
            }
        });

    QVERIFY(fixture.service->execute(plan, &error));
    const QList<MAV_CMD> commands = CameraProbeService::Commands();
    for (int index = 0; index < commands.size(); ++index) {
        QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), index + 1, 500);
        if (index == commands.size() - 1) {
            armCallback = true;
        }
        fixture.acknowledge(commands.at(index));
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QVERIFY(terminalCleanupObserved);
    QVERIFY(!successorPrepared);
    QVERIFY(!successorStarted);
    QVERIFY(!fixture.service->busy());
    QCOMPARE(fixture.frames.size(), commands.size());
}

QTEST_GUILESS_MAIN(CameraProbeServiceTest)

#include "test_cameraprobeservice.moc"
