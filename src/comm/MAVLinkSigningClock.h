#ifndef MAVLINKSIGNINGCLOCK_H
#define MAVLINKSIGNINGCLOCK_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <memory>

class QLockFile;

/**
 * Process-crash-safe source of MAVLink 2 signing timestamps.
 *
 * Timestamps are 10 microsecond ticks since 2015-01-01 UTC.  The on-disk
 * high-water mark is reserved ahead of use, so neither a process crash nor a
 * wall-clock rollback can cause a timestamp to be reused. Atomic replacement
 * prevents a torn state from an application failure; this does not claim
 * storage durability across sudden power loss. This class stores no key.
 */
class MAVLinkSigningClock final
{
public:
    static constexpr quint64 MaxTimestamp = (quint64(1) << 48) - 1;
    static constexpr qint64 EpochUnixMs = 1420070400000LL;

    explicit MAVLinkSigningClock(QString path);
    ~MAVLinkSigningClock();

    bool open(qint64 unixMs, QString *error = nullptr);
    bool next(qint64 unixMs, quint64 *timestamp, QString *error = nullptr);
    bool observeVerified(quint64 timestamp, qint64 unixMs,
                         QString *error = nullptr);
    quint64 current(qint64 unixMs) const;
    bool isOpen() const;

private:
    Q_DISABLE_COPY(MAVLinkSigningClock)

    static bool wallTimestamp(qint64 unixMs, quint64 *timestamp,
                              QString *error);
    bool reserveFor(quint64 timestamp, QString *error);
    bool writeReservedThrough(quint64 timestamp, bool expectedExists,
                              const QByteArray &expectedState,
                              QLockFile *lease, QString *error) const;

    const QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    QByteArray m_expectedState;
    quint64 m_nextFloor = 0;
    quint64 m_reservedThrough = 0;
    bool m_open = false;
    bool m_exhausted = false;
};

#endif // MAVLINKSIGNINGCLOCK_H
