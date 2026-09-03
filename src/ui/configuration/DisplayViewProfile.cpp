#include "DisplayViewProfile.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>
#include <QStandardPaths>

namespace {
constexpr qint64 kMaximumCustomProfileBytes = 2 * 1024 * 1024;

struct BooleanDefault
{
    const char *key;
    bool templateValue;
    bool basicValue;
    bool advancedValue;
};

// This is the exact union of the 11 CONFIG and 35 SETUP route flags. The two
// shared flags (Flight Modes and MAVFtp) intentionally occur only once.
const BooleanDefault kRouteDefaults[] = {
    {"displayADSB", true, true, true},
    {"displayAccelCalibration", true, true, true},
    {"displayAdvancedParams", false, false, false},
    {"displayAirSpeed", true, true, true},
    {"displayAntennaTracker", true, true, true},
    {"displayBasicTuning", true, true, true},
    {"displayBattMonitor", true, true, true},
    {"displayBluetooth", true, true, true},
    {"displayCAN", true, true, true},
    {"displayCameraGimbal", true, true, true},
    {"displayCompassConfiguration", true, true, true},
    {"displayCompassMotorCalib", true, true, true},
    {"displayEscCalibration", true, true, true},
    {"displayEsp", true, true, true},
    {"displayExtendedTuning", true, true, true},
    {"displayFFTSetup", true, true, true},
    {"displayFailSafe", true, true, true},
    {"displayFlightModes", true, true, true},
    {"displayFrameType", true, true, true},
    {"displayFullParamList", true, true, true},
    {"displayGPSOrder", true, true, true},
    {"displayGeoFence", true, true, true},
    {"displayHWIDs", true, true, true},
    {"displayInitialParams", true, true, true},
    {"displayInstallFirmware", true, true, true},
    {"displayJoystick", true, true, true},
    {"displayMavFTP", true, true, true},
    {"displayMotorTest", true, true, true},
    {"displayOSD", true, true, true},
    {"displayOpticalFlow", true, true, true},
    {"displayOsd", true, true, true},
    {"displayParachute", true, true, true},
    {"displayPlannerSettings", true, true, true},
    {"displayPx4Flow", true, true, true},
    {"displayREPL", true, true, true},
    {"displayRTKInject", true, true, true},
    {"displayRadioCalibration", true, true, true},
    {"displayRangeFinder", true, true, true},
    {"displaySerialPorts", true, true, true},
    {"displayServoOutput", true, true, true},
    {"displaySikRadio", true, true, true},
    {"displayStandardParams", false, false, false},
    {"displayTerminal", true, false, true},
    {"displayUserParam", true, true, true}
};

QJsonValue canonicalValue(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QJsonObject result;
        const QStringList keys = object.keys();
        for (const QString &key : keys) {
            result.insert(key, canonicalValue(object.value(key)));
        }
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &entry : array) {
            result.append(canonicalValue(entry));
        }
        return result;
    }
    return value;
}

DisplayViewPreset parsePreset(const QJsonValue &value)
{
    if (value.isDouble()) {
        const int number = value.toInt(-1);
        if (number == static_cast<int>(DisplayViewPreset::Advanced)) {
            return DisplayViewPreset::Advanced;
        }
        if (number == static_cast<int>(DisplayViewPreset::Custom)) {
            return DisplayViewPreset::Custom;
        }
        return DisplayViewPreset::Basic;
    }
    const QString name = value.toString().trimmed();
    if (name.compare(QStringLiteral("Advanced"), Qt::CaseInsensitive) == 0) {
        return DisplayViewPreset::Advanced;
    }
    if (name.compare(QStringLiteral("Custom"), Qt::CaseInsensitive) == 0) {
        return DisplayViewPreset::Custom;
    }
    return DisplayViewPreset::Basic;
}

QJsonObject presetValues(DisplayViewPreset preset)
{
    QJsonObject values;
    values.insert(QStringLiteral("displayName"), static_cast<int>(preset));
    for (const BooleanDefault &entry : kRouteDefaults) {
        const bool value = preset == DisplayViewPreset::Advanced
            ? entry.advancedValue : entry.basicValue;
        values.insert(QString::fromLatin1(entry.key), value);
    }
    values.insert(QStringLiteral("displayPlannerLayout"), true);
    values.insert(QStringLiteral("isAdvancedMode"),
                  preset == DisplayViewPreset::Advanced);
    return values;
}

