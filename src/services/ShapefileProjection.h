#ifndef SHAPEFILEPROJECTION_H
#define SHAPEFILEPROJECTION_H
#include "ShapefileTypes.h"

class ShapefileProjection final
{
public:
    // Empty PRJ means raw WGS84 x/y, like MP10. Any nonempty PRJ requires the
    // native GDAL/PROJ transformation; no approximate datum-identity shortcut.
    static Shapefile::ProjectionResult transform(const QString &esriWkt,
        const QVector<Shapefile::Feature> &features,
        const Shapefile::Cancel &cancel = {}, const Shapefile::Progress &progress = {});
};
#endif
