#ifndef SERVOOUTPUTDECODER_H
#define SERVOOUTPUTDECODER_H

#include <QVector>

#include <mavlink.h>

struct ServoOutputSample
{
    int Number = 0;
    int Pwm = 0;
};

class ServoOutputDecoder final
{
public:
    static QVector<ServoOutputSample> Decode(
        const mavlink_message_t &message);
    static QVector<ServoOutputSample> Decode(
        const mavlink_servo_output_raw_t &raw);
};

#endif
