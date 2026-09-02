#ifndef MISSIONTRANSFERCONTROLLER_H
#define MISSIONTRANSFERCONTROLLER_H

#include "MissionProtocolCoordinator.h"
#include "MissionTransferService.h"
#include "MissionTransferTransport.h"

#include <QMetaType>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

struct MissionTransferResult
{
    quint64 transferId = 0;
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION;
    MissionTransferService::Direction direction =
            MissionTransferService::Direction::None;
    MissionTransferService::State state = MissionTransferService::State::Idle;
    MAV_MISSION_RESULT result = MAV_MISSION_ERROR;
    QString errorString;
    QVector<mavlink_mission_item_int_t> downloadedItems;

    bool succeeded() const
    {
        return state == MissionTransferService::State::Complete
                && result == MAV_MISSION_ACCEPTED;
    }
};

Q_DECLARE_METATYPE(MissionTransferResult)

/**
 * Owns one MAVLink mission transfer and its protocol lease.
 *
 * The controller, coordinator and transport must all live on the same thread.
 * MissionTransferService remains the deterministic protocol state machine;
 * this class supplies lease, raw MAVLink codec, timeout and transport cleanup.
 */
class MissionTransferController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(quint64 transferId READ transferId NOTIFY transferIdChanged)

public:
    explicit MissionTransferController(
            MissionProtocolCoordinator *coordinator,
            MissionTransferTransport *transport,
            int timeoutMs = 1500,
            int maxRetries = 3,
            QObject *parent = nullptr);
    ~MissionTransferController() override;

    bool startDownload(MAV_MISSION_TYPE missionType);
    bool startUpload(
            MAV_MISSION_TYPE missionType,
            const QVector<mavlink_mission_item_int_t> &items);
    bool cancel(const QString &reason = QString());

    bool busy() const { return m_busy; }
    int progress() const { return m_progress; }
    quint64 transferId() const { return m_transferId; }

signals:
    void busyChanged(bool busy);
    void progressChanged(int progress);
    void transferIdChanged(quint64 transferId);
    void transferFinished(const MissionTransferResult &result);

private slots:
    void handleMessage(const mavlink_message_t &message);
    void handleUnavailable();
    void timeout();

private:
    bool startOperation(
            MAV_MISSION_TYPE missionType,
            MissionTransferService::Direction direction,
            const QVector<mavlink_mission_item_int_t> &items);
    bool applyTransition(const MissionTransferService::Transition &transition);
    bool sendOutbound(
            const MissionTransferService::OutboundMessage &outbound);
    void armTimeout();
    void disarmTimeout();
    void updateProgress();
    void setProgress(int progress);
    void failActive(const QString &reason);
    void finishTransfer();
    void cleanupWithoutSignal();
    bool ownsLease() const;

    static bool coordinatorTypeFor(
            MAV_MISSION_TYPE missionType,
            MissionProtocolCoordinator::MissionType *coordinatorType);
    static bool isTerminal(MissionTransferService::State state);

    QPointer<MissionProtocolCoordinator> m_coordinator;
    QPointer<MissionTransferTransport> m_transport;
    MissionTransferService m_service;
    MissionProtocolCoordinator::LeaseToken m_lease;
    QTimer m_timeoutTimer;
    int m_timeoutMs = 1500;
    bool m_busy = false;
    bool m_transportOperation = false;
    int m_progress = 0;
    quint64 m_transferId = 0;
    quint64 m_operationEpoch = 0;
    quint64 m_timerEpoch = 0;
    quint64 m_timerGeneration = 0;
    QMetaObject::Connection m_timeoutConnection;

    Q_DISABLE_COPY(MissionTransferController)
};

#endif // MISSIONTRANSFERCONTROLLER_H
