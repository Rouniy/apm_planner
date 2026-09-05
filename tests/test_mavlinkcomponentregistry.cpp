#include "comm/MavlinkComponentRegistry.h"
#include "comm/Px4FlowFrameAssembler.h"

#include <QPointer>
#include <QSignalSpy>
#include <QtTest/QTest>

#include <array>

namespace {

mavlink_message_t heartbeat(quint8 systemId, quint8 componentId,
                            quint8 type = MAV_TYPE_GENERIC,
                            quint8 autopilot = MAV_AUTOPILOT_GENERIC)
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

mavlink_message_t px4FlowHandshake(
        quint8 systemId, quint8 componentId, quint32 size = 4096,
        quint16 width = 64, quint16 height = 64,
        quint16 packets = 17, quint8 payload = 253,
        quint8 type = Px4FlowFrameAssembler::Raw8UStreamType)
{
    mavlink_data_transmission_handshake_t descriptor{};
    descriptor.type = type;
    descriptor.size = size;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.packets = packets;
    descriptor.payload = payload;
    descriptor.jpg_quality = 100;
    mavlink_message_t message{};
    mavlink_msg_data_transmission_handshake_encode(
            systemId, componentId, &message, &descriptor);
    return message;
}

mavlink_message_t encapsulatedData(quint8 systemId, quint8 componentId,
                                   quint16 sequence = 0)
{
    std::array<quint8, MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN> data{};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<quint8>(index);
    }
    mavlink_message_t message{};
    mavlink_msg_encapsulated_data_pack(
            systemId, componentId, &message, sequence, data.data());
    return message;
}

MavlinkComponentInstanceLease findLease(
        const MavlinkComponentRegistry &registry, int linkId,
        quint8 systemId, quint8 componentId)
{
    const auto components = registry.components();
    for (const auto &lease : components) {
        if (lease.endpoint.linkId == linkId
                && lease.endpoint.systemId == systemId
                && lease.endpoint.componentId == componentId) {
            return lease;
        }
    }
    return {};
}

} // namespace

class MavlinkComponentRegistryTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void configurableGenericHeartbeatAndGcsAreHandled();
    void validRaw8HandshakeAdmitsPx4FlowWithoutHeartbeat();
    void videoOnlyTrafficRenewsActivityWithoutHeartbeat();
    void heartbeatIdentityChangesRetireAndReAdmit();
    void linkEpochAndDuplicateEndpointIdentityRemainExact();
    void staleComponentReactivationGetsNewInstance();
    void capacityIsBoundedToSixtyFourComponents();
    void retirementMayReenterWithANewSession();
    void anotherLinkRetirementDoesNotSuppressThisSession();
    void staleRetirementMayReenterWithAReplacement();
    void retirementCallbackMayDeleteRegistry();
};

void MavlinkComponentRegistryTest::initTestCase()
{
    qRegisterMetaType<MavlinkComponentInstanceLease>();
}

void MavlinkComponentRegistryTest::configurableGenericHeartbeatAndGcsAreHandled()
{
    qint64 now = 100;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(4, 41);

    // PX4Flow's historical defaults are SYS_ID=81, SYS_COMP_ID=50, with a
    // generic heartbeat. Discovery must not require autopilot component 1.
    registry.observeMessage(4, 41, heartbeat(81, 50));
    registry.observeMessage(4, 41, heartbeat(42, 77));
    QCOMPARE(registry.components().size(), 2);
    QVERIFY(findLease(registry, 4, 81, 50).isValid());
    QVERIFY(findLease(registry, 4, 42, 77).isValid());

    registry.observeMessage(4, 41, heartbeat(43, 50, MAV_TYPE_GCS));
    registry.observeMessage(
            4, 41, heartbeat(44, MAV_COMP_ID_MISSIONPLANNER));
    registry.observeMessage(4, 41, heartbeat(0, 50));
    registry.observeMessage(4, 41, heartbeat(45, 0));
    QCOMPARE(registry.components().size(), 2);

    registry.observeMessage(4, 0, heartbeat(46, 50));
    registry.observeMessage(4, 40, heartbeat(47, 50));
    QCOMPARE(registry.components().size(), 2);
}

void MavlinkComponentRegistryTest::
validRaw8HandshakeAdmitsPx4FlowWithoutHeartbeat()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(7, 3);

    registry.observeMessage(7, 3, encapsulatedData(81, 50));
    QVERIFY(registry.components().isEmpty());

    registry.observeMessage(7, 3, px4FlowHandshake(81, 50));
    const auto lease = findLease(registry, 7, 81, 50);
    QVERIFY(lease.isValid());
    QCOMPARE(lease.linkSessionEpoch, quint64(3));

    registry.observeMessage(
            7, 3, px4FlowHandshake(82, 50, 4096, 64, 64, 17, 253, 0));
    registry.observeMessage(
            7, 3, px4FlowHandshake(83, 50, 4095, 64, 64, 17, 253));
    registry.observeMessage(
            7, 3, px4FlowHandshake(84, 50, 4096, 64, 64, 16, 253));
    registry.observeMessage(
            7, 3, px4FlowHandshake(85, 50, 4096, 64, 64, 17, 254));
    QCOMPARE(registry.components().size(), 1);
}

