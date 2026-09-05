#include "comm/MAVLinkSigningSession.h"
#include "comm/MAVLinkSigningClock.h"
#include "comm/MAVLinkFrameParser.h"

#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <mavlink.h>

#include <cstring>
#include <type_traits>

namespace {
constexpr qint64 Now = 1700000000000LL;
constexpr quint64 Stamp = 27992960000000ULL;
using Verdict = MAVLinkSigningSession::Verdict;
static_assert(!std::is_copy_constructible<MAVLinkSigningSession>::value, "No replay-state copies");
static_assert(!std::is_move_constructible<MAVLinkSigningSession>::value, "Stable key context identity");
QByteArray key()
{
    QByteArray result;
    for (int i = 1; i <= 32; ++i) result.append(char(i));
    return result;
}
const QByteArray Unsigned = QByteArray::fromHex("fd0900004d2a010000000000000002030003032eff");
// Independent pymavlink.dialects.v20.ardupilotmega known-answer fixture:
// key bytes 1..32, sys42/comp1, seq77, link9, timestamp27992960000000.
const QByteArray Signed = QByteArray::fromHex("fd0901004d2a01000000000000000203000303c9070900e06f9e7519f130582f8758");

std::shared_ptr<MAVLinkSigningClock> clockAt(const QString &path, qint64 now = Now)
{
    auto clock = std::make_shared<MAVLinkSigningClock>(path);
    if (!clock->open(now)) return {};
    return clock;
}

QByteArray nativeFrame(quint64 stamp, quint8 link = 9, quint8 sys = 42,
                       quint8 comp = 1, bool radio = false, bool sign = true,
                       bool v1 = false)
{
    mavlink_message_t message{};
    message.msgid = radio ? MAVLINK_MSG_ID_RADIO_STATUS : MAVLINK_MSG_ID_HEARTBEAT;
    const auto *entry = mavlink_get_msg_entry(message.msgid);
    mavlink_status_t status{};
    status.current_tx_seq = 77;
    if (v1) status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_signing_t signing{};
    signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
    signing.link_id = link;
    signing.timestamp = stamp;
    const QByteArray secret = key();
    std::memcpy(signing.secret_key, secret.constData(), 32);
    if (sign) status.signing = &signing;
    mavlink_finalize_message_buffer(&message, sys, comp, &status,
                                   entry->min_msg_len, entry->max_msg_len, entry->crc_extra);
    unsigned char bytes[MAVLINK_MAX_PACKET_LEN]{};
    const auto length = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), length);
}
}

class MAVLinkSigningSessionTest final : public QObject
{
    Q_OBJECT
private slots:
    void knownPymavlinkVectorAndNativeVerification();
    void crcAndSignatureAreIndependentGates();
    void radioExceptionDoesNotAcceptBadSignatures();
    void streamsAreSharedAcrossPhysicalLinks();
    void onlyAuthenticatedPacketsCanAdvanceTime();
    void streamCapacityNeverEvictsReplayHistory();
    void invalidInputsAndAliasedOutput();
    void clockAndKeyAreRequired();
    void signingPreservesPayloadAndSequence();
    void rejectsOversizedAndSeparatelyCorruptedFrames();
    void restartReplayWindowIsExplicit();
};

void MAVLinkSigningSessionTest::knownPymavlinkVectorAndNativeVerification()
{
    QTemporaryDir dir;
    auto clock = clockAt(dir.filePath("clock"));
    QVERIFY(clock);
    MAVLinkSigningSession session(key(), clock);
    QByteArray output;
    QVERIFY(session.signFrame(Unsigned, 9, Now, &output));
    QCOMPARE(output, Signed);
    MAVLinkFrameParser parser;
    mavlink_signing_t signing{};
    const QByteArray secret = key();
    std::memcpy(signing.secret_key, secret.constData(), 32);
    signing.timestamp = Stamp;
    mavlink_signing_streams_t streams{};
    parser.status().signing = &signing;
    parser.status().signing_streams = &streams;
    mavlink_message_t decoded{};
    unsigned state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : output) state = parser.parseByte(quint8(byte), &decoded);
    QCOMPARE(state, unsigned(MAVLINK_FRAMING_OK));
    QCOMPARE(decoded.seq, quint8(77));
    QCOMPARE(session.verifyFrame(Signed, Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(Signed, Now).verdict, Verdict::Replay);
    QCOMPARE(session.counters().signedSent, quint64(1));
    QCOMPARE(session.counters().signedAccepted, quint64(1));
}