QJsonObject templateValues()
{
    QJsonObject values;
    values.insert(QStringLiteral("displayName"),
                  static_cast<int>(DisplayViewPreset::Basic));
    for (const BooleanDefault &entry : kRouteDefaults) {
        values.insert(QString::fromLatin1(entry.key), entry.templateValue);
    }
    values.insert(QStringLiteral("displayPlannerLayout"), true);
    values.insert(QStringLiteral("isAdvancedMode"), false);
    return values;
}

QString parseErrorText(const QJsonParseError &parseError)
{
    return QObject::tr("DisplayView JSON is invalid at byte %1: %2")
        .arg(parseError.offset)
        .arg(parseError.errorString());
}
}

DisplayViewProfile::DisplayViewProfile()
    : m_values(templateValues())
{
}

DisplayViewProfile::DisplayViewProfile(const QJsonObject &values)
    : m_values(values)
{
}

DisplayViewProfile DisplayViewProfile::basic()
{
    return DisplayViewProfile(presetValues(DisplayViewPreset::Basic));
}

DisplayViewProfile DisplayViewProfile::advanced()
{
    return DisplayViewProfile(presetValues(DisplayViewPreset::Advanced));
}

bool DisplayViewProfile::fromJson(const QByteArray &json,
                                  DisplayViewProfile *profile,
                                  QString *error)
{
    if (!profile) {
        if (error) {
            *error = QObject::tr("No DisplayView profile output was provided.");
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = parseErrorText(parseError);
        }
        return false;
    }
    if (!document.isObject()) {
        if (error) {
            *error = QObject::tr("DisplayView JSON must contain an object.");
        }
        return false;
    }

    const QJsonObject source = document.object();
    QJsonObject normalized = source;
    const QJsonObject defaults = templateValues();
    const DisplayViewPreset parsedPreset = parsePreset(
        source.value(QStringLiteral("displayName")));
    normalized.insert(QStringLiteral("displayName"),
                      static_cast<int>(parsedPreset));
    for (const BooleanDefault &entry : kRouteDefaults) {
        const QString key = QString::fromLatin1(entry.key);
        const QJsonValue sourceValue = source.value(key);
        normalized.insert(key, sourceValue.isBool()
                                  ? sourceValue.toBool()
                                  : defaults.value(key).toBool());
    }
    const QJsonValue plannerLayout = source.value(
        QStringLiteral("displayPlannerLayout"));
    normalized.insert(QStringLiteral("displayPlannerLayout"),
                      plannerLayout.isBool() ? plannerLayout.toBool() : true);
    const QJsonValue advancedMode = source.value(
        QStringLiteral("isAdvancedMode"));
    normalized.insert(QStringLiteral("isAdvancedMode"),
                      advancedMode.isBool() ? advancedMode.toBool() : false);

    *profile = DisplayViewProfile(normalized);
    if (error) {
        error->clear();
    }
    return true;
}

DisplayViewPreset DisplayViewProfile::preset() const
{
    return parsePreset(m_values.value(QStringLiteral("displayName")));
}

