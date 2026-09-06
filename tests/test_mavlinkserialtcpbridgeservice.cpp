#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavlinkSerialTcpBridgeService.h"
#include "comm/SerialBridgeTcpServer.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>

#include <functional>
#include <cstring>
#include <memory>

namespace {
constexpr int LinkId = 74;
constexpr int SystemId = 37;
constexpr quint8 LocalSystemId = 252;
constexpr quint8 LocalComponentId = MAV_COMP_ID_MISSIONPLANNER;

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 2500)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    return condition();
}

mavlink_message_t heartbeat(
    int systemId, bool armed,
    MAV_AUTOPILOT autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, MAV_TYPE_QUADROTOR, autopilot,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t decode(const QByteArray &frame)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : frame) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_serial_control_t serialPayload(const mavlink_message_t &message)
{
    mavlink_serial_control_t payload{};
    mavlink_msg_serial_control_decode(&message, &payload);
    return payload;
}

mavlink_message_t reply(quint8 device, const QByteArray &bytes,
                        int systemId = SystemId,
                        int componentId = MAV_COMP_ID_AUTOPILOT1,
                        quint8 flags = SERIAL_CONTROL_FLAG_REPLY,
                        bool trimZeroPayload = false)
{
    QByteArray bounded = bytes.left(MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN);
    quint8 data[MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN]{};
    if (!bounded.isEmpty()) {
        std::memcpy(data, bounded.constData(), size_t(bounded.size()));
    }
    mavlink_message_t message{};
    mavlink_msg_serial_control_pack(
        static_cast<quint8>(systemId), static_cast<quint8>(componentId),
        &message, device, flags, 0, 0,
        static_cast<quint8>(bounded.size()), data);
    if (trimZeroPayload) message.len = 9;
    return message;
}

struct Fixture
{
    struct Sent
    {
        int linkId = -1;
        qint64 elapsedNs = 0;
        mavlink_message_t message{};
        mavlink_serial_control_t payload{};
    };

    Fixture()
        : registry([this] { return nowMs; })
        , transmitter([this](int linkId, const QByteArray &frame) {
              Sent sent;
              sent.linkId = linkId;
              sent.elapsedNs = wireClock.nsecsElapsed();
              sent.message = decode(frame);
              if (sent.message.msgid == MAVLINK_MSG_ID_SERIAL_CONTROL) {
                  sent.payload = serialPayload(sent.message);
              }
              transmissions.append(sent);
              if (writeHook) writeHook(sent);
              return writerResult;
          })
    {
        wireClock.start();
        epoch = registry.beginLinkSession(
            LinkId, QStringLiteral("Bridge test link"));
        transmitter.setLinkSessionEpoch(LinkId, epoch);
        QVERIFY(registry.observeMessage(
            LinkId, epoch, heartbeat(SystemId, false)));
        endpoint = registry.endpoints().constFirst();
        QVERIFY(targets.observeEndpoint(endpoint, true));
        targets.observeHeartbeat(endpoint, false,
                                 MAV_AUTOPILOT_ARDUPILOTMEGA,
                                 MAV_TYPE_QUADROTOR);
        service = new MavlinkSerialTcpBridgeService(
            &targets, &registry, &transmitter,
            LocalSystemId, LocalComponentId,
            [this](const SwarmVehicleInstanceLease &, QString *error) {
                if (routeHook) routeHook();
                if (!routeAllowed && error) {
                    *error = QStringLiteral("Injected route refusal.");
                }
                return routeAllowed;
            });
        QObject::connect(
            service, &MavlinkSerialTcpBridgeService::finished,
            service, [this](quint64 id, const QString &description) {
                finishedIds.append(id);
                finishedDescriptions.append(description);
            });
    }

    ~Fixture()
    {
        if (service) {
            service->shutdown();
            delete service.data();
        }
    }

