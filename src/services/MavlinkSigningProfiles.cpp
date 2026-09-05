#include "MavlinkSigningProfiles.h"

#include <QRegularExpression>
#include <QSettings>
#include <QUuid>

#include <utility>

namespace {

const QString ProfilesGroup = QStringLiteral("MAVLinkSigning/Profiles");
const QString FingerprintKey = QStringLiteral("fingerprint");
constexpr int FingerprintBytes = 32;

MavlinkSigningProfiles::Policy failed(const QString &message)
{
    MavlinkSigningProfiles::Policy policy;
    policy.required = true;
    policy.error = message;
    return policy;
}

QString settingsError(QSettings::Status status)
{
    switch (status) {
    case QSettings::AccessError:
        return QStringLiteral("MAVLink signing profile settings are not accessible.");
    case QSettings::FormatError:
        return QStringLiteral("MAVLink signing profile settings are malformed.");
    case QSettings::NoError:
        break;
    }
    return QStringLiteral("MAVLink signing profile settings are unavailable.");
}

QString profileKey(const QString &profileId)
{
    return ProfilesGroup + QLatin1Char('/') + profileId
        + QLatin1Char('/') + FingerprintKey;
}

bool decodeFingerprint(const QVariant &stored, QByteArray *fingerprint)
{
    if (fingerprint) fingerprint->clear();
    const QString encoded = stored.toString();
    static const QRegularExpression Hex64(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!Hex64.match(encoded).hasMatch()) return false;
    const QByteArray decoded = QByteArray::fromHex(encoded.toLatin1());
    if (decoded.size() != FingerprintBytes
        || QString::fromLatin1(decoded.toHex()) != encoded) {
        return false;
    }
    if (fingerprint) *fingerprint = decoded;
    return true;
}

} // namespace

bool MavlinkSigningProfiles::validProfileId(const QString &profileId)
{
    static const QRegularExpression Uuid(
        QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    if (Uuid.match(profileId).hasMatch()) {
        const QUuid parsed(profileId);
        return parsed.toString(QUuid::WithoutBraces).toLower() == profileId;
    }

    static const QRegularExpression StartupUdp(
        QStringLiteral("^startup-udp-([1-9][0-9]{0,4})$"));
    const QRegularExpressionMatch match = StartupUdp.match(profileId);
    if (!match.hasMatch()) return false;
    bool converted = false;
    const int port = match.captured(1).toInt(&converted);
    return converted && port >= 1 && port <= 65535
        && match.captured(1) == QString::number(port);
}

QString MavlinkSigningProfiles::newProfileId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

MavlinkSigningProfiles::Policy MavlinkSigningProfiles::load(
    QSettings &settings, const QString &profileId, bool requiredHint)
{
    if (!settings.group().isEmpty()) {
        return failed(QStringLiteral(
            "MAVLink signing profiles must be read from the QSettings root group."));
    }
    if (!validProfileId(profileId)) {
        return failed(QStringLiteral(
            "The MAVLink signing connection profile ID is invalid."));
    }
    if (settings.status() != QSettings::NoError) {
        return failed(settingsError(settings.status()));
    }

    settings.beginGroup(ProfilesGroup);
    const bool groupPresent = settings.childGroups().contains(profileId);
    settings.endGroup();
    const QString key = profileKey(profileId);
    const bool recordPresent = settings.contains(key);
    if (settings.status() != QSettings::NoError) {
        return failed(settingsError(settings.status()));
    }
    if (!groupPresent && !recordPresent) {
        if (requiredHint) {
            return failed(QStringLiteral(
                "Required MAVLink signing profile metadata is missing."));
        }
        return {};
    }
    if (!recordPresent) {
        return failed(QStringLiteral(
            "Required MAVLink signing profile fingerprint is missing."));
    }

    const QVariant stored = settings.value(key);
    if (settings.status() != QSettings::NoError) {
        return failed(settingsError(settings.status()));
    }
    QByteArray fingerprint;
    if (!decodeFingerprint(stored, &fingerprint)) {
        return failed(QStringLiteral(
            "Required MAVLink signing profile fingerprint is malformed."));
    }

    Policy policy;
    policy.required = true;
    policy.fingerprint = std::move(fingerprint);
    return policy;
}

bool MavlinkSigningProfiles::saveRequired(
    QSettings &settings, const QString &profileId,
    const QByteArray &fingerprint, QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!settings.group().isEmpty()) {
        return reject(QStringLiteral(
            "MAVLink signing profiles must be written from the QSettings root group."));
    }
    if (!validProfileId(profileId)) {
        return reject(QStringLiteral(
            "The MAVLink signing connection profile ID is invalid."));
    }
    if (fingerprint.size() != FingerprintBytes) {
        return reject(QStringLiteral(
            "Signing-key fingerprint must contain 32 bytes."));
    }
    if (settings.status() != QSettings::NoError) {
        return reject(settingsError(settings.status()));
    }

    // Pull in cooperative external changes before deciding whether this is a
    // first publication or an idempotent save. QSettings supplies the atomic
    // publication mechanism; this codec does not claim rollback protection.
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return reject(settingsError(settings.status()));
    }
    const Policy existing = load(settings, profileId);
    if (existing.required) {
        if (!existing.error.isEmpty()) return reject(existing.error);
        if (existing.fingerprint != fingerprint) {
            return reject(QStringLiteral(
                "The MAVLink signing profile already requires a different key."));
        }
    } else {
        settings.setValue(
            profileKey(profileId), QString::fromLatin1(fingerprint.toHex()));
    }

    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return reject(settingsError(settings.status()));
    }
    const Policy verified = load(settings, profileId, true);
    if (!verified.required || !verified.error.isEmpty()
        || verified.fingerprint != fingerprint) {
        return reject(verified.error.isEmpty()
            ? QStringLiteral(
                "Published MAVLink signing profile metadata could not be verified.")
            : verified.error);
    }
    return true;
}
