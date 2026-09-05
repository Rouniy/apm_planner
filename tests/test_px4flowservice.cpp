#include "comm/ExactLinkTransmitter.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/ParameterService.h"
#include "comm/Px4FlowFrameAssembler.h"
#include "comm/Px4FlowService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"

#include <QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

namespace
{

mavlink_message_t heartbeat(quint8 systemId, quint8 componentId)
{
    mavlink_heartbeat_t payload{};
    payload.type = MAV_TYPE_GENERIC;
    payload.autopilot = MAV_AUTOPILOT_GENERIC;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(systemId, componentId, &message, &payload);
    return message;
}

void copyParameterId(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    const int count = std::min(16, bytes.size());
    std::memset(id, 0, 16);
    if (count > 0) {
        std::memcpy(id, bytes.constData(), static_cast<std::size_t>(count));
    }
}

mavlink_message_t parameterValue(quint8 systemId, quint8 componentId,
                                 double value)
{
    bool encoded = false;
    const float wireValue = ParameterCodec::encodeClassic(
        value, ParameterType::Real32, ParameterEncoding::Bytewise, &encoded);
    Q_ASSERT(encoded);
    mavlink_param_value_t payload{};
    copyParameterId(QStringLiteral("VIDEO_ONLY"), payload.param_id);
    payload.param_value = wireValue;
    payload.param_type = static_cast<quint8>(ParameterType::Real32);
    payload.param_count = 1;
    payload.param_index = 0;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(systemId, componentId, &message, &payload);
    return message;
}

mavlink_message_t handshake(quint8 systemId, quint8 componentId,
                            int width, int height, int payloadBytes)
{
    mavlink_data_transmission_handshake_t payload{};
    payload.type = Px4FlowFrameAssembler::Raw8UStreamType;
    payload.size = static_cast<quint32>(width * height);
    payload.width = static_cast<quint16>(width);
    payload.height = static_cast<quint16>(height);
    payload.payload = static_cast<quint8>(payloadBytes);
    payload.packets = static_cast<quint16>(
        (payload.size + payload.payload - 1U) / payload.payload);
    payload.jpg_quality = 100;
    mavlink_message_t message{};
    mavlink_msg_data_transmission_handshake_encode(
        systemId, componentId, &message, &payload);
    return message;
}

mavlink_message_t imagePacket(quint8 systemId, quint8 componentId,
                              quint16 sequence, const QByteArray &bytes)
{
    std::array<quint8, MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN> data{};
    const int count = std::min(int(data.size()), bytes.size());
    if (count > 0) {
        std::memcpy(data.data(), bytes.constData(),
                    static_cast<std::size_t>(count));
    }
    mavlink_message_t message{};
    mavlink_msg_encapsulated_data_pack(
        systemId, componentId, &message, sequence, data.data());
    return message;
}

QString firstSourceId(const Px4FlowService &service)
{
    const QVariantList rows = service.sources();
    return rows.isEmpty()
        ? QString()
        : rows.first().toMap().value(QStringLiteral("id")).toString();
}

bool isVideoOnlySet(const mavlink_message_t &message,
                    quint8 *targetSystem = nullptr,
                    quint8 *targetComponent = nullptr,
                    double *value = nullptr)
{
    if (message.msgid != MAVLINK_MSG_ID_PARAM_SET) {
        return false;
    }
    mavlink_param_set_t payload{};
    mavlink_msg_param_set_decode(&message, &payload);
    char id[17]{};
    std::memcpy(id, payload.param_id, 16);
    if (QString::fromLatin1(id) != QStringLiteral("VIDEO_ONLY")) {
        return false;
    }
    if (targetSystem) *targetSystem = payload.target_system;
    if (targetComponent) *targetComponent = payload.target_component;
    if (value) {
        bool decoded = false;
        *value = ParameterCodec::decodeClassic(
            payload.param_value,
            static_cast<ParameterType>(payload.param_type),
            ParameterEncoding::Bytewise, &decoded).toDouble();
        if (!decoded) return false;
    }
    return true;
}

struct Fixture
{
    qint64 now = 0;
    VehicleTargetManager targets;
    std::function<void()> writerHook;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    MavlinkComponentRegistry registry;
    QVector<mavlink_message_t> transmitted;
    Px4FlowService service;

