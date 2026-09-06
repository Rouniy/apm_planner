#include "GuidedAltitudeStore.h"

#include <cmath>
#include <limits>

namespace {
bool reject(QString *error, const char *reason)
{
    if (error) *error = QString::fromLatin1(reason);
    return false;
}
bool sameTarget(const VehicleTargetLease &a, const VehicleTargetLease &b)
{
    return a.endpoint.sameIdentity(b.endpoint) && a.generation == b.generation;
}
bool sameContext(const GuidedAltitudeStore::Context &a, const GuidedAltitudeStore::Context &b)
{
    // Endpoint names are presentation metadata, not an identity or an intent.
    return sameTarget(a.target, b.target) && a.vehicle.sameInstance(b.vehicle)
        && a.revision == b.revision && a.altitudeSet == b.altitudeSet
        && a.altitudeM == b.altitudeM && a.frame == b.frame
        && a.pointSet == b.pointSet && a.latitude == b.latitude && a.longitude == b.longitude;
}
bool allowedFrame(MAV_FRAME frame)
{
    return frame == MAV_FRAME_GLOBAL || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
        || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT;
}
void advance(quint64 &value)
{
    // Exhaustion is terminal for admission, not an ABA-causing wraparound.
    if (value != std::numeric_limits<quint64>::max()) ++value;
}
}

GuidedAltitudeStore::GuidedAltitudeStore(VehicleTargetManager *targets,
    SwarmTelemetryRegistry *registry, QObject *parent)
    : QObject(parent), m_targets(targets), m_registry(registry)
{
    if (targets) {
        connect(targets, &VehicleTargetManager::targetGenerationChanged,
            this, [this] { notifyInvalidated(); });
        connect(targets, &VehicleTargetManager::targetGenerationSettled, this, [this] {
            advance(m_mutation);
            emit changed();
        });
        connect(targets, &QObject::destroyed, this, [this] {
            m_targets.clear(); m_entries.clear(); notifyInvalidated();
        });
    }
    if (registry) {
        connect(registry, &SwarmTelemetryRegistry::endpointRetired, this,
            [this](const SwarmVehicleInstanceLease &vehicle) { retire(vehicle); });
        connect(registry, &SwarmTelemetryRegistry::endpointActivated, this,
            [this](const SwarmTelemetrySnapshot &activated) {
                SwarmTelemetrySnapshot live;
                if (!m_registry || !m_registry->snapshotForLease(activated.lease, &live)) return;
                if (m_targets && m_targets->acquireTarget().endpoint.sameIdentity(activated.lease.endpoint))
                    notifyInvalidated();
                else { advance(m_mutation); emit changed(); }
            });
        connect(registry, &QObject::destroyed, this, [this] {
            m_registry.clear(); m_entries.clear(); notifyInvalidated();
        });
    }
}

void GuidedAltitudeStore::notifyInvalidated()
{
    advance(m_mutation);
    const quint64 mutation = m_mutation;
    QPointer<GuidedAltitudeStore> guard(this);
    emit contextInvalidated();
    if (guard && mutation == m_mutation) emit changed();
}

void GuidedAltitudeStore::retire(const SwarmVehicleInstanceLease &vehicle)
{
    const auto entry = m_entries.constFind(vehicle.endpoint);
    const bool removed = entry != m_entries.constEnd() && entry->vehicle.sameInstance(vehicle);
    if (removed) m_entries.remove(vehicle.endpoint);
    const bool selected = m_targets
        && m_targets->acquireTarget().endpoint.sameIdentity(vehicle.endpoint);
    // A delayed old retirement must not invalidate/remove a rediscovered
    // replacement. Registry event batches can already contain that successor.
    const bool absent = !m_registry || !m_registry->endpoints().contains(vehicle.endpoint);
    if (selected && absent) notifyInvalidated();
    else if (removed) { advance(m_mutation); emit changed(); }
}

