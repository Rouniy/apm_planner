#ifndef MAVAUTHKEYSTORE_H
#define MAVAUTHKEYSTORE_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>

/** Standalone encrypted named MAVLink signing-key vault.
 * Thread-confined, synchronous and noncopyable. No transport or UI side effects.
 * Create/unlock are intentionally separate: unreadable/missing vaults are never
 * silently replaced. The master passphrase is not retained after either call.
 */
class MavAuthKeyStore final
{
public:
    static constexpr int KeyBytes = 32;
    static constexpr int MaximumKeys = 128;
    static constexpr int MaximumNameBytes = 128;
    static constexpr int MaximumSeedBytes = 4096;
    static constexpr int MinimumPassphraseBytes = 12;
    static constexpr int MaximumPassphraseBytes = 1024;
    static constexpr int MaximumFileBytes = 65536;
    static constexpr int Pbkdf2Iterations = 600000;

    explicit MavAuthKeyStore(QString absoluteFilePath);
    ~MavAuthKeyStore();
    MavAuthKeyStore(const MavAuthKeyStore &) = delete;
    MavAuthKeyStore &operator=(const MavAuthKeyStore &) = delete;
    MavAuthKeyStore(MavAuthKeyStore &&) = delete;
    MavAuthKeyStore &operator=(MavAuthKeyStore &&) = delete;

    bool create(const QString &masterPassphrase, QString *error = nullptr);
    bool unlock(const QString &masterPassphrase, QString *error = nullptr);
    void lock();
    bool isUnlocked() const;
    QStringList keyNames() const;
    bool addSeed(const QString &name, const QString &seed, QString *error = nullptr);
    bool removeKey(const QString &name, QString *error = nullptr);

    // Explicit secret export. On failure output is cleared. Caller owns and
    // must best-effort cleanse every exported copy after its transport use.
    bool key(const QString &name, QByteArray *output, QString *error = nullptr) const;
    static bool deriveSigningKey(const QString &seed, QByteArray *output,
                                 QString *error = nullptr);

private:
    struct State;
    const QString m_requestedPath;
    std::unique_ptr<State> m_state;
};

#endif
