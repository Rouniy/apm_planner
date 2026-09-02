#ifndef FENCERALLYFILECODEC_H
#define FENCERALLYFILECODEC_H

#include "FenceRallyModel.h"

#include <QByteArray>
#include <QString>

namespace MissionPlanner
{

class FenceRallyFileCodec final
{
public:
    struct SaveResult
    {
        bool ok = false;
        QString error;
    };

    struct EncodeResult : SaveResult
    {
        QByteArray data;
    };

    struct FenceLoadResult : SaveResult
    {
        Fence fence;
    };

    struct RallyLoadResult : SaveResult
    {
        RallyPoints rally;
    };

    static FenceLoadResult DecodeLegacyFence(const QByteArray &data);
    static EncodeResult EncodeLegacyFence(const Fence &fence);
    static FenceLoadResult LoadLegacyFence(const QString &path);
    static SaveResult SaveLegacyFence(const QString &path,
                                      const Fence &fence);

    static RallyLoadResult DecodeLegacyRally(const QByteArray &data);
    static EncodeResult EncodeLegacyRally(const RallyPoints &rally);
    static RallyLoadResult LoadLegacyRally(const QString &path);
    static SaveResult SaveLegacyRally(const QString &path,
                                      const RallyPoints &rally);

private:
    FenceRallyFileCodec() = delete;
};

} // namespace MissionPlanner

#endif // FENCERALLYFILECODEC_H