void MAVLinkSigningSessionTest::crcAndSignatureAreIndependentGates()
{
    QTemporaryDir dir;
    auto clock = clockAt(dir.filePath("clock"));
    MAVLinkSigningSession session(key(), clock);
    for (int i = 0; i < Signed.size(); ++i) {
        QByteArray corrupt = Signed;
        corrupt[i] = char(quint8(corrupt[i]) ^ 0x80);
        QVERIFY2(!session.verifyFrame(corrupt, Now).accepted(), qPrintable(QString::number(i)));
    }
    QCOMPARE(session.streamCount(), 0);
    QCOMPARE(clock->current(Now), Stamp);
    QCOMPARE(session.verifyFrame(Signed.left(Signed.size() - 1), Now).verdict, Verdict::InvalidFrame);
    QCOMPARE(session.verifyFrame(Signed + Signed, Now).verdict, Verdict::InvalidFrame);
    QCOMPARE(session.verifyFrame(Signed, Now).verdict, Verdict::Signed);
}

void MAVLinkSigningSessionTest::radioExceptionDoesNotAcceptBadSignatures()
{
    QTemporaryDir dir;
    MAVLinkSigningSession session(key(), clockAt(dir.filePath("clock")));
    QCOMPARE(session.verifyFrame(Unsigned, Now).verdict, Verdict::UnsignedRejected);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 9, 42, 1, false, false, true), Now).verdict,
             Verdict::UnsignedRejected);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 9, 51, 68, true, false), Now).verdict,
             Verdict::UnsignedRadio);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 9, 51, 68, true, false, true), Now).verdict,
             Verdict::UnsignedRadio);
    QByteArray invalid = nativeFrame(Stamp, 9, 51, 68, true);
    invalid[invalid.size() - 1] = char(quint8(invalid.back()) ^ 1);
    QCOMPARE(session.verifyFrame(invalid, Now).verdict, Verdict::BadSignature);
    QCOMPARE(session.streamCount(), 0);
}

void MAVLinkSigningSessionTest::streamsAreSharedAcrossPhysicalLinks()
{
    QTemporaryDir dir;
    MAVLinkSigningSession session(key(), clockAt(dir.filePath("clock")));
    // Physical link A and B deliberately call the SAME key session. A duplicate
    // arriving through B after A, or after disconnect, must not become fresh.
    QCOMPARE(session.verifyFrame(Signed, Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(Signed, Now).verdict, Verdict::Replay);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp - 1), Now).verdict, Verdict::Replay);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 1), Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 10), Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 9, 43), Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 9, 42, 2), Now).verdict, Verdict::Signed);
    QCOMPARE(session.streamCount(), 4);
    // Match mavgen: strict monotonic check on existing streams, one-minute
    // freshness on new streams only (delayed independent radios remain useful).
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 2), Now + 120000).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 2, 11), Now + 120000).verdict, Verdict::TooOld);
}

void MAVLinkSigningSessionTest::onlyAuthenticatedPacketsCanAdvanceTime()
{
    QTemporaryDir dir;
    auto clock = clockAt(dir.filePath("clock"));
    MAVLinkSigningSession session(key(), clock);
    QByteArray future = nativeFrame(Stamp + 10000000);
    future[future.size() - 1] = char(quint8(future.back()) ^ 1);
    QCOMPARE(session.verifyFrame(future, Now).verdict, Verdict::BadSignature);
    QCOMPARE(clock->current(Now), Stamp);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp - 6000001), Now).verdict, Verdict::TooOld);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp - 6000000), Now).verdict, Verdict::Signed);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 10000000), Now).verdict, Verdict::Signed);
    QVERIFY(clock->current(Now - 1000) > Stamp + 10000000);
    QByteArray output;
    QVERIFY(session.signFrame(Unsigned, 9, Now - 1000, &output));
    quint64 sent = 0;
    for (int i = 0; i < 6; ++i) sent |= quint64(quint8(output[output.size() - 12 + i])) << (8 * i);
    QVERIFY(sent > Stamp + 10000000);
}

