#ifndef PROPAGATIONSETTINGSSTORE_H
#define PROPAGATIONSETTINGSSTORE_H

#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

/** Mission Planner 10 RF propagation overlay options. */
struct PropagationSettings
{
    double clearanceMeters = 5.0;
    int resolutionPixels = 4;
    double azimuthStepDegrees = 1.0;
    double convergenceDegrees = 1.0;
    double rangeKilometers = 2.0;
    double baseHeightMeters = 2.0;
    double tolerance = 0.8;
    double minimumAltitude = 100.0;
    double maximumAltitude = 400.0;
    bool elevationMap = false;
    bool terrainMap = false;
    bool rfMap = false;
    bool homeDistance = false;
    bool droneDistance = false;
    bool manualAltitudeRange = false;
    bool showScale = false;

    static PropagationSettings Default();

    bool operator==(const PropagationSettings &other) const noexcept;
    bool operator!=(const PropagationSettings &other) const noexcept
    {
        return !(*this == other);
    }
};

Q_DECLARE_METATYPE(PropagationSettings)

/**
 * Application-owned, live and autosaved owner for RF propagation settings.
 *
 * The exact MP10 keys are intentionally public so map consumers can share the
 * same contract. Production uses instance(); tests may inject an INI-backed
 * QSettings object, which remains owned by the caller.
 */
class PropagationSettingsStore final : public QObject
{
    Q_OBJECT

public:
    explicit PropagationSettingsStore(QSettings *settings = nullptr,
                                      QObject *parent = nullptr);
    ~PropagationSettingsStore() override;

    static PropagationSettingsStore *instance();
    static PropagationSettings defaults();

    static QString clearanceKey();
    static QString resolutionKey();
    static QString azimuthStepKey();
    static QString convergenceKey();
    static QString rangeKey();
    static QString baseHeightKey();
    static QString toleranceKey();
    static QString minimumAltitudeKey();
    static QString maximumAltitudeKey();
    static QString elevationMapKey();
    static QString terrainMapKey();
    static QString rfMapKey();
    static QString homeDistanceKey();
    static QString droneDistanceKey();
    static QString manualAltitudeRangeKey();
    static QString showScaleKey();

    PropagationSettings settings() const { return m_values; }
    PropagationSettings load() const { return m_values; }
    QString lastError() const { return m_lastError; }

    bool setSettings(const PropagationSettings &settings);
    bool save(const PropagationSettings &settings)
    {
        return setSettings(settings);
    }

public slots:
    void reload();

signals:
    void settingsChanged(const PropagationSettings &settings);
    void saveFailed(const QString &description);

private:
    PropagationSettings readSettings() const;
    double readDouble(const QString &key, double fallback) const;
    bool readBool(const QString &key, bool fallback) const;
    static QString invariant(double value);

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    PropagationSettings m_values;
    QString m_lastError;
};

#endif // PROPAGATIONSETTINGSSTORE_H