    Fixture()
        : transmitter([this](int, const QByteArray &) {
              const std::function<void()> hook = writerHook;
              if (hook) hook();
              return true;
          }),
          parameters(&targets, &transmitter),
          registry(nullptr, [this]() { return now; }),
          service(&registry, &parameters)
    {
        const bool configured = parameters.configureComponentExactTransactions(
            [this](const MavlinkComponentInstanceLease &lease) {
                return registry.validateLease(lease);
            },
            [](const MavlinkComponentInstanceLease &, QString *) {
                return true;
            });
        Q_ASSERT(configured);
        QObject::connect(
            &registry, &MavlinkComponentRegistry::componentRetired,
            &parameters, &ParameterService::retireComponent);
        QObject::connect(
            &transmitter, &ExactLinkTransmitter::messageSubmitted,
            &service, [this](int, quint64, const mavlink_message_t &message) {
                transmitted.append(message);
            });
    }

    void addSource(int linkId, quint64 epoch,
                   quint8 systemId, quint8 componentId)
    {
        transmitter.setLinkSessionEpoch(linkId, epoch);
        registry.beginLinkSession(linkId, epoch);
        registry.observeMessage(
            linkId, epoch, heartbeat(systemId, componentId));
    }

    void acknowledge(int linkId, quint64 epoch,
                     quint8 systemId, quint8 componentId, double value)
    {
        parameters.observeComponentMessage(
            linkId, epoch,
            parameterValue(systemId, componentId, value));
    }
};

} // namespace

class Px4FlowServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void sourceSelectionIsExplicitAndExact();
    void liveFramesAreExactAndRowsArePaddedSafely();
    void replayGenerationNeverMixesWithLiveTraffic();
    void parameterBusyDoesNotHidePinnedImageAndSameClickRetries();
    void deactivationRestoresZeroOnOriginalSource();
    void activationDuringCleanupDoesNotSelfReserve();
    void synchronousAckThenDeactivateStillRestoresZero();
    void sourceSwitchWaitsForOldSourceCleanup();
    void retiredSourceIsNotRedirectedToSuccessor();
    void sameEpochReplacementPacketsCannotCompleteOldFrame();
    void staleDiagnosticSurvivesNewHandshakesUntilCompletion();
};

void Px4FlowServiceTest::sourceSelectionIsExplicitAndExact()
{
    Fixture fixture;
    fixture.addSource(4, 41, 81, 50);
    fixture.addSource(5, 51, 81, 50);

    const QVariantList rows = fixture.service.sources();
    QCOMPARE(rows.size(), 2);
    QVERIFY(rows.at(0).toMap().value(QStringLiteral("id")).toString()
                != rows.at(1).toMap().value(QStringLiteral("id")).toString());
    QVERIFY(fixture.service.selectedSourceId().isEmpty());
    QVERIFY(!fixture.service.canToggle());

    const QString selected = rows.at(1).toMap()
                                 .value(QStringLiteral("id")).toString();
    fixture.service.selectSource(selected);
    QCOMPARE(fixture.service.selectedSourceId(), selected);
    QVERIFY(fixture.transmitted.isEmpty());

    fixture.service.activate();
    QCOMPARE(fixture.transmitted.size(), 1);
    QCOMPARE(fixture.transmitted.last().msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    fixture.acknowledge(5, 51, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());
    QVERIFY(!fixture.service.videoOnly());

    fixture.service.deactivate();
    QTRY_VERIFY(!fixture.service.busy());
}

void Px4FlowServiceTest::liveFramesAreExactAndRowsArePaddedSafely()
{
    Fixture fixture;
    fixture.addSource(7, 70, 81, 50);
    fixture.service.selectSource(firstSourceId(fixture.service));
    fixture.service.activate();
    fixture.acknowledge(7, 70, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());

    const mavlink_message_t begin = handshake(81, 50, 3, 2, 4);
    fixture.service.observeMessage(8, 70, begin);
    fixture.service.observeMessage(7, 69, begin);
    fixture.service.observeMessage(7, 70, handshake(82, 50, 3, 2, 4));
    fixture.service.observeMessage(7, 70, begin);
    fixture.service.observeMessage(
        7, 70, imagePacket(81, 50, 0, QByteArray::fromHex("01020304")));
    fixture.service.observeMessage(
        7, 70, imagePacket(81, 50, 1, QByteArray::fromHex("0506")));

    const QImage image = fixture.service.frame();
    QVERIFY(!image.isNull());
    QCOMPARE(image.format(), QImage::Format_Grayscale8);
    QCOMPARE(image.size(), QSize(3, 2));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(image.constScanLine(0)), 3),
             QByteArray::fromHex("010203"));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(image.constScanLine(1)), 3),
             QByteArray::fromHex("040506"));
    for (int row = 0; row < image.height(); ++row) {
        for (int column = image.width(); column < image.bytesPerLine(); ++column) {
            QCOMPARE(image.constScanLine(row)[column], uchar(0));
        }
    }
}

