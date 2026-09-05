#ifndef MAVAUTHKEYSERVICE_H
#define MAVAUTHKEYSERVICE_H

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

/** Application-owned, owner-thread-only asynchronous named signing-key vault.
 * One operation is admitted at a time; token 0 means refusal (see lastError).
 * No implicit creation, directory creation, overwrite, or plaintext persistence.
 * Closing an observer window does not lock or shut down this independent owner.
 */
class MavAuthKeyService final : public QObject
{
    Q_OBJECT
public:
    using KeyCallback = std::function<void(bool, const QByteArray &, const QString &)>;

    explicit MavAuthKeyService(const QString &absoluteVaultPath, QObject *parent = nullptr);
    ~MavAuthKeyService() override;

    quint64 create(const QString &masterPassphrase);
    quint64 unlock(const QString &masterPassphrase);
    quint64 addSeed(const QString &name, const QString &seed);
    quint64 removeKey(const QString &name);
    quint64 lock();

    // Only this explicit callback receives key bytes, on the service's owner
    // thread. The reference expires when the callback returns; any retained copy
    // must be cleansed by the caller. Capture window recipients with QPointer.
    // busy() remains true during the callback. Service destruction cancels
    // delivery; shutdown permits a pending callback only with failure/no key.
    quint64 requestKey(const QString &name, KeyCallback callback);

    bool busy() const;
    bool isUnlocked() const;
    bool isShuttingDown() const;
    QStringList keyNames() const;
    QString lastError() const;

    // Terminal: reject new jobs, hide metadata, drain the one admitted job and
    // destroy the vault in its worker thread. No thread termination. Destruction
    // waits for that bounded operation without pumping owner-thread events.
    void shutdown();

signals:
    void stateChanged();
    void operationFinished(quint64 token, bool success, const QString &error);

private:
    struct State;
    std::unique_ptr<State> m_state;
    quint64 submit(int operation, const QString &name, const QString &secret,
                   KeyCallback callback = {});
    quint64 refuse(const QString &error);
    void complete(quint64 token);
};

#endif
