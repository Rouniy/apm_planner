#include "MAVLinkSigningClock.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QDir>
#include <QLockFile>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace {

constexpr quint32 StateVersion = 1;
constexpr quint64 ReservationTicks = 100000;
constexpr int HeaderSize = 8 + 4 + 8;
constexpr int DigestSize = 32;
constexpr int StateSize = HeaderSize + DigestSize;
const QByteArray StateMagic("APMSCLK1", 8);

void appendU32(QByteArray *bytes, quint32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes->append(static_cast<char>((value >> shift) & 0xffU));
    }
}

void appendU64(QByteArray *bytes, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes->append(static_cast<char>((value >> shift) & 0xffU));
    }
}

quint32 readU32(const QByteArray &bytes, int offset)
{
    quint32 value = 0;
    for (int index = 0; index < 4; ++index) {
        value = (value << 8)
            | static_cast<quint8>(bytes.at(offset + index));
    }
    return value;
}

quint64 readU64(const QByteArray &bytes, int offset)
{
    quint64 value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8)
            | static_cast<quint8>(bytes.at(offset + index));
    }
    return value;
}

QByteArray encodeState(quint64 reservedThrough)
{
    QByteArray header;
    header.reserve(HeaderSize);
    header.append(StateMagic);
    appendU32(&header, StateVersion);
    appendU64(&header, reservedThrough);
    return header + QCryptographicHash::hash(
        header, QCryptographicHash::Sha256);
}

bool decodeState(const QByteArray &state, quint64 *reservedThrough,
                 QString *error)
{
    if (state.size() != StateSize) {
        if (error) {
            *error = QStringLiteral(
                "Signing clock state has an invalid length.");
        }
        return false;
    }
    const QByteArray header = state.left(HeaderSize);
    if (header.left(StateMagic.size()) != StateMagic
        || readU32(header, StateMagic.size()) != StateVersion) {
        if (error) {
            *error = QStringLiteral(
                "Signing clock state has an unsupported format.");
        }
        return false;
    }
    const QByteArray expectedDigest = QCryptographicHash::hash(
        header, QCryptographicHash::Sha256);
    if (state.mid(HeaderSize) != expectedDigest) {
        if (error) {
            *error = QStringLiteral(
                "Signing clock state failed its integrity check.");
        }
        return false;
    }
    const quint64 value = readU64(header, StateMagic.size() + 4);
    if (value > MAVLinkSigningClock::MaxTimestamp) {
        if (error) {
            *error = QStringLiteral(
                "Signing clock state contains an invalid timestamp.");
        }
        return false;
    }
    if (reservedThrough) {
        *reservedThrough = value;
    }
    return true;
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool pathHasCanonicalParent(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.fileName().isEmpty()) {
        setError(error, QStringLiteral(
            "Signing clock path must be an absolute file path."));
        return false;
    }
    const QString requestedParent = QDir::cleanPath(info.dir().absolutePath());
    const QString canonicalParent = info.dir().canonicalPath();
    const QString requestedPath = QDir::cleanPath(info.absoluteFilePath());
    const QString canonicalPath = canonicalParent.isEmpty()
        ? QString() : QDir(canonicalParent).filePath(info.fileName());
    if (canonicalParent.isEmpty() || requestedParent != canonicalParent
        || requestedPath != canonicalPath) {
        setError(error, QStringLiteral(
            "Signing clock parent path must exist and be canonical."));
        return false;
    }
    return true;
}

bool readStateSnapshot(const QString &path, bool *exists, QByteArray *state,
                       QString *error)
{
    if (exists) {
        *exists = false;
    }
    if (state) {
        state->clear();
    }
    const QFileInfo info(path);
    if (info.isSymLink()) {
        setError(error, QStringLiteral(
            "Signing clock state must not be a symbolic link."));
        return false;
    }
    if (!info.exists()) {
        return true;
    }
    if (!info.isFile() || info.size() != StateSize) {
        setError(error, QStringLiteral(
            "Signing clock state is not a valid bounded file."));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read signing clock state: %1")
                            .arg(file.errorString()));
        return false;
    }
    const QByteArray bytes = file.read(StateSize + 1);
    if (bytes.size() != StateSize) {
        setError(error, QStringLiteral(
            "Signing clock state changed while it was being read."));
        return false;
    }
    if (exists) {
        *exists = true;
    }
    if (state) {
        *state = bytes;
    }
    return true;
}

bool lockFileIsRestricted(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.exists() || !info.isFile()) {
        setError(error, QStringLiteral(
            "Signing clock lock file was replaced or removed."));
        return false;
    }
    const QFileDevice::Permissions permissions = info.permissions();
    if (!permissions.testFlag(QFileDevice::ReadOwner)
        || !permissions.testFlag(QFileDevice::WriteOwner)) {
        setError(error, QStringLiteral(
            "Signing clock lock file has invalid permissions."));
        return false;
    }
