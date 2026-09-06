#ifndef CONFIGGPSINJECTVIEWMODEL_H
#define CONFIGGPSINJECTVIEWMODEL_H

#include "comm/GpsCorrectionSource.h"
#include "comm/UbloxBaseStationService.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QSettings;
class QTimer;

struct BasePosRow
{
    QString Lat;
    QString Long;
    QString Alt;
    QString Name;
};

inline bool operator==(const BasePosRow &left, const BasePosRow &right)
{
    return left.Lat == right.Lat && left.Long == right.Long
        && left.Alt == right.Alt && left.Name == right.Name;
}

class ConfigGpsInjectViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigGpsInjectViewModel(QObject *parent = nullptr);
    ConfigGpsInjectViewModel(GpsCorrectionSource *source,
                             QSettings *settings,
                             QObject *parent = nullptr);
    ~ConfigGpsInjectViewModel() override;

    static QString NtripOption();

    QString Title() const;
    QStringList Ports() const { return m_ports; }
    QString SelectedPort() const { return m_selectedPort; }
    QStringList BaudRates() const { return m_baudRates; }
    QString SelectedBaud() const { return m_selectedBaud; }
    QStringList ReceiverTypes() const { return m_receiverTypes; }
    QString SelectedReceiverType() const { return m_selectedReceiverType; }

    QString Host() const { return m_host; }
    int Port() const { return m_port; }
    QString Mount() const { return m_mount; }
    QString Username() const { return m_username; }
    QString Password() const { return m_password; }
    bool NtripV1() const { return m_ntripV1; }
    bool SendGga() const { return m_sendGga; }
    bool AutoConfig() const { return m_autoConfig; }

    bool M8p130Plus() const { return m_m8p130Plus; }
    QString SurveyInAcc() const { return m_surveyInAcc; }
    QString SurveyInTime() const { return m_surveyInTime; }
    QString SurveyInStatus() const { return m_surveyInStatus; }
    bool SurveyInValid() const { return m_surveyInValid; }

    QStringList SeptentrioRtcmLevels() const
    {
        return m_septentrioRtcmLevels;
    }
    QString SelectedSeptentrioRtcmLevel() const
    {
        return m_selectedSeptentrioRtcmLevel;
    }
    bool SeptentrioGps() const { return m_septentrioGps; }
    bool SeptentrioGlonass() const { return m_septentrioGlonass; }
    bool SeptentrioGalileo() const { return m_septentrioGalileo; }
    bool SeptentrioBeidou() const { return m_septentrioBeidou; }
    QString SeptentrioRtcmInterval() const
    {
        return m_septentrioRtcmInterval;
    }
    bool SeptentrioFixedPosition() const
    {
        return m_septentrioFixedPosition;
    }
    QString SeptentrioLat() const { return m_septentrioLat; }
    QString SeptentrioLng() const { return m_septentrioLng; }
    QString SeptentrioAlt() const { return m_septentrioAlt; }

    bool IsNtrip() const { return m_selectedPort == NtripOption(); }
    bool IsSerial() const { return !IsNtrip(); }
    bool IsSeptentrio() const
    {
        return m_autoConfig
            && m_selectedReceiverType == QLatin1String("Septentrio");
    }
    bool Active() const { return m_active; }
    bool Connected() const { return m_connected; }
    QString ConnectLabel() const;
    bool CanEditSource() const;
    bool CanToggleConnect() const;
    bool ReceiverBusy() const;
    bool CanRestartSurveyIn() const;
    bool CanUseBasePosition() const;
    quint64 PendingUbloxAuthorization() const noexcept
    {
        return m_pendingUbloxAuthorization;
    }

    QString Status() const { return m_status; }
    QString Injected() const { return m_injected; }
    QString InputRate() const { return m_inputRate; }
    QString OutputRate() const { return m_outputRate; }
    QString MessagesSeen() const { return m_messagesSeen; }
    QString RtcmBasePos() const { return m_rtcmBasePos; }

    bool BaseFresh() const;
    bool GpsFresh() const;
    bool GlonassFresh() const;
    bool BeidouFresh() const;
    bool GalileoFresh() const;
    quint64 RtcmMessageCount(quint16 messageId) const;

    QList<BasePosRow> BasePositions() const { return m_basePositions; }
    bool HasCurrentBasePosition() const { return m_hasCurrentBasePosition; }
    BasePosRow CurrentBasePosition() const { return m_currentBasePosition; }
    bool HasActiveBasePosition() const { return m_hasActiveBasePosition; }
    BasePosRow ActiveBasePosition() const { return m_activeBasePosition; }

    GpsCorrectionSource *Source() const { return m_source.data(); }
    UbloxBaseStationService *UbloxService() const
    {
        return m_ubloxService.data();
    }

