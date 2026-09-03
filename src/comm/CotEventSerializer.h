#ifndef COTEVENTSERIALIZER_H
#define COTEVENTSERIALIZER_H

#include <QDateTime>
#include <QString>

/**
 * Canonical navigation values used by Cursor-on-Target output.
 *
 * These are deliberately independent of display-unit preferences. Mission
 * Planner labels altitudeAmsl as CoT "hae" and transmits speed in metres per
 * second; the Qt port retains that wire behavior without converting to feet,
 * knots or another presentation unit.
 */
struct CotNavigationState
{
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    double altitudeAmslMetres = 0.0;
    double courseDegrees = 0.0;
    double speedMetresPerSecond = 0.0;
};

/** Per-system identity row selected by the owning CoT service/view model. */
struct CotIdentityOverride
{
    QString uid;
    bool includeTakv = false;
    QString contactCallsign;
    QString contactEndpoint;
    QString vmf;
};

/** Live serializer settings from Mission Planner 10's CoT window. */
struct CotEventSettings
{
    QString eventType = QStringLiteral("a-f-A-M-F-Q");
    QString uidPrefix = QStringLiteral("MissionPlanner");
    QString callsign;
    bool indentXml = false;
    bool advancedIdentityFields = true;
};

/**
 * Pure Mission Planner 10 compatible CoT 2.0 XML serializer.
 *
 * No system clock, settings store, MAVLink state or transport is consulted.
 * The caller chooses the matching identity row (if any) and supplies the
 * timestamp, making output deterministic and independently testable.
 */
class CotEventSerializer final
{
public:
    /**
     * A present identity row always owns the UID, including an empty UID.
     * With no row, MP10 falls back to "prefix-system-component".
     */
    static QString ResolveUid(const CotEventSettings &settings,
                              int systemId, int componentId,
                              const CotIdentityOverride *identity = nullptr);

    /**
     * Validate every value that would be emitted by Serialize(). On failure,
     * errorMessage receives a stable, human-readable reason when supplied.
     */
    static bool Validate(const CotEventSettings &settings,
                         int systemId, int componentId,
                         const CotNavigationState &navigation,
                         const QDateTime &timestampUtc,
                         QString *errorMessage = nullptr,
                         const CotIdentityOverride *identity = nullptr);

    /**
     * Validate and serialize one event. xml is cleared on failure and
     * errorMessage receives the reason when supplied.
     */
    static bool TrySerialize(const CotEventSettings &settings,
                             int systemId, int componentId,
                             const CotNavigationState &navigation,
                             const QDateTime &timestampUtc,
                             QString *xml,
                             QString *errorMessage = nullptr,
                             const CotIdentityOverride *identity = nullptr);

    /**
     * Serialize one event without an XML declaration or trailing newline.
     * timestampUtc is converted to UTC before time/start/stale are formatted.
     * Returns a null QString when validation fails; callers that need the
     * failure reason should use TrySerialize() or Validate().
     */
    static QString Serialize(const CotEventSettings &settings,
                             int systemId, int componentId,
                             const CotNavigationState &navigation,
                             const QDateTime &timestampUtc,
                             const CotIdentityOverride *identity = nullptr);
};

#endif // COTEVENTSERIALIZER_H
