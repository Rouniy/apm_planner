#include "MavAuthKeyStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <map>
#include <utility>

namespace {
constexpr int SaltBytes = 16;
constexpr int NonceBytes = 12;
constexpr int TagBytes = 16;
constexpr int HeaderBytes = 8 + 4 + SaltBytes + NonceBytes + 4;
constexpr char Magic[] = "APMMAVK1";
constexpr auto OwnerPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

void clearError(QString *error) { if (error) error->clear(); }

void cleanse(QByteArray &bytes)
{
    if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}

struct SecretBytes
{
    QByteArray bytes;
    ~SecretBytes() { cleanse(bytes); }
    SecretBytes() = default;
    SecretBytes(const SecretBytes &) = delete;
    SecretBytes &operator=(const SecretBytes &) = delete;
};

struct KeyMaterial
{
    std::array<unsigned char, MavAuthKeyStore::KeyBytes> bytes{};
    ~KeyMaterial() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    bool isZero() const
    {
        unsigned char combined = 0;
        for (auto byte : bytes) combined |= byte;
        return combined == 0;
    }
};
using Entries = std::map<QString, KeyMaterial>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

bool validUnicode(const QString &text)
{
    for (int i = 0; i < text.size(); ++i) {
        if (text[i].isNull()) return false;
        if (text[i].isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
    }
    return true;
}

bool containsNonSpace(const QString &text)
{
    for (const auto ch : text) if (!ch.isSpace()) return true;
    return false;
}

bool validateName(const QString &name, QString *error)
{
    // The cheap UTF-16 bound runs before creating any potentially large UTF-8.
    if (name.isEmpty() || name.size() > MavAuthKeyStore::MaximumNameBytes
        || !validUnicode(name) || !containsNonSpace(name)
        || name.toUtf8().size() > MavAuthKeyStore::MaximumNameBytes)
        return fail(error, QStringLiteral("A key name must contain 1..128 UTF-8 bytes of valid non-blank text."));
    for (const auto ch : name)
        if (ch.category() == QChar::Other_Control)
            return fail(error, QStringLiteral("A key name cannot contain control characters."));
    return true;
}

bool passphraseBytes(const QString &passphrase, SecretBytes *encoded, QString *error)
{
    if (passphrase.size() > MavAuthKeyStore::MaximumPassphraseBytes
        || !validUnicode(passphrase) || !containsNonSpace(passphrase))
        return fail(error, QStringLiteral("The master passphrase must contain 12..1024 UTF-8 bytes of valid non-blank text."));
    encoded->bytes = passphrase.toUtf8();
    if (encoded->bytes.size() < MavAuthKeyStore::MinimumPassphraseBytes
        || encoded->bytes.size() > MavAuthKeyStore::MaximumPassphraseBytes)
        return fail(error, QStringLiteral("The master passphrase must contain 12..1024 UTF-8 bytes."));
    return true;
}

bool deriveWrappingKey(const QByteArray &passphrase, const QByteArray &salt,
                       KeyMaterial *key, QString *error)
{
    if (salt.size() != SaltBytes
        || PKCS5_PBKDF2_HMAC(passphrase.constData(), passphrase.size(),
                            reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
                            MavAuthKeyStore::Pbkdf2Iterations, EVP_sha256(),
                            MavAuthKeyStore::KeyBytes, key->bytes.data()) != 1
        || key->isZero())
        return fail(error, QStringLiteral("The cryptographic provider could not derive the vault key."));
    return true;
}

void appendU16(QByteArray &bytes, quint16 value)
{
    unsigned char raw[2]; qToBigEndian(value, raw);
    bytes.append(reinterpret_cast<const char *>(raw), 2);
}

void appendU32(QByteArray &bytes, quint32 value)
{
    unsigned char raw[4]; qToBigEndian(value, raw);
    bytes.append(reinterpret_cast<const char *>(raw), 4);
}

bool serialize(const Entries &entries, SecretBytes *plain, QString *error)
{
    if (entries.size() > size_t(MavAuthKeyStore::MaximumKeys))
        return fail(error, QStringLiteral("The vault has reached its 128-key limit."));
    // Reserve once: no retired reallocations containing copies of secret keys.
    plain->bytes.reserve(2 + int(entries.size()) * (2 + MavAuthKeyStore::MaximumNameBytes + MavAuthKeyStore::KeyBytes));
    appendU16(plain->bytes, quint16(entries.size()));
    for (const auto &entry : entries) {
        if (!validateName(entry.first, error) || entry.second.isZero())
            return fail(error, QStringLiteral("The key collection contains an invalid name or zero signing key."));
        const auto name = entry.first.toUtf8();
        appendU16(plain->bytes, quint16(name.size()));
        plain->bytes.append(name);
        plain->bytes.append(reinterpret_cast<const char *>(entry.second.bytes.data()), MavAuthKeyStore::KeyBytes);
    }
    return true;
}

bool deserialize(const QByteArray &plain, Entries *entries, QString *error)
{
    auto invalid = [&] { return fail(error, QStringLiteral("The authenticated vault contains an invalid key collection.")); };
    if (plain.size() < 2) return invalid();
    const auto *data = reinterpret_cast<const unsigned char *>(plain.constData());
    const int count = qFromBigEndian<quint16>(data);
    if (count > MavAuthKeyStore::MaximumKeys) return invalid();
    int offset = 2;
    for (int i = 0; i < count; ++i) {
        if (plain.size() - offset < 2) return invalid();
        const int length = qFromBigEndian<quint16>(data + offset); offset += 2;
        if (length < 1 || length > MavAuthKeyStore::MaximumNameBytes
            || plain.size() - offset < length + MavAuthKeyStore::KeyBytes) return invalid();
        const auto name = QString::fromUtf8(plain.constData() + offset, length);
        // Reject invalid UTF-8 instead of changing authenticated name identity
        // through Unicode replacement characters or normalization.
        if (name.toUtf8() != QByteArray::fromRawData(plain.constData() + offset, length)
            || !validateName(name, nullptr) || entries->count(name)) return invalid();
        offset += length;
        KeyMaterial key;
        std::memcpy(key.bytes.data(), data + offset, key.bytes.size()); offset += MavAuthKeyStore::KeyBytes;
        if (key.isZero()) return invalid();
        entries->emplace(name, key);
    }
    return offset == plain.size() || invalid();
}

bool readEnvelope(const QString &path, QByteArray *bytes, QString *error)
{
    const QFileInfo before(path);
    if (!before.exists() || !before.isFile() || before.isSymLink())
        return fail(error, QStringLiteral("The vault must be an existing regular file, not a symbolic link."));
    if (before.size() < HeaderBytes + TagBytes + 2 || before.size() > MavAuthKeyStore::MaximumFileBytes)
        return fail(error, QStringLiteral("The vault file has an invalid or excessive size; it was left unchanged."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("The vault file could not be opened for reading."));
    *bytes = file.read(MavAuthKeyStore::MaximumFileBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() != before.size()
        || !file.atEnd() || file.size() != before.size())
        return fail(error, QStringLiteral("The vault changed or could not be read completely."));
    return true;
}

bool validateEnvelope(const QByteArray &bytes, QString *error)
{
    if (bytes.size() < HeaderBytes + TagBytes + 2 || bytes.size() > MavAuthKeyStore::MaximumFileBytes)
        return fail(error, QStringLiteral("The vault file has an invalid size."));
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    if (std::memcmp(bytes.constData(), Magic, 8) != 0
        || qFromBigEndian<quint32>(data + 8) != MavAuthKeyStore::Pbkdf2Iterations
        || qFromBigEndian<quint32>(data + HeaderBytes - 4) != quint32(bytes.size() - HeaderBytes - TagBytes))
        return fail(error, QStringLiteral("The vault has an unsupported or corrupt header; it was left unchanged."));
    return true;
}

bool encrypt(const Entries &entries, const KeyMaterial &key, const QByteArray &salt,
             QByteArray *envelope, QString *error)
{
    SecretBytes plain;
    if (!serialize(entries, &plain, error)) return false;
    QByteArray nonce(NonceBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1)
        return fail(error, QStringLiteral("The cryptographic provider could not generate a vault nonce."));
    QByteArray header(Magic, 8);
    appendU32(header, MavAuthKeyStore::Pbkdf2Iterations);
    header += salt; header += nonce; appendU32(header, quint32(plain.bytes.size()));
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    QByteArray cipher(plain.bytes.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    QByteArray tag(TagBytes, '\0');
    int ignored = 0, used = 0, tail = 0;
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1
        || EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.bytes.data(), reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_EncryptUpdate(context.get(), nullptr, &ignored, reinterpret_cast<const unsigned char *>(header.constData()), header.size()) != 1
        || EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char *>(cipher.data()), &used,
                             reinterpret_cast<const unsigned char *>(plain.bytes.constData()), plain.bytes.size()) != 1
        || EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(cipher.data()) + used, &tail) != 1
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1
        || used + tail != plain.bytes.size())
        return fail(error, QStringLiteral("The cryptographic provider could not encrypt the vault."));
    cipher.resize(used + tail);
    *envelope = header + cipher + tag;
    return envelope->size() <= MavAuthKeyStore::MaximumFileBytes
        || fail(error, QStringLiteral("The encrypted vault exceeds its file-size limit."));
}

bool decrypt(const QByteArray &envelope, const KeyMaterial &key, Entries *entries, QString *error)
{
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    SecretBytes plain;
    const int length = envelope.size() - HeaderBytes - TagBytes;
    plain.bytes.resize(length + EVP_MAX_BLOCK_LENGTH);
    const auto *bytes = reinterpret_cast<const unsigned char *>(envelope.constData());
    int ignored = 0, used = 0, tail = 0;
    // No decrypted bytes are parsed or exposed until the GCM tag authenticates.
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, NonceBytes, nullptr) != 1
        || EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.bytes.data(), bytes + 12 + SaltBytes) != 1
        || EVP_DecryptUpdate(context.get(), nullptr, &ignored, bytes, HeaderBytes) != 1
        || EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char *>(plain.bytes.data()), &used, bytes + HeaderBytes, length) != 1
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, TagBytes,
                               const_cast<unsigned char *>(bytes + HeaderBytes + length)) != 1
        || EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(plain.bytes.data()) + used, &tail) != 1
        || used + tail != length)
        return fail(error, QStringLiteral("The master passphrase is incorrect or the vault is corrupt; it was left unchanged."));
    plain.bytes.resize(length);
    return deserialize(plain.bytes, entries, error);
}
}

