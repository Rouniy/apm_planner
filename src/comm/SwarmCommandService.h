#ifndef SWARMCOMMANDSERVICE_H
#define SWARMCOMMANDSERVICE_H

#include "SwarmTelemetryRegistry.h"

#include <QElapsedTimer>
#include <QFlags>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class ExactLinkTransmitter;

struct SwarmTelemetryRequirements
{
    enum Field {
        NoFields = 0x00,
        Position = 0x01,
        Velocity = 0x02,
        Heading = 0x04,
        Attitude = 0x08,
        VfrHud = 0x10,
        ExtendedSystemState = 0x20
    };
    Q_DECLARE_FLAGS(Fields, Field)

    Fields fields = NoFields;
    int heartbeatMaximumAgeMs = 5000;
    int positionMaximumAgeMs = 1500;
    int velocityMaximumAgeMs = 1500;
    int headingMaximumAgeMs = 1500;
    int attitudeMaximumAgeMs = 1500;
    int vfrHudMaximumAgeMs = 1500;
    int extendedSystemStateMaximumAgeMs = 2000;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(SwarmTelemetryRequirements::Fields)

struct SwarmCommandMember
{
    enum class FlightModeRequirement {
        Any,
        ArduPilotGuided
    };

    int slotId = 0;
    SwarmVehicleInstanceLease lease;
    SwarmTelemetryRequirements required;
    FlightModeRequirement flightMode = FlightModeRequirement::Any;
};

struct SwarmCommandSessionToken
{
    quint64 id = 0;

    bool isValid() const noexcept { return id != 0; }
};

struct SwarmPositionTarget
{
    int slotId = 0;
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    float relativeAltitudeM = 0.0F;
    float velocityNorthMps = 0.0F;
    float velocityEastMps = 0.0F;
    float velocityDownMps = 0.0F;
    bool useVelocity = true;
};

class SwarmCommandService final : public QObject
{
    Q_OBJECT

public:
    enum class Result {
        Reserved,
        SentAll,
        RejectedBeforeSend,
        PartialSend,
        Busy,
        InvalidSession,
        InvalidPlan,
        StaleLease,
        TelemetryStale,
        UnsafeRoute,
        InvalidPayload,
        RateLimited,
        TransportUnavailable,
        Cancelled
    };
    Q_ENUM(Result)

    struct MemberReport
    {
        int slotId = 0;
        SwarmVehicleInstanceLease lease;
        Result result = Result::RejectedBeforeSend;
        int framesPlanned = 0;
        int framesSent = 0;
        QString detail;
    };

    struct BatchReport
    {
        quint64 sessionId = 0;
        Result result = Result::RejectedBeforeSend;
        QVector<MemberReport> members;
        QString detail;

        bool allSent() const noexcept { return result == Result::SentAll; }
    };

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;
    using Clock = std::function<qint64()>;

    explicit SwarmCommandService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    SwarmCommandService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        Clock clock,
        QObject *parent = nullptr);

    void setLocalIdentity(quint8 systemId, quint8 componentId);

    /** Read-only UI preflight; reserve() repeats this check authoritatively. */
    bool routeIsEligible(const SwarmVehicleInstanceLease &lease,
                         QString *error = nullptr) const;

    Result reserve(QObject *owner,
                   const QVector<SwarmCommandMember> &members,
                   int maximumBatchHz,
                   SwarmCommandSessionToken *token,
                   QString *error = nullptr);
    Result release(const SwarmCommandSessionToken &token);

    BatchReport requestPositionAndAttitudeStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds,
        int rateHz);
    BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds,
        int rateHz);
    BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets);

    bool hasActiveSession() const noexcept;
    quint64 activeSessionId() const noexcept { return m_active.id; }

signals:
    void sessionCancelled(qulonglong sessionId, QString reason);

private:
    struct ActiveSession
    {
        quint64 id = 0;
        QPointer<QObject> owner;
        QVector<SwarmCommandMember> members;
        int maximumBatchHz = 0;
        qint64 lastBatchMs = -1;
        qint64 lastStreamRequestMs = -1;
        QMetaObject::Connection ownerDestroyed;
    };

    static bool validTarget(const SwarmPositionTarget &target);
    static quint64 nextSessionId();
    static QString endpointLabel(const SwarmVehicleInstanceLease &lease);
    mavlink_message_t positionMessage(
        const SwarmCommandMember &member,
        const SwarmPositionTarget &target) const;
    mavlink_message_t streamRequestMessage(
        const SwarmCommandMember &member, int streamId, int rateHz) const;

    const SwarmCommandMember *memberForSlot(int slotId) const;
    bool tokenIsCurrent(const SwarmCommandSessionToken &token) const;
    bool validateMember(const SwarmCommandMember &member,
                        bool requireTelemetryFields,
                        Result *failure,
                        QString *error) const;
    bool validateAll(QList<SwarmTelemetrySnapshot> *snapshots,
                     bool requireTelemetryFields,
                     Result *failure,
                     QString *error) const;
    bool validateRoute(const SwarmCommandMember &member,
                       QString *error) const;
    BatchReport preflightReport(quint64 requestedSessionId,
                                Result result,
                                const QString &detail,
                                bool includeActiveMembers = true) const;
    BatchReport sendMessages(
        const SwarmCommandSessionToken &token,
        const QVector<QPair<SwarmCommandMember, mavlink_message_t>> &messages,
        bool rateLimited,
        bool requireTelemetryFields);
    BatchReport requestStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds,
        int rateHz,
        bool includeAttitude);
    void cancelActive(const QString &reason, bool publishSignal);
    qint64 nowMs() const;

    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<ExactLinkTransmitter> m_transmitter;
    RouteValidator m_routeValidator;
    Clock m_clock;
    mutable QElapsedTimer m_monotonicClock;
    ActiveSession m_active;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    bool m_operationInFlight = false;
};

Q_DECLARE_METATYPE(SwarmCommandService::Result)
Q_DECLARE_METATYPE(SwarmCommandService::MemberReport)
Q_DECLARE_METATYPE(SwarmCommandService::BatchReport)

#endif // SWARMCOMMANDSERVICE_H
