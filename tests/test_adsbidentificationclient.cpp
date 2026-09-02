#include <QtTest>

#include "comm/AdsbIdentificationClient.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"

#include <QSignalSpy>
#include <QVector>

#include <cstring>

namespace {

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("link%1").arg(linkId);
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        std::memset(&message, 0, sizeof(message));
    }
    return message;
}

QByteArray flightIdOf(const mavlink_message_t &message)
{
    mavlink_uavionix_adsb_out_cfg_flightid_t decoded;
    std::memset(&decoded, 0, sizeof(decoded));
    mavlink_msg_uavionix_adsb_out_cfg_flightid_decode(&message, &decoded);
    return QByteArray(decoded.flight_id, 9);
}

QByteArray registrationOf(const mavlink_message_t &message)
{
    mavlink_uavionix_adsb_out_cfg_registration_t decoded;
    std::memset(&decoded, 0, sizeof(decoded));
    mavlink_msg_uavionix_adsb_out_cfg_registration_decode(&message, &decoded);
    return QByteArray(decoded.registration, 9);
}

// The pack helpers copy exactly 9 bytes: build a padded device field.
void deviceField(const char *text, char (&field)[9])
{
    std::memset(field, 0, sizeof(field));
    std::strncpy(field, text, 8);
}

mavlink_message_t flightIdReply(quint8 systemId, quint8 componentId, const char *text)
{
    char field[9];
    deviceField(text, field);
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_uavionix_adsb_out_cfg_flightid_pack(systemId, componentId, &message, field);
    return message;
}

mavlink_message_t registrationReply(quint8 systemId, quint8 componentId, const char *text)
{
    char field[9];
    deviceField(text, field);
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_uavionix_adsb_out_cfg_registration_pack(systemId, componentId, &message, field);
    return message;
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    AdsbIdentificationClient client;

    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              if (!writerAccepts) {
                  return false;
              }
              frames.append({linkId, bytes});
              return true;
          }),
          client(&targets, &transmitter)
    {
        client.setLocalIdentity(250, 190);
        client.setResendDelayMs(5);
    }

    VehicleTargetLease selectAndBind(int linkId = 9, int systemId = 42, int componentId = 1)
    {
        targets.observeEndpoint(endpoint(linkId, systemId, componentId));
        targets.selectTarget(linkId, systemId, componentId);
        const VehicleTargetLease lease = targets.acquireTarget();
        client.bind(lease);
        return lease;
    }

    mavlink_message_t message(int index) const { return decodeFrame(frames.at(index).bytes); }
};

} // namespace

class AdsbIdentificationClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void textHelpersMatchMissionPlanner();
    void bindRequiresTheCurrentLease();
    void requestIdentificationSendsBothGets();
    void saveFollowsMissionPlannerCadence();
    void saveRejectsBusyInvalidAndUnboundCalls();
    void observeMessageAcceptsOnlyTheLeasedEndpoint();
    void targetChangeCancelsTheSaveAndInvalidatesTheLease();
    void cancelIsIdempotentAndReentrant();
    void transportFailuresAreReportedWithoutCancelledFlag();
    void rebindAndUnbindCancelExactlyOnce();
    void reentrantSavesDuringBindAndUnbindAreRefused();
    void destructionDuringSaveIsSilent();
};

