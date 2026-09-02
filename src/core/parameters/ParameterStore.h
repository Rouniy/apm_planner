#ifndef PARAMETERSTORE_H
#define PARAMETERSTORE_H

#include "comm/VehicleEndpoint.h"
#include "core/VehicleTarget.h"
#include "core/parameters/ParameterCodec.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>

struct ParameterKey
{
    quint8 componentId = 0;
    QString name;
};

inline bool operator==(const ParameterKey &left, const ParameterKey &right)
{
    return left.componentId == right.componentId && left.name == right.name;
}

inline uint qHash(const ParameterKey &key, uint seed = 0)
{
    return qHash(key.name, seed) ^ (static_cast<uint>(key.componentId) << 24U);
}

struct ParameterRecord
{
    ParameterKey key;
    QVariant value;
    ParameterType type = ParameterType::Unknown;
    int index = -1;
    int reportedCount = 0;
    quint64 targetRevision = 0;
    QDateTime receivedAtUtc;
};

enum class ParameterLoadState
{
    Idle,
    Loading,
    Partial,
    Complete,
    Cancelled,
    Failed
};

struct ParameterProgress
{
    int received = 0;
    int reported = 0;

    int percent() const
    {
        return reported > 0 ? qMin(100, received * 100 / reported) : 0;
    }

    bool complete() const
    {
        return reported > 0 && received >= reported;
    }
};

class ParameterSnapshot
{
public:
    // VehicleTarget is retained for existing pure-core callers. New code
    // should use endpoint(), whose identity also includes the physical link.
    VehicleTarget target() const { return m_target; }
    VehicleEndpoint endpoint() const { return m_endpoint; }
    ParameterLoadState state() const { return m_state; }
    ParameterProgress progress() const { return m_progress; }
    QList<ParameterRecord> records() const { return m_records.values(); }
    bool isComplete() const { return m_complete; }
    bool contains(quint8 componentId, const QString &name) const;
    ParameterRecord value(quint8 componentId, const QString &name) const;

private:
    friend class ParameterStore;
    VehicleTarget m_target;
    VehicleEndpoint m_endpoint;
    ParameterLoadState m_state = ParameterLoadState::Idle;
    ParameterProgress m_progress;
    QHash<ParameterKey, ParameterRecord> m_records;
    bool m_complete = false;
};

/**
 * Repository of committed parameter snapshots, keyed by exact MAVLink
 * endpoint (link, system and component).
 *
 * A list refresh is transactional. Incoming records are staged and replace a
 * committed snapshot only after all distinct, non-sentinel indices have been
 * received. Cancelling or failing a refresh therefore never destroys the last
 * usable parameter set. Selecting another endpoint only changes the view; it
 * does not evict either endpoint's cache.
 */
class ParameterStore final : public QObject
{
    Q_OBJECT
public:
    explicit ParameterStore(QObject *parent = nullptr);

    VehicleTarget target() const;
    VehicleEndpoint endpoint() const;
    QList<VehicleEndpoint> cachedEndpoints() const;
    ParameterLoadState state() const;
    ParameterProgress progress() const;
    ParameterSnapshot snapshot() const;
    ParameterSnapshot snapshot(const VehicleEndpoint &endpoint) const;
    bool hasSnapshot(const VehicleEndpoint &endpoint) const;
    bool specializedPagesReady() const;

    void selectTarget(int linkId, quint8 systemId, quint8 componentId);
    void selectEndpoint(const VehicleEndpoint &endpoint);

    void beginLoad();
    void beginLoad(const VehicleEndpoint &endpoint);
    void finishLoad();
    void finishLoad(const VehicleEndpoint &endpoint);
    void cancelLoad();
    void cancelLoad(const VehicleEndpoint &endpoint);
    void failLoad();
    void failLoad(const VehicleEndpoint &endpoint);