QString DisplayViewProfile::presetName() const
{
    switch (preset()) {
    case DisplayViewPreset::Basic:
        return QStringLiteral("Basic");
    case DisplayViewPreset::Advanced:
        return QStringLiteral("Advanced");
    case DisplayViewPreset::Custom:
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Basic");
}

bool DisplayViewProfile::booleanValue(const char *key) const
{
    return m_values.value(QString::fromLatin1(key)).toBool();
}

bool DisplayViewProfile::isAdvancedMode() const
{
    return booleanValue("isAdvancedMode");
}

bool DisplayViewProfile::displayPlannerLayout() const
{
    return booleanValue("displayPlannerLayout");
}

DisplayViewConfigFlags DisplayViewProfile::configFlags() const
{
    DisplayViewConfigFlags flags;
    flags.displayFlightModes = booleanValue("displayFlightModes");
    flags.displayStandardParams = booleanValue("displayStandardParams");
    flags.displayAdvancedParams = booleanValue("displayAdvancedParams");
    flags.displayGeoFence = booleanValue("displayGeoFence");
    flags.displayBasicTuning = booleanValue("displayBasicTuning");
    flags.displayExtendedTuning = booleanValue("displayExtendedTuning");
    flags.displayOSD = booleanValue("displayOSD");
    flags.displayMavFTP = booleanValue("displayMavFTP");
    flags.displayUserParam = booleanValue("displayUserParam");
    flags.displayFullParamList = booleanValue("displayFullParamList");
    flags.displayPlannerSettings = booleanValue("displayPlannerSettings");
    return flags;
}

DisplayViewSetupFlags DisplayViewProfile::setupFlags() const
{
    DisplayViewSetupFlags flags;
    flags.displayADSB = booleanValue("displayADSB");
    flags.displayAccelCalibration = booleanValue("displayAccelCalibration");
    flags.displayAirSpeed = booleanValue("displayAirSpeed");
    flags.displayAntennaTracker = booleanValue("displayAntennaTracker");
    flags.displayBattMonitor = booleanValue("displayBattMonitor");
    flags.displayBluetooth = booleanValue("displayBluetooth");
    flags.displayCAN = booleanValue("displayCAN");
    flags.displayCameraGimbal = booleanValue("displayCameraGimbal");
    flags.displayCompassConfiguration = booleanValue(
        "displayCompassConfiguration");
    flags.displayCompassMotorCalib = booleanValue("displayCompassMotorCalib");
    flags.displayEscCalibration = booleanValue("displayEscCalibration");
    flags.displayEsp = booleanValue("displayEsp");
    flags.displayFFTSetup = booleanValue("displayFFTSetup");
    flags.displayFailSafe = booleanValue("displayFailSafe");
    flags.displayFlightModes = booleanValue("displayFlightModes");
    flags.displayFrameType = booleanValue("displayFrameType");
    flags.displayGPSOrder = booleanValue("displayGPSOrder");
    flags.displayHWIDs = booleanValue("displayHWIDs");
    flags.displayInitialParams = booleanValue("displayInitialParams");
    flags.displayInstallFirmware = booleanValue("displayInstallFirmware");
    flags.displayJoystick = booleanValue("displayJoystick");
    flags.displayMavFTP = booleanValue("displayMavFTP");
    flags.displayMotorTest = booleanValue("displayMotorTest");
    flags.displayOpticalFlow = booleanValue("displayOpticalFlow");
    flags.displayOsd = booleanValue("displayOsd");
    flags.displayParachute = booleanValue("displayParachute");
    flags.displayPx4Flow = booleanValue("displayPx4Flow");
    flags.displayREPL = booleanValue("displayREPL");
    flags.displayRTKInject = booleanValue("displayRTKInject");
    flags.displayRadioCalibration = booleanValue("displayRadioCalibration");
    flags.displayRangeFinder = booleanValue("displayRangeFinder");
    flags.displaySerialPorts = booleanValue("displaySerialPorts");
    flags.displayServoOutput = booleanValue("displayServoOutput");
    flags.displaySikRadio = booleanValue("displaySikRadio");
    flags.displayTerminal = booleanValue("displayTerminal");
    return flags;
}

DisplayViewProfile DisplayViewProfile::withAdvancedMode(bool enabled) const
{
    QJsonObject values = m_values;
    values.insert(QStringLiteral("isAdvancedMode"), enabled);
    values.insert(QStringLiteral("displayName"),
                  static_cast<int>(DisplayViewPreset::Custom));
    return DisplayViewProfile(values);
}

DisplayViewProfile DisplayViewProfile::asCustom() const
{
    QJsonObject values = m_values;
    values.insert(QStringLiteral("displayName"),
                  static_cast<int>(DisplayViewPreset::Custom));
    return DisplayViewProfile(values);
}

DisplayViewProfile DisplayViewProfile::withStartupParameterVisibility() const
{
    QJsonObject values = m_values;
    // MP10 deliberately applies these three startup overrides after loading
    // a stored profile. Friendly parameter lists stay hidden while the full
    // list remains recoverable, even for an authored Custom profile.
    values.insert(QStringLiteral("displayStandardParams"), false);
    values.insert(QStringLiteral("displayAdvancedParams"), false);
    values.insert(QStringLiteral("displayFullParamList"), true);
    return DisplayViewProfile(values);
}

QByteArray DisplayViewProfile::toJson() const
{
    return QJsonDocument(canonicalValue(m_values).toObject())
        .toJson(QJsonDocument::Compact);
}

bool DisplayViewProfile::operator==(const DisplayViewProfile &other) const
{
    return toJson() == other.toJson();
}

DisplayViewProfileService::DisplayViewProfileService(
    QSettings *settings, const QString &customProfilePath, QObject *parent)
    : QObject(parent),
      m_customProfilePath(customProfilePath.isEmpty()
                              ? defaultCustomProfilePath()
                              : QDir::cleanPath(customProfilePath))
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    // DisplayView is a single per-application profile. Do not inherit a
    // same-named value from a fallback organization/system scope.
    m_settings->setFallbacksEnabled(false);
    reload();
}