    MavlinkSerialTcpBridgeService::Plan prepare(
        quint32 baud = 0, quint8 device = SERIAL_CONTROL_DEV_GPS1)
    {
        MavlinkSerialTcpBridgeService::Options options;
        options.listenPort = 0;
        options.baudRate = baud;
        options.device = device;
        MavlinkSerialTcpBridgeService::Plan plan;
        QString error;
        if (!service->prepare(options, &plan, &error)) {
            qWarning() << error;
        }
        return plan;
    }

    quint64 start(const MavlinkSerialTcpBridgeService::Plan &plan)
    {
        quint64 id = 0;
        QString error;
        if (!service->start(plan, &id, &error)) {
            qWarning() << error;
            return 0;
        }
        if (!waitFor([this] { return service && service->boundPort() != 0; })) {
            return 0;
        }
        return id;
    }

    int countFrames(const std::function<bool(const Sent &)> &predicate) const
    {
        int count = 0;
        for (const Sent &sent : transmissions) {
            if (predicate(sent)) ++count;
        }
        return count;
    }

    qint64 nowMs = 1000;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QList<Sent> transmissions;
    std::function<void(const Sent &)> writeHook;
    bool writerResult = true;
    QElapsedTimer wireClock;
    ExactLinkTransmitter transmitter;
    QPointer<MavlinkSerialTcpBridgeService> service;
    QList<quint64> finishedIds;
    QStringList finishedDescriptions;
    std::function<void()> routeHook;
    bool routeAllowed = true;
    quint64 epoch = 0;
    VehicleEndpoint endpoint;
};
}

class MavlinkSerialTcpBridgeServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void preparePinsExactSingleSystemAndOptions();
    void prepareAcceptsNonArduPilotAutopilot();
    void listenerTouchesNoUartUntilClientAndBridgesBothWays();
    void clientDisconnectReleasesAndKeepsListener();
    void gracefulDisconnectDrainsAcceptedTailBeforeRelease();
    void dataWritesRemainPacedAcrossReentrantEvents();
    void armStopsAndReleasesOnlyOriginalSession();
    void staleHeartbeatStillReleasesOriginalInstance();
    void staleHeartbeatWithRefusedRouteDoesNotRelease();
    void retiredInstanceDoesNotReceiveRelease();
    void responsesAreStrictlyFiltered();
    void startPublishesOwnershipBeforeCallbacks();
    void terminalNotificationsKeepAdmissionBarrier();
    void deletionFromFrameWriterIsSafe();
    void deletionFromFinishedObserverIsSafe();
    void abruptDestructionDoesNotBypassRouteForRelease();
    void deletionDuringReleaseValidationIsSafe();
};

void MavlinkSerialTcpBridgeServiceTest::preparePinsExactSingleSystemAndOptions()
{
    Fixture fixture;
    const auto plan = fixture.prepare(57600);
    QVERIFY(plan.isValid());
    QCOMPARE(plan.options().device, quint8(SERIAL_CONTROL_DEV_GPS1));
    QCOMPARE(plan.options().baudRate, quint32(57600));
    QCOMPARE(plan.options().listenPort, quint16(0));
    QCOMPARE(plan.target().endpoint.linkId, LinkId);
    QCOMPARE(plan.vehicle().linkSessionEpoch, fixture.epoch);
    QVERIFY(plan.description().contains(QStringLiteral("GPS1")));
    QVERIFY(plan.description().contains(QStringLiteral("Link 74")));
    QVERIFY(plan.description().contains(QStringLiteral("37/1")));
    QString error;
    QVERIFY2(fixture.service->validate(plan, &error), qPrintable(error));

    MavlinkSerialTcpBridgeService::Options invalid;
    invalid.device = 4;
    MavlinkSerialTcpBridgeService::Plan rejected;
    QVERIFY(!fixture.service->prepare(invalid, &rejected, &error));
    QVERIFY(!rejected.isValid());

    VehicleEndpoint second;
    second.linkId = LinkId;
    second.systemId = SystemId + 1;
    second.componentId = MAV_COMP_ID_CAMERA;
    second.linkName = QStringLiteral("Bridge test link");
    QVERIFY(fixture.targets.observeEndpoint(second));
    QVERIFY(!fixture.service->validate(plan, &error));
    QVERIFY(error.contains(QStringLiteral("one"), Qt::CaseInsensitive));
}