void AdsbIdentificationClientTest::textHelpersMatchMissionPlanner()
{
    // AdsbConfigTests: padding is trimmed without losing content.
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText(QByteArrayLiteral("N123AB \0\0")),
             QStringLiteral("N123AB"));
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText("ABC12   \0", 9), QStringLiteral("ABC12"));
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText("\0\0\0\0\0\0\0\0\0", 9), QString());
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText("A B\0\0\0\0\0\0", 9), QStringLiteral("A B"));
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText("N\xC3\xA9X\0\0\0\0\0\0", 9),
             QStringLiteral("N??X")); // Encoding.ASCII maps high bytes to '?'
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText(nullptr, 9), QString());
    QCOMPARE(AdsbIdentificationClient::DecodeDeviceText(QByteArray()), QString());

    // AdsbConfigTests: empty identification needs an explicit clear confirmation.
    QVERIFY(AdsbIdentificationClient::NeedsClearConfirmation(QString()));
    QVERIFY(AdsbIdentificationClient::NeedsClearConfirmation(QStringLiteral("")));
    QVERIFY(AdsbIdentificationClient::NeedsClearConfirmation(QStringLiteral("   ")));
    QVERIFY(!AdsbIdentificationClient::NeedsClearConfirmation(QStringLiteral("N123AB")));

    // Strict MakeBytesSize(9): <= 8 printable ASCII characters, NUL padded.
    QString error;
    QCOMPARE(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("N123AB"), &error),
             QByteArray("N123AB\0\0\0", 9));
    QVERIFY(error.isEmpty());
    QCOMPARE(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("ABCDEFGH")),
             QByteArray("ABCDEFGH\0", 9));
    QCOMPARE(AdsbIdentificationClient::EncodeDeviceText(QString()), QByteArray(9, '\0'));
    QCOMPARE(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("A B ")),
             QByteArray("A B \0\0\0\0\0", 9));
    QVERIFY(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("ABCDEFGHI"), &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("8")), qPrintable(error));
    QVERIFY(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("Né"), &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("ASCII")), qPrintable(error));
    QVERIFY(AdsbIdentificationClient::EncodeDeviceText(QStringLiteral("A\tB"), &error).isEmpty());
    QCOMPARE(AdsbIdentificationClient::messageIdFor(AdsbIdentificationClient::Field::FlightId), 10005u);
    QCOMPARE(AdsbIdentificationClient::messageIdFor(AdsbIdentificationClient::Field::Registration), 10004u);
}

void AdsbIdentificationClientTest::bindRequiresTheCurrentLease()
{
    Fixture fixture;
    AdsbIdentificationClient &client = fixture.client;
    QVERIFY(!client.isBound());
    QVERIFY(!client.isBusy());
    QCOMPARE(client.requestIdentification(), AdsbIdentificationClient::SendResult::NotBound);
    QCOMPARE(client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("X")),
             AdsbIdentificationClient::SendResult::NotBound);
    QVERIFY(!client.bind(VehicleTargetLease{}));
    QCOMPARE(fixture.frames.size(), 0);

    // A lease that is no longer current is refused.
    QVERIFY(fixture.targets.observeEndpoint(endpoint(1), true));
    QVERIFY(fixture.targets.observeEndpoint(endpoint(2)));
    const VehicleTargetLease stale = fixture.targets.acquireTarget();
    QVERIFY(fixture.targets.selectTarget(2, 42, 1));
    QVERIFY(!client.bind(stale));
    QVERIFY(!client.isBound());

    const VehicleTargetLease current = fixture.targets.acquireTarget();
    QVERIFY(client.bind(current));
    QVERIFY(client.isBound());
    QCOMPARE(client.lease().endpoint.linkId, 2);
    QCOMPARE(client.lease().generation, current.generation);
    QCOMPARE(client.localSystemId(), quint8(250));
    QCOMPARE(client.localComponentId(), quint8(190));
    QCOMPARE(client.resendDelayMs(), 5);
    client.unbind();
    QVERIFY(!client.isBound());
}

void AdsbIdentificationClientTest::requestIdentificationSendsBothGets()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind(9, 42, 1);
    QSignalSpy requested(&fixture.client, &AdsbIdentificationClient::identificationRequested);

    QCOMPARE(fixture.client.requestIdentification(), AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 2);
    QCOMPARE(fixture.frames.at(0).linkId, 9);
    QCOMPARE(fixture.frames.at(1).linkId, 9);
    const mavlink_message_t first = fixture.message(0);
    const mavlink_message_t second = fixture.message(1);
    QCOMPARE(first.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(second.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&first), quint32(10004));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&second), quint32(10005));
    QCOMPARE(int(first.sysid), 250);
    QCOMPARE(int(first.compid), 190);
    QCOMPARE(int(second.seq), int(first.seq) + 1); // one link sequence
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.at(0).at(0).toULongLong(), lease.generation);
    QVERIFY(!fixture.client.isBusy());
}

