#ifndef SIGNINGPROVISIONINGTARGET_H
#define SIGNINGPROVISIONINGTARGET_H

#include <QPointer>
#include <QString>

// Secret-free, immutable consent identity. Fresh telemetry and route validity
// are checked again by LinkManager; this value is not itself an authorization.
struct SigningProvisioningTarget
{
    int linkId = -1;
    QString profileId;
    QPointer<QObject> identity;
    quint64 revision = 0;
    quint64 targetGeneration = 0;
    quint64 linkSessionEpoch = 0;
    quint64 instanceEpoch = 0;
    quint8 systemId = 0;
    quint8 componentId = 0;

    bool isValid() const
    {
        return linkId >= 0 && !profileId.isEmpty() && identity && revision
            && targetGeneration && linkSessionEpoch && instanceEpoch
            && systemId && componentId;
    }
};

inline bool operator==(const SigningProvisioningTarget &a,
                       const SigningProvisioningTarget &b)
{
    return a.linkId == b.linkId && a.profileId == b.profileId
        && a.identity == b.identity && a.revision == b.revision
        && a.targetGeneration == b.targetGeneration
        && a.linkSessionEpoch == b.linkSessionEpoch
        && a.instanceEpoch == b.instanceEpoch
        && a.systemId == b.systemId && a.componentId == b.componentId;
}

inline bool operator!=(const SigningProvisioningTarget &a,
                       const SigningProvisioningTarget &b)
{
    return !(a == b);
}

#endif