void Px4FlowServiceTest::replayGenerationNeverMixesWithLiveTraffic()
{
    Px4FlowService service(nullptr, nullptr);
    service.beginReplay(8);
    QCOMPARE(service.selectedSourceId(), QStringLiteral("replay:8"));
    QVERIFY(!service.canToggle());
    QCOMPARE(service.sources().size(), 1);

    const mavlink_message_t begin = handshake(81, 50, 2, 2, 4);
    const mavlink_message_t packet = imagePacket(
        81, 50, 0, QByteArray::fromHex("10203040"));
    service.observeReplay(7, begin);
    service.observeReplay(7, packet);
    QVERIFY(service.frame().isNull());
    service.observeMessage(1, 1, begin);
    service.observeMessage(1, 1, packet);
    QVERIFY(service.frame().isNull());

    service.observeReplay(8, begin);
    service.observeReplay(
        8, imagePacket(82, 50, 0, QByteArray::fromHex("aabbccdd")));
    QVERIFY(service.frame().isNull());
    service.observeReplay(8, packet);
    QCOMPARE(service.frame().size(), QSize(2, 2));
    const quint64 frameKey = service.frame().cacheKey();

    service.beginReplay(9);
    QCOMPARE(service.selectedSourceId(), QStringLiteral("replay:9"));
    QVERIFY(service.frame().isNull());
    service.observeReplay(8, begin);
    service.observeReplay(8, packet);
    QVERIFY(service.frame().isNull());
    service.observeReplay(9, begin);
    service.observeReplay(9, packet);
    QVERIFY(!service.frame().isNull());
    QVERIFY(service.frame().cacheKey() != frameKey);

    service.endReplay(8);
    QCOMPARE(service.selectedSourceId(), QStringLiteral("replay:9"));
    service.endReplay(9);
    QVERIFY(service.selectedSourceId().isEmpty());
}