void AdsbIdentificationClientTest::saveFollowsMissionPlannerCadence()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind(9, 42, 1);
    QSignalSpy started(&fixture.client, &AdsbIdentificationClient::saveStarted);
    QSignalSpy completed(&fixture.client, &AdsbIdentificationClient::saveCompleted);
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N123AB")),
             AdsbIdentificationClient::SendResult::Sent);
    QVERIFY(fixture.client.isBusy());
    QCOMPARE(started.count(), 1);
    QCOMPARE(fixture.frames.size(), 1); // first copy goes out immediately
    QTRY_COMPARE(fixture.frames.size(), 3);
    QTRY_COMPARE(completed.count(), 1);
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(failed.count(), 0);

    const mavlink_message_t first = fixture.message(0);
    const mavlink_message_t second = fixture.message(1);
    const mavlink_message_t third = fixture.message(2);
    QCOMPARE(first.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_FLIGHTID));
    QCOMPARE(second.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_FLIGHTID));
    QCOMPARE(flightIdOf(first), QByteArray("N123AB\0\0\0", 9));
    QCOMPARE(flightIdOf(second), QByteArray("N123AB\0\0\0", 9));
    QCOMPARE(third.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&third), quint32(10005));
    QCOMPARE(int(second.seq), int(first.seq) + 1);
    QCOMPARE(int(third.seq), int(second.seq) + 1);
    QCOMPARE(completed.at(0).at(0).value<AdsbIdentificationClient::Field>(),
             AdsbIdentificationClient::Field::FlightId);
    QCOMPARE(completed.at(0).at(1).toULongLong(), lease.generation);

    // Registration: 10004 twice, then GET 10004; an empty text clears.
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::Registration, QString()),
             AdsbIdentificationClient::SendResult::Sent);
    QTRY_COMPARE(fixture.frames.size(), 6);
    QTRY_COMPARE(completed.count(), 2);
    QCOMPARE(fixture.message(3).msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_REGISTRATION));
    QCOMPARE(registrationOf(fixture.message(3)), QByteArray(9, '\0'));
    QCOMPARE(fixture.message(4).msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_REGISTRATION));
    const mavlink_message_t registrationGet = fixture.message(5);
    QCOMPARE(registrationGet.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&registrationGet), quint32(10004));
    QCOMPARE(completed.at(1).at(0).value<AdsbIdentificationClient::Field>(),
             AdsbIdentificationClient::Field::Registration);
    QCOMPARE(failed.count(), 0);
}

void AdsbIdentificationClientTest::saveRejectsBusyInvalidAndUnboundCalls()
{
    Fixture fixture;
    fixture.client.setResendDelayMs(1000);
    fixture.selectAndBind(9, 42, 1);
    QSignalSpy started(&fixture.client, &AdsbIdentificationClient::saveStarted);

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("ABCDEFGHI")),
             AdsbIdentificationClient::SendResult::InvalidText);
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("Né")),
             AdsbIdentificationClient::SendResult::InvalidText);
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(started.count(), 0);

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::Registration, QStringLiteral("N2")),
             AdsbIdentificationClient::SendResult::Busy);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(started.count(), 1);
    // Reads are still allowed while a save is pending.
    QCOMPARE(fixture.client.requestIdentification(), AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 3);
    fixture.client.cancel();
}

