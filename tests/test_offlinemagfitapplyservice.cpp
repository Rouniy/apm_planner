#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"
#include "services/OfflineMagFitApplyService.h"

#include <QtTest>

#include <QHash>

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
    int count, int index)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, type, ParameterEncoding::Bytewise, &encoded);
    if (!encoded) payload.param_value = value.toFloat();
    copyParameterName(name, payload.param_id);
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = static_cast<quint16>(count);
    payload.param_index = static_cast<quint16>(index);
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId),
        &message, &payload);
    return message;
}

OfflineMagFitResult fit(int compass, bool ellipsoid)
{
    OfflineMagFitResult result;
    result.compass = compass;
    result.sourceSamples = 320;
    result.usedSamples = 280;
    result.coverageOctants = 8;
    result.loggedOffsets = MagVector{10.0, 20.0, 30.0};
    result.sphereOffsets = MagVector{11.0, 22.0, 33.0};
    result.offsets = MagVector{
        100.0 + compass, -200.0 - compass, 50.0 + compass};
    result.diagonals = MagVector{1.1, 0.9, 1.05};
    result.offDiagonals = MagVector{0.02, -0.03, 0.04};
    result.sphereRadius = 450.0;
    result.sphereRmsError = 3.5;
    result.rmsError = 2.5;
    result.hasEllipsoid = ellipsoid;
    return result;
}

OfflineMagFitReport reportFor(
    const QVector<OfflineMagFitResult> &results)
{
    OfflineMagFitReport report;
    report.success = true;
    report.sourcePath = QStringLiteral("/tmp/offline-magfit.bin");
    report.throttleThreshold = 30;
    report.useEllipsoid = std::any_of(
        results.cbegin(), results.cend(),
        [](const OfflineMagFitResult &result) {
            return result.hasEllipsoid;
        });
    report.results = results;
    report.applyEligible = true;
    for (const OfflineMagFitResult &result : results) {
        report.loggedDeviceIds.insert(
            result.compass, quint32(1000 + result.compass));
        const QString suffix = result.compass == 1
            ? QString() : QString::number(result.compass);
        report.loggedFrameParameters.insert(
            QStringLiteral("COMPASS_ORIENT%1").arg(suffix), 0.0);
        report.loggedFrameParameters.insert(
            result.compass == 1
                ? QStringLiteral("COMPASS_EXTERNAL")
                : QStringLiteral("COMPASS_EXTERN%1").arg(result.compass),
            1.0);
    }
    report.loggedFrameParameters.insert(
        QStringLiteral("AHRS_ORIENTATION"), 0.0);
    return report;
}