void Px4FlowServiceTest::parameterBusyDoesNotHidePinnedImageAndSameClickRetries()
{
    Fixture fixture;
    fixture.addSource(8, 80, 81, 50);
    const MavlinkComponentInstanceLease lease = fixture.registry.components().first();
    QObject blocker;
    ParameterService::ExactReservationToken blockedReservation;
    QCOMPARE(fixture.parameters.reserveComponentEndpoint(
                 &blocker, lease, &blockedReservation),
             ParameterService::ExactReservationResult::Reserved);

    const QString selected = firstSourceId(fixture.service);
    fixture.service.selectSource(selected);
    fixture.service.activate();
    QVERIFY(fixture.service.status().contains(
        QStringLiteral("busy"), Qt::CaseInsensitive));
    fixture.service.observeMessage(8, 80, handshake(81, 50, 2, 2, 4));
    fixture.service.observeMessage(
        8, 80, imagePacket(81, 50, 0, QByteArray::fromHex("01020304")));
    QCOMPARE(fixture.service.frame().size(), QSize(2, 2));

    QVERIFY(fixture.parameters.releaseExactReservation(blockedReservation));
    const int beforeRetry = fixture.transmitted.size();
    fixture.service.selectSource(selected);
    QCOMPARE(fixture.transmitted.size(), beforeRetry + 1);
    QCOMPARE(fixture.transmitted.last().msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    fixture.acknowledge(8, 80, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());
}

void Px4FlowServiceTest::deactivationRestoresZeroOnOriginalSource()
{
    Fixture fixture;
    fixture.addSource(9, 90, 81, 50);
    fixture.service.selectSource(firstSourceId(fixture.service));
    fixture.service.activate();
    fixture.acknowledge(9, 90, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());

    fixture.service.toggleFocus();
    fixture.acknowledge(9, 90, 81, 50, 1.0);
    QTRY_VERIFY(fixture.service.videoOnly());

    fixture.service.deactivate();
    QVERIFY(fixture.service.busy());
    quint8 targetSystem = 0;
    quint8 targetComponent = 0;
    double value = -1.0;
    QVERIFY(isVideoOnlySet(fixture.transmitted.last(),
                          &targetSystem, &targetComponent, &value));
    QCOMPARE(targetSystem, quint8(81));
    QCOMPARE(targetComponent, quint8(50));
    QCOMPARE(value, 0.0);
    fixture.acknowledge(9, 90, 81, 50, 0.0);
    QTRY_VERIFY(!fixture.service.busy());
    QVERIFY(!fixture.service.canToggle());
}

void Px4FlowServiceTest::activationDuringCleanupDoesNotSelfReserve()
{
    Fixture fixture;
    fixture.addSource(16, 160, 81, 50);
    fixture.service.selectSource(firstSourceId(fixture.service));
    fixture.service.activate();
    fixture.acknowledge(16, 160, 81, 50, 1.0);
    QTRY_VERIFY(fixture.service.videoOnly());

    fixture.service.deactivate();
    const int cleanupWriteCount = fixture.transmitted.size();
    fixture.service.activate();
    QCOMPARE(fixture.transmitted.size(), cleanupWriteCount);
    QVERIFY(fixture.service.busy());

    fixture.acknowledge(16, 160, 81, 50, 0.0);
    QTRY_VERIFY(fixture.transmitted.size() > cleanupWriteCount);
    QCOMPARE(fixture.transmitted.last().msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    fixture.acknowledge(16, 160, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());
}

void Px4FlowServiceTest::synchronousAckThenDeactivateStillRestoresZero()
{
    Fixture fixture;
    fixture.addSource(14, 140, 81, 50);
    fixture.service.selectSource(firstSourceId(fixture.service));
    fixture.service.activate();
    fixture.acknowledge(14, 140, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());

    bool reentered = false;
    fixture.writerHook = [&fixture, &reentered]() {
        if (reentered) return;
        reentered = true;
        fixture.writerHook = {};
        fixture.parameters.observeComponentMessage(
            14, 140, parameterValue(81, 50, 1.0));
        fixture.service.deactivate();
    };
    fixture.service.toggleFocus();

    QTRY_VERIFY(fixture.transmitted.size() >= 3);
    quint8 targetSystem = 0;
    quint8 targetComponent = 0;
    double value = -1.0;
    QVERIFY(isVideoOnlySet(fixture.transmitted.last(),
                          &targetSystem, &targetComponent, &value));
    QCOMPARE(targetSystem, quint8(81));
    QCOMPARE(targetComponent, quint8(50));
    QCOMPARE(value, 0.0);
    fixture.acknowledge(14, 140, 81, 50, 0.0);
    QTRY_VERIFY(!fixture.service.busy());
}

void Px4FlowServiceTest::sourceSwitchWaitsForOldSourceCleanup()
{
    Fixture fixture;
    fixture.addSource(10, 100, 81, 50);
    fixture.addSource(11, 110, 82, 51);
    const QVariantList rows = fixture.service.sources();
    const QString first = rows.at(0).toMap().value(QStringLiteral("id")).toString();
    const QString second = rows.at(1).toMap().value(QStringLiteral("id")).toString();

    fixture.service.selectSource(first);
    fixture.service.activate();
    fixture.acknowledge(10, 100, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());
    fixture.service.toggleFocus();
    fixture.acknowledge(10, 100, 81, 50, 1.0);
    QTRY_VERIFY(fixture.service.videoOnly());

    fixture.service.selectSource(second);
    QCOMPARE(fixture.service.selectedSourceId(), first);
    QVERIFY(fixture.service.busy());
    quint8 targetSystem = 0;
    quint8 targetComponent = 0;
    double value = -1.0;
    QVERIFY(isVideoOnlySet(fixture.transmitted.last(),
                          &targetSystem, &targetComponent, &value));
    QCOMPARE(targetSystem, quint8(81));
    QCOMPARE(targetComponent, quint8(50));
    QCOMPARE(value, 0.0);

    // A repeated/new navigation intent must not cancel the zero write that
    // is already draining on the original sensor.
    fixture.service.selectSource(second);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.service.selectedSourceId(), first);
    QVERIFY(fixture.service.busy());
    fixture.acknowledge(10, 100, 81, 50, 0.0);
    QTRY_COMPARE(fixture.service.selectedSourceId(), second);
    QCOMPARE(fixture.transmitted.last().msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    mavlink_param_request_read_t read{};
    mavlink_msg_param_request_read_decode(&fixture.transmitted.last(), &read);
    QCOMPARE(read.target_system, quint8(82));
    QCOMPARE(read.target_component, quint8(51));
}

void Px4FlowServiceTest::retiredSourceIsNotRedirectedToSuccessor()
{
    Fixture fixture;
    fixture.addSource(12, 120, 81, 50);
    fixture.addSource(13, 130, 82, 50);
    const QString selected = firstSourceId(fixture.service);
    fixture.service.selectSource(selected);
    fixture.service.activate();
    fixture.acknowledge(12, 120, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());
    const int transmissionsBeforeRetirement = fixture.transmitted.size();

    fixture.registry.endLinkSession(12, 120);
    QTRY_VERIFY(fixture.service.status().contains(
        QStringLiteral("retired"), Qt::CaseInsensitive));
    QCOMPARE(fixture.service.selectedSourceId(), selected);
    QVERIFY(!fixture.service.canToggle());
    QCOMPARE(fixture.transmitted.size(), transmissionsBeforeRetirement);
}

void Px4FlowServiceTest::sameEpochReplacementPacketsCannotCompleteOldFrame()
{
    Fixture fixture;
    fixture.addSource(15, 150, 81, 50);
    const QString original = firstSourceId(fixture.service);
    fixture.service.selectSource(original);
    fixture.service.activate();
    fixture.acknowledge(15, 150, 81, 50, 0.0);
    QTRY_VERIFY(fixture.service.canToggle());

    fixture.service.observeMessage(15, 150, handshake(81, 50, 2, 2, 2));
    fixture.service.observeMessage(
        15, 150, imagePacket(81, 50, 0, QByteArray::fromHex("0102")));
    QVERIFY(fixture.service.frame().isNull());

    fixture.now = MavlinkComponentRegistry::StaleAfterMs + 1;
    const mavlink_message_t replacementHandshake =
        handshake(81, 50, 2, 2, 2);
    fixture.registry.observeMessage(15, 150, replacementHandshake);
    fixture.service.observeMessage(15, 150, replacementHandshake);
    fixture.service.observeMessage(
        15, 150, imagePacket(81, 50, 0, QByteArray::fromHex("aabb")));
    fixture.service.observeMessage(
        15, 150, imagePacket(81, 50, 1, QByteArray::fromHex("ccdd")));
    QVERIFY(fixture.service.frame().isNull());
    QVERIFY(firstSourceId(fixture.service) != original);
}

void Px4FlowServiceTest::staleDiagnosticSurvivesNewHandshakesUntilCompletion()
{
    Px4FlowService service(nullptr, nullptr);
    service.beginReplay(21);
    service.observeReplay(21, handshake(81, 50, 2, 2, 4));
    QTRY_VERIFY_WITH_TIMEOUT(service.status().contains(
        QStringLiteral("stale"), Qt::CaseInsensitive), 4000);
    const QString stale = service.status();

    service.observeReplay(21, handshake(81, 50, 2, 2, 4));
    QCOMPARE(service.status(), stale);
    service.observeReplay(
        21, imagePacket(81, 50, 0, QByteArray::fromHex("01020304")));
    QVERIFY(!service.status().contains(
        QStringLiteral("stale"), Qt::CaseInsensitive));
    QCOMPARE(service.frame().size(), QSize(2, 2));
}

QTEST_MAIN(Px4FlowServiceTest)
#include "test_px4flowservice.moc"
