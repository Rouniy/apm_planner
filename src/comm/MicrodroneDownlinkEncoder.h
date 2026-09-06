#ifndef MICRODRONEDOWNLINKENCODER_H
#define MICRODRONEDOWNLINKENCODER_H

#include <QByteArray>
#include <QDateTime>
#include <QString>

// Exact values captured by MP10 MicrodroneTelemetry. CurrentState float
// properties are widened only after their original single-precision arithmetic.
struct MicrodroneTelemetry
{
    double latitude = 0, longitude = 0, altitude = 0;
    double gpsHdop = 0, satelliteCount = 0;
    double groundSpeed = 0, groundCourse = 0, verticalSpeed = 0;
    double roll = 0, pitch = 0, yaw = 0;
    double pressureTemperature = 0;
    double magnetometerX = 0, magnetometerY = 0, magnetometerZ = 0;
};

class MicrodroneDownlinkEncoder final
{
public:
    static quint8 Checksum(const QByteArray &payload);
    static QByteArray EncodeFrame(const MicrodroneTelemetry &telemetry,
        const QDateTime &utcNow, qint64 sampleCounter, QString *error = nullptr);
};
#endif