class Fixture
{
public:
    Fixture()
        : transmitter([this](int frameLinkId, const QByteArray &bytes) {
              frames.append(bytes);
              const mavlink_message_t message = decodeFrame(bytes);
              if (onFrame) onFrame(message);
              if (autoRespond) respond(frameLinkId, message);
              if (afterFrame) afterFrame(message);
              return true;
          })
        , parameters(&targets, &transmitter)
        , commands(&targets, &transmitter)
        , apply(&targets, &registry, &parameters, &commands,
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
        linkEpoch = registry.beginLinkSession(
            linkId, QStringLiteral("MagFit link"));
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

    void addSphereSchema(int compass, bool includeLearn = true)
    {
        const QString suffix = compass == 1
            ? QString() : QString::number(compass);
        live.insert(QStringLiteral("AHRS_ORIENTATION"),
                    LiveParameter{quint8(0), ParameterType::UInt8});
        live.insert(QStringLiteral("COMPASS_ORIENT%1").arg(suffix),
                    LiveParameter{quint8(0), ParameterType::UInt8});
        live.insert(compass == 1
                        ? QStringLiteral("COMPASS_EXTERNAL")
                        : QStringLiteral("COMPASS_EXTERN%1").arg(compass),
                    LiveParameter{quint8(1), ParameterType::UInt8});
        if (includeLearn) {
            live.insert(QStringLiteral("COMPASS_LEARN"),
                        LiveParameter{quint8(1), ParameterType::UInt8});
        }
        live.insert(QStringLiteral("COMPASS_DEV_ID%1").arg(suffix),
                    LiveParameter{quint32(1000 + compass),
                                  ParameterType::UInt32});
        live.insert(QStringLiteral("COMPASS_PRIO%1_ID").arg(compass),
                    LiveParameter{quint32(1000 + compass),
                                  ParameterType::UInt32});
        live.insert(QStringLiteral("COMPASS_OFS%1_X").arg(suffix),
                    LiveParameter{0.0F, ParameterType::Real32});
        live.insert(QStringLiteral("COMPASS_OFS%1_Y").arg(suffix),
                    LiveParameter{0.0F, ParameterType::Real32});
        live.insert(QStringLiteral("COMPASS_OFS%1_Z").arg(suffix),
                    LiveParameter{0.0F, ParameterType::Real32});
        live.insert(QStringLiteral("COMPASS_OFFS_MAX"),
                    LiveParameter{1800.0F, ParameterType::Real32});
        live.insert(QStringLiteral("COMPASS_CAL_FIT"),
                    LiveParameter{16.0F, ParameterType::Real32});
    }

    void addEllipsoidSchema(int compass)
    {
        const QString suffix = compass == 1
            ? QString() : QString::number(compass);
        for (const QString &prefix
             : {QStringLiteral("COMPASS_DIA%1_").arg(suffix),
                QStringLiteral("COMPASS_ODI%1_").arg(suffix)}) {
            for (const QChar axis : {QLatin1Char('X'), QLatin1Char('Y'),
                                     QLatin1Char('Z')}) {
                const float initial = prefix.contains(
                    QStringLiteral("DIA")) ? 1.0F : 0.0F;
                live.insert(prefix + axis,
                            LiveParameter{initial, ParameterType::Real32});
            }
        }
    }

    void publishSnapshot()
    {
        QStringList names = live.keys();
        std::sort(names.begin(), names.end());
        parameters.store()->beginLoad(endpoint);
        for (int index = 0; index < names.size(); ++index) {
            const QString &name = names.at(index);
            const LiveParameter parameter = live.value(name);
            QVERIFY(parameters.store()->ingest(
                endpoint, names.size(), index, name,
                parameter.value, parameter.type));
        }
        QVERIFY(parameters.store()->snapshot(endpoint).isComplete());
    }

    void respond(int frameLinkId, const mavlink_message_t &message)
    {
        if (frameLinkId != linkId
            || message.msgid != MAVLINK_MSG_ID_PARAM_SET) return;
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
        QStringList names = live.keys();
        std::sort(names.begin(), names.end());
        parameters.observePhysicalMessage(
            linkId, linkEpoch,
            parameterValue(endpoint, name, value, type,
                           names.size(), names.indexOf(name)));
    }

    static constexpr int linkId = 41;
    static constexpr int systemId = 71;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QVector<QByteArray> frames;
    QVector<QPair<QString, QVariant>> writes;
    QHash<QString, LiveParameter> live;
    std::function<void(const mavlink_message_t &)> onFrame;
    std::function<void(const mavlink_message_t &)> afterFrame;
    std::function<void()> routeHook;
    std::function<void()> parameterRouteHook;
    std::function<void()> commandRouteHook;
    bool routeAllowed = true;
    bool autoRespond = true;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    VehicleCommandService commands;
    OfflineMagFitApplyService apply;
    quint64 linkEpoch = 0;
    VehicleEndpoint endpoint;
};
}

class OfflineMagFitApplyServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void mappingMatchesMissionPlannerAndSphereDoesNotResetMatrix();
    void preparePinsCallerAnalysisAcrossRouteCallback();
    void eligibilityIdentityAndSchemaFailuresAreAtomic();
    void confirmedWritesAreOrderedAndReceipted();
    void snapshotMutationArmAndSelectionAbaFailClosed();
    void futureParameterMutationStopsAfterConfirmedReceipt();
    void frameParameterMutationStopsAfterConfirmedReceipt();
    void cancellationAfterPartialApplyIsTruthful();
    void overallDeadlineStopsAnAttemptedWriteAsUncertain();
    void admissionPublishesTokenAndReleasesReservations();
};

void OfflineMagFitApplyServiceTest::
mappingMatchesMissionPlannerAndSphereDoesNotResetMatrix()
{
    Fixture fixture;
    fixture.addSphereSchema(1);
    fixture.addEllipsoidSchema(1);
    fixture.addSphereSchema(2);
    fixture.addEllipsoidSchema(2);
    fixture.publishSnapshot();
    OfflineMagFitReport analysis = reportFor(
        {fit(2, false), fit(1, true)});
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY2(fixture.apply.prepare(analysis, &plan, &error),
             qPrintable(error));
    const auto writes = plan.writes();
    QCOMPARE(plan.results().at(0).compass, 1);
    QCOMPARE(plan.results().at(1).compass, 2);
    QCOMPARE(writes.size(), 13);
    QCOMPARE(writes.at(0).name, QStringLiteral("COMPASS_LEARN"));
    QCOMPARE(writes.at(1).name, QStringLiteral("COMPASS_OFS_X"));
    QCOMPARE(writes.at(4).name, QStringLiteral("COMPASS_DIA_X"));
    QCOMPARE(writes.at(7).name, QStringLiteral("COMPASS_ODI_X"));
    QCOMPARE(writes.at(10).name, QStringLiteral("COMPASS_OFS2_X"));
    QCOMPARE(writes.at(12).name, QStringLiteral("COMPASS_OFS2_Z"));
    for (const auto &write : writes) {
        QVERIFY(!write.name.startsWith(QStringLiteral("COMPASS_DIA2_")));
        QVERIFY(!write.name.startsWith(QStringLiteral("COMPASS_ODI2_")));
    }
}

void OfflineMagFitApplyServiceTest::
preparePinsCallerAnalysisAcrossRouteCallback()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitReport analysis = reportFor({fit(1, false)});
    const QString expectedSource = analysis.sourcePath;
    fixture.routeHook = [&analysis]() { analysis = {}; };
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY2(fixture.apply.prepare(analysis, &plan, &error),
             qPrintable(error));
    QVERIFY(!analysis.success);
    QCOMPARE(plan.sourcePath(), expectedSource);
    QCOMPARE(plan.writes().size(), 3);
}

