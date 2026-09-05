#include "MAVLinkSigningSession.h"
#include "MAVLinkSigningClock.h"

#include <QCryptographicHash>
#include <openssl/crypto.h>
#include <mavlink.h>

#include <algorithm>
#include <cstring>

namespace {
struct Frame {
    bool valid = false;
    bool v2 = false;
    bool signedPacket = false;
    int header = 0;
    int payload = 0;
    int crcOffset = 0;
    quint32 messageId = 0;
    quint8 sysid = 0;
    quint8 compid = 0;
    quint8 crcExtra = 0;
};

Frame inspect(const QByteArray &bytes)
{
    Frame f;
    if (bytes.size() < MAVLINK_CORE_HEADER_MAVLINK1_LEN + 3) return f;
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    f.v2 = p[0] == MAVLINK_STX;
    if (!f.v2 && p[0] != MAVLINK_STX_MAVLINK1) return f;
    f.header = f.v2 ? MAVLINK_NUM_HEADER_BYTES : MAVLINK_CORE_HEADER_MAVLINK1_LEN + 1;
    if (bytes.size() < f.header + 2) return f;
    f.payload = p[1];
    if (f.v2 && (p[2] & ~MAVLINK_IFLAG_SIGNED)) return f;
    f.signedPacket = f.v2 && (p[2] & MAVLINK_IFLAG_SIGNED);
    f.crcOffset = f.header + f.payload;
    if (bytes.size() != f.crcOffset + 2 + (f.signedPacket ? MAVLINK_SIGNATURE_BLOCK_LEN : 0))
        return f;
    f.messageId = f.v2 ? quint32(p[7]) | (quint32(p[8]) << 8) | (quint32(p[9]) << 16) : p[5];
    f.sysid = p[f.v2 ? 5 : 3];
    f.compid = p[f.v2 ? 6 : 4];
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(f.messageId);
    if (!entry || (!f.v2 && f.payload != entry->min_msg_len)
        || (f.v2 && (!f.payload || f.payload > entry->max_msg_len)))
        return f;
    f.crcExtra = entry->crc_extra;
    uint16_t crc = crc_calculate(p + 1, f.crcOffset - 1);
    crc_accumulate(f.crcExtra, &crc);
    f.valid = p[f.crcOffset] == (crc & 255) && p[f.crcOffset + 1] == (crc >> 8);
    return f;
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
}

MAVLinkSigningSession::MAVLinkSigningSession(
    const QByteArray &key, std::shared_ptr<MAVLinkSigningClock> clock)
    : m_clock(std::move(clock))
{
    m_validKey = key.size() == int(m_key.size())
        && std::any_of(key.cbegin(), key.cend(), [](char byte) { return byte != 0; });
    if (m_validKey) std::memcpy(m_key.data(), key.constData(), m_key.size());
}

MAVLinkSigningSession::~MAVLinkSigningSession()
{
    OPENSSL_cleanse(m_key.data(), m_key.size());
}

bool MAVLinkSigningSession::isReady() const
{
    return m_validKey && m_clock && m_clock->isOpen();
}

bool MAVLinkSigningSession::signFrame(const QByteArray &frame,
                                    quint8 stableSigningLinkId, qint64 unixMs,
                                    QByteArray *signedFrame, QString *error)
{
    if (error) error->clear();
    if (!signedFrame) return fail(error, QStringLiteral("No signed-frame destination."));
    // Support aliasing input/output without clearing the caller's input early.
    const QByteArray input = frame;
    signedFrame->clear();
    if (!isReady()) return fail(error, QStringLiteral("Signing key or persistent clock is unavailable."));
    const Frame f = inspect(input);
    if (!f.valid || !f.v2 || f.signedPacket)
        return fail(error, QStringLiteral("Signing requires exactly one valid unsigned MAVLink 2 frame."));

    quint64 timestamp = 0;
    if (!m_clock->next(unixMs, &timestamp, error)) return false;
    QByteArray result = input;
    result[2] = char(MAVLINK_IFLAG_SIGNED);
    auto *p = reinterpret_cast<unsigned char *>(result.data());
    uint16_t crc = crc_calculate(p + 1, f.crcOffset - 1);
    crc_accumulate(f.crcExtra, &crc);
    p[f.crcOffset] = crc & 255;
    p[f.crcOffset + 1] = crc >> 8;

    // Encode the protocol's LE48 explicitly. The older generated helper uses
    // a host-endian union for this field, which is not portable to big-endian.
    result.append(char(stableSigningLinkId));
    for (int i = 0; i < 6; ++i) result.append(char((timestamp >> (8 * i)) & 255));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(reinterpret_cast<const char *>(m_key.data()), int(m_key.size()));
    hash.addData(result);
    result.append(hash.result().left(6));
    *signedFrame = std::move(result);
    ++m_counters.signedSent;
    return true;
}

MAVLinkSigningSession::Verification MAVLinkSigningSession::verifyFrame(
    const QByteArray &frame, qint64 unixMs)
{
    const auto reject = [this](Verdict verdict, const QString &error = QString()) {
        ++m_counters.rejected;
        return Verification{verdict, error};
    };
    if (!isReady()) return reject(Verdict::NotReady);
    const Frame f = inspect(frame);
    if (!f.valid) return reject(Verdict::InvalidFrame);
    if (!f.signedPacket) {
        if (f.messageId == MAVLINK_MSG_ID_RADIO_STATUS || f.messageId == MAVLINK_MSG_ID_RADIO) {
            ++m_counters.unsignedRadioAccepted;
            return {Verdict::UnsignedRadio, {}};
        }
        return reject(Verdict::UnsignedRejected);
    }

    const auto *p = reinterpret_cast<const unsigned char *>(frame.constData());
    const auto *signature = p + f.crcOffset + 2;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(reinterpret_cast<const char *>(m_key.data()), int(m_key.size()));
    hash.addData(frame.constData(), f.crcOffset + 2 + 7);
    const QByteArray digest = hash.result();
    if (CRYPTO_memcmp(digest.constData(), signature + 7, 6) != 0)
        return reject(Verdict::BadSignature);

    quint64 timestamp = 0;
    for (int i = 0; i < 6; ++i) timestamp |= quint64(signature[i + 1]) << (8 * i);
    const quint32 stream = (quint32(f.sysid) << 16) | (quint32(f.compid) << 8) | signature[0];
    const auto previous = m_streams.constFind(stream);
    if (previous != m_streams.constEnd()) {
        if (timestamp <= previous.value()) return reject(Verdict::Replay);
    } else {
        const quint64 current = m_clock->current(unixMs);
        if (!current) return reject(Verdict::ClockUnavailable);
        if (timestamp < current && current - timestamp > 6000000ULL)
            return reject(Verdict::TooOld);
        if (m_streams.size() >= MaximumStreams) return reject(Verdict::StreamLimit);
    }
    QString error;
    // Never learn a clock or allocate a stream until CRC, MAC AND replay checks
    // succeed. Publish nothing if durable forward-clock reservation fails.
    if (!m_clock->observeVerified(timestamp, unixMs, &error))
        return reject(Verdict::ClockUnavailable, error);
    m_streams.insert(stream, timestamp);
    ++m_counters.signedAccepted;
    return {Verdict::Signed, {}};
}