void AdsbIdentificationClientTest::observeMessageAcceptsOnlyTheLeasedEndpoint()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind(9, 42, 1);
    QSignalSpy flightId(&fixture.client, &AdsbIdentificationClient::flightIdReceived);
    QSignalSpy registration(&fixture.client, &AdsbIdentificationClient::registrationReceived);

    fixture.client.observeMessage(9, flightIdReply(42, 1, "ABC12   "));
    QCOMPARE(flightId.count(), 1);
    QCOMPARE(flightId.at(0).at(0).toULongLong(), lease.generation);
    QCOMPARE(flightId.at(0).at(1).toString(), QStringLiteral("ABC12"));

    fixture.client.observeMessage(9, registrationReply(42, 1, "N8644B "));
    QCOMPARE(registration.count(), 1);
    QCOMPARE(registration.at(0).at(1).toString(), QStringLiteral("N8644B"));

    // Wrong link, system, component or message id: ignored.
    fixture.client.observeMessage(4, flightIdReply(42, 1, "OTHER"));
    fixture.client.observeMessage(9, flightIdReply(43, 1, "OTHER"));
    fixture.client.observeMessage(9, flightIdReply(42, 2, "OTHER"));
    mavlink_message_t heartbeat;
    std::memset(&heartbeat, 0, sizeof(heartbeat));
    mavlink_heartbeat_t payload;
    std::memset(&payload, 0, sizeof(payload));
    mavlink_msg_heartbeat_encode(42, 1, &heartbeat, &payload);
    fixture.client.observeMessage(9, heartbeat);
    QCOMPARE(flightId.count(), 1);
    QCOMPARE(registration.count(), 1);

    // Unbound clients ignore everything.
    fixture.client.unbind();
    fixture.client.observeMessage(9, flightIdReply(42, 1, "LATE"));
    QCOMPARE(flightId.count(), 1);
}

void AdsbIdentificationClientTest::targetChangeCancelsTheSaveAndInvalidatesTheLease()
{
    Fixture fixture;
    fixture.client.setResendDelayMs(1000);
    const VehicleTargetLease lease = fixture.selectAndBind(9, 42, 1);
    QVERIFY(fixture.targets.observeEndpoint(endpoint(4, 42, 1)));
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);
    QSignalSpy invalidated(&fixture.client, &AdsbIdentificationClient::leaseInvalidated);
    QSignalSpy flightId(&fixture.client, &AdsbIdentificationClient::flightIdReceived);

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 1);

    QVERIFY(fixture.targets.selectTarget(4, 42, 1)); // generation changes
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(2).toBool(), true);
    QCOMPARE(failed.at(0).at(0).value<AdsbIdentificationClient::Field>(),
             AdsbIdentificationClient::Field::FlightId);
    QCOMPARE(invalidated.count(), 1);
    QCOMPARE(invalidated.at(0).at(0).toULongLong(), lease.generation);
    QVERIFY(!fixture.client.isBound());
    QVERIFY(!fixture.client.isBusy());

    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 1); // the cadence stopped
    fixture.client.observeMessage(9, flightIdReply(42, 1, "LATE"));
    QCOMPARE(flightId.count(), 0);
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N2")),
             AdsbIdentificationClient::SendResult::NotBound);
    QCOMPARE(fixture.client.requestIdentification(), AdsbIdentificationClient::SendResult::NotBound);

    // Re-selecting the same endpoint does not resurrect the old lease.
    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(invalidated.count(), 1);
    QVERIFY(!fixture.client.isBound());
    QVERIFY(fixture.client.bind(fixture.targets.acquireTarget()));
    QCOMPARE(fixture.client.requestIdentification(), AdsbIdentificationClient::SendResult::Sent);
}

