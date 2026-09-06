#ifndef MICRODRONETELEMETRYSTATE_H
#define MICRODRONETELEMETRYSTATE_H

#include "MicrodroneDownlinkEncoder.h"
#include <mavlink.h>

// One selected component's CurrentState-compatible telemetry cache. No source
// selection, I/O or timers; the service supplies the message receipt timestamp.
class MicrodroneTelemetryState final
{
public:
    bool Apply(const mavlink_message_t &message, const QDateTime &receivedUtc);
    MicrodroneTelemetry snapshot() const { return m_values; }
    void clear();
private:
    void setAltitude(float metres, const QDateTime &receivedUtc);
    MicrodroneTelemetry m_values;
    bool m_globalPosition = false;
    double m_homeAltitude = 0;
    float m_oldAltitude = 0, m_verticalSpeed = 0;
    QDateTime m_lastAltitude = QDateTime(QDate(1, 1, 1), QTime(0, 0), Qt::UTC);
};
#endif
