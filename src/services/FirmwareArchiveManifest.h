#ifndef FIRMWAREARCHIVEMANIFEST_H
#define FIRMWAREARCHIVEMANIFEST_H

#include "FirmwareArchiveTypes.h"
#include <QDomDocument>
#include <QHash>

class FirmwareArchiveManifest final
{
public:
    struct Download { QUrl uri; QString relativePath; };
    struct Plan {
        bool success = false;
        QString error;
        QDomDocument document;
        QVector<Download> downloads;
    };
    static bool allowedUrl(const QUrl &url, bool httpsOnly);
    static QString relativePath(const QUrl &url, QString *error = nullptr);
    static Plan parse(const QByteArray &xml);
    // Keys are canonical FullyEncoded URL strings; only successful URLs change.
    // Preserve all unrelated fields/comments and leave failures as network URLs.
    static QByteArray rewrite(const Plan &plan,
        const QHash<QString, QString> &successfulPaths, QString *error = nullptr);
};
#endif
