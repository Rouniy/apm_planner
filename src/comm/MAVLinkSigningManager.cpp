#include "MAVLinkSigningManager.h"

#include "MAVLinkSigningClock.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <utility>

namespace {

constexpr quint32 RegistryVersion = 1;
constexpr int MaximumProfiles = 256;
constexpr int MaximumContexts = 256;
constexpr int MaximumProfileBytes = 512;
constexpr int MaximumKeyNameBytes = 256;
constexpr int RegistryPrefixBytes = 8 + 4 + 2;
constexpr int RegistryDigestBytes = 32;
constexpr int MaximumRegistryBytes = RegistryPrefixBytes
    + MaximumProfiles * (1 + 2 + MaximumProfileBytes)
    + RegistryDigestBytes;
const QByteArray RegistryMagic("APMSID01", 8);
const QString ClockFileName = QStringLiteral("signing-clock.state");
const QString RegistryFileName = QStringLiteral("signing-link-ids.state");

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

void appendU16(QByteArray *bytes, quint16 value)
{
    bytes->append(static_cast<char>((value >> 8) & 0xffU));
    bytes->append(static_cast<char>(value & 0xffU));
}

void appendU32(QByteArray *bytes, quint32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes->append(static_cast<char>((value >> shift) & 0xffU));
    }
}

quint16 readU16(const QByteArray &bytes, int offset)
{
    return (quint16(static_cast<quint8>(bytes.at(offset))) << 8)
        | quint16(static_cast<quint8>(bytes.at(offset + 1)));
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

bool readBoundedFile(const QString &path, bool *exists, QByteArray *bytes,
                     QString *error)
{
    if (exists) *exists = false;
    if (bytes) bytes->clear();
    const QFileInfo info(path);
    if (info.isSymLink()) {
        setError(error, QStringLiteral(
            "Signing link-ID registry must not be a symbolic link."));
        return false;
    }
    if (!info.exists()) return true;
    if (!info.isFile() || info.size() > MaximumRegistryBytes
        || info.size() < RegistryPrefixBytes + RegistryDigestBytes) {
        setError(error, QStringLiteral(
            "Signing link-ID registry is not a valid bounded file."));
        return false;
    }
#ifndef Q_OS_WIN
    const QFileDevice::Permissions permissions = info.permissions();
    if (!permissions.testFlag(QFileDevice::ReadOwner)
        || !permissions.testFlag(QFileDevice::WriteOwner)
        || (permissions & (QFileDevice::ExeOwner | QFileDevice::ReadGroup
                           | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                           | QFileDevice::ReadOther | QFileDevice::WriteOther
                           | QFileDevice::ExeOther))) {
        setError(error, QStringLiteral(
            "Signing link-ID registry permissions are not owner-only."));
        return false;
    }
#endif
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read signing link-ID registry: %1")
                            .arg(file.errorString()));
        return false;
    }
    const QByteArray content = file.read(MaximumRegistryBytes + 1);
    if (content.size() != info.size()) {
        setError(error, QStringLiteral(
            "Signing link-ID registry changed while it was being read."));
        return false;
    }
    if (exists) *exists = true;
    if (bytes) *bytes = content;
    return true;
}

QByteArray encodeRegistry(const QMap<QString, quint8> &registry)
{
    QVector<QByteArray> profiles(registry.size());
    for (auto entry = registry.constBegin(); entry != registry.constEnd(); ++entry) {
        const int id = entry.value();
        if (id < 0 || id >= profiles.size() || !profiles.at(id).isEmpty()) {
            return {};
        }
        profiles[id] = entry.key().toUtf8();
    }

    QByteArray body;
    body.reserve(RegistryPrefixBytes
                 + profiles.size() * (1 + 2 + MaximumProfileBytes));
    body.append(RegistryMagic);
    appendU32(&body, RegistryVersion);
    appendU16(&body, static_cast<quint16>(profiles.size()));
    for (int id = 0; id < profiles.size(); ++id) {
        if (profiles.at(id).isEmpty()) return {};
        body.append(static_cast<char>(id));
        appendU16(&body, static_cast<quint16>(profiles.at(id).size()));
        body.append(profiles.at(id));
    }
    return body + QCryptographicHash::hash(body, QCryptographicHash::Sha256);
}

