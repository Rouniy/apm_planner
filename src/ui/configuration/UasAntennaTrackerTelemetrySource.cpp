#include "UasAntennaTrackerTelemetrySource.h"

#include "LinkManager.h"
#include "SerialLinkInterface.h"
#include "UASInterface.h"
#include "UASManager.h"

#include <QFileInfo>
#include <QSerialPortInfo>

#include <cmath>

namespace {

QString portIdentity(const QString &portName)
{
    const QSerialPortInfo info(portName);
    QString candidate = info.systemLocation();
    if (candidate.isEmpty()) {
        candidate = portName;
    }
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    return canonical.isEmpty() ? candidate : canonical;
}

bool knownPosition(double latitude, double longitude)
{
    // MP10 CurrentState treats lat == 0 && lng == 0 (and TrackerLocation.Lat == 0)
    // as "no position".
    return std::isfinite(latitude) && std::isfinite(longitude)
        && !(latitude == 0.0 && longitude == 0.0);
}

} // namespace

UasAntennaTrackerTelemetrySource::UasAntennaTrackerTelemetrySource(QObject *parent)
    : AntennaTrackerTelemetrySource(parent)
{
    UASManager *manager = UASManager::instance();
    connect(manager, QOverload<UASInterface *>::of(&UASManager::activeUASSet), this,
            [this](UASInterface *) { emit telemetrySourceChanged(); });
    connect(manager, &UASManager::homePositionChanged, this,
            [this](double, double, double) { emit telemetrySourceChanged(); });
}

AntennaTrackerVehicleFix UasAntennaTrackerTelemetrySource::vehicleFix() const
{
    AntennaTrackerVehicleFix fix;
    UASInterface *uas = UASManager::instance()->getActiveUAS();
    if (!uas) {
        return fix;
    }
    const double latitude = uas->getLatitude();
    const double longitude = uas->getLongitude();
    const double altitude = uas->getAltitudeAMSL();
    if (!knownPosition(latitude, longitude) || !std::isfinite(altitude)) {
        return fix;
    }
    fix.valid = true;
    fix.latitude = latitude;
    fix.longitude = longitude;
    fix.altitudeAmsl = altitude;
    return fix;
}

AntennaTrackerPosition UasAntennaTrackerTelemetrySource::trackerLocation(bool *valid) const
{
    if (m_trackerHomeSet) {
        if (valid) {
            *valid = true;
        }
        return m_trackerHome;
    }
    const UASManager *manager = UASManager::instance();
    const AntennaTrackerPosition home(manager->getHomeLatitude(), manager->getHomeLongitude(),
                                      manager->getHomeAltitude());
    const bool known = home.isValid() && knownPosition(home.latitude, home.longitude);
    if (valid) {
        *valid = known;
    }
    return known ? home : AntennaTrackerPosition();
}

double UasAntennaTrackerTelemetrySource::localSnrDb() const
{
    // No structured RADIO_STATUS consumer exists yet (see the c19 plan, R5).
    return 0.0;
}

void UasAntennaTrackerTelemetrySource::setTrackerHome(const AntennaTrackerPosition &position)
{
    // MP10 accepts the explicit location only with a non-zero longitude.
    if (!position.isValid() || position.longitude == 0.0) {
        clearTrackerHome();
        return;
    }
    m_trackerHome = position;
    m_trackerHomeSet = true;
    emit telemetrySourceChanged();
}

void UasAntennaTrackerTelemetrySource::clearTrackerHome()
{
    if (!m_trackerHomeSet) {
        return;
    }
    m_trackerHomeSet = false;
    m_trackerHome = AntennaTrackerPosition();
    emit telemetrySourceChanged();
}

bool UasAntennaTrackerTelemetrySource::PortOwnedByVehicleLink(const QString &portName)
{
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return false;
    }
    const QString wanted = portIdentity(portName);
    for (int linkId : manager->getLinks()) {
        if (!manager->getLinkConnected(linkId)) {
            continue;
        }
        auto *serial = qobject_cast<SerialLinkInterface *>(manager->getLink(linkId));
        if (serial && portIdentity(serial->getPortName()) == wanted) {
            return true;
        }
    }
    return false;
}