void MavlinkSerialTcpBridgeServiceTest::
prepareAcceptsNonArduPilotAutopilot()
{
    Fixture fixture;
    QVERIFY(fixture.registry.observeMessage(
        LinkId, fixture.epoch,
        heartbeat(SystemId, false, MAV_AUTOPILOT_PX4)));
    fixture.targets.observeHeartbeat(
        fixture.endpoint, false, MAV_AUTOPILOT_PX4,
        MAV_TYPE_QUADROTOR);

    const auto plan = fixture.prepare();
    QVERIFY(plan.isValid());
}

void MavlinkSerialTcpBridgeServiceTest::
listenerTouchesNoUartUntilClientAndBridgesBothWays()
{
    Fixture fixture;
    const auto plan = fixture.prepare(57600);
    const quint64 id = fixture.start(plan);
    QVERIFY(id != 0);
    QCOMPARE(fixture.transmissions.size(), 0);

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));
    QVERIFY(waitFor([&] {
        return fixture.countFrames([](const Fixture::Sent &sent) {
            return sent.payload.flags
                == (SERIAL_CONTROL_FLAG_EXCLUSIVE
                    | SERIAL_CONTROL_FLAG_RESPOND
                    | SERIAL_CONTROL_FLAG_MULTI)
                && sent.payload.timeout == 100
                && sent.payload.baudrate == 57600;
        }) >= 1;
    }));

    QByteArray tcpBytes(281, '\0');
    for (int i = 0; i < tcpBytes.size(); ++i) {
        tcpBytes[i] = static_cast<char>(i & 0xff);
    }
    QCOMPARE(client.write(tcpBytes), qint64(tcpBytes.size()));
    QVERIFY(client.waitForBytesWritten(1000));
    QVERIFY(waitFor([&] { return fixture.service->bytesFromTcp() == 281; }));
    QList<Fixture::Sent> dataFrames;
    for (const Fixture::Sent &sent : fixture.transmissions) {
        if (sent.payload.count > 0) dataFrames.append(sent);
    }
    QCOMPARE(dataFrames.size(), 5);
    QCOMPARE(dataFrames.at(0).payload.count, quint8(70));
    QCOMPARE(dataFrames.at(1).payload.count, quint8(70));
    QCOMPARE(dataFrames.at(2).payload.count, quint8(70));
    QCOMPARE(dataFrames.at(3).payload.count, quint8(70));
    QCOMPARE(dataFrames.at(4).payload.count, quint8(1));
    QVERIFY(dataFrames.at(3).payload.flags & SERIAL_CONTROL_FLAG_RESPOND);
    QVERIFY(dataFrames.at(4).payload.flags & SERIAL_CONTROL_FLAG_RESPOND);

    // MAVLink 2 may omit the zero-filled data tail even when count describes
    // logical zero bytes. The service materializes the decoded zero tail.
    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS1, QByteArray(4, '\0'),
              SystemId, MAV_COMP_ID_AUTOPILOT1,
              SERIAL_CONTROL_FLAG_REPLY, true));
    QVERIFY(waitFor([&] { return client.bytesAvailable() == 4; }));
    QCOMPARE(client.readAll(), QByteArray(4, '\0'));
    QVERIFY(waitFor([&] { return fixture.service->bytesToTcp() == 4; }));

    QVERIFY(fixture.service->stop(id));
    QVERIFY(!fixture.service->busy());
    QCOMPARE(fixture.finishedIds, QList<quint64>{id});
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 1);
}