bool decodeRegistry(const QByteArray &bytes, QMap<QString, quint8> *registry,
                    QString *error)
{
    if (!registry || bytes.size() < RegistryPrefixBytes + RegistryDigestBytes
        || bytes.size() > MaximumRegistryBytes) {
        setError(error, QStringLiteral("Signing link-ID registry is invalid."));
        return false;
    }
    const QByteArray body = bytes.left(bytes.size() - RegistryDigestBytes);
    if (bytes.right(RegistryDigestBytes)
        != QCryptographicHash::hash(body, QCryptographicHash::Sha256)) {
        setError(error, QStringLiteral(
            "Signing link-ID registry failed its integrity check."));
        return false;
    }
    if (body.left(RegistryMagic.size()) != RegistryMagic
        || readU32(body, RegistryMagic.size()) != RegistryVersion) {
        setError(error, QStringLiteral(
            "Signing link-ID registry has an unsupported format."));
        return false;
    }
    const int count = readU16(body, RegistryMagic.size() + 4);
    if (count > MaximumProfiles) {
        setError(error, QStringLiteral(
            "Signing link-ID registry exceeds its profile limit."));
        return false;
    }

    QMap<QString, quint8> decoded;
    int offset = RegistryPrefixBytes;
    for (int expectedId = 0; expectedId < count; ++expectedId) {
        if (body.size() - offset < 3) {
            setError(error, QStringLiteral(
                "Signing link-ID registry contains a truncated entry."));
            return false;
        }
        const int id = static_cast<quint8>(body.at(offset++));
        const int length = readU16(body, offset);
        offset += 2;
        if (id != expectedId || length < 1 || length > MaximumProfileBytes
            || body.size() - offset < length) {
            setError(error, QStringLiteral(
                "Signing link-ID registry contains an invalid entry."));
            return false;
        }
        const QByteArray encoded = body.mid(offset, length);
        offset += length;
        const QString profile = QString::fromUtf8(encoded);
        const bool hasControl = std::any_of(
            profile.cbegin(), profile.cend(), [](QChar character) {
                return character.unicode() < 0x20
                    || character.unicode() == 0x7f;
            });
        if (profile.toUtf8() != encoded || profile.isEmpty()
            || profile != profile.normalized(QString::NormalizationForm_C)
            || profile.trimmed() != profile || hasControl
            || decoded.contains(profile)) {
            setError(error, QStringLiteral(
                "Signing link-ID registry contains an invalid profile identity."));
            return false;
        }
        decoded.insert(profile, static_cast<quint8>(id));
    }
    if (offset != body.size()) {
        setError(error, QStringLiteral(
            "Signing link-ID registry contains trailing data."));
        return false;
    }
    *registry = decoded;
    return true;
}

} // namespace

bool MAVLinkSigningManager::Verification::accepted() const
{
    return verdict == VerifyVerdict::Signed
        || verdict == VerifyVerdict::UnsignedRadio
        || verdict == VerifyVerdict::Unprotected;
}

bool MAVLinkSigningManager::Verification::authenticated() const
{
    return verdict == VerifyVerdict::Signed;
}

MAVLinkSigningManager::MAVLinkSigningManager(
    QString canonicalStateDirectory)
    : m_stateDirectory(std::move(canonicalStateDirectory))
    , m_ownerThread(QThread::currentThreadId())
{
}

MAVLinkSigningManager::~MAVLinkSigningManager() = default;

