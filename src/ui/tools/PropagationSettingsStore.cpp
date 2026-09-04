#include "PropagationSettingsStore.h"

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>

#include <cmath>

PropagationSettings PropagationSettings::Default()
{
    return PropagationSettings();
}

bool PropagationSettings::operator==(
    const PropagationSettings &other) const noexcept
{
    return clearanceMeters == other.clearanceMeters
        && resolutionPixels == other.resolutionPixels
        && azimuthStepDegrees == other.azimuthStepDegrees
        && convergenceDegrees == other.convergenceDegrees
        && rangeKilometers == other.rangeKilometers
        && baseHeightMeters == other.baseHeightMeters
        && tolerance == other.tolerance
        && minimumAltitude == other.minimumAltitude
        && maximumAltitude == other.maximumAltitude
        && elevationMap == other.elevationMap
        && terrainMap == other.terrainMap
        && rfMap == other.rfMap
        && homeDistance == other.homeDistance
        && droneDistance == other.droneDistance
        && manualAltitudeRange == other.manualAltitudeRange
        && showScale == other.showScale;
}

PropagationSettingsStore::PropagationSettingsStore(
    QSettings *settings, QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    qRegisterMetaType<PropagationSettings>("PropagationSettings");
    m_settings->setFallbacksEnabled(false);
    m_values = readSettings();
}

PropagationSettingsStore::~PropagationSettingsStore() = default;

PropagationSettingsStore *PropagationSettingsStore::instance()
{
    static PropagationSettingsStore *const store =
        new PropagationSettingsStore(nullptr, QCoreApplication::instance());
    return store;
}

PropagationSettings PropagationSettingsStore::defaults()
{
    return PropagationSettings::Default();
}

QString PropagationSettingsStore::clearanceKey()
{
    return QStringLiteral("Propagation_Clearance");
}

QString PropagationSettingsStore::resolutionKey()
{
    return QStringLiteral("Propagation_Resolution");
}

QString PropagationSettingsStore::azimuthStepKey()
{
    return QStringLiteral("Propagation_Rotational");
}

QString PropagationSettingsStore::convergenceKey()
{
    return QStringLiteral("Propagation_Converge");
}

QString PropagationSettingsStore::rangeKey()
{
    return QStringLiteral("Propagation_Range");
}

QString PropagationSettingsStore::baseHeightKey()
{
    return QStringLiteral("Propagation_Height");
}

QString PropagationSettingsStore::toleranceKey()
{
    return QStringLiteral("Propagation_Tolerance");
}

QString PropagationSettingsStore::minimumAltitudeKey()
{
    return QStringLiteral("Propagation_Minalt");
}

QString PropagationSettingsStore::maximumAltitudeKey()
{
    return QStringLiteral("Propagation_Maxalt");
}

QString PropagationSettingsStore::elevationMapKey()
{
    return QStringLiteral("Propagation_Elemap");
}

QString PropagationSettingsStore::terrainMapKey()
{
    return QStringLiteral("Propagation_Termap");
}

QString PropagationSettingsStore::rfMapKey()
{
    return QStringLiteral("Propagation_RFmap");
}

QString PropagationSettingsStore::homeDistanceKey()
{
    return QStringLiteral("Propagation_home_kmleft");
}

QString PropagationSettingsStore::droneDistanceKey()
{
    return QStringLiteral("Propagation_drone_kmleft");
}

QString PropagationSettingsStore::manualAltitudeRangeKey()
{
    return QStringLiteral("Propagation_Setalt");
}

QString PropagationSettingsStore::showScaleKey()
{
    return QStringLiteral("Propagation_ShowScale");
}