void MavlinkSerialTcpBridgeServiceTest::clientDisconnectReleasesAndKeepsListener()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    quint16 port = fixture.service->boundPort();
    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(first.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));
    first.disconnectFromHost();
    QVERIFY(first.waitForDisconnected(1000)
            || first.state() == QAbstractSocket::UnconnectedState);
    QVERIFY(waitFor([&] {
        return fixture.service->busy() && !fixture.service->hasClient();
    }));
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 1);
    QCOMPARE(fixture.service->boundPort(), port);

    QTcpSocket second;
    second.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(second.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));
    QVERIFY(fixture.service->stop(id));
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 2);
}

void MavlinkSerialTcpBridgeServiceTest::
gracefulDisconnectDrainsAcceptedTailBeforeRelease()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    QByteArray expected(281, '\0');
    for (int i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<char>((i * 17) & 0xff);
    }
    QCOMPARE(client.write(expected), qint64(expected.size()));
    client.disconnectFromHost();

    QVERIFY(waitFor([&] {
        return fixture.service->bytesFromTcp() == quint64(expected.size())
            && fixture.service->busy()
            && !fixture.service->hasClient();
    }));
    QByteArray forwarded;
    for (const Fixture::Sent &sent : fixture.transmissions) {
        if (sent.payload.count > 0) {
            forwarded.append(
                reinterpret_cast<const char *>(sent.payload.data),
                sent.payload.count);
        }
    }
    QCOMPARE(forwarded, expected);
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 1);
    QVERIFY(fixture.service->stop(id));
}

void MavlinkSerialTcpBridgeServiceTest::
dataWritesRemainPacedAcrossReentrantEvents()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    bool injected = false;
    bool routePumpedEvents = false;
    fixture.routeHook = [&] {
        if (routePumpedEvents) return;
        routePumpedEvents = true;
        auto *server = fixture.service
            ? fixture.service->findChild<SerialBridgeTcpServer *>()
            : nullptr;
        if (server) {
            QMetaObject::invokeMethod(
                server, "readyRead", Qt::DirectConnection);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    };
    fixture.writeHook = [&](const Fixture::Sent &sent) {
        if (sent.payload.count == 0 || injected) return;
        injected = true;
        QCOMPARE(client.write(QByteArray(71, 'z')), qint64(71));
        client.flush();
        // Deliver readyRead and any zero-delay timer nested inside the frame
        // writer. The monotonic guard must still enforce the full interval.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    };
    QCOMPARE(client.write(QByteArray(140, 'a')), qint64(140));
    client.flush();
    QVERIFY(waitFor([&] {
        return fixture.service->bytesFromTcp() == 211;
    }));
    QVERIFY(routePumpedEvents);

    QList<qint64> dataWriteTimes;
    for (const Fixture::Sent &sent : fixture.transmissions) {
        if (sent.payload.count > 0) dataWriteTimes.append(sent.elapsedNs);
    }
    QVERIFY(dataWriteTimes.size() >= 4);
    for (int i = 1; i < dataWriteTimes.size(); ++i) {
        QVERIFY2(dataWriteTimes.at(i) - dataWriteTimes.at(i - 1)
                     >= qint64(10) * 1000000,
                 "SERIAL_CONTROL data writes were less than 10 ms apart");
    }
    QVERIFY(fixture.service->stop(id));
}

void MavlinkSerialTcpBridgeServiceTest::armStopsAndReleasesOnlyOriginalSession()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    QVERIFY(fixture.registry.observeMessage(
        LinkId, fixture.epoch, heartbeat(SystemId, true)));
    fixture.targets.observeHeartbeat(
        fixture.endpoint, true, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR);
    QVERIFY(waitFor([&] { return !fixture.service->busy(); }));
    QCOMPARE(fixture.finishedIds, QList<quint64>{id});
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.linkId == LinkId && sent.payload.flags == 0;
    }), 1);
}

