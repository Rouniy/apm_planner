#include "ServoOutputDecoder.h"

QVector<ServoOutputSample> ServoOutputDecoder::Decode(
    const mavlink_message_t &message)
{
    if (message.msgid != MAVLINK_MSG_ID_SERVO_OUTPUT_RAW) {
        return {};
    }
    mavlink_servo_output_raw_t raw{};
    mavlink_msg_servo_output_raw_decode(&message, &raw);
    return Decode(raw);
}

QVector<ServoOutputSample> ServoOutputDecoder::Decode(
    const mavlink_servo_output_raw_t &raw)
{
    if (raw.port > 1) {
        return {};
    }

    const quint16 outputs[] = {
        raw.servo1_raw, raw.servo2_raw,
        raw.servo3_raw, raw.servo4_raw,
        raw.servo5_raw, raw.servo6_raw,
        raw.servo7_raw, raw.servo8_raw,
        raw.servo9_raw, raw.servo10_raw,
        raw.servo11_raw, raw.servo12_raw,
        raw.servo13_raw, raw.servo14_raw,
        raw.servo15_raw, raw.servo16_raw
    };
    const int firstChannel = raw.port * 16 + 1;
    QVector<ServoOutputSample> samples;
    samples.reserve(16);
    for (int index = 0; index < 16; ++index) {
        samples.append({firstChannel + index,
                        static_cast<int>(outputs[index])});
    }
    return samples;
}