bool GuidedAltitudeStore::prepareCurrent(Context *context, QString *error) const
{
    if (context) *context = Context();
    if (error) error->clear();
    if (!context) return reject(error, "A guided context output is required.");
    if (!m_targets || !m_registry)
        return reject(error, "Exact target services are unavailable.");
    if (m_mutation == std::numeric_limits<quint64>::max())
        return reject(error, "Guided context revision space is exhausted.");
    if (!m_targets->isTargetGenerationSettled())
        return reject(error, "The selected target is still changing.");
    const auto target = m_targets->acquireTarget();
    if (!target.isValid()) return reject(error, "Select an exact vehicle target first.");

    QPointer<const GuidedAltitudeStore> guard(this);
    const auto targets = m_targets;
    const auto registry = m_registry;
    const quint64 mutation = m_mutation;
    // acquireGroup takes its injectable clock reading before looking up map
    // records, unlike the single-endpoint helper. No iterator crosses a callback.
    const auto group = registry->acquireGroup({target.endpoint}, 3000);
    if (!guard) return false;
    if (!targets || !registry || targets != m_targets || registry != m_registry
        || mutation != m_mutation || !targets->isTargetGenerationSettled()
        || !sameTarget(target, targets->acquireTarget()))
        return reject(error, "The exact target changed while acquiring guided intent.");
    if (group.members.size() != 1 || !group.members.first().isValid())
        return reject(error, "The selected vehicle has no fresh autopilot heartbeat.");
    const auto vehicle = group.members.first();
    SwarmTelemetrySnapshot snapshot;
    if (!vehicle.endpoint.sameIdentity(target.endpoint) || !registry->snapshotForLease(vehicle, &snapshot))
        return reject(error, "The selected vehicle instance was retired.");

    Context value;
    const auto entry = m_entries.constFind(vehicle.endpoint);
    if (entry != m_entries.constEnd() && entry->vehicle.sameInstance(vehicle)) value = *entry;
    else if (entry == m_entries.constEnd()
        && m_entries.size() >= SwarmTelemetryRegistry::MaximumVehicleEndpoints)
        return reject(error, "The bounded guided intent store is full.");
    value.target = target;
    value.vehicle = vehicle;
    *context = value;
    return true;
}

bool GuidedAltitudeStore::validate(const Context &context, QString *error) const
{
    const Context expected = context;
    QPointer<const GuidedAltitudeStore> guard(this);
    Context actual;
    if (!prepareCurrent(&actual, error)) return false;
    if (!guard) return false;
    if (!sameContext(expected, actual))
        return reject(error, "The guided intent snapshot is stale or does not match this exact instance.");
    return true;
}

GuidedAltitudeStore::Context GuidedAltitudeStore::current() const
{
    Context value;
    prepareCurrent(&value);
    return value;
}

bool GuidedAltitudeStore::commitAltitude(const Context &context, double metres,
    MAV_FRAME frame, Context *updated, QString *error)
{
    return commit(context, metres, frame, false, 0, 0, updated, error);
}

bool GuidedAltitudeStore::recordTarget(const Context &context, double latitude,
    double longitude, double metres, MAV_FRAME frame, Context *updated, QString *error)
{
    return commit(context, metres, frame, true, latitude, longitude, updated, error);
}

bool GuidedAltitudeStore::commit(const Context &context, double metres, MAV_FRAME frame,
    bool setPoint, double latitude, double longitude, Context *updated, QString *error)
{
    const Context expected = context; // updated may alias the supplied context.
    if (updated) *updated = Context();
    if (error) error->clear();
    if (!std::isfinite(metres) || std::abs(metres) > double(std::numeric_limits<float>::max()))
        return reject(error, "Guided altitude must be finite and representable as a float in metres.");
    const float altitude = float(metres);
    if (metres != 0 && altitude == 0)
        return reject(error, "Guided altitude is too small to represent as a float in metres.");
    if (!allowedFrame(frame)) return reject(error, "Unsupported guided altitude frame.");
    if (setPoint && (!std::isfinite(latitude) || !std::isfinite(longitude)
        || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180))
        return reject(error, "Guided target latitude/longitude is outside its finite geographic range.");
    QPointer<GuidedAltitudeStore> guard(this);
    if (!validate(expected, error) || !guard) return false;
    if (m_nextRevision == std::numeric_limits<quint64>::max())
        return reject(error, "Guided intent revision space is exhausted.");
    Context value = expected;
    value.altitudeSet = true;
    value.altitudeM = altitude;
    value.frame = frame;
    if (setPoint) { value.pointSet = true; value.latitude = latitude; value.longitude = longitude; }
    value.revision = ++m_nextRevision;
    m_entries.insert(value.vehicle.endpoint, value);
    advance(m_mutation);
    // Receipt before notification; the listener may replace intent, retarget,
    // or delete this store. A successful commit is not a later write authority.
    if (updated) *updated = value;
    emit changed();
    return true;
}
