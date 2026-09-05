#ifndef MAVLINKINSPECTORTRAFFICSOURCE_H
#define MAVLINKINSPECTORTRAFFICSOURCE_H

#include <QObject>
#include <QPointer>
#include <QString>

#include "comm/MAVLinkMessageMetaType.h"

/**
 * Pins one MAVLink Inspector to one physical live-link instance or one replay
 * generation.
 *
 * The source deliberately has no transport or LinkManager dependency.  Its
 * owner supplies the already captured physical-link epoch (or replay
 * generation) with every event.  Once bound, a source cannot be rebound to a
 * different kind or identity; opening another inspector creates another
 * source.
 */
class MAVLinkInspectorTrafficSource final : public QObject
{
    Q_OBJECT

public:
    explicit MAVLinkInspectorTrafficSource(QObject *parent = nullptr);

    bool bindLive(QObject *physicalLink, int linkId, quint64 currentEpoch,
                  const QString &name);
    bool bindReplay(quint64 generation, const QString &name);

    QString status() const;

public slots:
    void observeLive(QObject *physicalLink, int linkId, quint64 epoch,
                     mavlink_message_t message);
    void beginLiveSession(QObject *physicalLink, int linkId, quint64 epoch);
    void endLiveSession(int linkId, quint64 epoch);
    void removeLiveLink(int linkId);

    void observeReplay(quint64 generation, mavlink_message_t message);
    void endReplay(quint64 generation);

signals:
    void messageReceived(mavlink_message_t message);
    void sourceReset();
    void statusChanged(QString status);

private:
    enum class SourceKind
    {
        Unbound,
        Live,
        Replay
    };

    bool liveSessionMatches(QObject *physicalLink, int linkId,
                            quint64 epoch) const;
    bool liveSessionIsCurrent(QObject *physicalLink, int linkId,
                              quint64 epoch) const;
    bool replaySessionMatches(quint64 generation) const;
    bool replaySessionIsCurrent(quint64 generation) const;
    quint64 setStatus(const QString &status);
    QString liveWaitingStatus() const;
    QString liveReceivingStatus() const;
    QString liveDisconnectedStatus(bool reconnect) const;
    QString replayWaitingStatus() const;
    QString replayReceivingStatus() const;
    void liveObjectDestroyed();

    SourceKind m_kind = SourceKind::Unbound;
    QPointer<QObject> m_physicalLink;
    int m_liveLinkId = -1;
    quint64 m_liveEpoch = 0;
    quint64 m_replayGeneration = 0;
    bool m_active = false;
    bool m_accepting = false;
    bool m_terminal = false;
    bool m_receiving = false;
    QString m_sourceName;
    QString m_status;
    quint64 m_stateRevision = 0;
};

#endif // MAVLINKINSPECTORTRAFFICSOURCE_H