void OfflineMagFitApplyServiceTest::
eligibilityIdentityAndSchemaFailuresAreAtomic()
{
    Fixture fixture;
    fixture.addSphereSchema(1);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;

    OfflineMagFitReport analysis = reportFor({fit(1, false)});
    analysis.applyEligible = false;
    analysis.applyUnavailableReason = QStringLiteral("Identity proof absent.");
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QCOMPARE(error, QStringLiteral("Identity proof absent."));
    QVERIFY(!plan.isValid());

    analysis = reportFor({fit(1, false)});
    analysis.loggedDeviceIds[1] = 9999;
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("identity"), Qt::CaseInsensitive));

    analysis = reportFor({fit(1, false)});
    analysis.isTelemetryLog = true;
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(!plan.isValid());

    analysis = reportFor({fit(1, false)});
    analysis.loggedFrameParameters.remove(QStringLiteral("COMPASS_ORIENT"));
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("COMPASS_ORIENT")));

    fixture.live[QStringLiteral("COMPASS_PRIO1_ID")].value = quint32(7777);
    fixture.publishSnapshot();
    analysis = reportFor({fit(1, false)});
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("priority"), Qt::CaseInsensitive));
    fixture.live[QStringLiteral("COMPASS_PRIO1_ID")].value = quint32(1001);
    fixture.publishSnapshot();

    OfflineMagFitResult unsafeFit = fit(1, false);
    unsafeFit.sphereRadius = 150.0;
    QVERIFY(!fixture.apply.prepare(
        reportFor({unsafeFit}), &plan, &error));
    QVERIFY(error.contains(QStringLiteral("safety"), Qt::CaseInsensitive));

    unsafeFit = fit(1, false);
    unsafeFit.offsets.x = std::nextafter(1800.0, 0.0);
    QVERIFY(!fixture.apply.prepare(
        reportFor({unsafeFit}), &plan, &error));
    QVERIFY(error.contains(QStringLiteral("wire"), Qt::CaseInsensitive));

    analysis = reportFor({fit(1, true)});
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("sphere-only")));
    QVERIFY(fixture.frames.isEmpty());

    fixture.live.insert(QStringLiteral("COMPASS_DIA_X"),
                        LiveParameter{1.0F, ParameterType::Real32});
    fixture.publishSnapshot();
    QVERIFY(!fixture.apply.prepare(analysis, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("COMPASS_DIA_Y")));
    QVERIFY(fixture.frames.isEmpty());
}