struct MavAuthKeyStore::State
{
    QString path;
    QString canonicalParent;
    std::unique_ptr<QLockFile> writerLock;
    QByteArray salt;
    QByteArray revision;
    KeyMaterial wrappingKey;
    Entries entries;

    bool acquire(const QString &requested, QString *error)
    {
        if (requested.isEmpty() || requested.size() > 4096 || !validUnicode(requested)
            || !QDir::isAbsolutePath(requested))
            return fail(error, QStringLiteral("The vault requires an absolute file path."));
        const QFileInfo requestedInfo(QDir::cleanPath(requested));
        if (requestedInfo.exists() && !requestedInfo.isFile())
            return fail(error, QStringLiteral("The vault destination is not a regular file."));
        const QFileInfo parent(requestedInfo.absolutePath());
        canonicalParent = parent.canonicalFilePath();
        if (!parent.exists() || !parent.isDir() || canonicalParent.isEmpty())
            return fail(error, QStringLiteral("The vault directory must already exist."));
        path = QDir(canonicalParent).filePath(requestedInfo.fileName());
        if (requestedInfo.fileName().isEmpty() || QFileInfo(path).isSymLink())
            return fail(error, QStringLiteral("The vault cannot use a directory or symbolic-link destination."));
        const QString lockPath = path + QStringLiteral(".lock");
        if (QFileInfo(lockPath).isSymLink())
            return fail(error, QStringLiteral("The vault lock cannot be a symbolic link."));
        writerLock.reset(new QLockFile(lockPath));
        writerLock->setStaleLockTime(0);
        if (!writerLock->tryLock(0))
            return fail(error, QStringLiteral("The vault is locked by another writer or its lock cannot be created."));
        if (!QFile::setPermissions(lockPath, OwnerPermissions))
            return fail(error, QStringLiteral("The vault lock permissions could not be restricted."));
        return true;
    }

