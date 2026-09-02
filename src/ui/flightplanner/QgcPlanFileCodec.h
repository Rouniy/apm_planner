#ifndef QGCPLANFILECODEC_H
#define QGCPLANFILECODEC_H

#include "FenceRallyModel.h"
#include "WpRow.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace MissionPlanner
{

// Lossless reader/writer for the QGroundControl Plan JSON format.  The
// standard QGC sections are always emitted.  Since those sections do not
// carry all MAVLink fields for fence and rally items, an optional apmPlanner
// extension stores the exact WpRowData representation of all three stores.
class QgcPlanFileCodec final
{
public:
    struct PlanData
    {
        GeoCoordinate Home;
        QVector<WpRowData> Mission;
        QVector<WpRowData> Fence;
        QVector<WpRowData> Rally;

        double CruiseSpeed = 15.0;
        double HoverSpeed = 5.0;
        int FirmwareType = 12; // MAV_AUTOPILOT_ARDUPILOTMEGA
        int VehicleType = 2;   // MAV_TYPE_QUADROTOR
    };

    struct Result
    {
        bool ok = false;
        QString error;
    };

    struct EncodeResult : Result
    {
        QByteArray data;
    };

    struct DecodeResult : Result
    {
        PlanData plan;
    };

    static EncodeResult EncodePlan(const PlanData &plan);
    static DecodeResult DecodePlan(const QByteArray &data);
    static Result SavePlan(const QString &path, const PlanData &plan);
    static DecodeResult LoadPlan(const QString &path);

private:
    QgcPlanFileCodec() = delete;
};

} // namespace MissionPlanner

#endif // QGCPLANFILECODEC_H
