#ifndef COTOUTPUTSERVICE_H
#define COTOUTPUTSERVICE_H

#include "CotEventSerializer.h"
#include "CotOutputTransport.h"
#include "VehicleEndpoint.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

#include <mavlink.h>

struct CotOutputServiceSettings
{
    CotOutputTransport::Settings transport;
    double updateIntervalSeconds = 10.0;
    CotEventSettings event;
    // Identity is deliberately system-wide: every component with the same
    // MAVLink system id receives the same MP10 identity override.
    QHash<int, CotIdentityOverride> identities;
};

/**
 * Live Cursor-on-Target output session pinned to one physical MAVLink link.
 *
 * updateEndpoints() supplies the current LinkManager snapshot. Every exact
 * (link, system, component) endpoint on the pinned link owns independent
 * navigation state and produces one event per tick after receiving a position.
 * No payload is queued by the service: transport backpressure drops that live
 * event and is reflected in bounded status counters.
 */
class CotOutputService final : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Stopped,
        Starting,
        Emitting,
        Failed
    };
    Q_ENUM(State)

    struct Status
    {
        State state = State::Stopped;
        QString text;
        int linkId = -1;
        QString linkName;
        CotOutputTransport::State transportState =
            CotOutputTransport::State::Stopped;
        QString preview;
        bool previewTruncated = false;
        int registeredEndpoints = 0;
        int positionedEndpoints = 0;
        quint64 ticks = 0;
        quint64 eventsGenerated = 0;
        quint64 eventsSent = 0;
        quint64 eventsDropped = 0;
        quint64 eventsWithoutPeer = 0;
        quint64 errors = 0;
        CotOutputTransport::SendResult lastSendResult =
            CotOutputTransport::SendResult::NotReady;
        QString lastError;
    };

    using TransportFactory =
        std::function<std::unique_ptr<CotOutputTransport>(
            const CotOutputTransport::Settings &settings, QObject *parent)>;
    // CotOutputTransport is final. This narrow sender hook keeps payload/result
    // behavior deterministic in unit tests while production calls send().
    using Sender = std::function<CotOutputTransport::SendResult(
        CotOutputTransport *transport, const QByteArray &payload)>;
    using Clock = std::function<QDateTime()>;

    static constexpr double MinimumIntervalSeconds = 0.1;
    static constexpr double MaximumIntervalSeconds = 3600.0;
    static constexpr int MaximumStatusTextLength = 1024;
    static constexpr int MaximumPreviewCharacters = 64 * 1024;

    static TransportFactory ProductionTransportFactory();
    static Sender ProductionSender();
    static QString StoppedText();
    static QString ErrorStartingText(const QString &reason);
    static QString SourceRemovedText(const QString &linkName);
    static bool IsValidInterval(double seconds);

    explicit CotOutputService(
        TransportFactory transportFactory = ProductionTransportFactory(),
        Sender sender = ProductionSender(), Clock clock = Clock(),
        QObject *parent = nullptr);
    ~CotOutputService() override;

    bool start(int linkId, const QString &linkName,
               const CotOutputServiceSettings &settings,
               QString *error = nullptr);
    void stop();

    void updateEndpoints(const QList<VehicleEndpoint> &endpoints);
    void setEventSettings(const CotEventSettings &settings);
    void setIdentityOverrides(
        const QHash<int, CotIdentityOverride> &identities);
    bool setUpdateIntervalSeconds(double seconds);

    bool isRunning() const { return m_transport != nullptr; }
    State state() const { return m_state; }
    Status status() const;
    CotOutputServiceSettings settings() const { return m_settings; }
    int linkId() const { return m_linkId; }
    QString linkName() const { return m_linkName; }
    QList<VehicleEndpoint> activeEndpoints() const
    {
        return m_activeEndpoints;
    }
    int timerIntervalMs() const;

    /** Execute one prepared timer tick immediately (deterministic test hook). */
    void emitNow();

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void clear();

signals:
    void statusChanged();

private slots:
    void handleTransportStateChanged(CotOutputTransport::State state);
    void handleTransportStatusChanged(const QString &text);
    void handleTransportError(const QString &error);

private:
    struct EndpointState
    {
        CotNavigationState navigation;
        bool hasPosition = false;
    };

    void rebuildActiveEndpoints();
    void activateIfPrepared();
    void updateTimerInterval();
    void releaseTransport();
    void setStopped(const QString &text);
    void fail(const QString &text, const QString &error = QString());
    void resetCounters();
    void recordTransportError(const QString &error);
    bool applyMessage(EndpointState &state,
                      const mavlink_message_t &message);

    static QString boundedStatusText(const QString &text);
    static CotEventSettings boundedEventSettings(
        const CotEventSettings &settings);
    static QHash<int, CotIdentityOverride> boundedIdentities(
        const QHash<int, CotIdentityOverride> &identities);
    static void incrementSaturated(quint64 &counter);

    TransportFactory m_transportFactory;
    Sender m_sender;
    Clock m_clock;
    std::unique_ptr<CotOutputTransport> m_transport;
    QTimer m_timer;
    State m_state = State::Stopped;
    QString m_text;
    CotOutputServiceSettings m_settings;
    int m_linkId = -1;
    QString m_linkName;
    QList<VehicleEndpoint> m_knownEndpoints;
    QList<VehicleEndpoint> m_activeEndpoints;
    QHash<VehicleEndpoint, EndpointState> m_endpointStates;
    QString m_preview;
    QString m_lastError;
    bool m_previewTruncated = false;
    quint64 m_tickGeneration = 0;
    quint64 m_generation = 0;
    quint64 m_endpointGeneration = 0;
    quint64 m_ticks = 0;
    quint64 m_eventsGenerated = 0;
    quint64 m_eventsSent = 0;
    quint64 m_eventsDropped = 0;
    quint64 m_eventsWithoutPeer = 0;
    quint64 m_errors = 0;
    CotOutputTransport::SendResult m_lastSendResult =
        CotOutputTransport::SendResult::NotReady;
};

#endif // COTOUTPUTSERVICE_H
