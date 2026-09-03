#include <QtTest>

#include "comm/CotOutputService.h"

#include <QPointer>

#include <functional>
#include <memory>

namespace {

struct TransportHarness
{
    CotOutputTransport *transport = nullptr;
    QList<QByteArray> payloads;
    CotOutputTransport::SendResult nextResult =
        CotOutputTransport::SendResult::Sent;
    std::function<void()> duringSend;
};

struct Fixture
{
    std::shared_ptr<TransportHarness> harness =
        std::make_shared<TransportHarness>();
    std::shared_ptr<int> clockCalls = std::make_shared<int>(0);

    CotOutputService::TransportFactory factory()
    {
        return [state = harness](
                   const CotOutputTransport::Settings &settings,
                   QObject *parent) {
            auto transport = std::unique_ptr<CotOutputTransport>(
                new CotOutputTransport(settings, parent));
            state->transport = transport.get();
            return transport;
        };
    }

    CotOutputService::Sender sender()
    {
        return [state = harness](CotOutputTransport *,
                                 const QByteArray &payload) {
            state->payloads.append(payload);
            const CotOutputTransport::SendResult result = state->nextResult;
            state->nextResult = CotOutputTransport::SendResult::Sent;
            if (state->duringSend) {
                const std::function<void()> callback = state->duringSend;
                state->duringSend = std::function<void()>();
                callback();
            }
            return result;
        };
    }

    CotOutputService::Clock clock()
    {
        return [calls = clockCalls]() {
            ++*calls;
            return QDateTime(QDate(2031, 4, 5), QTime(6, 7, 8, 900),
                             Qt::UTC);
        };
    }
};

VehicleEndpoint endpoint(int linkId, int systemId, int componentId,
                         const QString &linkName = QStringLiteral("Radio"))
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = linkName;
    return result;
}

CotOutputTransport::Settings readyTransport()
{
    CotOutputTransport::Settings settings =
        CotOutputTransport::Defaults(CotOutputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 14551;
    return settings;
}

CotOutputTransport::Settings listeningTransport(
    CotOutputTransport::Mode mode)
{
    CotOutputTransport::Settings settings =
        CotOutputTransport::Defaults(mode);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    return settings;
}

mavlink_message_t globalPosition(int systemId, int componentId,
                                 int latitude = 481173000,
                                 int longitude = 115166667)
{
    mavlink_global_position_int_t position{};
    position.lat = latitude;
    position.lon = longitude;
    position.alt = 545400;
    position.vx = 300;
    position.vy = 400;
    position.hdg = 8440;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<uint8_t>(systemId),
        static_cast<uint8_t>(componentId), &message, &position);
    return message;
}

mavlink_message_t gpsRaw(int systemId, int componentId)
{
    mavlink_gps_raw_int_t gps{};
    gps.lat = 0;
    gps.lon = 0;
    gps.alt = 123400;
    gps.vel = 250;
    gps.cog = 1234;
    mavlink_message_t message{};
    mavlink_msg_gps_raw_int_encode(static_cast<uint8_t>(systemId),
                                   static_cast<uint8_t>(componentId),
                                   &message, &gps);
    return message;
}

mavlink_message_t vfrHud(int systemId, int componentId)
{
    mavlink_vfr_hud_t hud{};
    hud.alt = 99.5f;
    hud.groundspeed = 8.25f;
    hud.heading = 271;
    mavlink_message_t message{};
    mavlink_msg_vfr_hud_encode(static_cast<uint8_t>(systemId),
                               static_cast<uint8_t>(componentId),
                               &message, &hud);
    return message;
}

mavlink_message_t highLatency(int systemId, int componentId)
{
    mavlink_high_latency_t state{};
    state.latitude = 350000000;
    state.longitude = 330000000;
    state.altitude_amsl = 1234;
    state.groundspeed = 8;
    state.heading = 12345;
    mavlink_message_t message{};
    mavlink_msg_high_latency_encode(static_cast<uint8_t>(systemId),
                                    static_cast<uint8_t>(componentId),
                                    &message, &state);
    return message;
}

mavlink_message_t highLatency2(int systemId, int componentId)
{
    mavlink_high_latency2_t state{};
    state.latitude = -350000000;
    state.longitude = -330000000;
    state.altitude = 432;
    state.groundspeed = 42;
    state.heading = 123;
    mavlink_message_t message{};
    mavlink_msg_high_latency2_encode(static_cast<uint8_t>(systemId),
                                     static_cast<uint8_t>(componentId),
                                     &message, &state);
    return message;
}

CotOutputServiceSettings serviceSettings(
    const CotOutputTransport::Settings &transport = readyTransport())
{
    CotOutputServiceSettings settings;
    settings.transport = transport;
    return settings;
}

QString payloadText(const QByteArray &payload)
{
    return QString::fromUtf8(payload);
}

} // namespace

class CotOutputServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void pinsPhysicalLinkFiltersExactlyAndSortsEndpoints();
    void appliesCanonicalSiMessagesAndRequiresPosition();
    void updatesPreviewWithoutUdpOrTcpHostPeer();
    void appliesLiveEventIdentityAndIntervalSettings();
    void rejectsInvalidXmlWithoutSendingAnEmptyPayload();
    void boundsDropsErrorsAndStopsExplicitly();
    void handlesReentrantStopAndDestruction();
};

void CotOutputServiceTest::pinsPhysicalLinkFiltersExactlyAndSortsEndpoints()
{
    Fixture fixture;
    CotOutputService service(fixture.factory(), fixture.sender(),
                             fixture.clock());
    service.updateEndpoints({endpoint(8, 9, 1), endpoint(7, 1, 1),
                             endpoint(8, 7, 42), endpoint(8, 7, 1),
                             endpoint(8, 7, 1)});

    CotOutputServiceSettings settings = serviceSettings();
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("shared-system-7");
    settings.identities.insert(7, identity);
    settings.event.callsign = QStringLiteral("Борт");
    QVERIFY(service.start(8, QStringLiteral("Pinned Radio"), settings));
    QCOMPARE(service.linkId(), 8);
    QCOMPARE(service.linkName(), QStringLiteral("Pinned Radio"));
    QCOMPARE(service.activeEndpoints().size(), 3);
    QCOMPARE(service.activeEndpoints().at(0), endpoint(8, 7, 1));
    QCOMPARE(service.activeEndpoints().at(1), endpoint(8, 7, 42));
    QCOMPARE(service.activeEndpoints().at(2), endpoint(8, 9, 1));
    QCOMPARE(service.status().ticks, quint64(1));
    QVERIFY(fixture.harness->payloads.isEmpty());

    service.observeMessage(7, globalPosition(7, 1));
    service.observeMessage(8, globalPosition(7, 2));
    service.observeMessage(8, globalPosition(8, 1));
    service.emitNow();
    QVERIFY(fixture.harness->payloads.isEmpty());

    service.observeMessage(8, globalPosition(9, 1));
    service.observeMessage(8, globalPosition(7, 42));
    service.observeMessage(8, globalPosition(7, 1));
    const int clockCallsBeforeTick = *fixture.clockCalls;
    service.emitNow();
    QCOMPARE(*fixture.clockCalls, clockCallsBeforeTick + 1);
    QCOMPARE(fixture.harness->payloads.size(), 3);
    QVERIFY(payloadText(fixture.harness->payloads.at(0))
                .contains(QStringLiteral("uid=\"shared-system-7\"")));
    QVERIFY(payloadText(fixture.harness->payloads.at(1))
                .contains(QStringLiteral("uid=\"shared-system-7\"")));
    QVERIFY(payloadText(fixture.harness->payloads.at(2))
                .contains(QStringLiteral("uid=\"MissionPlanner-9-1\"")));
    for (const QByteArray &payload : fixture.harness->payloads) {
        QVERIFY(payload.endsWith('\n'));
        QCOMPARE(QString::fromUtf8(payload).toUtf8(), payload);
        QVERIFY(payloadText(payload).contains(
            QStringLiteral("time=\"2031-04-05T06:07:08.900Z\"")));
    }
}

