#ifndef DRONECANGETSETCLIENT_H
#define DRONECANGETSETCLIENT_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtGlobal>

class DroneCanGetSetClient final : public QObject
{
    Q_OBJECT

public:
    static constexpr quint8 GetSetDataTypeId = 11;
    static constexpr quint64 GetSetSignature =
        Q_UINT64_C(0xA7B622F939D1A4D5);
    static constexpr quint16 GetSetBaseCrc = 0xFB10U;
    static constexpr int ServicePriority = 30;
    static constexpr int RequestTimeoutMs = 1000;
    static constexpr int MinimumRetryIntervalMs = 500;
    static constexpr int MaximumAttempts = 10;
    static constexpr int MaximumRetries = MaximumAttempts - 1;
    static constexpr int AssemblyTimeoutMs = 2000;
    static constexpr int MaximumParameterNameBytes = 92;
    static constexpr int MaximumStringValueBytes = 128;
    static constexpr int MaximumClassicResponsePayloadBytes = 370;
    static constexpr int MaximumFdResponsePayloadBytes = 371;
    // A maximum-size CAN-FD response can acquire up to five zero padding
    // bytes in order to fit a canonical CAN-FD DLC. Padding is part of the
    // transfer CRC and is rejected unless it is zero.
    static constexpr int MaximumAssemblyBytes = 376;
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

    struct Value {
        enum Type : quint8 {
            Empty = 0,
            Integer = 1,
            Real = 2,
            Boolean = 3,
            String = 4
        };

        Type type = Empty;
        qint64 integerValue = 0;
        float realValue = 0.0F;
        bool booleanValue = false;
        QByteArray stringValue;

        static Value fromInteger(qint64 value);
        static Value fromReal(float value);
        static Value fromBoolean(bool value);
        static Value fromString(const QByteArray &value);
        bool isValid() const;
        bool operator==(const Value &other) const;
    };

    struct NumericValue {
        enum Type : quint8 {
            Empty = 0,
            Integer = 1,
            Real = 2
        };

        Type type = Empty;
        qint64 integerValue = 0;
        float realValue = 0.0F;

        bool isValid() const;
        bool operator==(const NumericValue &other) const;
    };

    struct Parameter {
        quint64 brokerGeneration = 0;
        int busIndex = -1;
        int nodeId = -1;
        quint8 transferId = 0;
        bool canFd = false;
        quint16 requestedIndex = 0;
        QByteArray requestedName;
        QByteArray name;
        Value value;
        Value defaultValue;
        NumericValue maximumValue;
        NumericValue minimumValue;

        bool exists() const
        {
            return !name.isEmpty() && value.type != Value::Empty;
        }
    };

    explicit DroneCanGetSetClient(QObject *parent = nullptr);

    bool bindSession(const Session &session);
    void clearPendingRequest();
    void resetSession();
    bool isBound() const;
    Session session() const;
    bool hasPendingRequest() const;
    int pendingRemoteNodeId() const;

    bool getByIndex(int remoteNodeId, quint16 index, bool preferCanFd,
                    qint64 nowMs);
    bool getByName(int remoteNodeId, const QByteArray &name,
                   bool preferCanFd, qint64 nowMs);
    bool setByName(int remoteNodeId, const QByteArray &name,
                   const Value &value, bool preferCanFd, qint64 nowMs);
    bool requestParameter(int remoteNodeId, quint16 index,
                          const QByteArray &name, const Value &value,
                          bool preferCanFd, qint64 nowMs);
    bool cancelPendingRequest(
        const QString &reason = QStringLiteral("GetSet request cancelled"));
    bool acceptFrame(quint64 brokerGeneration, int busIndex,
                     quint32 canId, const QByteArray &data,
                     bool canFd, qint64 nowMs);
    void tick(qint64 nowMs);

signals:
    void transmitRequested(quint32 canId, const QByteArray &data,
                           bool canFd);
    void parameterReceived(
        const DroneCanGetSetClient::Parameter &parameter);
    void requestFailed(int nodeId, const QString &reason);
    void requestCancelled(int nodeId, const QString &reason);

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
        quint64 serial = 0;
        int remoteNodeId = -1;
        quint16 index = 0;
        QByteArray name;
        Value value;
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
    static bool encodeRequestPayload(const Request &request,
                                     QByteArray *payload);
    static bool decodeResponse(const QByteArray &payload, bool canFd,
                               Parameter *parameter, QString *error);
    static QList<QByteArray> makeTransferFrames(
        const QByteArray &logicalPayload, bool canFd, quint8 transferId);

    quint32 requestCanId(int remoteNodeId) const;
    quint8 allocateTransferId(int remoteNodeId);
    void transmit(qint64 nowMs);
    void dropAssembly(qint64 nowMs);
    void failRequest(const QString &reason);
    bool finishTransfer(const QByteArray &payload, bool canFd,
                        qint64 nowMs);

    Session m_session;
    Request m_request;
    bool m_hasPendingRequest = false;
    quint64 m_nextRequestSerial = 1;
    // A service transfer descriptor contains the bus and destination node.
    // Keep an independent 5-bit sequence for every descriptor.
    QHash<quint16, quint8> m_nextTransferIdByDescriptor;
};

Q_DECLARE_METATYPE(DroneCanGetSetClient::Value)
Q_DECLARE_METATYPE(DroneCanGetSetClient::NumericValue)
Q_DECLARE_METATYPE(DroneCanGetSetClient::Parameter)

#endif // DRONECANGETSETCLIENT_H
