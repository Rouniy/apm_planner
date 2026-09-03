#ifndef NMEAOUTPUTSERVICE_H
#define NMEAOUTPUTSERVICE_H

#include "MavlinkMirrorOutput.h"
#include "NmeaSentenceBuilder.h"
#include "VehicleEndpoint.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class QTimer;

struct NmeaOutputSettings
{
    QString portSelection;
    int baud = 4800;
    double rateHz = 5.0;
};

/**
 * Mission Planner 10 NMEA Output session without UI or LinkManager coupling.
 *
 * A session pins one exact vehicle endpoint. Incoming MAVLink messages update a
 * small CurrentState-compatible snapshot and a GUI-thread timer emits the five
 * MP10 NMEA sentences. Output is live data: a complete tick is dropped instead
 * of being queued behind a slow consumer.
 *
 * The proven mirror transports are reused only as bounded Serial/TCP/UDP byte
 * sinks. This service owns the NMEA-specific port labels, status and one-line-
 * per-datagram behavior; it does not mirror MAVLink or permit write-back.
 */
class NmeaOutputService final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Emitting,
        Failed
    };
    Q_ENUM(State)

    struct Status
    {
        State state = State::Stopped;
        QString text;
        QString selection;
        VehicleEndpoint endpoint;
        QString linkName;
        QString lastSentence;
        quint64 sentencesSent = 0;
        quint64 droppedSentences = 0;
        quint64 ticks = 0;
    };

    using OutputFactory = std::function<std::unique_ptr<MavlinkMirrorOutput>(
        const NmeaOutputSettings &settings, QString *error)>;
    using UdpPortGuard = std::function<bool(quint16 port, QString *reason)>;
    using Clock = std::function<QDateTime()>;

    static constexpr quint16 DefaultHostPort = 14551;

    static QString TcpHostSelection();
    static QString UdpHostSelection();
    static bool IsTcpHostSelection(const QString &selection);
    static bool IsUdpHostSelection(const QString &selection);
    static QStringList Selections(const QStringList &serialPorts);
    static OutputFactory ProductionOutputFactory();

    static QString StoppedText();
    static QString PickPortText();
    static QString ErrorConnectingText(const QString &reason);
    static QString SourceRemovedText(const QString &linkName);

    explicit NmeaOutputService(
        OutputFactory outputFactory = ProductionOutputFactory(),
        Clock clock = Clock(), QObject *parent = nullptr);
    ~NmeaOutputService() override;

    void setUdpPortGuard(UdpPortGuard guard);
    bool start(const VehicleEndpoint &endpoint, const QString &linkName,
               const NmeaOutputSettings &settings, QString *error = nullptr);
    void stop();
    void setRateHz(double rateHz);

    bool isRunning() const { return m_output != nullptr; }
    State state() const { return m_state; }
    Status status() const;
    VehicleEndpoint endpoint() const { return m_endpoint; }
    int timerIntervalMs() const;

    /** Emits one timer tick immediately; public for deterministic service tests. */
    void emitNow();

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void clear();

signals:
    void statusChanged();

private:
    void refreshOutputStatus();
    void fail(const QString &text);
    void releaseOutput();
    void resetSession();
    void setStopped(const QString &text);
    void updateTimerInterval();

    OutputFactory m_outputFactory;
    UdpPortGuard m_udpPortGuard;
    Clock m_clock;
    std::unique_ptr<MavlinkMirrorOutput> m_output;
    QTimer *m_timer = nullptr;
    State m_state = State::Stopped;
    QString m_text;
    NmeaOutputSettings m_settings;
    VehicleEndpoint m_endpoint;
    QString m_linkName;
    NmeaVehicleState m_vehicleState;
    QString m_lastSentence;
    quint64 m_sentencesSent = 0;
    quint64 m_droppedSentences = 0;
    quint64 m_ticks = 0;
};

#endif // NMEAOUTPUTSERVICE_H
