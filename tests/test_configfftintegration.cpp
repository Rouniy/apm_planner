#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"
#include "ui/configuration/ConfigFFTIntegration.h"
#include "ui/configuration/ConfigFFTView.h"

#include <QtTest>

#include <QHash>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace {

VehicleEndpoint endpoint(int linkId = 51, int systemId = 42)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = QStringLiteral("AUTOPILOT1");
    return value;
}

SwarmVehicleInstanceLease exactLease(const VehicleEndpoint &source)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint = source;
    lease.linkSessionEpoch = 7;
    lease.instanceEpoch = 11;
    return lease;
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

QString parameterName(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(id, length);
}

void copyParameterName(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    std::memset(id, 0, 16);
    std::memcpy(id, bytes.constData(), static_cast<size_t>(
        std::min(16, bytes.size())));
}

mavlink_message_t parameterValue(const VehicleEndpoint &source,
                                 const QString &name,
                                 const QVariant &value,
                                 ParameterType type)
{
    bool encoded = false;
    const float wire = ParameterCodec::encodeClassic(
        value, type, ParameterEncoding::Bytewise, &encoded);
    Q_ASSERT(encoded);
    mavlink_param_value_t payload{};
    copyParameterName(name, payload.param_id);
    payload.param_value = wire;
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = 3;
    payload.param_index = 0;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId), &message, &payload);
    return message;
}

const ParamField *field(const ConfigFFTViewModel *model,
                        const QString &name)
{
    if (!model) {
        return nullptr;
    }
    const QList<ParamField> fields = model->fields();
    for (const ParamField &candidate : fields) {
        if (candidate.name == name) {
            // Tests only consume the value immediately; keep a stable copy.
            static ParamField found;
            found = candidate;
            return &found;
        }
    }
    return nullptr;
}

class Fixture final
{
public:
    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
            const mavlink_message_t message = decodeFrame(bytes);
            frames.append(message);
            if (!servicePointer) {
                return true;
            }
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
                mavlink_param_request_read_t request{};
                mavlink_msg_param_request_read_decode(&message, &request);
                const QString name = parameterName(request.param_id);
                servicePointer->observeMessage(
                    linkId, parameterValue(selected, name,
                                           values.value(name),
                                           ParameterType::Int32));
            } else if (message.msgid == MAVLINK_MSG_ID_PARAM_SET
                       && acknowledgeWrites) {
                mavlink_param_set_t request{};
                mavlink_msg_param_set_decode(&message, &request);
                const QString name = parameterName(request.param_id);
                const ParameterType type =
                    static_cast<ParameterType>(request.param_type);
                bool decoded = false;
                const QVariant value = ParameterCodec::decodeClassic(
                    request.param_value, type,
                    ParameterEncoding::Bytewise, &decoded);
                Q_ASSERT(decoded);
                values.insert(name, value);
                servicePointer->observeMessage(
                    linkId, parameterValue(selected, name, value, type));
            }
            return true;
        })
        , service(&targets, &transmitter)
    {
        values.insert(QStringLiteral("INS_LOG_BAT_CNT"), qint32(4));
        values.insert(QStringLiteral("INS_LOG_BAT_MASK"), qint32(3));
        values.insert(QStringLiteral("LOG_BITMASK"), qint32(65535));
        targets.observeEndpoint(selected, true);
        targets.observeHeartbeat(
            selected, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        lease = exactLease(selected);
        QVERIFY(service.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &candidate) {
                return exactLeaseCurrent && candidate.sameInstance(lease);
            },
            [](const SwarmVehicleInstanceLease &, QString *error) {
                if (error) {
                    error->clear();
                }
                return true;
            }));
        QVERIFY(service.configureSingleVehicleExactRoute(
            [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
        servicePointer = &service;
    }

    int frameCount(quint32 messageId) const
    {
        return static_cast<int>(std::count_if(
            frames.constBegin(), frames.constEnd(),
            [messageId](const mavlink_message_t &message) {
                return message.msgid == messageId;
            }));
    }

    ConfigFFTIntegration *bind(ConfigFFTView *view)
    {
        return BindConfigFFTViewToExactParameters(
            view, &service, &targets,
            [this](const VehicleTargetLease &target) {
                return exactLeaseCurrent && target.isValid()
                        && target.endpoint.sameIdentity(selected)
                    ? lease : SwarmVehicleInstanceLease{};
            });
    }

    VehicleEndpoint selected = endpoint();
    QHash<QString, QVariant> values;
    QVector<mavlink_message_t> frames;
    bool acknowledgeWrites = true;
    bool exactLeaseCurrent = true;
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter;
    ParameterService service;
    ParameterService *servicePointer = nullptr;
    SwarmVehicleInstanceLease lease;
};

} // namespace

class ConfigFFTIntegrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void refreshPublishesOnlyTheCompleteExactSet();
    void writeUsesExactAckAndReleasesReservation();
    void targetReplacementCancelsOnlyOwnedWorkAndKeepsOfflineFft();
};

