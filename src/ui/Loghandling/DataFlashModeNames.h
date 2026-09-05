#ifndef DATAFLASHMODENAMES_H
#define DATAFLASHMODENAMES_H

#include <QByteArray>
#include <QString>
#include <cstddef>

// Immutable MP10 bundled-label snapshot for offline binary M fields, 2026-09-06.
// Copter: ParameterMetaDataLocal.xml FLTMODE1; Plane/Rover: backup metadata;
// Tracker and Plane16: ArduPilot/Common.cs. Never touch a live vehicle or mutable
// GUI metadata repository from an exporter worker. Unknowns remain numeric.
namespace DataFlashModeNames {
struct Entry { int code; const char *name; };
template<std::size_t N>
inline QString lookup(const Entry (&entries)[N], int code)
{
    for (const Entry &entry : entries)
        if (entry.code == code) return QString::fromLatin1(entry.name);
    return {};
}
inline QString resolve(const QByteArray &firmware, int mode)
{
    static constexpr Entry copter[] = {
        {0, "Stabilize"},
        {1, "Acro"},
        {2, "AltHold"},
        {3, "Auto"},
        {4, "Guided"},
        {5, "Loiter"},
        {6, "RTL"},
        {7, "Circle"},
        {9, "Land"},
        {11, "Drift"},
        {13, "Sport"},
        {14, "Flip"},
        {15, "AutoTune"},
        {16, "PosHold"},
        {17, "Brake"},
        {18, "Throw"},
        {19, "Avoid_ADSB"},
        {20, "Guided_NoGPS"},
        {21, "Smart_RTL"},
        {22, "FlowHold"},
        {23, "Follow"},
        {24, "ZigZag"},
        {25, "SystemID"},
        {26, "Heli_Autorotate"},
        {27, "Auto RTL"},
        {28, "Turtle"},
        {31, "ModelCal"},
    };
    static constexpr Entry plane[] = {
        {0, "Manual"},
        {1, "CIRCLE"},
        {2, "STABILIZE"},
        {3, "TRAINING"},
        {4, "ACRO"},
        {5, "FBWA"},
        {6, "FBWB"},
        {7, "CRUISE"},
        {8, "AUTOTUNE"},
        {10, "Auto"},
        {11, "RTL"},
        {12, "Loiter"},
        {13, "TAKEOFF"},
        {14, "AVOID_ADSB"},
        {15, "Guided"},
        {16, "INITIALISING"},
        {17, "QSTABILIZE"},
        {18, "QHOVER"},
        {19, "QLOITER"},
        {20, "QLAND"},
        {21, "QRTL"},
        {22, "QAUTOTUNE"},
        {23, "QACRO"},
        {24, "THERMAL"},
        {25, "Loiter to QLand"},
    };
    static constexpr Entry rover[] = {
        {0, "Manual"},
        {1, "Acro"},
        {3, "Steering"},
        {4, "Hold"},
        {5, "Loiter"},
        {6, "Follow"},
        {7, "Simple"},
        {10, "Auto"},
        {11, "RTL"},
        {12, "SmartRTL"},
        {15, "Guided"},
    };
    static constexpr Entry tracker[] = {
        {0, "MANUAL"},
        {1, "STOP"},
        {2, "SCAN"},
        {3, "SERVO_TEST"},
        {10, "AUTO"},
        {16, "INITIALISING"},
    };
    if (firmware == "ArduCopter2") return lookup(copter, mode);
    if (firmware == "ArduPlane") return lookup(plane, mode);
    if (firmware == "ArduRover") return lookup(rover, mode);
    if (firmware == "ArduTracker") return lookup(tracker, mode);
    return {};
}
} // namespace DataFlashModeNames

#endif // DATAFLASHMODENAMES_H
