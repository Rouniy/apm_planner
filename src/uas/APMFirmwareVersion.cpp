/****************************************************************************
 *
 *   (c) 2009-2016 APMPLANNER PROJECT <http://www.qgroundcontrol.org>
 *
 * APM Planner is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


/// @file
/// @author Bill Bonney <bill@communistech.com>
/// @author Don Gagne <don@thegagnes.com>
///

#include "APMFirmwareVersion.h"
#include <QRegularExpression>

namespace {
const QRegularExpression kVersionExpression(QStringLiteral(
    "^(APM:Copter|APM:Plane|APM:Rover|APM:Sub|ArduCopter|ArduPlane|"
    "ArduRover|ArduSub) +[vV](\\d+)\\.(\\d+)\\.(\\d+)"));
}

APMFirmwareVersion::APMFirmwareVersion()
    : _major(0), _minor(0), _patch(0), _releaseType(-1)
{
}

APMFirmwareVersion::APMFirmwareVersion(const QString &versionText):
    _major(0), _minor(0), _patch(0), _releaseType(-1)
{
    parseVersion(versionText);
}

bool APMFirmwareVersion::isValid() const
{
    return !_versionString.isEmpty();
}

bool APMFirmwareVersion::isBeta() const
{
    if (_releaseType >= 0) {
        return _releaseType >= 64 && _releaseType < 255;
    }
    return _versionString.contains(QStringLiteral("rc"), Qt::CaseInsensitive)
        || _versionString.contains(QStringLiteral("beta"), Qt::CaseInsensitive)
        || _versionString.contains(QStringLiteral("alpha"), Qt::CaseInsensitive);
}

bool APMFirmwareVersion::isDev() const
{
    if (_releaseType >= 0) {
        return _releaseType < 64;
    }
    return _versionString.contains(QStringLiteral("dev"), Qt::CaseInsensitive);
}

bool APMFirmwareVersion::isOfficial() const
{
    // A STATUSTEXT suffix is not authoritative: custom builds may omit a
    // pre-release marker. Only the packed MAVLink release type can prove that
    // a version matches an official stable artifact.
    return isValid() && _releaseType == 255;
}

bool APMFirmwareVersion::operator <(const APMFirmwareVersion& other) const
{
    int myVersion = _major << 16 | _minor << 8 | _patch ;
    int otherVersion = other.majorNumber() << 16 | other.minorNumber() << 8 | other.patchNumber();
    return myVersion < otherVersion;
}

void APMFirmwareVersion::parseVersion(const QString &versionText)
{
    const QRegularExpressionMatch match = kVersionExpression.match(
        versionText.trimmed());
    if (!match.hasMatch()) {
        return;
    }

    bool majorOk = false;
    bool minorOk = false;
    bool patchOk = false;
    const uint major = match.captured(2).toUInt(&majorOk);
    const uint minor = match.captured(3).toUInt(&minorOk);
    const uint patch = match.captured(4).toUInt(&patchOk);
    if (!majorOk || !minorOk || !patchOk
        || major > 255 || minor > 255 || patch > 255) {
        return;
    }
    _vehicleType = match.captured(1);
    if (_releaseType >= 0) {
        return;
    }
    _versionString = versionText;
    _major = int(major);
    _minor = int(minor);
    _patch = int(patch);
    _releaseType = -1;
    _flightCustomVersion.clear();
}

void APMFirmwareVersion::parseFlightSwVersion(
    quint32 flightSwVersion, const QByteArray &flightCustomVersion,
    const QString &vehicleType)
{
    if (flightSwVersion == 0) {
        return;
    }
    _major = int((flightSwVersion >> 24) & 0xff);
    _minor = int((flightSwVersion >> 16) & 0xff);
    _patch = int((flightSwVersion >> 8) & 0xff);
    _releaseType = int(flightSwVersion & 0xff);
    _flightCustomVersion = flightCustomVersion.left(8);
    if (!vehicleType.trimmed().isEmpty()) {
        _vehicleType = vehicleType.trimmed();
    }
    _versionString = QStringLiteral("%1.%2.%3")
        .arg(_major).arg(_minor).arg(_patch);
}