void MAVLinkSigningSessionTest::streamCapacityNeverEvictsReplayHistory()
{
    QTemporaryDir dir;
    MAVLinkSigningSession session(key(), clockAt(dir.filePath("clock")));
    for (int i = 0; i < MAVLinkSigningSession::MaximumStreams; ++i)
        QCOMPARE(session.verifyFrame(nativeFrame(Stamp + i, quint8(i)), Now).verdict, Verdict::Signed);
    QCOMPARE(session.streamCount(), MAVLinkSigningSession::MaximumStreams);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 256, 0, 43), Now).verdict, Verdict::StreamLimit);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp, 0), Now).verdict, Verdict::Replay);
    QCOMPARE(session.verifyFrame(nativeFrame(Stamp + 257, 0), Now).verdict, Verdict::Signed);
}

void MAVLinkSigningSessionTest::invalidInputsAndAliasedOutput()
{
    QTemporaryDir dir;
    auto clock = clockAt(dir.filePath("clock"));
    MAVLinkSigningSession session(key(), clock);
    QByteArray output("old");
    QString error;
    QVERIFY(!session.signFrame(Signed, 9, Now, &output, &error));
    QVERIFY(output.isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!session.signFrame(Unsigned + Unsigned, 9, Now, &output));
    QVERIFY(!session.signFrame(nativeFrame(Stamp, 9, 42, 1, false, false, true), 9, Now, &output));
    QVERIFY(!session.signFrame(Unsigned, 9, Now, nullptr));
    QCOMPARE(clock->current(Now), Stamp);
    output = Unsigned;
    QVERIFY(session.signFrame(output, 9, Now, &output));
    QCOMPARE(output, Signed);
}

void MAVLinkSigningSessionTest::clockAndKeyAreRequired()
{
    QTemporaryDir dir;
    const auto unopened = std::make_shared<MAVLinkSigningClock>(dir.filePath("clock"));
    MAVLinkSigningSession noClock(key(), unopened);
    QVERIFY(!noClock.isReady());
    QCOMPARE(noClock.verifyFrame(Signed, Now).verdict, Verdict::NotReady);
    QVERIFY(unopened->open(Now));
    MAVLinkSigningSession zeroKey(QByteArray(32, 0), unopened);
    MAVLinkSigningSession shortKey(QByteArray(31, 1), unopened);
    QVERIFY(!zeroKey.isReady());
    QVERIFY(!shortKey.isReady());
    QByteArray output;
    QVERIFY(!zeroKey.signFrame(Unsigned, 9, Now, &output));
}

void MAVLinkSigningSessionTest::signingPreservesPayloadAndSequence()
{
    QTemporaryDir dir;
    MAVLinkSigningSession session(key(), clockAt(dir.filePath("clock")));
    mavlink_message_t message{};
    mavlink_status_t status{};
    status.current_tx_seq = 254;
    message.msgid = MAVLINK_MSG_ID_COMMAND_ACK;
    auto *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint16_t(payload, 0, MAV_CMD_COMPONENT_ARM_DISARM);
    _mav_put_uint8_t(payload, 8, 42);
    _mav_put_uint8_t(payload, 9, 7);
    mavlink_finalize_message_buffer(&message, 250, 190, &status,
                                   MAVLINK_MSG_ID_COMMAND_ACK_MIN_LEN,
                                   MAVLINK_MSG_ID_COMMAND_ACK_LEN,
                                   MAVLINK_MSG_ID_COMMAND_ACK_CRC);
    unsigned char bytes[MAVLINK_MAX_PACKET_LEN]{};
    int length = mavlink_msg_to_send_buffer(bytes, &message);
    const QByteArray original(reinterpret_cast<const char *>(bytes), length);
    QByteArray signedFrame;
    QVERIFY(session.signFrame(original, 255, Now, &signedFrame));
    QCOMPARE(signedFrame[4], original[4]);
    QCOMPARE(signedFrame.mid(5, 5 + message.len), original.mid(5, 5 + message.len));
    QCOMPARE(signedFrame.size(), original.size() + MAVLINK_SIGNATURE_BLOCK_LEN);
    QCOMPARE(session.verifyFrame(signedFrame, Now).verdict, Verdict::Signed);
}