void ConfigFFTIntegrationTest::refreshPublishesOnlyTheCompleteExactSet()
{
    Fixture fixture;
    ConfigFFTView view;
    ConfigFFTIntegration *const integration = fixture.bind(&view);
    QVERIFY(integration);
    QVERIFY(integration->parameterTargetUsable());
    QVERIFY(!view.viewModel()->snapshotReady());

    ParameterStore *const store = fixture.service.store();
    store->beginLoad(fixture.selected);
    QVERIFY(store->ingest(
        fixture.selected, 2, 0, QStringLiteral("INS_LOG_BAT_CNT"),
        qint32(2), ParameterType::Int32));
    QVERIFY(store->ingest(
        fixture.selected, 2, 1, QStringLiteral("INS_LOG_BAT_MASK"),
        qint32(1), ParameterType::Int32));
    QVERIFY(view.viewModel()->snapshotReady());
    const ParamField *const missing = field(
        view.viewModel(), QStringLiteral("LOG_BITMASK"));
    QVERIFY(missing);
    QVERIFY(missing->readOnly);
    const ParamField *const available = field(
        view.viewModel(), QStringLiteral("INS_LOG_BAT_CNT"));
    QVERIFY(available);
    QVERIFY(!available->readOnly);

    QVERIFY(view.viewModel()->refreshParameters());
    QVERIFY(view.viewModel()->snapshotReady());
    QVERIFY(!integration->operationActive());
    QCOMPARE(fixture.frameCount(MAVLINK_MSG_ID_PARAM_REQUEST_READ), 3);
    QCOMPARE(field(view.viewModel(), QStringLiteral("INS_LOG_BAT_CNT"))
                 ->value.toInt(), 4);
    QCOMPARE(field(view.viewModel(), QStringLiteral("INS_LOG_BAT_MASK"))
                 ->value.toInt(), 3);
    QCOMPARE(field(view.viewModel(), QStringLiteral("LOG_BITMASK"))
                 ->value.toInt(), 65535);

    QObject independentOwner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(fixture.service.reserveExactEndpoints(
                 &independentOwner, {fixture.lease}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(fixture.service.releaseExactReservation(reservation));
}

void ConfigFFTIntegrationTest::writeUsesExactAckAndReleasesReservation()
{
    Fixture fixture;
    ConfigFFTView view;
    ConfigFFTIntegration *const integration = fixture.bind(&view);
    QVERIFY(view.viewModel()->refreshParameters());
    QCOMPARE(fixture.frameCount(MAVLINK_MSG_ID_PARAM_SET), 0);

    QVERIFY(view.viewModel()->stageParameterValue(
        QStringLiteral("INS_LOG_BAT_CNT"), qint32(8)));
    QCOMPARE(fixture.frameCount(MAVLINK_MSG_ID_PARAM_SET), 1);
    QVERIFY(!view.viewModel()->parameterWriteBusy());
    QVERIFY(!view.viewModel()->reconciliationRequired());
    QCOMPARE(field(view.viewModel(), QStringLiteral("INS_LOG_BAT_CNT"))
                 ->value.toInt(), 8);
    QVERIFY(!integration->operationActive());

    const int writesBeforeArmedAttempt =
        fixture.frameCount(MAVLINK_MSG_ID_PARAM_SET);
    fixture.targets.observeHeartbeat(
        fixture.selected, true, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR);
    view.writeRequested(
        999, fixture.selected.componentId,
        QStringLiteral("INS_LOG_BAT_CNT"), qint32(9));
    QCOMPARE(fixture.frameCount(MAVLINK_MSG_ID_PARAM_SET),
             writesBeforeArmedAttempt);

    QObject independentOwner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(fixture.service.reserveExactEndpoints(
                 &independentOwner, {fixture.lease}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(fixture.service.releaseExactReservation(reservation));
}

void ConfigFFTIntegrationTest::
targetReplacementCancelsOnlyOwnedWorkAndKeepsOfflineFft()
{
    Fixture fixture;
    ConfigFFTView view;
    ConfigFFTIntegration *const integration = fixture.bind(&view);
    QVERIFY(view.viewModel()->refreshParameters());
    fixture.acknowledgeWrites = false;
    QVERIFY(view.viewModel()->stageParameterValue(
        QStringLiteral("LOG_BITMASK"), qint32(7)));
    QVERIFY(integration->operationActive());
    QVERIFY(view.viewModel()->parameterWriteBusy());

    const VehicleEndpoint replacement = endpoint(52, 43);
    fixture.targets.observeEndpoint(replacement);
    fixture.targets.observeHeartbeat(
        replacement, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR);
    QVERIFY(fixture.targets.selectTarget(
        replacement.linkId, replacement.systemId,
        replacement.componentId));

    QVERIFY(!integration->parameterTargetUsable());
    QVERIFY(!integration->operationActive());
    QVERIFY(!view.viewModel()->parameterWriteBusy());
    QVERIFY(view.viewModel()->reconciliationRequired());
    QVERIFY(view.viewModel()->canAnalyze());

    Fixture retiredFixture;
    ConfigFFTView retiredView;
    ConfigFFTIntegration *const retiredIntegration =
        retiredFixture.bind(&retiredView);
    QVERIFY(retiredView.viewModel()->refreshParameters());
    retiredFixture.acknowledgeWrites = false;
    QVERIFY(retiredView.viewModel()->stageParameterValue(
        QStringLiteral("LOG_BITMASK"), qint32(9)));
    retiredFixture.exactLeaseCurrent = false;
    QTimer *const freshness = retiredIntegration->findChild<QTimer *>(
        QStringLiteral("configFftExactTargetFreshnessTimer"));
    QVERIFY(freshness);
    QVERIFY(QMetaObject::invokeMethod(
        freshness, "timeout", Qt::DirectConnection));
    QVERIFY(!retiredIntegration->parameterTargetUsable());
    QVERIFY(!retiredIntegration->operationActive());
    QVERIFY(retiredView.viewModel()->reconciliationRequired());
    QVERIFY(retiredView.viewModel()->canAnalyze());
}

QTEST_MAIN(ConfigFFTIntegrationTest)
#include "test_configfftintegration.moc"
