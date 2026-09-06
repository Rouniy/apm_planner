#ifndef SHAPEFILEIMPORTSERVICE_H
#define SHAPEFILEIMPORTSERVICE_H

#include "ShapefileTypes.h"
#include <QSharedPointer>

class ShapefileImportService final
{
    struct PlanData;
public:
    struct Output {
        QString path;
        qint64 pointCount = 0;
        bool replaces = false;
    };
    class Plan {
    public:
        bool isValid() const;
        QString source() const;
        QString directory() const;
        QString projectionName() const;
        QVector<Output> outputs() const;
        qint64 pointCount() const;
        qint64 discardedPoints() const;
        QStringList warnings() const;
    private:
        QSharedPointer<const PlanData> d;
        friend class ShapefileImportService;
    };
    struct Preparation {
        Plan plan;
        bool cancelled = false;
        QString error;
    };
    struct Result {
        bool success = false, cancelled = false;
        QStringList files; // Exactly the successfully published files, in order.
        qint64 pointCount = 0;
        QString projectionName, error;
        QStringList warnings;
    };
    static Preparation prepare(const QString &input,
        const Shapefile::Cancel &cancel = {}, const Shapefile::Progress &progress = {});
    static Result exportPolyFiles(const Plan &plan,
        const Shapefile::Cancel &cancel = {}, const Shapefile::Progress &progress = {});
};
#endif