bool MAVLinkSigningManager::protectLink(
    int linkId, QString connectionProfileId, QString keyName,
    const QByteArray &key, qint64 unixMs, QString *error)
{
    if (error) error->clear();
    if (!onOwnerThread()) {
        setError(error, QStringLiteral(
            "Signing policy may only be changed on its owning thread."));
        return false;
    }
    if (linkId < 0 || m_epochs.value(linkId, 0) != 0) {
        setError(error, QStringLiteral(
            "A signing policy can only be selected for an offline link."));
        return false;
    }
    if (!validIdentity(connectionProfileId, MaximumProfileBytes,
                       QStringLiteral("Connection profile"), error)
        || !validIdentity(keyName, MaximumKeyNameBytes,
                          QStringLiteral("Signing key name"), error)
        || key.size() != 32
        || std::all_of(key.cbegin(), key.cend(), [](char value) {
               return value == 0;
           })) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "Signing key must contain 32 bytes and must not be all zero.");
        }
        return false;
    }

    const QByteArray fingerprint = QCryptographicHash::hash(
        key, QCryptographicHash::Sha256);
    const auto existing = m_bindings.constFind(linkId);
    if (existing != m_bindings.constEnd()) {
        // Renaming even a friendly alias is an explicit future offline policy
        // transition. Idempotence cannot silently mutate visible provenance.
        if (existing->connectionProfileId == connectionProfileId
            && existing->keyName == keyName && existing->context
            && existing->context->fingerprint == fingerprint) {
            return true;
        }
        setError(error, QStringLiteral(
            "The link already has a different protected signing policy."));
        return false;
    }
    for (auto binding = m_bindings.constBegin();
         binding != m_bindings.constEnd(); ++binding) {
        if (binding.key() != linkId
            && binding->connectionProfileId == connectionProfileId) {
            setError(error, QStringLiteral(
                "The connection profile is already bound to another link."));
            return false;
        }
    }

    // Key contexts retain replay state across disconnects and aliases, so
    // evicting one would make a previously rejected frame admissible again.
    // Bound that deliberately persistent state and fail closed before opening
    // or mutating any signing state files. Known fingerprints remain reusable
    // at the limit.
    if (!m_contexts.contains(fingerprint)
        && m_contexts.size() >= MaximumContexts) {
        setError(error, QStringLiteral(
            "All 256 retained MAVLink signing key contexts are allocated."));
        return false;
    }

    if (!stateDirectoryIsUsable(error) || !ensureClock(unixMs, error)
        || !ensureRegistry(error) || !registryRevisionIsCurrent(error)) {
        return false;
    }

    std::shared_ptr<Context> context = m_contexts.value(fingerprint);
    const bool newContext = !context;
    if (newContext) {
        context = std::make_shared<Context>();
        context->fingerprint = fingerprint;
        context->session = std::make_shared<MAVLinkSigningSession>(key, m_clock);
        if (!context->session->isReady()) {
            setError(error, QStringLiteral(
                "Signing key context could not be initialized."));
            return false;
        }
    }

    QMap<QString, quint8> prospectiveRegistry = m_registry;
    quint8 signingLinkId = 0;
    const auto registered = prospectiveRegistry.constFind(connectionProfileId);
    if (registered != prospectiveRegistry.constEnd()) {
        signingLinkId = registered.value();
    } else {
        if (prospectiveRegistry.size() >= MaximumProfiles) {
            setError(error, QStringLiteral(
                "All 256 stable MAVLink signing link IDs are allocated."));
            return false;
        }
        signingLinkId = static_cast<quint8>(prospectiveRegistry.size());
        prospectiveRegistry.insert(connectionProfileId, signingLinkId);
        if (!publishRegistry(prospectiveRegistry, error)) {
            return false;
        }
    }

    if (newContext) {
        m_contexts.insert(fingerprint, context);
    }

    Binding binding;
    binding.connectionProfileId = std::move(connectionProfileId);
    binding.keyName = std::move(keyName);
    binding.signingLinkId = signingLinkId;
    binding.context = std::move(context);
    m_bindings.insert(linkId, std::move(binding));
    return true;
}

bool MAVLinkSigningManager::beginEpoch(int linkId, quint64 epoch,
                                       QString *error)
{
    if (error) error->clear();
    if (!onOwnerThread() || linkId < 0 || epoch == 0) {
        setError(error, QStringLiteral("Invalid signing link epoch."));
        return false;
    }
    if (m_epochs.value(linkId, 0) != 0) {
        setError(error, QStringLiteral("A signing link epoch is already active."));
        return false;
    }
    m_epochs.insert(linkId, epoch);
    return true;
}

bool MAVLinkSigningManager::endEpoch(int linkId, quint64 epoch,
                                     QString *error)
{
    if (error) error->clear();
    if (!onOwnerThread() || linkId < 0 || epoch == 0
        || m_epochs.value(linkId, 0) != epoch) {
        setError(error, QStringLiteral("Signing link epoch is stale or absent."));
        return false;
    }
    m_epochs.remove(linkId);
    return true;
}

void MAVLinkSigningManager::removeLink(int linkId)
{
    if (!onOwnerThread() || linkId < 0
        || m_epochs.value(linkId, 0) != 0) {
        return;
    }
    m_epochs.remove(linkId);
    m_bindings.remove(linkId);
}

