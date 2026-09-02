#ifndef DRONECANGETNODEINFOCLIENT_H
#define DRONECANGETNODEINFOCLIENT_H

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtGlobal>

class DroneCanGetNodeInfoClient final : public QObject
{
    Q_OBJECT

public:
    static constexpr quint8 GetNodeInfoDataTypeId = 1;
    static constexpr quint64 GetNodeInfoSignature =
        Q_UINT64_C(0xEE468A8121C46A9E);
    static constexpr quint16 GetNodeInfoBaseCrc = 0xD9A7U;
    static constexpr int ServicePriority = 30;
    static constexpr int RequestTimeoutMs = 1000;
    static constexpr int MinimumRetryIntervalMs = 500;
    static constexpr int MaximumAttempts = 10;
    static constexpr int MaximumRetries = MaximumAttempts - 1;
    static constexpr int AssemblyTimeoutMs = 2000;
    static constexpr int MaximumAssemblyBytes = 392;
    static constexpr int MaximumClassicResponsePayloadBytes = 376;
    static constexpr int MaximumFdResponsePayloadBytes = 377;
    static constexpr quint32 ExtendedFrameFlag = 0x80000000U;
    static constexpr quint32 RemoteTransmissionFlag = 0x40000000U;
    static constexpr quint32 ErrorFrameFlag = 0x20000000U;
    static constexpr quint32 ExtendedIdMask = 0x1FFFFFFFU;

    struct Session {
        quint64 brokerGeneration = 0;
        int busIndex = -1;
        int localNodeId = 127;

        bool isValid() const;
        bool operator==(const Session &other) const;
        bool operator!=(const Session &other) const
        {
            return !(*this == other);
        }
    };

    struct NodeStatus {
        quint32 uptimeSeconds = 0;
        quint8 health = 0;
        quint8 mode = 0;
        quint8 subMode = 0;
        quint16 vendorSpecificStatusCode = 0;
    };

    struct SoftwareVersion {
        quint8 major = 0;
        quint8 minor = 0;
        quint8 optionalFieldFlags = 0;
        quint32 vcsCommit = 0;
        quint64 imageCrc = 0;

        bool hasVcsCommit() const
        {
            return (optionalFieldFlags & 1U) != 0;
        }
        bool hasImageCrc() const
        {
            return (optionalFieldFlags & 2U) != 0;
        }
    };

    struct HardwareVersion {
        quint8 major = 0;
        quint8 minor = 0;
        QByteArray uniqueId;
        QByteArray certificateOfAuthenticity;
    };

    struct NodeInfo {
        quint64 brokerGeneration = 0;
        int busIndex = -1;
        int nodeId = -1;
        quint8 transferId = 0;
        bool canFd = false;
        NodeStatus status;
        SoftwareVersion softwareVersion;
        HardwareVersion hardwareVersion;
        QString name;
    };

    explicit DroneCanGetNodeInfoClient(QObject *parent = nullptr);

    bool bindSession(const Session &session);
    void clearPendingRequests();
    void resetSession();
    bool isBound() const;
    Session session() const;
    int pendingRequestCount() const;

    bool requestNodeInfo(int remoteNodeId, bool preferCanFd,
                         qint64 nowMs);
    bool acceptFrame(quint64 brokerGeneration, int busIndex,
                     quint32 canId, const QByteArray &data,
                     bool canFd, qint64 nowMs);
    void tick(qint64 nowMs);

signals:
    void transmitRequested(quint32 canId, const QByteArray &data,
                           bool canFd);
    void nodeInfoReceived(
        const DroneCanGetNodeInfoClient::NodeInfo &info);
    void requestFailed(int nodeId, const QString &reason);

private:
    struct Assembly {
        bool active = false;
        bool canFd = false;
        bool expectedToggle = true;
        qint64 startedAtMs = 0;
        quint16 expectedCrc = 0;
        QByteArray payload;
    };

    struct Request {
        int remoteNodeId = -1;
        bool preferCanFd = false;
        quint8 transferId = 0;
        int retryCount = 0;
        qint64 lastTransmitMs = 0;
        qint64 deadlineMs = 0;
        Assembly assembly;
    };

    static bool isCanonicalFrameLength(int length, bool canFd);
    static quint16 addCrcByte(quint16 crc, quint8 byte);
    static quint16 payloadCrc(const QByteArray &payload);
    static bool decodeResponse(const QByteArray &payload, bool canFd,
                               NodeInfo *info, QString *error);
    static bool validNodeName(const QByteArray &name);

    quint32 requestCanId(int remoteNodeId) const;
    quint8 allocateTransferId(int remoteNodeId);
    void transmit(Request *request, qint64 nowMs);
    void dropAssembly(Request *request, qint64 nowMs);
    void failRequest(int remoteNodeId, const QString &reason);
    bool finishTransfer(Request *request, const QByteArray &payload,
                        bool canFd, qint64 nowMs);

    Session m_session;
    QHash<int, Request> m_requests;
    // UAVCAN transfer-ID state belongs to the service transfer descriptor;
    // GetNodeInfo therefore needs an independent sequence per destination.
    QHash<int, quint8> m_nextTransferIdByNode;
};

Q_DECLARE_METATYPE(DroneCanGetNodeInfoClient::NodeInfo)

#endif // DRONECANGETNODEINFOCLIENT_H
