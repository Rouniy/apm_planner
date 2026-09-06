#ifndef FIRMWAREARCHIVESERVICE_H
#define FIRMWAREARCHIVESERVICE_H

#include "FirmwareArchiveTypes.h"

// Worker-only value API, no UI or vehicle access. The caller owns cancellation.
class FirmwareArchiveService final
{
public:
    static QVector<QUrl> officialManifestUris();
    static QString nextDirectory(const QString &parent, const QDateTime &utcNow,
                                 QString *error = nullptr);
    static FirmwareArchive::Result download(
        const QVector<QUrl> &manifests, const QString &destination,
        const FirmwareArchive::Fetch &fetch,
        const FirmwareArchive::Cancel &cancel = {},
        const FirmwareArchive::Progress &progress = {});
};
#endif