bool MAVLinkSigningManager::signFrame(
    int linkId, quint64 epoch, const QByteArray &frame, qint64 unixMs,
    QByteArray *signedFrame, QString *error)
{
    if (error) error->clear();
    // Preserve the input if a caller uses one QByteArray for both arguments.
    const QByteArray input = frame;
    if (signedFrame) signedFrame->clear();
    if (!onOwnerThread() || !signedFrame || linkId < 0 || epoch == 0
        || m_epochs.value(linkId, 0) != epoch) {
        setError(error, QStringLiteral("Signing link epoch is stale or absent."));
        return false;
    }
    const auto binding = m_bindings.constFind(linkId);
    if (binding == m_bindings.constEnd()) {
        if (input.isEmpty()) {
            setError(error, QStringLiteral("MAVLink frame is empty."));
            return false;
        }
        *signedFrame = input;
        return true;
    }
    if (!binding->context || !binding->context->session) {
        setError(error, QStringLiteral("Protected signing context is unavailable."));
        return false;
    }
    return binding->context->session->signFrame(
        input, binding->signingLinkId, unixMs, signedFrame, error);
}

MAVLinkSigningManager::Verification MAVLinkSigningManager::verifyFrame(
    int linkId, quint64 epoch, const QByteArray &frame, qint64 unixMs)
{
    if (!onOwnerThread() || linkId < 0 || epoch == 0
        || m_epochs.value(linkId, 0) != epoch) {
        return {VerifyVerdict::EpochMismatch,
                QStringLiteral("Signing link epoch is stale or absent.")};
    }
    const auto binding = m_bindings.constFind(linkId);
    if (binding == m_bindings.constEnd()) {
        return {VerifyVerdict::Unprotected, {}};
    }
    if (!binding->context || !binding->context->session) {
        return {VerifyVerdict::NotReady,
                QStringLiteral("Protected signing context is unavailable.")};
    }
    const MAVLinkSigningSession::Verification verification =
        binding->context->session->verifyFrame(frame, unixMs);
    return {mapVerdict(verification.verdict), verification.error};
}

bool MAVLinkSigningManager::protectedLink(int linkId) const
{
    return onOwnerThread() && linkId >= 0 && m_bindings.contains(linkId);
}

MAVLinkSigningManager::LinkStatus MAVLinkSigningManager::status(int linkId) const
{
    LinkStatus result;
    if (!onOwnerThread() || linkId < 0) return result;
    result.activeEpoch = m_epochs.value(linkId, 0);
    const auto binding = m_bindings.constFind(linkId);
    if (binding == m_bindings.constEnd()) return result;
    result.protectedLink = true;
    result.connectionProfileId = binding->connectionProfileId;
    result.keyName = binding->keyName;
    result.signingLinkId = binding->signingLinkId;
    if (binding->context) {
        result.keyFingerprint = QString::fromLatin1(
            binding->context->fingerprint.toHex());
        if (binding->context->session) {
            result.counters = binding->context->session->counters();
        }
    }
    return result;
}

bool MAVLinkSigningManager::rawWritesAllowed(int linkId) const
{
    return onOwnerThread() && linkId >= 0
        && m_epochs.value(linkId, 0) != 0 && !m_bindings.contains(linkId);
}

bool MAVLinkSigningManager::onOwnerThread() const
{
    return QThread::currentThreadId() == m_ownerThread;
}

bool MAVLinkSigningManager::stateDirectoryIsUsable(QString *error) const
{
    const QFileInfo info(m_stateDirectory);
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QString canonical = info.canonicalFilePath();
    if (!info.isAbsolute() || info.isSymLink() || !info.exists() || !info.isDir()
        || canonical.isEmpty() || absolute != canonical) {
        setError(error, QStringLiteral(
            "MAVLink signing state directory must exist as a canonical absolute path."));
        return false;
    }
    return true;
}

bool MAVLinkSigningManager::ensureClock(qint64 unixMs, QString *error)
{
    if (!m_clock) {
        m_clock = std::make_shared<MAVLinkSigningClock>(
            QDir(m_stateDirectory).filePath(ClockFileName));
    }
    return m_clock->isOpen() || m_clock->open(unixMs, error);
}