#ifndef Q_OS_WIN
    if (permissions & (QFileDevice::ExeOwner | QFileDevice::ReadGroup
                       | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                       | QFileDevice::ReadOther | QFileDevice::WriteOther
                       | QFileDevice::ExeOther)) {
        setError(error, QStringLiteral(
            "Signing clock lock file permissions are too broad."));
        return false;
    }
#endif
    return true;
}

} // namespace

MAVLinkSigningClock::MAVLinkSigningClock(QString path)
    : m_path(std::move(path))
{
}

MAVLinkSigningClock::~MAVLinkSigningClock() = default;

bool MAVLinkSigningClock::open(qint64 unixMs, QString *error)
{
    if (error) {
        error->clear();
    }
    if (m_open) {
        setError(error, QStringLiteral("Signing clock is already open."));
        return false;
    }
    if (m_path.isEmpty()) {
        setError(error, QStringLiteral("Signing clock path is empty."));
        return false;
    }
    if (!pathHasCanonicalParent(m_path, error)) {
        return false;
    }

    quint64 wall = 0;
    if (!wallTimestamp(unixMs, &wall, error)) {
        return false;
    }

    const QString lockPath = m_path + QStringLiteral(".lock");
    if (QFileInfo(lockPath).isSymLink()) {
        setError(error, QStringLiteral(
            "Signing clock lock must not be a symbolic link."));
        return false;
    }
    auto lock = std::make_unique<QLockFile>(lockPath);
    // Never take over an apparently stale signing clock.  A second process
    // must fail closed instead of risking a duplicate reserved range.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        const QString message = lock->error() == QLockFile::LockFailedError
            ? QStringLiteral("Signing clock is locked by another process.")
            : lock->error() == QLockFile::PermissionError
                ? QStringLiteral(
                    "Cannot create the signing clock lock file.")
                : QStringLiteral("Cannot acquire the signing clock lock.");
        setError(error, message);
        return false;
    }
    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QFile::setPermissions(lockPath, ownerOnly)
        || !lockFileIsRestricted(lockPath, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "Cannot restrict signing clock lock permissions.");
        }
        lock->unlock();
        return false;
    }

    quint64 persistedReservedThrough = 0;
    bool stateExists = false;
    QByteArray stateSnapshot;
    if (!readStateSnapshot(m_path, &stateExists, &stateSnapshot, error)) {
        return false;
    }
    if (stateExists) {
        if (!decodeState(stateSnapshot, &persistedReservedThrough, error)) {
            return false;
        }
    }

    if (persistedReservedThrough == MaxTimestamp) {
        setError(error, QStringLiteral("Signing timestamp space is exhausted."));
        return false;
    }
    const quint64 persistedFloor = persistedReservedThrough + 1;
    const quint64 initialFloor = std::max(persistedFloor, wall);
    if (initialFloor == 0 || initialFloor > MaxTimestamp) {
        setError(error, QStringLiteral("Signing timestamp space is exhausted."));
        return false;
    }

    const quint64 reserveEnd = initialFloor > MaxTimestamp - (ReservationTicks - 1)
        ? MaxTimestamp : initialFloor + ReservationTicks - 1;
    if (!writeReservedThrough(reserveEnd, stateExists, stateSnapshot,
                              lock.get(), error)) {
        return false;
    }

    m_lock = std::move(lock);
    m_expectedState = encodeState(reserveEnd);
    m_nextFloor = initialFloor;
    m_reservedThrough = reserveEnd;
    m_exhausted = false;
    m_open = true;
    return true;
}

bool MAVLinkSigningClock::next(qint64 unixMs, quint64 *timestamp,
                               QString *error)
{
    if (error) {
        error->clear();
    }
    if (timestamp) {
        *timestamp = 0;
    }
    if (!m_open) {
        setError(error, QStringLiteral("Signing clock is not open."));
        return false;
    }
    if (!timestamp) {
        setError(error, QStringLiteral("Signing timestamp output is null."));
        return false;
    }
    if (m_exhausted) {
        setError(error, QStringLiteral("Signing timestamp space is exhausted."));
        return false;
    }

    quint64 wall = 0;
    if (!wallTimestamp(unixMs, &wall, error)) {
        return false;
    }
    const quint64 candidate = std::max(m_nextFloor, wall);
    if (candidate == 0 || candidate > MaxTimestamp) {
        setError(error, QStringLiteral("Signing timestamp space is exhausted."));
        return false;
    }
    if (!reserveFor(candidate, error)) {
        return false;
    }

    *timestamp = candidate;
    if (candidate == MaxTimestamp) {
        m_exhausted = true;
    } else {
        m_nextFloor = candidate + 1;
    }
    return true;
}

