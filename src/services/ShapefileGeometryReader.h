#ifndef SHAPEFILEGEOMETRYREADER_H
#define SHAPEFILEGEOMETRYREADER_H
#include "ShapefileTypes.h"

class ShapefileGeometryReader final
{
public:
    static Shapefile::GeometryResult read(const QByteArray &shp,
        bool hasDbf, const QByteArray &dbf = {}, const QByteArray &cpg = {},
        const Shapefile::Cancel &cancel = {}, const Shapefile::Progress &progress = {});
};
#endif
