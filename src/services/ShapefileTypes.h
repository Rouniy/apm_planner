#ifndef SHAPEFILETYPES_H
#define SHAPEFILETYPES_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace Shapefile {
using Cancel = std::function<bool()>;
using Progress = std::function<void(qint64 completed, qint64 total, const QString &phase)>;
struct Coordinate { double x = 0, y = 0, z = 0; };
struct Feature { QVector<Coordinate> points; };
struct GeometryResult {
    bool success = false, cancelled = false;
    QString error;
    QVector<Feature> features;
    QStringList warnings;
};
struct ProjectionResult {
    bool success = false, cancelled = false;
    QString error, projectionName;
    QVector<Feature> features; // x=longitude, y=latitude, z=altitude after conversion
    qint64 discardedPoints = 0;
    QStringList warnings;
};
constexpr qint64 MaximumShpBytes = 256LL * 1024 * 1024;
constexpr qint64 MaximumDbfBytes = 64LL * 1024 * 1024;
constexpr qint64 MaximumPrjBytes = 64 * 1024;
constexpr qint64 MaximumCpgBytes = 4096;
constexpr int MaximumFeatures = 20000;
constexpr int MaximumPoints = 2000000;
}
#endif
