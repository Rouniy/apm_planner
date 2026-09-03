#ifndef RADIOSTATUSMONITOR_H
#define RADIOSTATUSMONITOR_H

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>

#include <mavlink.h>

/*
 * Link-scoped telemetry-radio statistics (SiK RADIO_STATUS #109 and the legacy
 * ArduPilot RADIO #166) with Mission Planner 10's localsnrdb / remotesnrdb
 * semantics (ExtLibs/ArduPilot/CurrentState.cs).
 *
 * MP10 stores the raw uint8 rssi/remrssi/noise/remnoise fields unchanged
 * (:3425-3436) and propagates RADIO/RADIO_STATUS to every vehicle on the link
 * regardless of sysid/compid (:2302-2303), so the state is keyed by the
 * physical link only. Its localsnrdb getter (:1936-1944) is READ driven: a read
 * returns the cached value unless at least one second passed since the last
 * filter step, in which case exactly one step
 *     snr = (rssi - noise) / 1.9 * 0.5 + snr * 0.5
 * is applied to the latest raw sample. Messages only replace the raw sample;
 * they never advance the filter. MP10 keeps the latest sample forever, so this
 * class has no age-based expiry either: exact-target switches select another
 * link id and forgetLink()/clear() drop state when links go away.
 *
 * All time is passed in explicitly (milliseconds, any monotonic or wall clock)
 * so tests are deterministic. Nothing here references a LinkInterface pointer.
 */
struct RadioStatusSample
{
    bool valid = false;       // false until observe() stored a packet
    qint64 receivedMs = 0;    // observe() time in the caller's clock (any sign)
    quint32 messageId = 0;    // MAVLINK_MSG_ID_RADIO_STATUS or MAVLINK_MSG_ID_RADIO
    quint8 systemId = 0;      // sender of the radio packet (SiK: 51 / 68)
    quint8 componentId = 0;
    quint8 rssi = 0;          // raw radio units, as MP10 stores them
    quint8 remrssi = 0;
    quint8 txbuf = 0;
    quint8 noise = 0;
    quint8 remnoise = 0;
    quint16 rxerrors = 0;
    quint16 fixedCount = 0;   // MAVLink "fixed" (corrected packets)

    bool isValid() const { return valid; }
    // Single-sample MP10 ratios before the 50 % filter.
    double localSnrDbRaw() const;
    double remoteSnrDbRaw() const;
};

Q_DECLARE_METATYPE(RadioStatusSample)

class RadioStatusMonitor final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 SnrHoldMs = 1000;   // CurrentState.cs:1940 lastrssi.AddSeconds(1)
    static constexpr double SnrDivisor = 1.9;   // CurrentState.cs:1942
    static constexpr double SnrAlpha = 0.5;     // CurrentState.cs:1942

    explicit RadioStatusMonitor(QObject *parent = nullptr);

    static bool IsRadioStatusMessage(quint32 messageId) noexcept;

    // Stores the decoded packet as the link's latest sample and emits
    // sampleReceived(). Returns false for every other message id. The sender
    // sysid/compid are recorded but never used as a filter (MP10 :2302).
    bool observe(int linkId, const mavlink_message_t &message, qint64 nowMs);

    bool hasSample(int linkId) const;
    RadioStatusSample lastSample(int linkId) const;
    QList<int> linkIds() const;

    // MP10 localsnrdb / remotesnrdb: 0 without a sample; otherwise the filtered
    // value, advanced by one step when this is the first read for the link or
    // at least SnrHoldMs passed since the previous step. A clock that moved
    // backwards re-anchors the hold without stepping.
    double localSnrDb(int linkId, qint64 nowMs);
    double remoteSnrDb(int linkId, qint64 nowMs);

    void forgetLink(int linkId);
    void clear();

signals:
    void sampleReceived(int linkId, const RadioStatusSample &sample);
    void linkForgotten(int linkId);

private:
    struct SnrFilter
    {
        bool stepped = false;
        qint64 lastStepMs = 0;
        double value = 0.0;
        double read(double raw, qint64 nowMs);
    };
    struct LinkState
    {
        RadioStatusSample sample;
        SnrFilter local;
        SnrFilter remote;
    };

    QHash<int, LinkState> m_links;
};

#endif // RADIOSTATUSMONITOR_H
