#ifndef DISPLAYVIEWPROFILE_H
#define DISPLAYVIEWPROFILE_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

enum class DisplayViewPreset
{
    Basic = 0,
    Advanced = 1,
    Custom = 2
};

struct DisplayViewConfigFlags
{
    bool displayFlightModes = true;
    bool displayStandardParams = false;
    bool displayAdvancedParams = false;
    bool displayGeoFence = true;
    bool displayBasicTuning = true;
    bool displayExtendedTuning = true;
    bool displayOSD = true;
    bool displayMavFTP = true;
    bool displayUserParam = true;
    bool displayFullParamList = true;
    bool displayPlannerSettings = true;
};

struct DisplayViewSetupFlags
{
    bool displayADSB = true;
    bool displayAccelCalibration = true;
    bool displayAirSpeed = true;
    bool displayAntennaTracker = true;
    bool displayBattMonitor = true;
    bool displayBluetooth = true;
    bool displayCAN = true;
    bool displayCameraGimbal = true;
    bool displayCompassConfiguration = true;
    bool displayCompassMotorCalib = true;
    bool displayEscCalibration = true;
    bool displayEsp = true;
    bool displayFFTSetup = true;
    bool displayFailSafe = true;
    bool displayFlightModes = true;
    bool displayFrameType = true;
    bool displayGPSOrder = true;
    bool displayHWIDs = true;
    bool displayInitialParams = true;
    bool displayInstallFirmware = true;
    bool displayJoystick = true;
    bool displayMavFTP = true;
    bool displayMotorTest = true;
    bool displayOpticalFlow = true;
    bool displayOsd = true;
    bool displayParachute = true;
    bool displayPx4Flow = true;
    bool displayREPL = true;
    bool displayRTKInject = true;
    bool displayRadioCalibration = true;
    bool displayRangeFinder = true;
    bool displaySerialPorts = true;
    bool displayServoOutput = true;
    bool displaySikRadio = true;
    bool displayTerminal = true;
};

/**
 * The part of Mission Planner's DisplayView profile consumed by CONFIG,
 * SETUP and the Planner layout selector.
 *
 * A default-constructed value follows MP10's missing-member template. The
 * application startup default is intentionally Advanced and is selected by
 * DisplayViewProfileService when no persisted profile exists.
 */
class DisplayViewProfile final
{
public:
    DisplayViewProfile();

    static DisplayViewProfile basic();
    static DisplayViewProfile advanced();
    static bool fromJson(const QByteArray &json, DisplayViewProfile *profile,
                         QString *error = nullptr);

    DisplayViewPreset preset() const;
    QString presetName() const;
    bool isAdvancedMode() const;
    bool displayPlannerLayout() const;
    DisplayViewConfigFlags configFlags() const;
    DisplayViewSetupFlags setupFlags() const;

    // Custom profile editors can change shell mode without replacing their
    // other authored visibility flags.
    DisplayViewProfile withAdvancedMode(bool enabled) const;
    DisplayViewProfile asCustom() const;
    DisplayViewProfile withStartupParameterVisibility() const;

    QByteArray toJson() const;
    QJsonObject jsonObject() const { return m_values; }

    bool operator==(const DisplayViewProfile &other) const;
    bool operator!=(const DisplayViewProfile &other) const
    {
        return !(*this == other);
    }

private:
    explicit DisplayViewProfile(const QJsonObject &values);
    bool booleanValue(const char *key) const;

    QJsonObject m_values;
};

/**
 * Shared owner for the active DisplayView profile.
 *
 * Production callers may use instance(). Tests and other isolated consumers
 * can inject an INI-backed QSettings object and a temporary Custom path. The
 * injected QSettings object remains owned by its caller.
 */
class DisplayViewProfileService final : public QObject
{
    Q_OBJECT

public:
    explicit DisplayViewProfileService(
        QSettings *settings = nullptr,
        const QString &customProfilePath = QString(),
        QObject *parent = nullptr);
    ~DisplayViewProfileService() override;

    static DisplayViewProfileService *instance();
    static QString settingsKey();
    static QString defaultCustomProfilePath();

    DisplayViewProfile current() const { return m_current; }
    QString customProfilePath() const { return m_customProfilePath; }

    bool reload(QString *error = nullptr);
    bool setProfile(const DisplayViewProfile &profile,
                    QString *error = nullptr);
    bool applyPreset(DisplayViewPreset preset, QString *error = nullptr);

signals:
    void changed();

private:
    bool loadCustomProfile(DisplayViewProfile *profile,
                           QString *error) const;

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    QString m_customProfilePath;
    DisplayViewProfile m_current = DisplayViewProfile::advanced();
};

#endif // DISPLAYVIEWPROFILE_H