void MavlinkSerialTcpBridgeServiceTest::
staleHeartbeatStillReleasesOriginalInstance()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    fixture.nowMs += 4000;
    QVERIFY(waitFor([&] { return !fixture.service->busy(); }));
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 1);
    QVERIFY(!fixture.finishedDescriptions.isEmpty());
    QVERIFY(fixture.finishedDescriptions.constLast().contains(
        QStringLiteral("release was submitted"), Qt::CaseInsensitive));
    QCOMPARE(fixture.finishedIds.constLast(), id);
}

void MavlinkSerialTcpBridgeServiceTest::
staleHeartbeatWithRefusedRouteDoesNotRelease()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    fixture.routeAllowed = false;
    fixture.nowMs += 4000;
    QVERIFY(waitFor([&] { return !fixture.service->busy(); }));
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 0);
    QVERIFY(!fixture.finishedDescriptions.isEmpty());
    QVERIFY(fixture.finishedDescriptions.constLast().contains(
        QStringLiteral("not submitted"), Qt::CaseInsensitive));
    QCOMPARE(fixture.finishedIds.constLast(), id);
}

void MavlinkSerialTcpBridgeServiceTest::
retiredInstanceDoesNotReceiveRelease()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    QVERIFY(fixture.registry.endLinkSession(LinkId, fixture.epoch));
    QVERIFY(waitFor([&] { return !fixture.service->busy(); }));
    QCOMPARE(fixture.countFrames([](const Fixture::Sent &sent) {
        return sent.payload.flags == 0;
    }), 0);
    QVERIFY(!fixture.finishedDescriptions.isEmpty());
    QVERIFY(fixture.finishedDescriptions.constLast().contains(
        QStringLiteral("not submitted"), Qt::CaseInsensitive));
    QCOMPARE(fixture.finishedIds.constLast(), id);
}

void MavlinkSerialTcpBridgeServiceTest::responsesAreStrictlyFiltered()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS1, QByteArray("wrong system"),
              SystemId + 1));
    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS1, QByteArray("wrong component"),
              SystemId, MAV_COMP_ID_LOG));
    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS2, QByteArray("wrong device")));
    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS1, QByteArray("not reply"),
              SystemId, MAV_COMP_ID_AUTOPILOT1,
              SERIAL_CONTROL_FLAG_RESPOND));
    QTest::qWait(30);
    QCOMPARE(client.bytesAvailable(), qint64(0));

    fixture.service->observeMessage(
        LinkId, fixture.epoch,
        reply(SERIAL_CONTROL_DEV_GPS1, QByteArray("accepted")));
    QVERIFY(waitFor([&] { return client.bytesAvailable() == 8; }));
    QCOMPARE(client.readAll(), QByteArray("accepted"));
    QVERIFY(fixture.service->stop(id));
}

void MavlinkSerialTcpBridgeServiceTest::startPublishesOwnershipBeforeCallbacks()
{
    Fixture fixture;
    const auto plan = fixture.prepare();
    quint64 observedId = 0;
    QObject::connect(
        fixture.service, &MavlinkSerialTcpBridgeService::stateChanged,
        fixture.service, [&] {
            if (!observedId && fixture.service
                && fixture.service->status().contains(
                    QStringLiteral("Validating"))) {
                observedId = fixture.service->operationId();
                QVERIFY(observedId != 0);
                QVERIFY(fixture.service->stop(observedId));
            }
        });
    quint64 returnedId = 0;
    QString error;
    QVERIFY(!fixture.service->start(plan, &returnedId, &error));
    QCOMPARE(returnedId, observedId);
    QVERIFY(!fixture.service->busy());
    QCOMPARE(fixture.transmissions.size(), 0);
    QCOMPARE(fixture.finishedIds, QList<quint64>{observedId});
}