    // Compatibility overload: the link and system are taken from the current
    // selection, while componentId still identifies the exact packet source.
    bool ingest(quint8 componentId,
                int parameterCount,
                int parameterIndex,
                const QString &name,
                const QVariant &value,
                ParameterType type);
    bool ingest(const VehicleEndpoint &endpoint,
                int parameterCount,
                int parameterIndex,
                const QString &name,
                const QVariant &value,
                ParameterType type);

    bool removeEndpoint(const VehicleEndpoint &endpoint);
    void clear();

signals:
    void targetChanged();
    void selectedEndpointChanged(int linkId, int systemId, int componentId);
    void stateChanged(ParameterLoadState state);
    void progressChanged(int received, int reported, int percent);
    void parameterChanged(int componentId, const QString &name);

    void endpointStateChanged(int linkId,
                              int systemId,
                              int componentId,
                              ParameterLoadState state);
    void endpointProgressChanged(int linkId,
                                 int systemId,
                                 int componentId,
                                 int received,
                                 int reported,
                                 int percent);
    void endpointParameterChanged(int linkId,
                                  int systemId,
                                  int componentId,
                                  const QString &name);

private:
    static constexpr int UnknownWireValue = 65535;

    struct EndpointKey
    {
        int linkId = -1;
        int systemId = 0;
        int componentId = 0;
    };
    friend inline bool operator==(const EndpointKey &left,
                                  const EndpointKey &right)
    {
        return left.linkId == right.linkId
            && left.systemId == right.systemId
            && left.componentId == right.componentId;
    }
    friend inline uint qHash(const EndpointKey &key, uint seed)
    {
        seed = ::qHash(key.linkId, seed);
        seed = ::qHash(key.systemId, seed);
        return ::qHash(key.componentId, seed);
    }

    struct ParameterIndexKey
    {
        quint8 componentId = 0;
        int index = -1;
    };
    friend inline bool operator==(const ParameterIndexKey &left,
                                  const ParameterIndexKey &right)
    {
        return left.componentId == right.componentId
            && left.index == right.index;
    }
    friend inline uint qHash(const ParameterIndexKey &key, uint seed)
    {
        return ::qHash(key.index, seed)
            ^ (static_cast<uint>(key.componentId) << 24U);
    }

    struct EndpointCache
    {
        VehicleEndpoint endpoint;
        ParameterLoadState state = ParameterLoadState::Idle;

        QHash<ParameterKey, ParameterRecord> committedRecords;
        QHash<quint8, int> committedReportedByComponent;
        QSet<ParameterIndexKey> committedReceivedIndices;
        quint64 committedRevision = 0;
        bool committedComplete = false;

        QHash<ParameterKey, ParameterRecord> stagingRecords;
        QHash<quint8, int> stagingReportedByComponent;
        QSet<ParameterIndexKey> stagingReceivedIndices;
        quint64 stagingRevision = 0;
        bool stagingActive = false;
    };

    static bool endpointIsValid(const VehicleEndpoint &endpoint);
    static EndpointKey endpointKey(const VehicleEndpoint &endpoint);
    static ParameterProgress progressFor(
        const QHash<quint8, int> &reportedByComponent,
        const QSet<ParameterIndexKey> &receivedIndices);
    static ParameterProgress visibleProgress(const EndpointCache &cache);
    static void mergeEndpointMetadata(VehicleEndpoint *stored,
                                      const VehicleEndpoint &incoming);

    EndpointCache &ensureCacheLocked(const VehicleEndpoint &endpoint);
    const EndpointCache *findCacheLocked(const VehicleEndpoint &endpoint) const;
    bool isSelectedLocked(const VehicleEndpoint &endpoint) const;
    ParameterSnapshot snapshotLocked(const VehicleEndpoint &endpoint,
                                     const EndpointCache *cache) const;
    void commitStagingLocked(EndpointCache *cache);
    void discardStagingLocked(EndpointCache *cache);

    mutable QReadWriteLock m_lock;
    VehicleEndpoint m_endpoint;
    VehicleTarget m_target;
    quint64 m_revisionCounter = 0;
    QHash<EndpointKey, EndpointCache> m_caches;
};

Q_DECLARE_METATYPE(ParameterLoadState)

#endif
