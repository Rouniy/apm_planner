#ifndef UBLOXBASESTATIONPROTOCOL_H
#define UBLOXBASESTATIONPROTOCOL_H

#include <QByteArray>
#include <QDateTime>
#include <QStringList>
#include <QVector>

// Pure UBX framing/receiver configuration. No serial ownership, sleeps, vehicle
// writes or implied ACK success. The caller executes Command timing/baud steps.
class UbloxBaseStationProtocol final
{
public:
    static constexpr int MaximumPayload = 8192;
    struct Command {
        QByteArray bytes;
        int baudRate = 0; // Set before sending; zero means keep current baud.
        int delayBeforeMs = 0, delayAfterMs = 0;
        QString description;
    };
    struct Packet { quint8 messageClass = 0, messageId = 0; QByteArray payload; };
    struct Version { QString software, hardware; QStringList extensions; };
    struct SurveyIn {
        quint32 timeOfWeekMs = 0, durationSeconds = 0, observations = 0;
        bool valid = false, active = false, hasPosition = false;
        double xMeters = 0, yMeters = 0, zMeters = 0, accuracyMeters = 0;
        double latitude = 0, longitude = 0, altitudeMeters = 0;
    };
    struct Position {
        quint32 timeOfWeekMs = 0;
        QDateTime utc;
        quint8 fixType = 0, satellites = 0, carrierSolution = 0;
        bool fixOk = false, differential = false;
        double latitude = 0, longitude = 0, altitudeMeters = 0, mslAltitudeMeters = 0;
        double horizontalAccuracyMeters = 0, verticalAccuracyMeters = 0;
    };
    struct Acknowledgement { bool accepted = false; quint8 messageClass = 0, messageId = 0; };
    struct Statistics { quint64 frames = 0, badChecksums = 0, oversizedFrames = 0; };

    // Incremental byte API keeps both retained memory and each return bounded.
    bool read(quint8 byte, Packet *completed);
    void reset(); // Clears parser AND counters.
    Statistics statistics() const { return m_statistics; }
    int bufferedBytes() const { return m_buffer.size(); }
    static QByteArray frame(quint8 messageClass, quint8 messageId, const QByteArray &payload = {});
    static bool decodeVersion(const Packet &, Version *, QString *error = nullptr);
    static bool decodeSurveyIn(const Packet &, SurveyIn *, QString *error = nullptr);
    static bool decodePosition(const Packet &, Position *, QString *error = nullptr);
    static bool decodeAcknowledgement(const Packet &, Acknowledgement *, QString *error = nullptr);

    // SetupM8P is shared by M8P/F9P in MP10. Includes the baud hunt, UU prefix,
    // USB/UART protocol setup, 1 Hz stationary NAV and all reference message rates.
    static QVector<Command> autoConfigure(int initialBaud, bool m8p130Plus = true);
    static QVector<Command> disableBase(); // TMODE3 disable, BBR save, receiver reset.
    static QByteArray surveyIn(quint32 durationSeconds = 60, double accuracyMeters = 2,
                              QString *error = nullptr);
    // Exact MP10 legacy fixedPosAcc scaling (metres *1000, unlike SVIN *10000).
    // No ECEF-via-latitude guessing; this method accepts explicit LLA only.
    static QByteArray fixedLla(double latitude, double longitude, double altitudeMeters,
                              double accuracyMeters = 0.001, QString *error = nullptr);
    static QVector<Command> restartSurvey(int initialBaud, quint32 durationSeconds,
                                         double accuracyMeters, bool m8p130Plus = true,
                                         QString *error = nullptr);
private:
    QByteArray m_buffer;
    int m_expected = 0;
    Statistics m_statistics;
};

#endif