public slots:
    void SetSelectedPort(const QString &value);
    void SetSelectedBaud(const QString &value);
    void SetSelectedReceiverType(const QString &value);
    void SetHost(const QString &value);
    void SetPort(int value);
    void SetMount(const QString &value);
    void SetUsername(const QString &value);
    void SetPassword(const QString &value);
    void SetNtripV1(bool value);
    void SetSendGga(bool value);
    void SetAutoConfig(bool value);
    void SetM8p130Plus(bool value);
    void SetSurveyInAcc(const QString &value);
    void SetSurveyInTime(const QString &value);
    void SetSelectedSeptentrioRtcmLevel(const QString &value);
    void SetSeptentrioGps(bool value);
    void SetSeptentrioGlonass(bool value);
    void SetSeptentrioGalileo(bool value);
    void SetSeptentrioBeidou(bool value);
    void SetSeptentrioRtcmInterval(const QString &value);
    void SetSeptentrioFixedPosition(bool value);
    void SetSeptentrioLat(const QString &value);
    void SetSeptentrioLng(const QString &value);
    void SetSeptentrioAlt(const QString &value);

    void SetVehiclePosition(double latitude, double longitude,
                            double altitudeMsl, bool valid = true);
    void SetCurrentBasePosition(double latitude, double longitude,
                                double altitude, bool valid = true);
    void SetSurveyInStatus(const QString &status, bool valid);
    void SetReceiverStatus(const QString &status);

    bool RefreshPorts();
    bool ToggleConnect();
    bool ResolveUbloxAuthorization(quint64 authorizationId,
                                   bool accepted);
    void UpdateStats();
    void ReportInjectionResult(qint64 bytes, bool accepted,
                               const QString &reason = QString());

    bool RestartSurveyIn();
    bool SaveCurrentPosition();
    bool UseBasePos(const BasePosRow &row);
    bool DeleteBasePos(const BasePosRow &row);
    bool ApplySeptentrioRtcm();
    bool ApplySeptentrioPosition();

signals:
    void propertiesChanged();
    void stateChanged();
    void statusChanged(const QString &status);
    void basePositionsChanged();
    void rtcmDataReady(const QByteArray &frame);

    void ubloxAuthorizationRequested(quint64 authorizationId,
                                     const QString &confirmationText);

    // Compatibility notifications for receiver integrations not owned by
    // this model. u-blox actions above use UbloxBaseStationService directly;
    // Septentrio remains explicitly unavailable in this slice.
    void ubloxConfigureRequested(bool m8p130Plus);
    void ubloxSurveyInRequested(int durationSeconds,
                                double accuracyMeters,
                                bool m8p130Plus);
    void ubloxBasePositionRequested(double latitude, double longitude,
                                    double altitude,
                                    int surveyInDurationSeconds,
                                    double surveyInAccuracyMeters);
    void septentrioConfigureRequested();
    void septentrioPositionRequested(bool fixedPosition,
                                     double latitude, double longitude,
                                     double altitude);
    void septentrioRtcmRequested(const QString &level,
                                 double intervalSeconds,
                                 bool gps, bool glonass,
                                 bool galileo, bool beidou);

private slots:
    void sourceStateChanged(bool active, bool connected);
    void sourceStatusChanged(const QString &status);
    void sourceInputBytes(qint64 bytes);
    void sourceRtcmFrame(const QByteArray &frame, quint16 messageId);
    void ubloxStateChanged();
    void ubloxOperationFinished(
        const UbloxBaseStationService::Report &report);

