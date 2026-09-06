#ifndef LOGINDEXFILES_H
#define LOGINDEXFILES_H

#include "LogIndexTypes.h"
#include <memory>

class LogIndexFiles final
{
    struct DeletePlanData;
public:
    class DeletePlan {
    public:
        bool isValid() const;
        QString rootPath() const;
        QVector<LogIndex::Entry> entries() const;
        // Complete exact source + companion paths reviewed by the operator.
        QStringList paths() const;
    private:
        friend class LogIndexFiles;
        std::shared_ptr<const DeletePlanData> d;
    };
    struct DeletePreparation {
        bool success = false, cancelled = false;
        QString error;
        QStringList warnings;
        DeletePlan plan;
    };
    static LogIndex::Discovery discover(const QString &root,
                                       const LogIndex::Cancel &cancel = {});
    static bool unchanged(const LogIndex::Entry &entry, QString *error = nullptr);
    static LogIndex::ThumbnailResult thumbnail(
        const LogIndex::Entry &entry, const QVector<LogIndex::Point> &track,
        const LogIndex::TileReader &tiles = {}, const LogIndex::Cancel &cancel = {});
    static DeletePreparation prepareDelete(
        const QString &root, const QVector<LogIndex::Entry> &entries,
        const LogIndex::Cancel &cancel = {});
    static LogIndex::DeleteResult executeDelete(
        const DeletePlan &plan, const LogIndex::Cancel &cancel = {},
        const LogIndex::Progress &progress = {});
};

#endif
