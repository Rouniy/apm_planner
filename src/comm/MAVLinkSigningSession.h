#ifndef MAVLINKSIGNINGSESSION_H
#define MAVLINKSIGNINGSESSION_H

#include <QByteArray>
#include <QHash>
#include <QString>

#include <array>
#include <memory>

class MAVLinkSigningClock;

/** Authentication state for ONE secret key, shared by all physical links using it.
 *
 * This is deliberately separate from byte framing and from vehicle provisioning.
 * A caller must retain this object across physical disconnects: clearing its
 * stream table on reconnect would admit replay on another physical link.
 * Single owning thread; no callbacks, implicit key adoption or transport writes.
 */
class MAVLinkSigningSession final
{
public:
    enum class Verdict {
        Signed, UnsignedRadio, UnsignedRejected, InvalidFrame, BadSignature,
        Replay, TooOld, StreamLimit, ClockUnavailable, NotReady
    };
    struct Verification {
        Verdict verdict = Verdict::NotReady;
        QString error;
        bool accepted() const {
            return verdict == Verdict::Signed || verdict == Verdict::UnsignedRadio;
        }
    };
    struct Counters {
        quint64 signedAccepted = 0;
        quint64 unsignedRadioAccepted = 0;
        quint64 rejected = 0;
        quint64 signedSent = 0;
    };
    static constexpr int MaximumStreams = 256;

    MAVLinkSigningSession(const QByteArray &key,
                         std::shared_ptr<MAVLinkSigningClock> clock);
    ~MAVLinkSigningSession();
    MAVLinkSigningSession(const MAVLinkSigningSession &) = delete;
    MAVLinkSigningSession &operator=(const MAVLinkSigningSession &) = delete;

    bool isReady() const;
    // Only one complete, CRC-valid, UNSIGNED MAVLink 2 frame is accepted.
    // Header identity, sequence, payload length and extension bytes are retained.
    // Timestamp is consumed before returning the frame, even if transport fails.
    bool signFrame(const QByteArray &frame, quint8 stableSigningLinkId,
                   qint64 unixMs, QByteArray *signedFrame, QString *error = nullptr);
    Verification verifyFrame(const QByteArray &frame, qint64 unixMs);
    Counters counters() const { return m_counters; }
    int streamCount() const { return m_streams.size(); }

private:
    std::array<unsigned char, 32> m_key{};
    bool m_validKey = false;
    std::shared_ptr<MAVLinkSigningClock> m_clock;
    QHash<quint32, quint64> m_streams;
    Counters m_counters;
};

#endif
