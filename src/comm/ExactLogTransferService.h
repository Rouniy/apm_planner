#ifndef EXACTLOGTRANSFERSERVICE_H
#define EXACTLOGTRANSFERSERVICE_H

#include "LogDownloadTracker.h"
#include "SwarmTelemetryRegistry.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <limits>
#include <memory>

class ExactLinkTransmitter;
class QSaveFile;
struct ExactLogTransferResult;

struct ExactLogTransferToken
{
    quint64 id = 0;

    bool isValid() const noexcept { return id != 0; }
};

inline bool operator==(const ExactLogTransferToken &left,
                       const ExactLogTransferToken &right) noexcept
{
    return left.id == right.id;
}

inline bool operator!=(const ExactLogTransferToken &left,
                       const ExactLogTransferToken &right) noexcept
{
    return !(left == right);
}

struct ExactLogEntry
{
    quint16 id = 0;
    quint16 numLogs = 0;
    quint16 lastLogNumber = 0;
    quint32 timeUtc = 0;
    quint32 size = 0;
};

/**
 * Application-owned LOG_* protocol service pinned to one exact vehicle
 * instance and physical route identity.
 *
 * One operation owns the protocol application-wide. All public calls and
 * direct signal receivers must run on this object's QObject thread.
 */
class ExactLogTransferService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    enum class StartResult
    {
        Started,
        Busy,
        InvalidOwner,
        InvalidLease,
        InvalidArgument,
        UnsafeRoute,
        IoError,
        TransportUnavailable,
        IdentifierExhausted,
        ShuttingDown
    };
    Q_ENUM(StartResult)

    enum class Operation
    {
        None,
        List,
        Download,
        Erase
    };
    Q_ENUM(Operation)

    enum class Outcome
    {
        Completed,
        SubmittedUnconfirmed,
        Cancelled,
        TimedOut,
        LeaseRetired,
        TransportFailed,
        IoError,
        ProtocolError
    };
    Q_ENUM(Outcome)

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &vehicle, QString *error)>;
    using RouteIdentityProvider = std::function<QString(
        const SwarmVehicleInstanceLease &vehicle)>;
    using Clock = std::function<qint64()>;

    static constexpr int DefaultListTimeoutMs = 5000;
    static constexpr int DefaultListRetries = 4;
    static constexpr int DefaultStreamSilenceMs = 3000;
    static constexpr int DefaultRepairSilenceMs = 500;
    static constexpr int DefaultSilenceBudgetMs = 30000;
    static constexpr quint32 MaximumRepairRequest =
        LogDownloadTracker::PacketSize * 50;
    static constexpr int MaximumDeferredBytes =
        static_cast<int>(LogDownloadTracker::PacketSize
                         * LogDownloadTracker::MaximumRanges);

    ExactLogTransferService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ExactLogTransferService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        Clock clock,
        int listTimeoutMs = DefaultListTimeoutMs,
        int listRetries = DefaultListRetries,
        int streamSilenceMs = DefaultStreamSilenceMs,
        int repairSilenceMs = DefaultRepairSilenceMs,
        int silenceBudgetMs = DefaultSilenceBudgetMs,
        QObject *parent = nullptr);
    ~ExactLogTransferService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setRouteIdentityProvider(RouteIdentityProvider provider);

    StartResult requestList(
        QObject *owner,
        const SwarmVehicleInstanceLease &vehicle,
        ExactLogTransferToken *token,
        QString *error = nullptr);
    StartResult startDownload(
        QObject *owner,
        const SwarmVehicleInstanceLease &vehicle,
        quint16 logId,
        quint32 advertisedSize,
        const QString &destinationPath,
        ExactLogTransferToken *token,
        QString *error = nullptr);
    StartResult erase(
        QObject *owner,
        const SwarmVehicleInstanceLease &vehicle,
        ExactLogTransferToken *token,
        QString *error = nullptr);

    bool cancel(const ExactLogTransferToken &token,
                const QString &reason = QString());
    void shutdown();

    bool busy() const noexcept
    {
        return m_startInFlight || m_finishing || m_active.token.isValid();
    }
    ExactLogTransferToken activeToken() const noexcept
    {
        return m_active.token;
    }

    /** Feed one packet with its immutable ingress physical-session epoch. */
    void observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);

signals:
    void busyChanged(bool busy);
    void progress(ExactLogTransferToken token, qulonglong completed,
                  qulonglong total, bool totalKnown);
    void transferFinished(ExactLogTransferResult result);

private slots:
    void timeout();

