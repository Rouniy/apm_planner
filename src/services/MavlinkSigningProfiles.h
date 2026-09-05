#ifndef MAVLINKSIGNINGPROFILES_H
#define MAVLINKSIGNINGPROFILES_H

#include <QByteArray>
#include <QString>

class QSettings;

/** Strict, secret-free MAVLink signing policy metadata in QSettings. */
class MavlinkSigningProfiles final
{
public:
    struct Policy {
        bool required = false;
        bool provisioningUnconfirmed = false;
        QByteArray fingerprint;
        QString error;
    };

    static bool validProfileId(const QString &profileId);
    static QString newProfileId();
    static Policy load(QSettings &settings, const QString &profileId,
                       bool requiredHint = false);
    static bool saveRequired(QSettings &settings, const QString &profileId,
                             const QByteArray &fingerprint,
                             QString *error = nullptr);
    /**
     * Atomically publishes the first required fingerprint together with a
     * permanent no-ACK provisioning marker. Any pre-existing required or
     * malformed record is refused; changing and disabling keys are separate
     * future workflows.
     */
    static bool beginInitialProvisioning(
        QSettings &settings, const QString &profileId,
        const QByteArray &fingerprint, QString *error = nullptr);

private:
    MavlinkSigningProfiles() = delete;
};

#endif // MAVLINKSIGNINGPROFILES_H
