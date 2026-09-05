#ifndef MAVLINKREPLAYSOURCE_H
#define MAVLINKREPLAYSOURCE_H

#include "comm/MAVLinkMessageMetaType.h"

#include <QMetaType>
#include <QObject>
#include <QString>

struct MAVLinkReplayLease
{
    quint64 generation = 0;
    QString displayName;

    bool isValid() const noexcept
    {
        return generation != 0;
    }
};

Q_DECLARE_METATYPE(MAVLinkReplayLease)

/**
 * GUI-thread source token for replayed MAVLink packets.
 *
 * A packet is published only while its generation owns the active lease.
 * Ending a source clears the lease before notifying observers, so queued
 * packets from an unloaded replay cannot leak into its successor.
 */
class MAVLinkReplaySource final : public QObject
{
    Q_OBJECT

public:
    explicit MAVLinkReplaySource(QObject *parent = nullptr);

    MAVLinkReplayLease beginSource(const QString &displayName);
    MAVLinkReplayLease activeLease() const;
    bool isActive(quint64 generation) const;
    bool publish(quint64 generation, const mavlink_message_t &message);
    bool endSource(quint64 expectedGeneration);

signals:
    void replayMessageObserved(quint64 generation,
                               mavlink_message_t message);
    void replaySourceEnded(quint64 generation);

private:
    quint64 m_lastGeneration = 0;
    MAVLinkReplayLease m_activeLease;
};

#endif // MAVLINKREPLAYSOURCE_H