void MavlinkComponentRegistryTest::
videoOnlyTrafficRenewsActivityWithoutHeartbeat()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(2, 9);
    registry.observeMessage(2, 9, px4FlowHandshake(81, 50));
    const auto lease = findLease(registry, 2, 81, 50);
    QVERIFY(lease.isValid());

    now = MavlinkComponentRegistry::StaleAfterMs - 1;
    registry.observeMessage(2, 9, encapsulatedData(81, 50, 1));
    now = MavlinkComponentRegistry::StaleAfterMs + 1;
    registry.expireStale();
    QVERIFY(registry.validateLease(lease));

    now = 2 * MavlinkComponentRegistry::StaleAfterMs - 1;
    QVERIFY(!registry.validateLease(lease));
    QSignalSpy retired(&registry,
                       &MavlinkComponentRegistry::componentRetired);
    registry.expireStale();
    QCOMPARE(retired.count(), 1);
    QVERIFY(registry.components().isEmpty());
}

void MavlinkComponentRegistryTest::
heartbeatIdentityChangesRetireAndReAdmit()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(12, 8);
    registry.observeMessage(12, 8, px4FlowHandshake(81, 50));
    const auto imageOnly = findLease(registry, 12, 81, 50);
    QVERIFY(imageOnly.isValid());

    QSignalSpy retired(&registry,
                       &MavlinkComponentRegistry::componentRetired);
    registry.observeMessage(12, 8, heartbeat(81, 50));
    const auto metadataEstablished = findLease(registry, 12, 81, 50);
    QVERIFY(metadataEstablished.sameInstance(imageOnly));
    QCOMPARE(retired.count(), 0);

    registry.observeMessage(
            12, 8, heartbeat(81, 50, MAV_TYPE_QUADROTOR));
    const auto changedType = findLease(registry, 12, 81, 50);
    QVERIFY(changedType.isValid());
    QVERIFY(!changedType.sameInstance(metadataEstablished));
    QCOMPARE(retired.count(), 1);

    registry.observeMessage(
            12, 8, heartbeat(81, 50, MAV_TYPE_QUADROTOR,
                             MAV_AUTOPILOT_ARDUPILOTMEGA));
    const auto changedAutopilot = findLease(registry, 12, 81, 50);
    QVERIFY(changedAutopilot.isValid());
    QVERIFY(!changedAutopilot.sameInstance(changedType));
    QCOMPARE(retired.count(), 2);

    registry.observeMessage(12, 8, heartbeat(81, 50, MAV_TYPE_GCS));
    QVERIFY(!findLease(registry, 12, 81, 50).isValid());
    QCOMPARE(retired.count(), 3);
}

void MavlinkComponentRegistryTest::
linkEpochAndDuplicateEndpointIdentityRemainExact()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(1, 10);
    registry.beginLinkSession(2, 20);
    registry.observeMessage(1, 10, heartbeat(81, 50));
    registry.observeMessage(2, 20, heartbeat(81, 50));
    const auto first = findLease(registry, 1, 81, 50);
    const auto second = findLease(registry, 2, 81, 50);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(!first.sameInstance(second));
    QCOMPARE(registry.components().size(), 2);

    QSignalSpy retired(&registry,
                       &MavlinkComponentRegistry::componentRetired);
    registry.beginLinkSession(1, 11);
    QVERIFY(!registry.validateLease(first));
    QVERIFY(registry.validateLease(second));
    QCOMPARE(retired.count(), 1);

    registry.observeMessage(1, 10, heartbeat(81, 50));
    QVERIFY(!findLease(registry, 1, 81, 50).isValid());
    registry.observeMessage(1, 11, heartbeat(81, 50));
    const auto replacement = findLease(registry, 1, 81, 50);
    QVERIFY(replacement.isValid());
    QVERIFY(!replacement.sameInstance(first));
    QVERIFY(registry.validateLease(second));

    registry.endLinkSession(1, 10);
    QVERIFY(registry.validateLease(replacement));
    registry.endLinkSession(1, 11);
    QVERIFY(!registry.validateLease(replacement));
    QVERIFY(registry.validateLease(second));
}

void MavlinkComponentRegistryTest::staleComponentReactivationGetsNewInstance()
{
    qint64 now = 10;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(5, 6);
    registry.observeMessage(5, 6, heartbeat(25, 50));
    const auto original = findLease(registry, 5, 25, 50);
    QVERIFY(original.isValid());

    QSignalSpy retired(&registry,
                       &MavlinkComponentRegistry::componentRetired);
    now += MavlinkComponentRegistry::StaleAfterMs;
    registry.expireStale();
    QCOMPARE(retired.count(), 1);
    QVERIFY(!registry.validateLease(original));

    registry.observeMessage(5, 6, encapsulatedData(25, 50));
    QVERIFY(registry.components().isEmpty());
    registry.observeMessage(5, 6, px4FlowHandshake(25, 50));
    const auto replacement = findLease(registry, 5, 25, 50);
    QVERIFY(replacement.isValid());
    QVERIFY(!replacement.sameInstance(original));
    QVERIFY(replacement.instanceEpoch > original.instanceEpoch);
}