private:
    void initialize();
    void loadSettings();
    void loadBasePositions();
    void saveConnectSettings();
    void saveSeptentrioSettings();
    void saveBasePositions();
    void saveActiveBasePosition();
    void resetStatistics();
    void markFreshness(quint16 messageId, qint64 now);
    bool beginSource(const GpsCorrectionSourceSettings &settings);
    void requestAutoConfiguration();
    bool validateSurveySettings(quint32 *durationSeconds,
                                double *accuracyMeters,
                                QString *error) const;
    void setSurveyInPresentation(const QString &baseStatus, bool valid);
    void rebuildSurveyInPresentation();
    void refreshUbloxObservations();
    QString connectedStatus() const;
    GpsCorrectionSourceSettings sourceSettings() const;
    void setStatus(const QString &status);
    void notifyProperties();

    static bool parsePosition(const BasePosRow &row,
                              double *latitude, double *longitude,
                              double *altitude);
    static int parseInt(const QString &value);
    static double parseDouble(const QString &value);
    static QString sanitize(const QString &value);
    static bool validateFrame(const QByteArray &frame, quint16 messageId);
    static bool isFresh(qint64 seenAt, int timeoutSeconds);

    QPointer<GpsCorrectionSource> m_source;
    QPointer<UbloxBaseStationService> m_ubloxService;
    QSettings *m_settings = nullptr;
    QTimer *m_statisticsTimer = nullptr;
    bool m_ownsSettings = false;

    QStringList m_ports;
    QString m_selectedPort = QStringLiteral("NTRIP");
    QStringList m_baudRates;
    QString m_selectedBaud = QStringLiteral("115200");
    QStringList m_receiverTypes;
    QString m_selectedReceiverType = QStringLiteral("UBlox M8P/F9P");

    QString m_host;
    int m_port = 2101;
    QString m_mount;
    QString m_username;
    QString m_password;
    bool m_ntripV1 = false;
    bool m_sendGga = true;
    bool m_autoConfig = false;

    bool m_m8p130Plus = true;
    QString m_surveyInAcc = QStringLiteral("2");
    QString m_surveyInTime = QStringLiteral("60");
    QString m_surveyInBaseStatus = QStringLiteral("Survey In: not started");
    QString m_surveyInStatus = QStringLiteral("Survey In: not started");
    bool m_surveyInValid = false;

    QStringList m_septentrioRtcmLevels;
    QString m_selectedSeptentrioRtcmLevel = QStringLiteral("Basic");
    bool m_septentrioGps = true;
    bool m_septentrioGlonass = true;
    bool m_septentrioGalileo = true;
    bool m_septentrioBeidou = true;
    QString m_septentrioRtcmInterval = QStringLiteral("1.0");
    bool m_septentrioFixedPosition = false;
    QString m_septentrioLat = QStringLiteral("0");
    QString m_septentrioLng = QStringLiteral("0");
    QString m_septentrioAlt = QStringLiteral("0");

    bool m_active = false;
    bool m_connected = false;
    bool m_receiverActionsRequested = false;
    quint64 m_nextUbloxAuthorization = 1;
    quint64 m_pendingUbloxAuthorization = 0;
    quint64 m_ownedUbloxOperation = 0;
    GpsCorrectionSourceSettings m_pendingSourceSettings;
    bool m_pendingM8p130Plus = true;
    bool m_pendingFixedPosition = false;
    UbloxBaseStationService::FixedPosition m_pendingFixed;
    bool m_ubloxConnectAuthorized = false;
    QString m_status = QStringLiteral(
        "Select a serial port or NTRIP and press Connect.");
    QString m_injected = QStringLiteral("0 bytes");
    QString m_inputRate = QStringLiteral("0 bps");
    QString m_outputRate = QStringLiteral("0 bps");
    QString m_messagesSeen;
    QString m_rtcmBasePos;

    qint64 m_inputBytesThisSecond = 0;
    qint64 m_outputBytesThisSecond = 0;
    qint64 m_injectedBytes = 0;
    QHash<quint16, quint64> m_messageCounts;
    qint64 m_baseSeenAt = 0;
    qint64 m_gpsSeenAt = 0;
    qint64 m_glonassSeenAt = 0;
    qint64 m_beidouSeenAt = 0;
    qint64 m_galileoSeenAt = 0;

    bool m_vehiclePositionValid = false;
    double m_vehicleLatitude = 0.0;
    double m_vehicleLongitude = 0.0;
    double m_vehicleAltitudeMsl = 0.0;

    QList<BasePosRow> m_basePositions;
    bool m_hasCurrentBasePosition = false;
    BasePosRow m_currentBasePosition;
    bool m_hasActiveBasePosition = false;
    BasePosRow m_activeBasePosition;
};

#endif // CONFIGGPSINJECTVIEWMODEL_H