bool MAVLinkSigningClock::observeVerified(quint64 timestamp, qint64 unixMs,
                                          QString *error)
{
    if (error) {
        error->clear();
    }
    if (!m_open) {
        setError(error, QStringLiteral("Signing clock is not open."));
        return false;
    }
    quint64 ignoredWall = 0;
    if (!wallTimestamp(unixMs, &ignoredWall, error)) {
        return false;
    }
    if (m_exhausted || timestamp >= MaxTimestamp) {
        setError(error, QStringLiteral("Verified signing timestamp cannot be advanced."));
        return false;
    }

    const quint64 advancedFloor = std::max(m_nextFloor, timestamp + 1);
    if (!reserveFor(advancedFloor, error)) {
        return false;
    }
    m_nextFloor = advancedFloor;
    return true;
}

quint64 MAVLinkSigningClock::current(qint64 unixMs) const
{
    if (!m_open || m_exhausted) {
        return 0;
    }
    quint64 wall = 0;
    if (!wallTimestamp(unixMs, &wall, nullptr)) {
        return 0;
    }
    const quint64 value = std::max(m_nextFloor, wall);
    return value <= MaxTimestamp ? value : 0;
}

bool MAVLinkSigningClock::isOpen() const
{
    return m_open;
}

bool MAVLinkSigningClock::wallTimestamp(qint64 unixMs, quint64 *timestamp,
                                        QString *error)
{
    if (!timestamp || unixMs < EpochUnixMs) {
        setError(error, QStringLiteral(
            "Wall clock predates the MAVLink signing epoch."));
        return false;
    }
    const quint64 milliseconds = static_cast<quint64>(unixMs - EpochUnixMs);
    if (milliseconds > MaxTimestamp / 100) {
        setError(error, QStringLiteral(
            "Wall clock exceeds the MAVLink signing timestamp range."));
        return false;
    }
    *timestamp = milliseconds * 100;
    return true;
}

bool MAVLinkSigningClock::reserveFor(quint64 timestamp, QString *error)
{
    if (!m_lock || !m_lock->isLocked()) {
        setError(error, QStringLiteral("Signing clock lock was lost."));
        return false;
    }
    if (timestamp <= m_reservedThrough) {
        return true;
    }
    if (timestamp == 0 || timestamp > MaxTimestamp) {
        setError(error, QStringLiteral("Signing timestamp space is exhausted."));
        return false;
    }
    const quint64 reserveEnd = timestamp > MaxTimestamp - (ReservationTicks - 1)
        ? MaxTimestamp : timestamp + ReservationTicks - 1;
    if (!writeReservedThrough(reserveEnd, true, m_expectedState,
                              m_lock.get(), error)) {
        return false;
    }
    m_expectedState = encodeState(reserveEnd);
    m_reservedThrough = reserveEnd;
    return true;
}

bool MAVLinkSigningClock::writeReservedThrough(
    quint64 timestamp, bool expectedExists, const QByteArray &expectedState,
    QLockFile *lease, QString *error) const
{
    if (!lease || !lease->isLocked()) {
        setError(error, QStringLiteral("Signing clock lock was lost."));
        return false;
    }
    if (!pathHasCanonicalParent(m_path, error)
        || !lockFileIsRestricted(m_path + QStringLiteral(".lock"), error)) {
        return false;
    }
    bool actualExists = false;
    QByteArray actualState;
    if (!readStateSnapshot(m_path, &actualExists, &actualState, error)) {
        return false;
    }
    if (actualExists != expectedExists
        || (expectedExists && actualState != expectedState)) {
        setError(error, QStringLiteral(
            "Signing clock state changed outside this process."));
        return false;
    }

    const QByteArray state = encodeState(timestamp);
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot stage signing clock state: %1")
                            .arg(file.errorString()));
        return false;
    }
    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(ownerOnly)) {
        setError(error, QStringLiteral(
            "Cannot restrict signing clock state permissions."));
        file.cancelWriting();
        return false;
    }
    if (file.write(state) != state.size()) {
        setError(error, QStringLiteral("Cannot write signing clock state: %1")
                            .arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Cannot publish signing clock state: %1")
                            .arg(file.errorString()));
        return false;
    }
    bool publishedExists = false;
    QByteArray publishedState;
    if (!readStateSnapshot(
            m_path, &publishedExists, &publishedState, error)
        || !publishedExists || publishedState != state) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "Published signing clock state could not be verified.");
        }
        return false;
    }
    return true;
}