void AdsbIdentificationClientTest::cancelIsIdempotentAndReentrant()
{
    Fixture fixture;
    fixture.client.setResendDelayMs(1000);
    fixture.selectAndBind(9, 42, 1);
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);
    QSignalSpy started(&fixture.client, &AdsbIdentificationClient::saveStarted);

    fixture.client.cancel(); // nothing running
    QCOMPARE(failed.count(), 0);

    // A slot reacting to the failure starts the next save re-entrantly; the
    // cancel in progress must not sweep it up.
    bool restarted = false;
    connect(&fixture.client, &AdsbIdentificationClient::saveFailed, &fixture.client,
            [&fixture, &restarted]() {
                if (!restarted) {
                    restarted = fixture.client.save(
                                    AdsbIdentificationClient::Field::Registration,
                                    QStringLiteral("N2"))
                                == AdsbIdentificationClient::SendResult::Sent;
                }
            });

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    fixture.client.cancel();
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(2).toBool(), true);
    QVERIFY(restarted);
    QVERIFY(fixture.client.isBusy()); // the re-entrant save survived
    QCOMPARE(started.count(), 2);
    QCOMPARE(fixture.frames.size(), 2);

    fixture.client.cancel();
    QCOMPARE(failed.count(), 2);
    QVERIFY(!fixture.client.isBusy());
    fixture.client.cancel();
    QCOMPARE(failed.count(), 2);
}

void AdsbIdentificationClientTest::transportFailuresAreReportedWithoutCancelledFlag()
{
    Fixture fixture;
    fixture.selectAndBind(9, 42, 1);
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);
    QSignalSpy started(&fixture.client, &AdsbIdentificationClient::saveStarted);
    QSignalSpy completed(&fixture.client, &AdsbIdentificationClient::saveCompleted);

    // First frame refused: nothing starts, the reason is available.
    fixture.writerAccepts = false;
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::TransportUnavailable);
    QVERIFY(!fixture.client.isBusy());
    QVERIFY(!fixture.client.lastError().isEmpty());
    QCOMPARE(started.count(), 0);
    QCOMPARE(fixture.client.requestIdentification(),
             AdsbIdentificationClient::SendResult::TransportUnavailable);

    // Failure in the middle of the cadence: reported once, not cancelled.
    fixture.writerAccepts = true;
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    QVERIFY(fixture.client.lastError().isEmpty());
    fixture.writerAccepts = false;
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(2).toBool(), false);
    QVERIFY2(failed.at(0).at(1).toString().startsWith(QStringLiteral("send failed: ")),
             qPrintable(failed.at(0).at(1).toString()));
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(completed.count(), 0);
    QTest::qWait(30);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(fixture.frames.size(), 1);
}

void AdsbIdentificationClientTest::rebindAndUnbindCancelExactlyOnce()
{
    Fixture fixture;
    fixture.client.setResendDelayMs(1000);
    fixture.selectAndBind(9, 42, 1);
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    QVERIFY(fixture.client.bind(fixture.targets.acquireTarget())); // rebind to the same target
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(2).toBool(), true);
    QVERIFY(!fixture.client.isBusy());
    QVERIFY(fixture.client.isBound());

    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::Registration, QStringLiteral("N2")),
             AdsbIdentificationClient::SendResult::Sent);
    fixture.client.unbind();
    QCOMPARE(failed.count(), 2);
    QVERIFY(!fixture.client.isBusy());
    QVERIFY(!fixture.client.isBound());
    fixture.client.unbind();
    QCOMPARE(failed.count(), 2);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 2);
}