    bool unchanged(bool creating, QString *error) const
    {
        if (!writerLock || !writerLock->isLocked()
            || QFileInfo(canonicalParent).canonicalFilePath() != canonicalParent)
            return fail(error, QStringLiteral("The vault directory or writer lease changed; no file was replaced."));
        const QFileInfo info(path);
        if (info.isSymLink()) return fail(error, QStringLiteral("The vault destination became a symbolic link."));
        if (creating)
            return !info.exists() || fail(error, QStringLiteral("A vault already exists at this path; it was left unchanged."));
        QByteArray current;
        if (!readEnvelope(path, &current, error)) return false;
        return QCryptographicHash::hash(current, QCryptographicHash::Sha256) == revision
            || fail(error, QStringLiteral("The vault was modified outside this session; lock and unlock it again before editing."));
    }

    bool save(const Entries &candidate, bool creating, QString *error)
    {
        if (!unchanged(creating, error)) return false;
        QByteArray encoded;
        if (!encrypt(candidate, wrappingKey, salt, &encoded, error)) return false;
        const QByteArray nextRevision = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return fail(error, QStringLiteral("The encrypted vault could not be staged for atomic replacement."));
        if (!file.setPermissions(OwnerPermissions) || file.write(encoded) != encoded.size()
            || !file.flush() || file.error() != QFileDevice::NoError) {
            file.cancelWriting();
            return fail(error, QStringLiteral("The encrypted vault could not be written privately and completely."));
        }
        if (!unchanged(creating, error)) { file.cancelWriting(); return false; }
        if (!file.commit()) return fail(error, QStringLiteral("The encrypted vault could not be committed; active keys are unchanged."));
        revision = nextRevision;
        return true;
    }
};