void MavlinkComponentRegistryTest::capacityIsBoundedToSixtyFourComponents()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(3, 1);

    for (int systemId = 1;
         systemId <= MavlinkComponentRegistry::MaximumComponents;
         ++systemId) {
        registry.observeMessage(
                3, 1, heartbeat(static_cast<quint8>(systemId), 50));
    }
    QCOMPARE(registry.components().size(),
             MavlinkComponentRegistry::MaximumComponents);
    registry.observeMessage(3, 1, heartbeat(65, 50));
    QCOMPARE(registry.components().size(),
             MavlinkComponentRegistry::MaximumComponents);
    QVERIFY(!findLease(registry, 3, 65, 50).isValid());

    now = MavlinkComponentRegistry::StaleAfterMs;
    registry.expireStale();
    QVERIFY(registry.components().isEmpty());
    registry.observeMessage(3, 1, heartbeat(65, 50));
    const auto admitted = findLease(registry, 3, 65, 50);
    QVERIFY(admitted.isValid());
    QVERIFY(admitted.instanceEpoch
            > quint64(MavlinkComponentRegistry::MaximumComponents));
}

void MavlinkComponentRegistryTest::retirementMayReenterWithANewSession()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(8, 1);
    registry.observeMessage(8, 1, heartbeat(10, 50));
    registry.observeMessage(8, 1, heartbeat(11, 50));

    MavlinkComponentInstanceLease reentrantLease;
    bool reentered = false;
    connect(&registry, &MavlinkComponentRegistry::componentRetired,
            &registry, [&](MavlinkComponentInstanceLease) {
        if (reentered) return;
        reentered = true;
        registry.beginLinkSession(8, 2);
        registry.observeMessage(8, 2, heartbeat(99, 50));
        reentrantLease = findLease(registry, 8, 99, 50);
    });

    // The callback's newer state must not be overwritten or orphaned when
    // this outer begin resumes after emitting retirement notifications.
    registry.beginLinkSession(8, 3);
    QVERIFY(reentrantLease.isValid());
    QVERIFY(registry.validateLease(reentrantLease));
    registry.observeMessage(8, 3, heartbeat(100, 50));
    QVERIFY(!findLease(registry, 8, 100, 50).isValid());
}

void MavlinkComponentRegistryTest::
staleRetirementMayReenterWithAReplacement()
{
    qint64 now = 0;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(9, 4);
    registry.observeMessage(9, 4, heartbeat(21, 50));
    const auto original = findLease(registry, 9, 21, 50);

    MavlinkComponentInstanceLease replacement;
    connect(&registry, &MavlinkComponentRegistry::componentRetired,
            &registry, [&](MavlinkComponentInstanceLease retired) {
        if (!retired.sameInstance(original) || replacement.isValid()) return;
        registry.observeMessage(9, 4, px4FlowHandshake(21, 50));
        replacement = findLease(registry, 9, 21, 50);
    });

    now = MavlinkComponentRegistry::StaleAfterMs;
    registry.expireStale();
    QVERIFY(replacement.isValid());
    QVERIFY(!replacement.sameInstance(original));
    QVERIFY(registry.validateLease(replacement));
    QCOMPARE(registry.components().size(), 1);
}

void MavlinkComponentRegistryTest::retirementCallbackMayDeleteRegistry()
{
    qint64 now = 0;
    QPointer<MavlinkComponentRegistry> registry =
            new MavlinkComponentRegistry(nullptr, [&now]() { return now; });
    registry->beginLinkSession(6, 1);
    registry->observeMessage(6, 1, heartbeat(30, 50));
    connect(registry.data(), &MavlinkComponentRegistry::componentRetired,
            registry.data(), [registry](MavlinkComponentInstanceLease) mutable {
        delete registry.data();
    });

    registry->endLinkSession(6, 1);
    QVERIFY(registry.isNull());
}

void MavlinkComponentRegistryTest::anotherLinkRetirementDoesNotSuppressThisSession()
{
    qint64 now = 100;
    MavlinkComponentRegistry registry(nullptr, [&now]() { return now; });
    registry.beginLinkSession(4, 41);
    registry.observeMessage(4, 41, heartbeat(81, 50));
    connect(&registry, &MavlinkComponentRegistry::componentRetired,
            &registry, [&registry](const MavlinkComponentInstanceLease &) {
        registry.beginLinkSession(5, 51);
        registry.observeMessage(5, 51, heartbeat(82, 50));
    });
    registry.beginLinkSession(4, 42);
    registry.observeMessage(4, 42, heartbeat(81, 50));
    QCOMPARE(registry.components().size(), 2);
    QVERIFY(findLease(registry, 4, 81, 50).isValid());
    QVERIFY(findLease(registry, 5, 82, 50).isValid());
}

QTEST_GUILESS_MAIN(MavlinkComponentRegistryTest)

#include "test_mavlinkcomponentregistry.moc"