void OfflineMagFitApplyServiceTest::confirmedWritesAreOrderedAndReceipted()
{
    Fixture fixture;
    fixture.addSphereSchema(1);
    fixture.publishSnapshot();
    const OfflineMagFitReport analysis = reportFor({fit(1, false)});
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY2(fixture.apply.prepare(analysis, &plan, &error),
             qPrintable(error));
    quint64 operationId = 0;
    QSignalSpy finished(&fixture.apply,
                        &OfflineMagFitApplyService::operationFinished);
    QCOMPARE(fixture.apply.execute(plan, &operationId, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QVERIFY(operationId != 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto terminal = fixture.apply.lastReport();
    QCOMPARE(terminal.outcome,
             OfflineMagFitApplyService::Outcome::Completed);
    QCOMPARE(terminal.totalWrites, 4);
    QCOMPARE(terminal.confirmedWrites, 4);
    QCOMPARE(terminal.remainingWrites, 0);
    QCOMPARE(terminal.receipts.size(), 4);
    QCOMPARE(fixture.writes.size(), 4);
    QCOMPARE(fixture.writes.at(0).first,
             QStringLiteral("COMPASS_LEARN"));
    QCOMPARE(fixture.writes.at(1).first,
             QStringLiteral("COMPASS_OFS_X"));
    QCOMPARE(fixture.writes.at(2).first,
             QStringLiteral("COMPASS_OFS_Y"));
    QCOMPARE(fixture.writes.at(3).first,
             QStringLiteral("COMPASS_OFS_Z"));
}

void OfflineMagFitApplyServiceTest::
snapshotMutationArmAndSelectionAbaFailClosed()
{
    {
        Fixture fixture;
        fixture.addSphereSchema(1);
        fixture.publishSnapshot();
        const OfflineMagFitReport analysis = reportFor({fit(1, false)});
        OfflineMagFitApplyService::Plan plan;
        QString error;
        QVERIFY(fixture.apply.prepare(analysis, &plan, &error));
        fixture.live[QStringLiteral("COMPASS_OFS_X")].value = 17.0F;
        fixture.publishSnapshot();
        QVERIFY(!fixture.apply.validate(plan, &error));
        quint64 id = 0;
        QCOMPARE(fixture.apply.execute(plan, &id, &error),
                 OfflineMagFitApplyService::SubmitResult::Unavailable);
        QVERIFY(id != 0);
        QVERIFY(fixture.frames.isEmpty());
    }
    {
        Fixture fixture;
        fixture.addSphereSchema(1);
        fixture.publishSnapshot();
        OfflineMagFitApplyService::Plan plan;
        QString error;
        QVERIFY(fixture.apply.prepare(
            reportFor({fit(1, false)}), &plan, &error));
        quint64 id = 0;
        int validations = 0;
        fixture.routeHook = [&]() {
            if (fixture.apply.busy() && ++validations == 4) {
                fixture.setArmed(true);
            }
        };
        QSignalSpy finished(&fixture.apply,
                            &OfflineMagFitApplyService::operationFinished);
        QCOMPARE(fixture.apply.execute(plan, &id, &error),
                 OfflineMagFitApplyService::SubmitResult::Started);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
        QVERIFY(fixture.frames.isEmpty());
        QCOMPARE(fixture.apply.lastReport().outcome,
                 OfflineMagFitApplyService::Outcome::Rejected);
    }
    {
        Fixture fixture;
        fixture.addSphereSchema(1);
        fixture.publishSnapshot();
        OfflineMagFitApplyService::Plan plan;
        QString error;
        QVERIFY(fixture.apply.prepare(
            reportFor({fit(1, false)}), &plan, &error));
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
        quint64 id = 0;
        QCOMPARE(fixture.apply.execute(plan, &id, &error),
                 OfflineMagFitApplyService::SubmitResult::Unavailable);
        QVERIFY(id != 0);
        QVERIFY(fixture.frames.isEmpty());
    }
    {
        Fixture fixture;
        fixture.addSphereSchema(1);
        fixture.publishSnapshot();
        OfflineMagFitApplyService::Plan plan;
        QString error;
        QVERIFY(fixture.apply.prepare(
            reportFor({fit(1, false)}), &plan, &error));
        QVERIFY(fixture.registry.endLinkSession(
            Fixture::linkId, fixture.linkEpoch));
        fixture.linkEpoch = fixture.registry.beginLinkSession(
            Fixture::linkId, QStringLiteral("Replacement MagFit link"));
        fixture.setArmed(false);
        quint64 id = 0;
        QCOMPARE(fixture.apply.execute(plan, &id, &error),
                 OfflineMagFitApplyService::SubmitResult::Unavailable);
        QVERIFY(id != 0);
        QVERIFY(fixture.frames.isEmpty());
    }
}

void OfflineMagFitApplyServiceTest::
futureParameterMutationStopsAfterConfirmedReceipt()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY(fixture.apply.prepare(
        reportFor({fit(1, false)}), &plan, &error));
    fixture.afterFrame = [&fixture](const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_SET
            && fixture.writes.size() == 1) {
            fixture.live[QStringLiteral("COMPASS_OFS_Y")].value = 17.0F;
            fixture.publishSnapshot();
        }
    };
    quint64 id = 0;
    QSignalSpy finished(&fixture.apply,
                        &OfflineMagFitApplyService::operationFinished);
    QCOMPARE(fixture.apply.execute(plan, &id, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(fixture.frames.size(), 1);
    const auto terminal = fixture.apply.lastReport();
    QCOMPARE(terminal.outcome,
             OfflineMagFitApplyService::Outcome::Rejected);
    QCOMPARE(terminal.confirmedWrites, 1);
    QCOMPARE(terminal.receipts.size(), 1);
    QCOMPARE(terminal.remainingWrites, 2);
    QVERIFY(terminal.description.contains(
        QStringLiteral("COMPASS_OFS_Y")));
}

void OfflineMagFitApplyServiceTest::
frameParameterMutationStopsAfterConfirmedReceipt()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY(fixture.apply.prepare(
        reportFor({fit(1, false)}), &plan, &error));
    fixture.afterFrame = [&fixture](const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_SET
            && fixture.writes.size() == 1) {
            fixture.live[QStringLiteral("AHRS_ORIENTATION")].value = quint8(1);
            fixture.publishSnapshot();
        }
    };
    quint64 id = 0;
    QSignalSpy finished(&fixture.apply,
                        &OfflineMagFitApplyService::operationFinished);
    QCOMPARE(fixture.apply.execute(plan, &id, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(fixture.frames.size(), 1);
    const auto terminal = fixture.apply.lastReport();
    QCOMPARE(terminal.outcome,
             OfflineMagFitApplyService::Outcome::Rejected);
    QCOMPARE(terminal.confirmedWrites, 1);
    QCOMPARE(terminal.remainingWrites, 2);
    QVERIFY(terminal.description.contains(
        QStringLiteral("AHRS_ORIENTATION")));
}

void OfflineMagFitApplyServiceTest::
cancellationAfterPartialApplyIsTruthful()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY(fixture.apply.prepare(
        reportFor({fit(1, false)}), &plan, &error));
    fixture.afterFrame = [&fixture](const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_SET
            && fixture.writes.size() == 1) {
            fixture.autoRespond = false;
        }
    };
    quint64 id = 0;
    QSignalSpy finished(&fixture.apply,
                        &OfflineMagFitApplyService::operationFinished);
    QCOMPARE(fixture.apply.execute(plan, &id, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.frames.size() >= 2, 1000);
    QVERIFY(fixture.apply.cancel(id));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto terminal = fixture.apply.lastReport();
    QCOMPARE(terminal.outcome,
             OfflineMagFitApplyService::Outcome::OutcomeUncertain);
    QCOMPARE(terminal.confirmedWrites, 1);
    QCOMPARE(terminal.receipts.size(), 1);
    QCOMPARE(terminal.remainingWrites, 2);
}

void OfflineMagFitApplyServiceTest::
overallDeadlineStopsAnAttemptedWriteAsUncertain()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY(fixture.apply.prepare(
        reportFor({fit(1, false)}), &plan, &error));
    fixture.autoRespond = false;
    fixture.apply.setOverallDeadlineForTesting(15);
    quint64 id = 0;
    QSignalSpy finished(&fixture.apply,
                        &OfflineMagFitApplyService::operationFinished);
    QCOMPARE(fixture.apply.execute(plan, &id, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QVERIFY(!fixture.frames.isEmpty());
    const auto terminal = fixture.apply.lastReport();
    QCOMPARE(terminal.outcome,
             OfflineMagFitApplyService::Outcome::OutcomeUncertain);
    QCOMPARE(terminal.confirmedWrites, 0);
    QCOMPARE(terminal.remainingWrites, 3);
}

void OfflineMagFitApplyServiceTest::
admissionPublishesTokenAndReleasesReservations()
{
    Fixture fixture;
    fixture.addSphereSchema(1, false);
    fixture.publishSnapshot();
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY(fixture.apply.prepare(
        reportFor({fit(1, false)}), &plan, &error));
    const auto lease = plan.vehicle();
    quint64 id = 0;
    bool cancelled = false;
    fixture.routeHook = [&]() {
        if (!cancelled && fixture.apply.busy()) {
            QVERIFY(id != 0);
            cancelled = fixture.apply.cancel(id);
        }
    };
    QCOMPARE(fixture.apply.execute(plan, &id, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QVERIFY(cancelled);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.apply.busy(), 500);
    QVERIFY(fixture.frames.isEmpty());

    QObject owner;
    const VehicleTargetLease target = fixture.targets.acquireTarget();
    VehicleCommandService::ExactReservationToken commandReservation;
    QCOMPARE(fixture.commands.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &commandReservation, &error),
             VehicleCommandService::ExactReservationResult::Reserved);
    ParameterService::ExactReservationToken parameterReservation;
    QCOMPARE(fixture.parameters.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &parameterReservation, &error),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(fixture.parameters.releaseExactReservation(
        parameterReservation));
    QVERIFY(fixture.commands.releaseExactReservation(commandReservation));
}

QTEST_GUILESS_MAIN(OfflineMagFitApplyServiceTest)

#include "test_offlinemagfitapplyservice.moc"
