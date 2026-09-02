#include <QtTest>

#include "uas/ServoOutputDecoder.h"

#include <limits>

namespace {
mavlink_message_t servoMessage(quint8 port, quint16 first, quint16 last)
{
    mavlink_servo_output_raw_t raw{};
    raw.port = port;
    raw.servo1_raw = first;
    raw.servo2_raw = 1002;
    raw.servo3_raw = 1003;
    raw.servo4_raw = 1004;
    raw.servo5_raw = 1005;
    raw.servo6_raw = 1006;
    raw.servo7_raw = 1007;
    raw.servo8_raw = 1008;
    raw.servo9_raw = 1009;
    raw.servo10_raw = 1010;
    raw.servo11_raw = 1011;
    raw.servo12_raw = 1012;
    raw.servo13_raw = 1013;
    raw.servo14_raw = 1014;
    raw.servo15_raw = 1015;
    raw.servo16_raw = last;
    mavlink_message_t message{};
    mavlink_msg_servo_output_raw_encode(42, 1, &message, &raw);
    return message;
}
}

class ServoOutputDecoderTest final : public QObject
{
    Q_OBJECT

private slots:
    void mapsPortsToMissionPlannerChannelRanges();
    void preservesWireValuesIncludingSentinels();
    void rejectsOtherPortsAndMessageTypes();
};

void ServoOutputDecoderTest::mapsPortsToMissionPlannerChannelRanges()
{
    QVector<ServoOutputSample> samples =
        ServoOutputDecoder::Decode(servoMessage(0, 1101, 1116));
    QCOMPARE(samples.size(), 16);
    QCOMPARE(samples.first().Number, 1);
    QCOMPARE(samples.first().Pwm, 1101);
    QCOMPARE(samples.last().Number, 16);
    QCOMPARE(samples.last().Pwm, 1116);

    samples = ServoOutputDecoder::Decode(
        servoMessage(1, 1201, 1216));
    QCOMPARE(samples.size(), 16);
    QCOMPARE(samples.first().Number, 17);
    QCOMPARE(samples.first().Pwm, 1201);
    QCOMPARE(samples.last().Number, 32);
    QCOMPARE(samples.last().Pwm, 1216);
}

void ServoOutputDecoderTest::preservesWireValuesIncludingSentinels()
{
    const QVector<ServoOutputSample> samples =
        ServoOutputDecoder::Decode(servoMessage(
            0, 0, std::numeric_limits<quint16>::max()));
    QCOMPARE(samples.size(), 16);
    QCOMPARE(samples.first().Pwm, 0);
    QCOMPARE(samples.last().Pwm,
             static_cast<int>(std::numeric_limits<quint16>::max()));
}

void ServoOutputDecoderTest::rejectsOtherPortsAndMessageTypes()
{
    QVERIFY(ServoOutputDecoder::Decode(
        servoMessage(2, 1301, 1316)).isEmpty());

    mavlink_heartbeat_t heartbeat{};
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(42, 1, &message, &heartbeat);
    QVERIFY(ServoOutputDecoder::Decode(message).isEmpty());
}

QTEST_APPLESS_MAIN(ServoOutputDecoderTest)
#include "test_servooutputdecoder.moc"
