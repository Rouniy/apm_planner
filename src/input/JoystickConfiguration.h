#ifndef JOYSTICKCONFIGURATION_H
#define JOYSTICKCONFIGURATION_H

#include <QMap>
#include <QStringList>
#include <QVector>
class QSettings;

// Local configuration only: no connection, vehicle writes or automatic enable.
class JoystickConfiguration final
{
public:
    static constexpr int ChannelCount = 16;
    static constexpr int MaximumControls = 128;
    struct Range { int minimum = -32768, maximum = 32767; };
    using Ranges = QMap<int, Range>;
    struct Channel { int channel = 1; QString axis = QStringLiteral("None"); bool reverse = false; int expo = 0; };
    struct Button {
        int buttonno = -1;
        QString function = QStringLiteral("ChangeMode"), mode;
        double p1 = 0, p2 = 0, p3 = 0, p4 = 0;
        bool state = false;
    };
    struct Profile {
        QString deviceId, deviceName, firmware; // Empty firmware selects generic MP XML names.
        bool elevons = false, manualControl = false;
        QVector<Channel> channels;
        QVector<Button> buttons;
        QMap<QString, Ranges> calibration;
    };
    static Profile defaults();
    // Restores exactly 16 channel rows/128 button rows; clamps editable limits.
    // Unknown axis/function strings are retained and validation rejects them.
    static void normalize(Profile *profile);
    static bool validate(const Profile &, QString *error = nullptr);
    static QStringList axisNames(int axes = 0, int hats = 0);
    static QStringList buttonFunctions();
    static bool validRange(const Range &);
    static quint16 normalizeAxis(qint16 raw, const Range &range);
    static bool axisValue(const QString &, const QVector<quint16> &axes,
                          const QVector<quint8> &hats, quint16 *value);
    // Exact MP10 triangular Expo, not a cubic approximation. Input -500..500.
    static double expo(double input, double percent, double minimum, double maximum, double trim);
    static int channelValue(quint16 normalized, const Channel &, bool manualControl = false,
                            int minimum = 1000, int maximum = 2000, int trim = 1500);
    static bool load(QSettings *, Profile *, QString *error = nullptr);
    static bool save(QSettings *, const Profile &, QString *error = nullptr);
    // MP10 ZIP/XML joycfg. Qt-only device/elevon/manual/calibration data is a
    // bounded extra JSON member ignored by MP10. Import never extracts paths.
    static bool importConfig(const QString &path, Profile *, QString *error = nullptr);
    static bool exportConfig(const QString &path, const Profile &, QString *error = nullptr);
};

#endif