private:
    struct ActiveOperation
    {
        ExactLogTransferToken token;
        SwarmVehicleInstanceLease vehicle;
        Operation operation = Operation::None;
        QPointer<QObject> owner;
        QMetaObject::Connection ownerDestroyed;
        QString routeIdentity;
        QString destinationPath;
        quint16 logId = 0;
        quint32 advertisedSize = 0;
        QMap<quint16, ExactLogEntry> entries;
        quint16 expectedListCount = 0;
        quint16 lastLogNumber = 0;
        int listRetriesRemaining = 0;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
        std::unique_ptr<QSaveFile> file;
        LogDownloadTracker tracker;
        QMap<quint32, char> deferredBytes;
        LogDownloadRequest lastRequest;
        int silenceBudgetRemainingMs = 0;
        qint64 deadlineMs = -1;
        quint64 activityRevision = 0;
    };

    StartResult beginOperation(
        QObject *owner,
        const SwarmVehicleInstanceLease &vehicle,
        Operation operation,
        const QString &destinationPath,
        quint16 logId,
        quint32 advertisedSize,
        ExactLogTransferToken *token,
        QString *error);
    bool exactLeaseIsCurrent(
        const SwarmVehicleInstanceLease &vehicle) const;
    bool validateStartRoute(
        const SwarmVehicleInstanceLease &vehicle,
        QString *identity,
        QString *error);
    bool validateActiveRoute(
        const ExactLogTransferToken &token,
        QString *error);
    bool tokenIsCurrent(
        const ExactLogTransferToken &token) const noexcept;

    bool sendListRequest(const ExactLogTransferToken &token,
                         QString *error = nullptr);
    bool sendDataRequest(const ExactLogTransferToken &token,
                         const LogDownloadRequest &request,
                         QString *error = nullptr);
    bool sendErase(const ExactLogTransferToken &token,
                   QString *error = nullptr);
    bool sendEndBestEffort(const ExactLogTransferToken &token);
    bool sendMessage(const ExactLogTransferToken &token,
                     mavlink_message_t message,
                     QString *error = nullptr,
                     bool validateRoute = true);

    void observeListEntry(const ExactLogTransferToken &token,
                          const mavlink_message_t &message);
    void observeLogData(const ExactLogTransferToken &token,
                        const mavlink_message_t &message);
    bool storeDeferredData(quint32 offset, const QByteArray &data,
                           QString *error);
    bool writeData(quint32 offset, const QByteArray &data,
                   QString *error);
    bool flushDeferred(QString *error);
    quint64 immediateWriteLimit() const;
    bool commitDownload(const ExactLogTransferToken &token,
                        QString *error);

    void armTimeout(int intervalMs);
    void disarmTimeout();
    int currentDownloadSilenceWindow() const;
    void finishActive(const ExactLogTransferToken &token,
                      Outcome outcome,
                      const QString &error = QString());
    void failForRouteChange(const ExactLogTransferToken &token,
                            const QString &error);
    QList<ExactLogEntry> completedEntries() const;
    qint64 nowMs() const;
    void assertServiceThread() const;

    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<ExactLinkTransmitter> m_transmitter;
    RouteValidator m_routeValidator;
    RouteIdentityProvider m_routeIdentityProvider;
    Clock m_clock;
    mutable QElapsedTimer m_monotonicClock;
    mutable qint64 m_lastNowMs = 0;
    QTimer m_timeoutTimer;
    ActiveOperation m_active;
    quint64 m_nextTokenId = 0;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    int m_listTimeoutMs = DefaultListTimeoutMs;
    int m_listRetries = DefaultListRetries;
    int m_streamSilenceMs = DefaultStreamSilenceMs;
    int m_repairSilenceMs = DefaultRepairSilenceMs;
    int m_silenceBudgetMs = DefaultSilenceBudgetMs;
    bool m_startInFlight = false;
    bool m_finishing = false;
    bool m_shuttingDown = false;
};

struct ExactLogTransferResult
{
    ExactLogTransferToken token;
    SwarmVehicleInstanceLease vehicle;
    ExactLogTransferService::Operation operation =
        ExactLogTransferService::Operation::None;
    ExactLogTransferService::Outcome outcome =
        ExactLogTransferService::Outcome::ProtocolError;
    QString errorString;
    QString destinationPath;
    QList<ExactLogEntry> entries;
    quint64 covered = 0;
    quint64 total = 0;
    bool totalKnown = false;

    bool succeeded() const noexcept
    {
        return outcome == ExactLogTransferService::Outcome::Completed;
    }
};

Q_DECLARE_METATYPE(ExactLogTransferToken)
Q_DECLARE_METATYPE(ExactLogEntry)
Q_DECLARE_METATYPE(ExactLogTransferResult)
Q_DECLARE_METATYPE(ExactLogTransferService::StartResult)
Q_DECLARE_METATYPE(ExactLogTransferService::Operation)
Q_DECLARE_METATYPE(ExactLogTransferService::Outcome)

#endif // EXACTLOGTRANSFERSERVICE_H