MavAuthKeyStore::MavAuthKeyStore(QString absoluteFilePath)
    : m_requestedPath(std::move(absoluteFilePath)) {}
MavAuthKeyStore::~MavAuthKeyStore() { lock(); }
bool MavAuthKeyStore::isUnlocked() const { return bool(m_state); }
void MavAuthKeyStore::lock() { m_state.reset(); }

bool MavAuthKeyStore::create(const QString &masterPassphrase, QString *error)
{
    clearError(error);
    if (m_state) return fail(error, QStringLiteral("Lock the current vault before creating or unlocking another session."));
    SecretBytes master;
    if (!passphraseBytes(masterPassphrase, &master, error)) return false;
    auto next = std::unique_ptr<State>(new State);
    if (!next->acquire(m_requestedPath, error) || !next->unchanged(true, error)) return false;
    next->salt.resize(SaltBytes);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(next->salt.data()), next->salt.size()) != 1)
        return fail(error, QStringLiteral("The cryptographic provider could not generate a vault salt."));
    if (!deriveWrappingKey(master.bytes, next->salt, &next->wrappingKey, error)) return false;
    if (!next->save(next->entries, true, error)) return false;
    m_state.swap(next);
    return true;
}

bool MavAuthKeyStore::unlock(const QString &masterPassphrase, QString *error)
{
    clearError(error);
    if (m_state) return fail(error, QStringLiteral("The vault is already unlocked."));
    SecretBytes master;
    if (!passphraseBytes(masterPassphrase, &master, error)) return false;
    auto next = std::unique_ptr<State>(new State);
    if (!next->acquire(m_requestedPath, error)) return false;
    QByteArray encoded;
    if (!readEnvelope(next->path, &encoded, error) || !validateEnvelope(encoded, error)) return false;
    next->salt = encoded.mid(12, SaltBytes);
    if (!deriveWrappingKey(master.bytes, next->salt, &next->wrappingKey, error)
        || !decrypt(encoded, next->wrappingKey, &next->entries, error)) return false;
    next->revision = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    if (!next->unchanged(false, error)) return false;
    m_state.swap(next);
    return true;
}

