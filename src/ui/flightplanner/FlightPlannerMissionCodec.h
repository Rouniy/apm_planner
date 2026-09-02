#ifndef FLIGHTPLANNERMISSIONCODEC_H
#define FLIGHTPLANNERMISSIONCODEC_H

#include "FlightPlannerMissionModel.h"
#include "QGCMAVLink.h"

#include <QString>
#include <QVector>

class FlightPlannerMissionCodec final
{
public:
    struct DecodeResult
    {
        bool ok = false;
        QString error;
        QVector<WpRowData> rows;
        bool homeValid = false;
        double homeLatitude = 0.0;
        double homeLongitude = 0.0;
        double homeAltitude = 0.0;
    };

    struct EncodeResult
    {
        bool ok = false;
        QString error;
        QVector<mavlink_mission_item_int_t> items;
    };

    static MAV_MISSION_TYPE missionType(
            FlightPlannerMissionModel::MissionStore store);

    static DecodeResult decode(
            FlightPlannerMissionModel::MissionStore store,
            const QVector<mavlink_mission_item_int_t> &items);

    static EncodeResult encode(
            FlightPlannerMissionModel::MissionStore store,
            const QVector<WpRowData> &rows,
            bool homeValid = false,
            double homeLatitude = 0.0,
            double homeLongitude = 0.0,
            double homeAltitude = 0.0);

private:
    FlightPlannerMissionCodec() = delete;
};

#endif // FLIGHTPLANNERMISSIONCODEC_H
