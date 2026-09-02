#ifndef MISSIONPROTOCOLCOORDINATOR_H
#define MISSIONPROTOCOLCOORDINATOR_H

#include <QObject>
#include <QPointer>

class MissionProtocolCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class MissionType : quint8
    {
        Mission = 0,
        Fence = 1,
        Rally = 2,
    };
    Q_ENUM(MissionType)

    struct LeaseToken
    {
        QPointer<QObject> owner;
        MissionType missionType = MissionType::Mission;
        quint64 generation = 0;

        bool isValid() const
        {
            return !owner.isNull() && generation != 0;
        }
    };

    explicit MissionProtocolCoordinator(QObject *parent = nullptr);

    // This coordinator intentionally has no locking. All methods and lease
    // owners must live on the coordinator's thread (the application UI thread
    // in production).
    LeaseToken tryAcquire(QObject *owner, MissionType missionType);
    bool owns(const LeaseToken &token) const;
    bool release(const LeaseToken &token);

    QObject *owner() const;
    MissionType missionType() const;
    quint64 generation() const;

private:
    static bool isSupportedMissionType(MissionType missionType);
    void assertCoordinatorThread() const;
    void clearLease();

    QPointer<QObject> m_owner;
    MissionType m_missionType = MissionType::Mission;
    quint64 m_generation = 0;
    QMetaObject::Connection m_ownerDestroyedConnection;
};

#endif
