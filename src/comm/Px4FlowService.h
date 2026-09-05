#ifndef PX4FLOWSERVICE_H
#define PX4FLOWSERVICE_H

#include "MavlinkComponentInstanceLease.h"
#include "ParameterService.h"
#include "Px4FlowFrameAssembler.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <mavlink.h>

class MavlinkComponentRegistry;

/**
 * Application-owned PX4Flow controller.
 *
 * The selected sensor is an exact component instance on one physical-link
 * session.  It is deliberately independent of the globally selected
 * autopilot.  A reservation remains owned by this object while VIDEO_ONLY is
 * being restored to zero, even if every view observing the service is closed.
 */
class Px4FlowService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaximumSources = 64;
    static constexpr int PartialFrameTimeoutMs = 2000;
    static constexpr int StreamStaleTimeoutMs = 3000;
    static constexpr int CleanupDeadlineMs = 15000;
    static constexpr int QuarantinePollMs = 100;

    explicit Px4FlowService(MavlinkComponentRegistry *registry,
                            ParameterService *parameterService,
                            QObject *parent = nullptr);
    ~Px4FlowService() override;

    QVariantList sources() const;
    QString selectedSourceId() const { return m_selectedSourceId; }
    QString status() const;
    QImage frame() const { return m_frame; }
    bool videoOnly() const { return m_modeKnown && m_videoOnly; }
    bool canToggle() const;
    bool busy() const;

public slots:
    void activate();
    void deactivate();
    void selectSource(const QString &sourceId);
    void toggleFocus();
    void observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);
    void beginReplay(quint64 generation);
    void observeReplay(quint64 generation,
                       const mavlink_message_t &message);
    void endReplay(quint64 generation);
    void shutdown();

signals:
    void changed();

private:
    enum class OperationPurpose {
        None,
        DiscoverMode,
        ToggleMode,
        CleanupRead,
        CleanupWrite
    };

    static QString sourceId(const MavlinkComponentInstanceLease &lease);
    static QString replaySourceId(quint64 generation);
    static bool terminalIsUncertain(
        ParameterService::ExactTerminalResult terminal);
    static bool modeFromValue(const QVariant &value, bool *ok = nullptr);

    MavlinkComponentInstanceLease findSource(const QString &id) const;
    bool selectedLeaseIsCurrent() const;
    void bindSelectedSource();
    bool releaseReservation();
    void submitRead(OperationPurpose purpose);
    void submitWrite(bool enabled, OperationPurpose purpose);
    void handleExactReport(
        const ParameterService::ExactOperationReport &report);
    void beginCleanup(const QString &reason);
    void advanceCleanup();
    void finishCleanup(bool safe, const QString &message = QString());
    void applyPendingTransition();
    void retireSource(const MavlinkComponentInstanceLease &lease);

    void observeImageMessage(const mavlink_message_t &message);
    void beginFrame(const mavlink_message_t &message);
    void addFramePacket(const mavlink_message_t &message);
    void publishFrame(const Px4FlowFrameAssembler::Frame &frame);
    void resetStreamState(bool clearImage);
    void startReplayNow(quint64 generation);
    void emitChanged();

    QPointer<MavlinkComponentRegistry> m_registry;
    QPointer<ParameterService> m_parameterService;
    QMetaObject::Connection m_exactFinishedConnection;
    QMetaObject::Connection m_registryChangedConnection;
    QMetaObject::Connection m_registryRetiredConnection;

    QString m_selectedSourceId;
    QString m_pendingSourceId;
    QString m_liveSourceBeforeReplay;
    QString m_operationStatus;
    QString m_streamStatus;
    QString m_persistentWarning;
    MavlinkComponentInstanceLease m_boundLease;
    ParameterService::ExactReservationToken m_reservation;
    ParameterService::ExactOperationToken m_operation;
    OperationPurpose m_operationPurpose = OperationPurpose::None;
    ParameterType m_modeType = ParameterType::Unknown;
    QVariant m_uncertainValue;

    Px4FlowFrameAssembler m_assembler;
    QImage m_frame;
    QTimer m_partialFrameTimer;
    QTimer m_staleFrameTimer;
    QTimer m_cleanupPollTimer;
    QElapsedTimer m_cleanupElapsed;

    quint64 m_replayGeneration = 0;
    quint64 m_pendingReplayGeneration = 0;
    quint8 m_replaySystemId = 0;
    quint8 m_replayComponentId = 0;
    bool m_active = false;
    bool m_activeBeforeReplay = false;
    bool m_replay = false;
    bool m_modeKnown = false;
    bool m_videoOnly = false;
    bool m_modeUncertain = false;
    bool m_cleanupRequested = false;
    bool m_cleanupAttempted = false;
    bool m_sourceTransitionPending = false;
    bool m_apiCallInFlight = false;
    bool m_replayEndpointPinned = false;
    bool m_streamStaleLatched = false;
    bool m_shuttingDown = false;
};

#endif // PX4FLOWSERVICE_H