void MAVLinkSigningSessionTest::rejectsOversizedAndSeparatelyCorruptedFrames()
{
    QTemporaryDir dir;
    auto clock = clockAt(dir.filePath("clock"));
    MAVLinkSigningSession session(key(), clock);
    const auto resign = [](QByteArray frame) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(key());
        hash.addData(frame.constData(), frame.size() - 6);
        frame.replace(frame.size() - 6, 6, hash.result().left(6));
        return frame;
    };
    QByteArray badCrc = Signed;
    badCrc[19] = char(quint8(badCrc[19]) ^ 1);
    QCOMPARE(session.verifyFrame(resign(badCrc), Now).verdict, Verdict::InvalidFrame);
    QByteArray badMac = Signed;
    badMac[4] = char(quint8(badMac[4]) ^ 1);
    quint16 crc = crc_calculate(reinterpret_cast<const uint8_t *>(badMac.constData()) + 1, 18);
    crc_accumulate(MAVLINK_MSG_ID_HEARTBEAT_CRC, &crc);
    badMac[19] = char(crc & 255); badMac[20] = char(crc >> 8);
    QCOMPARE(session.verifyFrame(badMac, Now).verdict, Verdict::BadSignature);
    for (bool radio : {false, true}) {
        QByteArray oversized = nativeFrame(Stamp, 9, 42, 1, radio, false);
        const int size = (radio ? MAVLINK_MSG_ID_RADIO_STATUS_LEN : MAVLINK_MSG_ID_HEARTBEAT_LEN) + 1;
        oversized = oversized.left(10);
        oversized[1] = char(size);
        oversized.append(QByteArray(size, 0));
        crc = crc_calculate(reinterpret_cast<const uint8_t *>(oversized.constData()) + 1, oversized.size() - 1);
        crc_accumulate(radio ? MAVLINK_MSG_ID_RADIO_STATUS_CRC : MAVLINK_MSG_ID_HEARTBEAT_CRC, &crc);
        oversized.append(char(crc & 255)); oversized.append(char(crc >> 8));
        QCOMPARE(session.verifyFrame(oversized, Now).verdict, Verdict::InvalidFrame);
        QByteArray output;
        QVERIFY(!session.signFrame(oversized, 9, Now, &output));
        oversized[2] = 1;
        crc = crc_calculate(reinterpret_cast<const uint8_t *>(oversized.constData()) + 1, oversized.size() - 3);
        crc_accumulate(radio ? MAVLINK_MSG_ID_RADIO_STATUS_CRC : MAVLINK_MSG_ID_HEARTBEAT_CRC, &crc);
        oversized[oversized.size() - 2] = char(crc & 255); oversized[oversized.size() - 1] = char(crc >> 8);
        oversized.append(Signed.right(13));
        QCOMPARE(session.verifyFrame(resign(oversized), Now).verdict, Verdict::InvalidFrame);
    }
    QCOMPARE(session.streamCount(), 0);
    QCOMPARE(clock->current(Now), Stamp);
    QCOMPARE(session.verifyFrame(nativeFrame(MAVLinkSigningClock::MaxTimestamp), Now).verdict,
             Verdict::ClockUnavailable);
    QCOMPARE(session.streamCount(), 0);
}

void MAVLinkSigningSessionTest::restartReplayWindowIsExplicit()
{
    QTemporaryDir dir;
    {
        MAVLinkSigningSession first(key(), clockAt(dir.filePath("clock")));
        QCOMPARE(first.verifyFrame(Signed, Now).verdict, Verdict::Signed);
        QCOMPARE(first.verifyFrame(Signed, Now).verdict, Verdict::Replay);
    }
    // The durable TX high-water is NOT a durable RX stream table. A fresh
    // process can admit an old frame inside the new-stream one-minute window.
    // This documented limit must not be confused with reconnect protection.
    MAVLinkSigningSession restarted(key(), clockAt(dir.filePath("clock")));
    QCOMPARE(restarted.verifyFrame(Signed, Now).verdict, Verdict::Signed);
}

QTEST_GUILESS_MAIN(MAVLinkSigningSessionTest)
#include "test_mavlinksigningsession.moc"