DisplayViewProfileService::~DisplayViewProfileService() = default;

DisplayViewProfileService *DisplayViewProfileService::instance()
{
    // The profile is application-global by design: MainWindow, CONFIG and
    // SETUP must never observe independent visibility state.
    static DisplayViewProfileService service;
    return &service;
}

QString DisplayViewProfileService::settingsKey()
{
    return QStringLiteral("displayview");
}

QString DisplayViewProfileService::defaultCustomProfilePath()
{
    QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    return QDir(root).filePath(QStringLiteral("custom.displayview"));
}

bool DisplayViewProfileService::reload(QString *error)
{
    DisplayViewProfile loaded = DisplayViewProfile::advanced();
    QString loadError;
    if (m_settings->contains(settingsKey())) {
        const QByteArray json = m_settings->value(settingsKey())
                                    .toString().toUtf8();
        if (!DisplayViewProfile::fromJson(json, &loaded, &loadError)) {
            // Settings.GetDisplayView in MP10 falls back to the raw
            // DisplayView constructor (Basic), not the application startup
            // default (Advanced), when a stored value is corrupt.
            loaded = DisplayViewProfile();
            m_settings->setValue(settingsKey(),
                                 QString::fromUtf8(loaded.toJson()));
            m_settings->sync();
        } else {
            loaded = loaded.withStartupParameterVisibility();
        }
    } else {
        const QString legacyAdvancedModeKey =
            QStringLiteral("QGC_MAINWINDOW/ADVANCED_MODE");
        if (m_settings->contains(legacyAdvancedModeKey)) {
            loaded = m_settings->value(legacyAdvancedModeKey).toBool()
                ? DisplayViewProfile::advanced()
                : DisplayViewProfile::basic();
            m_settings->setValue(settingsKey(),
                                 QString::fromUtf8(loaded.toJson()));
            m_settings->sync();
            if (m_settings->status() != QSettings::NoError) {
                loadError = tr("Could not migrate the legacy layout setting.");
            }
        }
    }

    const bool changedProfile = loaded != m_current;
    m_current = loaded;
    if (error) {
        *error = loadError;
    }
    if (changedProfile) {
        emit changed();
    }
    return loadError.isEmpty();
}

bool DisplayViewProfileService::setProfile(
    const DisplayViewProfile &profile, QString *error)
{
    m_settings->setValue(settingsKey(), QString::fromUtf8(profile.toJson()));
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        if (error) {
            *error = tr("Could not save the DisplayView profile.");
        }
        return false;
    }

    const bool changedProfile = profile != m_current;
    m_current = profile;
    if (error) {
        error->clear();
    }
    if (changedProfile) {
        emit changed();
    }
    return true;
}

bool DisplayViewProfileService::applyPreset(DisplayViewPreset preset,
                                            QString *error)
{
    switch (preset) {
    case DisplayViewPreset::Basic:
        return setProfile(DisplayViewProfile::basic(), error);
    case DisplayViewPreset::Advanced:
        return setProfile(DisplayViewProfile::advanced(), error);
    case DisplayViewPreset::Custom: {
        DisplayViewProfile profile;
        if (!loadCustomProfile(&profile, error)) {
            return false;
        }
        return setProfile(profile.asCustom(), error);
    }
    }
    if (error) {
        *error = tr("Unknown DisplayView preset.");
    }
    return false;
}

bool DisplayViewProfileService::loadCustomProfile(
    DisplayViewProfile *profile, QString *error) const
{
    QFile file(m_customProfilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Could not open Custom DisplayView profile: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    if (file.size() > kMaximumCustomProfileBytes) {
        if (error) {
            *error = tr("Custom DisplayView profile is larger than %1 bytes.")
                         .arg(kMaximumCustomProfileBytes);
        }
        return false;
    }
    return DisplayViewProfile::fromJson(file.readAll(), profile, error);
}