bool MAVLinkSigningManager::ensureRegistry(QString *error)
{
    if (m_registryLoaded) return true;
    bool exists = false;
    QByteArray snapshot;
    const QString path = QDir(m_stateDirectory).filePath(RegistryFileName);
    if (!readBoundedFile(path, &exists, &snapshot, error)) return false;
    QMap<QString, quint8> registry;
    if (exists && !decodeRegistry(snapshot, &registry, error)) return false;
    m_registry = std::move(registry);
    m_registrySnapshot = std::move(snapshot);
    m_registryExists = exists;
    m_registryLoaded = true;
    return true;
}

bool MAVLinkSigningManager::registryRevisionIsCurrent(QString *error) const
{
    if (!m_registryLoaded) {
        setError(error, QStringLiteral("Signing link-ID registry is not loaded."));
        return false;
    }
    bool exists = false;
    QByteArray snapshot;
    if (!readBoundedFile(QDir(m_stateDirectory).filePath(RegistryFileName),
                         &exists, &snapshot, error)) {
        return false;
    }
    if (exists != m_registryExists
        || (exists && snapshot != m_registrySnapshot)) {
        setError(error, QStringLiteral(
            "Signing link-ID registry changed outside this process."));
        return false;
    }
    return true;
}

bool MAVLinkSigningManager::publishRegistry(
    const QMap<QString, quint8> &registry, QString *error)
{
    if (!registryRevisionIsCurrent(error)) return false;
    const QByteArray encoded = encodeRegistry(registry);
    if (encoded.isEmpty() || encoded.size() > MaximumRegistryBytes) {
        setError(error, QStringLiteral(
            "Signing link-ID registry could not be encoded safely."));
        return false;
    }
    const QString path = QDir(m_stateDirectory).filePath(RegistryFileName);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot stage signing link-ID registry: %1")
                            .arg(file.errorString()));
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(encoded) != encoded.size()) {
        setError(error, QStringLiteral(
            "Cannot write private signing link-ID registry."));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Cannot publish signing link-ID registry: %1")
                            .arg(file.errorString()));
        return false;
    }
    bool publishedExists = false;
    QByteArray published;
    if (!readBoundedFile(path, &publishedExists, &published, error)
        || !publishedExists || published != encoded) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "Published signing link-ID registry could not be verified.");
        }
        return false;
    }
    m_registry = registry;
    m_registrySnapshot = encoded;
    m_registryExists = true;
    return true;
}

bool MAVLinkSigningManager::validIdentity(
    const QString &value, int maximumBytes, const QString &label,
    QString *error)
{
    const QByteArray encoded = value.toUtf8();
    if (value.isEmpty() || value.trimmed() != value
        || value != value.normalized(QString::NormalizationForm_C)
        || encoded.isEmpty() || encoded.size() > maximumBytes
        || QString::fromUtf8(encoded) != value) {
        setError(error, QStringLiteral("%1 is invalid or too long.").arg(label));
        return false;
    }
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            setError(error, QStringLiteral("%1 contains control characters.")
                                .arg(label));
            return false;
        }
    }
    return true;
}

MAVLinkSigningManager::VerifyVerdict MAVLinkSigningManager::mapVerdict(
    MAVLinkSigningSession::Verdict verdict)
{
    using SessionVerdict = MAVLinkSigningSession::Verdict;
    switch (verdict) {
    case SessionVerdict::Signed: return VerifyVerdict::Signed;
    case SessionVerdict::UnsignedRadio: return VerifyVerdict::UnsignedRadio;
    case SessionVerdict::UnsignedRejected: return VerifyVerdict::UnsignedRejected;
    case SessionVerdict::InvalidFrame: return VerifyVerdict::InvalidFrame;
    case SessionVerdict::BadSignature: return VerifyVerdict::BadSignature;
    case SessionVerdict::Replay: return VerifyVerdict::Replay;
    case SessionVerdict::TooOld: return VerifyVerdict::TooOld;
    case SessionVerdict::StreamLimit: return VerifyVerdict::StreamLimit;
    case SessionVerdict::ClockUnavailable: return VerifyVerdict::ClockUnavailable;
    case SessionVerdict::NotReady: return VerifyVerdict::NotReady;
    }
    return VerifyVerdict::NotReady;
}