void CotOutputServiceTest::appliesCanonicalSiMessagesAndRequiresPosition()
{
    Fixture fixture;
    CotOutputService service(fixture.factory(), fixture.sender(),
                             fixture.clock());
    service.updateEndpoints({endpoint(5, 42, 1)});
    QVERIFY(service.start(5, QStringLiteral("Link"), serviceSettings()));

    service.observeMessage(5, globalPosition(42, 1, 0, 0));
    service.emitNow();
    QVERIFY(fixture.harness->payloads.isEmpty());

    service.observeMessage(5, gpsRaw(42, 1));
    service.emitNow();
    QString xml = payloadText(fixture.harness->payloads.takeLast());
    QVERIFY(xml.contains(QStringLiteral("lat=\"0.0000000\"")));
    QVERIFY(xml.contains(QStringLiteral("lon=\"0.0000000\"")));
    QVERIFY(xml.contains(QStringLiteral("hae=\"123.40\"")));
    QVERIFY(xml.contains(QStringLiteral("course=\"12.34\"")));
    QVERIFY(xml.contains(QStringLiteral("speed=\"2.50\"")));

    service.observeMessage(5, vfrHud(42, 1));
    service.emitNow();
    xml = payloadText(fixture.harness->payloads.takeLast());
    QVERIFY(xml.contains(QStringLiteral("hae=\"99.50\"")));
    QVERIFY(xml.contains(QStringLiteral("course=\"271.00\"")));
    QVERIFY(xml.contains(QStringLiteral("speed=\"8.25\"")));

    service.observeMessage(5, highLatency(42, 1));
    service.emitNow();
    xml = payloadText(fixture.harness->payloads.takeLast());
    QVERIFY(xml.contains(QStringLiteral("lat=\"35.0000000\"")));
    QVERIFY(xml.contains(QStringLiteral("hae=\"1234.00\"")));
    // Intentional fix: HIGH_LATENCY heading is a centidegree CoT course.
    QVERIFY(xml.contains(QStringLiteral("course=\"123.45\"")));
    QVERIFY(xml.contains(QStringLiteral("speed=\"8.00\"")));

    service.observeMessage(5, highLatency2(42, 1));
    service.emitNow();
    xml = payloadText(fixture.harness->payloads.takeLast());
    QVERIFY(xml.contains(QStringLiteral("lat=\"-35.0000000\"")));
    QVERIFY(xml.contains(QStringLiteral("hae=\"432.00\"")));
    // Intentional fix: HIGH_LATENCY2 heading is encoded in half-degrees.
    QVERIFY(xml.contains(QStringLiteral("course=\"246.00\"")));
    QVERIFY(xml.contains(QStringLiteral("speed=\"8.40\"")));

    service.observeMessage(5, globalPosition(42, 1));
    service.emitNow();
    xml = payloadText(fixture.harness->payloads.takeLast());
    QVERIFY(xml.contains(QStringLiteral("hae=\"545.40\"")));
    QVERIFY(xml.contains(QStringLiteral("course=\"84.40\"")));
    QVERIFY(xml.contains(QStringLiteral("speed=\"5.00\"")));
}

void CotOutputServiceTest::updatesPreviewWithoutUdpOrTcpHostPeer()
{
    const QList<CotOutputTransport::Mode> modes = {
        CotOutputTransport::Mode::UdpHost,
        CotOutputTransport::Mode::TcpHost
    };
    for (CotOutputTransport::Mode mode : modes) {
        Fixture fixture;
        CotOutputService service(fixture.factory(), fixture.sender(),
                                 fixture.clock());
        service.updateEndpoints({endpoint(6, 43, 1)});
        QVERIFY(service.start(
            6, QStringLiteral("Host Link"),
            serviceSettings(listeningTransport(mode))));
        QCOMPARE(service.state(), CotOutputService::State::Emitting);
        QCOMPARE(service.status().transportState,
                 CotOutputTransport::State::Listening);
        service.observeMessage(6, globalPosition(43, 1));
        service.emitNow();
        QVERIFY(fixture.harness->payloads.isEmpty());
        QVERIFY(service.status().preview.startsWith(
            QStringLiteral("<event")));
        QCOMPARE(service.status().eventsWithoutPeer, quint64(1));
        QCOMPARE(service.status().eventsDropped, quint64(0));
    }
}

void CotOutputServiceTest::appliesLiveEventIdentityAndIntervalSettings()
{
    Fixture fixture;
    CotOutputService service(fixture.factory(), fixture.sender(),
                             fixture.clock());
    service.updateEndpoints({endpoint(3, 10, 1)});
    QVERIFY(service.start(3, QStringLiteral("Link"), serviceSettings()));
    service.observeMessage(3, globalPosition(10, 1));

    CotEventSettings event = service.settings().event;
    event.eventType = QStringLiteral("a-f-G-U-C");
    event.uidPrefix = QStringLiteral("Live");
    event.callsign = QStringLiteral("LiveCall");
    service.setEventSettings(event);
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("live-uid");
    identity.includeTakv = true;
    service.setIdentityOverrides({{10, identity}});
    service.emitNow();
    const QString xml = payloadText(fixture.harness->payloads.last());
    QVERIFY(xml.contains(QStringLiteral("type=\"a-f-G-U-C\"")));
    QVERIFY(xml.contains(QStringLiteral("uid=\"live-uid\"")));
    QVERIFY(xml.contains(QStringLiteral("<takv />")));

    QVERIFY(service.setUpdateIntervalSeconds(0.1));
    QCOMPARE(service.timerIntervalMs(), 100);
    QVERIFY(service.setUpdateIntervalSeconds(3600.0));
    QCOMPARE(service.timerIntervalMs(), 3600000);
    QVERIFY(!service.setUpdateIntervalSeconds(0.09));
    QVERIFY(!service.setUpdateIntervalSeconds(3600.1));
    QCOMPARE(service.timerIntervalMs(), 3600000);

    service.updateEndpoints({});
    QVERIFY(service.isRunning());
    service.emitNow();
    QVERIFY(service.status().preview.isEmpty());
}

