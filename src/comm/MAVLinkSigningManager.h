#ifndef MAVLINKSIGNINGMANAGER_H
#define MAVLINKSIGNINGMANAGER_H

#include "MAVLinkSigningSession.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QtGlobal>

#include <memory>

class MAVLinkSigningClock;

/**
 * Thread-confined owner of transport signing policy and shared key contexts.
 *
 * Secrets remain only inside MAVLinkSigningSession. Friendly key aliases and
 * stable connection-profile signing IDs are metadata; they never select a key
 * implicitly and never follow a transient physical link id.
 */
class MAVLinkSigningManager final
{
public:
    enum class VerifyVerdict {
        Signed,
        UnsignedRadio,
        Unprotected,
        EpochMismatch,
        UnsignedRejected,
        InvalidFrame,
        BadSignature,
        Replay,
        TooOld,
        StreamLimit,
        ClockUnavailable,
        NotReady
    };

    struct Verification {
        VerifyVerdict verdict = VerifyVerdict::NotReady;
        QString error;
        bool accepted() const;
        bool authenticated() const;
    };

    struct LinkStatus {
        bool protectedLink = false;
        bool keyAvailable = false;
        quint64 activeEpoch = 0;
        QString connectionProfileId;
        QString keyName;
        QString keyFingerprint;
        int signingLinkId = -1;
        MAVLinkSigningSession::Counters counters;
    };

    explicit MAVLinkSigningManager(QString canonicalStateDirectory);
    ~MAVLinkSigningManager();
    MAVLinkSigningManager(const MAVLinkSigningManager &) = delete;
    MAVLinkSigningManager &operator=(const MAVLinkSigningManager &) = delete;
    MAVLinkSigningManager(MAVLinkSigningManager &&) = delete;
    MAVLinkSigningManager &operator=(MAVLinkSigningManager &&) = delete;

    bool protectLink(int linkId, QString connectionProfileId, QString keyName,
                     const QByteArray &key, qint64 unixMs,
                     QString *error = nullptr);
    bool requireSigning(int linkId, const QString &connectionProfileId,
                        const QByteArray &expectedFingerprint,
                        QString *error = nullptr);
    bool beginEpoch(int linkId, quint64 epoch, QString *error = nullptr);
    bool endEpoch(int linkId, quint64 epoch, QString *error = nullptr);
    void removeLink(int linkId);

    bool signFrame(int linkId, quint64 epoch, const QByteArray &frame,
                   qint64 unixMs, QByteArray *signedFrame,
                   QString *error = nullptr);
    Verification verifyFrame(int linkId, quint64 epoch,
                             const QByteArray &frame, qint64 unixMs);

    bool protectedLink(int linkId) const;
    LinkStatus status(int linkId) const;
    bool rawWritesAllowed(int linkId) const;

private:
    struct Context {
        QByteArray fingerprint;
        std::shared_ptr<MAVLinkSigningSession> session;
    };
    struct Binding {
        QString connectionProfileId;
        QString keyName;
        QByteArray expectedFingerprint;
        int signingLinkId = -1;
        std::shared_ptr<Context> context;
    };

    bool onOwnerThread() const;
    bool stateDirectoryIsUsable(QString *error) const;
    bool ensureClock(qint64 unixMs, QString *error);
    bool ensureRegistry(QString *error);
    bool registryRevisionIsCurrent(QString *error) const;
    bool publishRegistry(const QMap<QString, quint8> &registry,
                         QString *error);
    static bool validIdentity(const QString &value, int maximumBytes,
                              const QString &label, QString *error);
    static VerifyVerdict mapVerdict(MAVLinkSigningSession::Verdict verdict);

    const QString m_stateDirectory;
    const Qt::HANDLE m_ownerThread;
    std::shared_ptr<MAVLinkSigningClock> m_clock;
    QHash<QByteArray, std::shared_ptr<Context>> m_contexts;
    QHash<int, Binding> m_bindings;
    QHash<int, quint64> m_epochs;
    QMap<QString, quint8> m_registry;
    QByteArray m_registrySnapshot;
    bool m_registryExists = false;
    bool m_registryLoaded = false;
};

#endif // MAVLINKSIGNINGMANAGER_H