QStringList MavAuthKeyStore::keyNames() const
{
    QStringList result;
    if (m_state) for (const auto &entry : m_state->entries) result.append(entry.first);
    return result;
}

bool MavAuthKeyStore::deriveSigningKey(const QString &seed, QByteArray *output, QString *error)
{
    clearError(error);
    if (!output) return fail(error, QStringLiteral("A signing-key output buffer is required."));
    cleanse(*output);
    if (seed.isEmpty() || seed.size() > MaximumSeedBytes || !validUnicode(seed) || !containsNonSpace(seed))
        return fail(error, QStringLiteral("A signing seed must contain 1..4096 UTF-8 bytes of valid non-blank text."));
    SecretBytes encoded; encoded.bytes = seed.toUtf8();
    if (encoded.bytes.size() > MaximumSeedBytes)
        return fail(error, QStringLiteral("The signing seed exceeds 4096 UTF-8 bytes."));
    KeyMaterial derived;
    unsigned int length = 0;
    if (EVP_Digest(encoded.bytes.constData(), size_t(encoded.bytes.size()), derived.bytes.data(), &length, EVP_sha256(), nullptr) != 1
        || length != KeyBytes || derived.isZero())
        return fail(error, QStringLiteral("The cryptographic provider could not derive a non-zero signing key."));
    *output = QByteArray(reinterpret_cast<const char *>(derived.bytes.data()), KeyBytes);
    return true;
}

bool MavAuthKeyStore::key(const QString &name, QByteArray *output, QString *error) const
{
    clearError(error);
    if (!output) return fail(error, QStringLiteral("A signing-key output buffer is required."));
    cleanse(*output);
    if (!m_state) return fail(error, QStringLiteral("The signing-key vault is locked."));
    const auto found = m_state->entries.find(name);
    if (found == m_state->entries.end()) return fail(error, QStringLiteral("No signing key has this exact name."));
    *output = QByteArray(reinterpret_cast<const char *>(found->second.bytes.data()), KeyBytes);
    return true;
}

bool MavAuthKeyStore::addSeed(const QString &name, const QString &seed, QString *error)
{
    clearError(error);
    if (!m_state) return fail(error, QStringLiteral("The signing-key vault is locked."));
    if (!validateName(name, error)) return false;
    if (m_state->entries.count(name)) return fail(error, QStringLiteral("A signing key already has this exact name; it was not overwritten."));
    if (m_state->entries.size() >= size_t(MaximumKeys)) return fail(error, QStringLiteral("The vault has reached its 128-key limit."));
    SecretBytes exported;
    if (!deriveSigningKey(seed, &exported.bytes, error)) return false;
    KeyMaterial material;
    std::memcpy(material.bytes.data(), exported.bytes.constData(), material.bytes.size());
    Entries candidate = m_state->entries;
    candidate.emplace(name, material);
    if (!m_state->save(candidate, false, error)) return false;
    m_state->entries.swap(candidate);
    return true;
}

bool MavAuthKeyStore::removeKey(const QString &name, QString *error)
{
    clearError(error);
    if (!m_state) return fail(error, QStringLiteral("The signing-key vault is locked."));
    if (!m_state->entries.count(name)) return fail(error, QStringLiteral("No signing key has this exact name."));
    Entries candidate = m_state->entries;
    candidate.erase(name);
    if (!m_state->save(candidate, false, error)) return false;
    m_state->entries.swap(candidate);
    return true;
}