void CotOutputServiceTest::rejectsInvalidXmlWithoutSendingAnEmptyPayload()
{
    Fixture fixture;
    CotOutputService service(fixture.factory(), fixture.sender(),
                             fixture.clock());
    service.updateEndpoints({endpoint(3, 10, 1)});
    QVERIFY(service.start(3, QStringLiteral("Link"), serviceSettings()));
    service.observeMessage(3, globalPosition(10, 1));

    CotEventSettings invalid = service.settings().event;
    invalid.eventType.append(QChar(0x0001));
    service.setEventSettings(invalid);
    service.emitNow();
    QVERIFY(fixture.harness->payloads.isEmpty());
    QCOMPARE(service.status().eventsGenerated, quint64(0));
    QCOMPARE(service.status().eventsDropped, quint64(1));
    QCOMPARE(service.status().errors, quint64(1));
    QVERIFY(service.status().lastError.contains(QStringLiteral("XML 1.0")));
    QVERIFY(service.status().text.contains(QStringLiteral("Warning")));

    invalid.eventType = QStringLiteral("a-f-A-M-F-Q");
    service.setEventSettings(invalid);
    service.emitNow();
    QCOMPARE(fixture.harness->payloads.size(), 1);
    QVERIFY(fixture.harness->payloads.first().startsWith("<event"));
}

void CotOutputServiceTest::boundsDropsErrorsAndStopsExplicitly()
{
    Fixture fixture;
    CotOutputService service(fixture.factory(), fixture.sender(),
                             fixture.clock());
    service.updateEndpoints({endpoint(12, 50, 1)});
    QVERIFY(service.start(12, QStringLiteral("Original Link"),
                          serviceSettings()));
    service.observeMessage(12, globalPosition(50, 1));

    fixture.harness->nextResult =
        CotOutputTransport::SendResult::Backpressure;
    service.emitNow();
    QCOMPARE(service.status().eventsDropped, quint64(1));
    fixture.harness->nextResult = CotOutputTransport::SendResult::IoError;
    service.emitNow();
    QCOMPARE(service.status().eventsDropped, quint64(2));
    QCOMPARE(service.status().errors, quint64(1));

    const QString longError(2000, QLatin1Char('e'));
    fixture.harness->transport->errorOccurred(longError);
    QVERIFY(service.isRunning());
    QCOMPARE(service.status().lastError.size(),
             CotOutputService::MaximumStatusTextLength);
    QCOMPARE(service.status().errors, quint64(2));

    service.forgetLink(11);
    QVERIFY(service.isRunning());
    service.forgetLink(12);
    QVERIFY(!service.isRunning());
    QCOMPARE(service.state(), CotOutputService::State::Stopped);
    QCOMPARE(service.status().text,
             QStringLiteral(
                 "Stopped: source link Original Link was removed."));

    CotOutputServiceSettings invalid = serviceSettings();
    invalid.updateIntervalSeconds = 0.01;
    QString error;
    QVERIFY(!service.start(12, QStringLiteral("Link"), invalid, &error));
    QVERIFY(error.contains(QStringLiteral("0.1 and 3600")));
    QCOMPARE(service.state(), CotOutputService::State::Failed);
}

void CotOutputServiceTest::handlesReentrantStopAndDestruction()
{
    Fixture failed;
    CotOutputService failedService(failed.factory(), failed.sender(),
                                   failed.clock());
    failedService.updateEndpoints({endpoint(2, 19, 1)});
    QVERIFY(failedService.start(2, QStringLiteral("Link"),
                                serviceSettings()));
    failed.harness->transport->stateChanged(
        CotOutputTransport::State::Error);
    QCOMPARE(failedService.state(), CotOutputService::State::Failed);
    QVERIFY(!failedService.isRunning());
    QCOMPARE(failedService.status().errors, quint64(1));

    Fixture stopped;
    CotOutputService service(stopped.factory(), stopped.sender(),
                             stopped.clock());
    service.updateEndpoints({endpoint(4, 20, 1)});
    QVERIFY(service.start(4, QStringLiteral("Link"), serviceSettings()));
    service.observeMessage(4, globalPosition(20, 1));
    stopped.harness->duringSend = [&service]() { service.stop(); };
    service.emitNow();
    QCOMPARE(service.state(), CotOutputService::State::Stopped);
    QVERIFY(!service.isRunning());

    Fixture destroyed;
    QPointer<CotOutputService> guarded = new CotOutputService(
        destroyed.factory(), destroyed.sender(), destroyed.clock());
    guarded->updateEndpoints({endpoint(9, 21, 1)});
    QVERIFY(guarded->start(9, QStringLiteral("Link"), serviceSettings()));
    guarded->observeMessage(9, globalPosition(21, 1));
    destroyed.harness->duringSend = [&guarded]() { delete guarded.data(); };
    guarded->emitNow();
    QVERIFY(guarded.isNull());
}

QTEST_GUILESS_MAIN(CotOutputServiceTest)
#include "test_cotoutputservice.moc"