void MavlinkSerialTcpBridgeServiceTest::
terminalNotificationsKeepAdmissionBarrier()
{
    Fixture fixture;
    const auto plan = fixture.prepare();
    const quint64 firstId = fixture.start(plan);
    QVERIFY(firstId != 0);
    bool busyNotificationRejected = false;
    bool finishedNotificationRejected = false;
    quint64 successorId = 0;

    QObject::connect(
        fixture.service, &MavlinkSerialTcpBridgeService::stateChanged,
        fixture.service, [&] {
            if (!fixture.service || fixture.service->operationId() != 0)
                return;
            if (fixture.service->busy()) {
                quint64 rejectedId = 0;
                QString error;
                busyNotificationRejected =
                    !fixture.service->start(plan, &rejectedId, &error)
                    && rejectedId == 0;
            } else if (successorId == 0) {
                QString error;
                QVERIFY2(fixture.service->start(
                             plan, &successorId, &error),
                         qPrintable(error));
            }
        });
    QObject::connect(
        fixture.service, &MavlinkSerialTcpBridgeService::finished,
        fixture.service, [&] (quint64 id, const QString &) {
            if (id != firstId) return;
            quint64 rejectedId = 0;
            QString error;
            finishedNotificationRejected =
                !fixture.service->start(plan, &rejectedId, &error)
                && rejectedId == 0;
        });

    QVERIFY(fixture.service->stop(firstId));
    QVERIFY(busyNotificationRejected);
    QVERIFY(finishedNotificationRejected);
    QVERIFY(successorId != 0);
    QCOMPARE(fixture.service->operationId(), successorId);
    QVERIFY(fixture.service->stop(successorId));
}

void MavlinkSerialTcpBridgeServiceTest::deletionFromFrameWriterIsSafe()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    fixture.writeHook = [&fixture](const Fixture::Sent &) {
        fixture.writeHook = {};
        delete fixture.service.data();
    };

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return !fixture.service; }));
    QCOMPARE(fixture.transmissions.size(), 1);
    QCOMPARE(fixture.transmissions.constFirst().payload.flags,
             quint8(SERIAL_CONTROL_FLAG_EXCLUSIVE
                    | SERIAL_CONTROL_FLAG_RESPOND
                    | SERIAL_CONTROL_FLAG_MULTI));
}

void MavlinkSerialTcpBridgeServiceTest::deletionFromFinishedObserverIsSafe()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    QObject::connect(
        fixture.service, &MavlinkSerialTcpBridgeService::finished,
        &fixture.targets, [&fixture](quint64, const QString &) {
            delete fixture.service.data();
        });

    QVERIFY(!fixture.service->stop(id));
    QVERIFY(!fixture.service);
}

void MavlinkSerialTcpBridgeServiceTest::
abruptDestructionDoesNotBypassRouteForRelease()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    const int beforeDelete = fixture.transmissions.size();
    delete fixture.service.data();
    QVERIFY(!fixture.service);
    QCOMPARE(fixture.transmissions.size(), beforeDelete);
    QVERIFY(waitFor([&] {
        return client.state() == QAbstractSocket::UnconnectedState;
    }));
}

void MavlinkSerialTcpBridgeServiceTest::
deletionDuringReleaseValidationIsSafe()
{
    Fixture fixture;
    const quint64 id = fixture.start(fixture.prepare());
    QVERIFY(id != 0);
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost,
                         fixture.service->boundPort());
    QVERIFY(client.waitForConnected(1000));
    QVERIFY(waitFor([&] { return fixture.service->hasClient(); }));

    const int beforeStop = fixture.transmissions.size();
    fixture.routeHook = [&fixture] {
        fixture.routeHook = {};
        delete fixture.service.data();
    };
    QVERIFY(!fixture.service->stop(id));
    QVERIFY(!fixture.service);
    QCOMPARE(fixture.transmissions.size(), beforeStop);
}

QTEST_GUILESS_MAIN(MavlinkSerialTcpBridgeServiceTest)
#include "test_mavlinkserialtcpbridgeservice.moc"