bool PropagationSettingsStore::setSettings(
    const PropagationSettings &settings)
{
    if (settings == m_values) {
        m_lastError.clear();
        return true;
    }

    m_settings->setValue(clearanceKey(), invariant(settings.clearanceMeters));
    m_settings->setValue(resolutionKey(), QString::number(
        settings.resolutionPixels));
    m_settings->setValue(azimuthStepKey(), invariant(
        settings.azimuthStepDegrees));
    m_settings->setValue(convergenceKey(), invariant(
        settings.convergenceDegrees));
    m_settings->setValue(rangeKey(), invariant(settings.rangeKilometers));
    m_settings->setValue(baseHeightKey(), invariant(
        settings.baseHeightMeters));
    m_settings->setValue(toleranceKey(), invariant(settings.tolerance));
    m_settings->setValue(minimumAltitudeKey(), invariant(
        settings.minimumAltitude));
    m_settings->setValue(maximumAltitudeKey(), invariant(
        settings.maximumAltitude));
    const auto boolText = [](bool value) {
        return value ? QStringLiteral("True") : QStringLiteral("False");
    };
    m_settings->setValue(elevationMapKey(), boolText(settings.elevationMap));
    m_settings->setValue(terrainMapKey(), boolText(settings.terrainMap));
    m_settings->setValue(rfMapKey(), boolText(settings.rfMap));
    m_settings->setValue(homeDistanceKey(), boolText(settings.homeDistance));
    m_settings->setValue(droneDistanceKey(), boolText(settings.droneDistance));
    m_settings->setValue(manualAltitudeRangeKey(), boolText(
        settings.manualAltitudeRange));
    m_settings->setValue(showScaleKey(), boolText(settings.showScale));
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_lastError = tr("RF propagation settings could not be saved.");
        emit saveFailed(m_lastError);
        return false;
    }

    m_lastError.clear();
    m_values = settings;
    emit settingsChanged(m_values);
    return true;
}

void PropagationSettingsStore::reload()
{
    m_settings->sync();
    const PropagationSettings loaded = readSettings();
    if (loaded == m_values) {
        return;
    }
    m_values = loaded;
    emit settingsChanged(m_values);
}

PropagationSettings PropagationSettingsStore::readSettings() const
{
    const PropagationSettings fallback = defaults();
    PropagationSettings result;
    result.clearanceMeters = readDouble(
        clearanceKey(), fallback.clearanceMeters);
    bool ok = false;
    const int resolution = m_settings->value(
        resolutionKey(), fallback.resolutionPixels).toInt(&ok);
    result.resolutionPixels = ok ? resolution : fallback.resolutionPixels;
    result.azimuthStepDegrees = readDouble(
        azimuthStepKey(), fallback.azimuthStepDegrees);
    result.convergenceDegrees = readDouble(
        convergenceKey(), fallback.convergenceDegrees);
    result.rangeKilometers = readDouble(
        rangeKey(), fallback.rangeKilometers);
    result.baseHeightMeters = readDouble(
        baseHeightKey(), fallback.baseHeightMeters);
    result.tolerance = readDouble(toleranceKey(), fallback.tolerance);
    result.minimumAltitude = readDouble(
        minimumAltitudeKey(), fallback.minimumAltitude);
    result.maximumAltitude = readDouble(
        maximumAltitudeKey(), fallback.maximumAltitude);
    result.elevationMap = readBool(
        elevationMapKey(), fallback.elevationMap);
    result.terrainMap = readBool(terrainMapKey(), fallback.terrainMap);
    result.rfMap = readBool(rfMapKey(), fallback.rfMap);
    result.homeDistance = readBool(
        homeDistanceKey(), fallback.homeDistance);
    result.droneDistance = readBool(
        droneDistanceKey(), fallback.droneDistance);
    result.manualAltitudeRange = readBool(
        manualAltitudeRangeKey(), fallback.manualAltitudeRange);
    result.showScale = readBool(showScaleKey(), fallback.showScale);
    return result;
}

double PropagationSettingsStore::readDouble(
    const QString &key, double fallback) const
{
    if (!m_settings->contains(key)) {
        return fallback;
    }
    const QString text = m_settings->value(key).toString().trimmed();
    bool ok = false;
    double value = QLocale::c().toDouble(text, &ok);
    if (!ok) {
        value = QLocale().toDouble(text, &ok);
    }
    return ok && std::isfinite(value) ? value : fallback;
}

bool PropagationSettingsStore::readBool(
    const QString &key, bool fallback) const
{
    return m_settings->contains(key)
        ? m_settings->value(key).toBool() : fallback;
}

QString PropagationSettingsStore::invariant(double value)
{
    QString result = QString::number(value, 'f', 3);
    while (result.contains(QLatin1Char('.'))
           && result.endsWith(QLatin1Char('0'))) {
        result.chop(1);
    }
    if (result.endsWith(QLatin1Char('.'))) {
        result.chop(1);
    }
    return result;
}