void AdsbIdentificationClientTest::reentrantSavesDuringBindAndUnbindAreRefused()
{
    Fixture fixture;
    fixture.client.setResendDelayMs(1000);
    fixture.selectAndBind(9, 42, 1);
    QSignalSpy failed(&fixture.client, &AdsbIdentificationClient::saveFailed);
    QSignalSpy started(&fixture.client, &AdsbIdentificationClient::saveStarted);

    // A slot on saveFailed immediately tries to save and to read again.
    QVector<AdsbIdentificationClient::SendResult> reentrantSaves;
    QVector<AdsbIdentificationClient::SendResult> reentrantReads;
    QVector<bool> boundDuringSlot;
    connect(&fixture.client, &AdsbIdentificationClient::saveFailed, &fixture.client,
            [&fixture, &reentrantSaves, &reentrantReads, &boundDuringSlot]() {
                boundDuringSlot.append(fixture.client.isBound());
                reentrantSaves.append(fixture.client.save(
                    AdsbIdentificationClient::Field::Registration, QStringLiteral("R1")));
                reentrantReads.append(fixture.client.requestIdentification());
            });

    // bind() while a save is running: the slot sees an unbound client.
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
             AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.client.bind(fixture.targets.acquireTarget()));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(boundDuringSlot, QVector<bool>{false});
    QCOMPARE(reentrantSaves, QVector<AdsbIdentificationClient::SendResult>{
                                 AdsbIdentificationClient::SendResult::NotBound});
    QCOMPARE(reentrantReads, QVector<AdsbIdentificationClient::SendResult>{
                                 AdsbIdentificationClient::SendResult::NotBound});
    QVERIFY(!fixture.client.isBusy()); // no operation survived the rebind
    QVERIFY(fixture.client.isBound());  // the requested lease was installed afterwards
    QCOMPARE(started.count(), 1);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 1); // no timer survived either

    // The fresh binding is fully usable.
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N2")),
             AdsbIdentificationClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 2);
    QCOMPARE(started.count(), 2);

    // unbind() while that save is running: same guarantees, client stays unbound.
    fixture.client.unbind();
    QCOMPARE(failed.count(), 2);
    QCOMPARE(boundDuringSlot, (QVector<bool>{false, false}));
    QCOMPARE(reentrantSaves.last(), AdsbIdentificationClient::SendResult::NotBound);
    QCOMPARE(reentrantReads.last(), AdsbIdentificationClient::SendResult::NotBound);
    QVERIFY(!fixture.client.isBusy());
    QVERIFY(!fixture.client.isBound());
    QCOMPARE(started.count(), 2);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 2);

    // A bind() to a stale lease also leaves the client unbound after cancelling.
    QVERIFY(fixture.client.bind(fixture.targets.acquireTarget()));
    QCOMPARE(fixture.client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N3")),
             AdsbIdentificationClient::SendResult::Sent);
    const VehicleTargetLease stale = fixture.targets.acquireTarget();
    QVERIFY(fixture.targets.observeEndpoint(endpoint(4, 42, 1)));
    QVERIFY(fixture.targets.selectTarget(4, 42, 1)); // cancels + invalidates (failed 3)
    QCOMPARE(failed.count(), 3);
    QVERIFY(!fixture.client.bind(stale));
    QVERIFY(!fixture.client.isBound());
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(failed.count(), 3);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 3);
}

void AdsbIdentificationClientTest::destructionDuringSaveIsSilent()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter([&frames](int linkId, const QByteArray &bytes) {
        frames.append({linkId, bytes});
        return true;
    });
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 1), true));
    int failures = 0;
    {
        AdsbIdentificationClient client(&targets, &transmitter);
        client.setResendDelayMs(1000);
        QVERIFY(client.bind(targets.acquireTarget()));
        connect(&client, &AdsbIdentificationClient::saveFailed, &client, [&failures]() { ++failures; });
        QCOMPARE(client.save(AdsbIdentificationClient::Field::FlightId, QStringLiteral("N1")),
                 AdsbIdentificationClient::SendResult::Sent);
        QVERIFY(client.isBusy());
    }
    QCOMPARE(failures, 0);
    QTest::qWait(30);
    QCOMPARE(frames.size(), 1);
    // No dangling connection to the manager remains.
    QVERIFY(targets.observeEndpoint(endpoint(4, 42, 1)));
    QVERIFY(targets.selectTarget(4, 42, 1));
}

QTEST_GUILESS_MAIN(AdsbIdentificationClientTest)
#include "test_adsbidentificationclient.moc"
