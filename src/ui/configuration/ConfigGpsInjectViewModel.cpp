#include "ConfigGpsInjectViewModel.h"

#include "comm/Rtcm3Parser.h"

#include <QDateTime>
#include <QSettings>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kStatisticsIntervalMs = 1000;
constexpr int kBaseFreshnessSeconds = 20;
constexpr int kConstellationFreshnessSeconds = 5;

const QString kConnectedStatus = QStringLiteral(
    "Connected — receiving RTCM correction data.");
const QString kQueuedStatus = QStringLiteral(
    "Connected — RTCM queued to vehicle.");

bool inRange(quint16 value, quint16 first, quint16 last)
{
    return value >= first && value <= last;
}

QString numberForSetting(double value)
{
    return QString::number(value, 'g', 15);
}
} // namespace

ConfigGpsInjectViewModel::ConfigGpsInjectViewModel(QObject *parent)
    : ConfigGpsInjectViewModel(nullptr, nullptr, parent)
{
}

ConfigGpsInjectViewModel::ConfigGpsInjectViewModel(
    GpsCorrectionSource *source, QSettings *settings, QObject *parent)
    : QObject(parent),
      m_source(source),
      m_settings(settings ? settings : new QSettings),
      m_ownsSettings(!settings)
{
    if (!m_source) {
        m_source = new QtGpsCorrectionSource(this);
    }
    m_ubloxService = new UbloxBaseStationService(m_source, this);
    initialize();
}

ConfigGpsInjectViewModel::~ConfigGpsInjectViewModel()
{
    if (m_statisticsTimer) {
        m_statisticsTimer->stop();
    }
    if (m_ubloxService) {
        disconnect(m_ubloxService, nullptr, this, nullptr);
        m_ubloxService->shutdown();
    }
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
        m_source->stop();
    }
    if (m_ownsSettings) {
        delete m_settings;
        m_settings = nullptr;
    }
}

QString ConfigGpsInjectViewModel::NtripOption()
{
    return QStringLiteral("NTRIP");
}

QString ConfigGpsInjectViewModel::Title() const
{
    return tr("RTK/GPS Inject");
}

QString ConfigGpsInjectViewModel::ConnectLabel() const
{
    if (m_pendingUbloxAuthorization != 0) {
        return tr("Awaiting Confirmation");
    }
    return m_active ? tr("Disconnect") : tr("Connect");
}

bool ConfigGpsInjectViewModel::CanEditSource() const
{
    return !m_active && m_pendingUbloxAuthorization == 0
        && !(m_ubloxService && m_ubloxService->busy());
}

bool ConfigGpsInjectViewModel::CanToggleConnect() const
{
    if (m_active) {
        return true;
    }
    return m_pendingUbloxAuthorization == 0
        && !(m_ubloxService && m_ubloxService->busy());
}

bool ConfigGpsInjectViewModel::ReceiverBusy() const
{
    return m_pendingUbloxAuthorization != 0
        || (m_ubloxService && m_ubloxService->busy());
}

bool ConfigGpsInjectViewModel::CanRestartSurveyIn() const
{
    return m_connected && IsSerial()
        && m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")
        && m_ubloxService && m_ubloxService->available()
        && !ReceiverBusy();
}

bool ConfigGpsInjectViewModel::CanUseBasePosition() const
{
    return !ReceiverBusy();
}

void ConfigGpsInjectViewModel::initialize()
{
    m_baudRates = QStringList({
        QStringLiteral("4800"), QStringLiteral("9600"),
        QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200"),
        QStringLiteral("230400"), QStringLiteral("460800"),
        QStringLiteral("921600")
    });
    m_receiverTypes = QStringList({
        QStringLiteral("UBlox M8P/F9P"),
        QStringLiteral("Septentrio"),
        QStringLiteral("Unicore UM982")
    });
    m_septentrioRtcmLevels = QStringList({
        QStringLiteral("Lite"),
        QStringLiteral("Basic"),
        QStringLiteral("Full")
    });

    m_settings->setFallbacksEnabled(false);
    loadSettings();
    loadBasePositions();

    connect(m_source, &GpsCorrectionSource::stateChanged,
            this, &ConfigGpsInjectViewModel::sourceStateChanged);
    connect(m_source, &GpsCorrectionSource::statusChanged,
            this, &ConfigGpsInjectViewModel::sourceStatusChanged);
    connect(m_source, &GpsCorrectionSource::inputBytes,
            this, &ConfigGpsInjectViewModel::sourceInputBytes);
    connect(m_source, &GpsCorrectionSource::rtcmFrame,
            this, &ConfigGpsInjectViewModel::sourceRtcmFrame);
    connect(m_source, &GpsCorrectionSource::plaintextCredentialsWarning,
            this, [this]() {
        setStatus(tr("Warning: NTRIP credentials are being sent without TLS."));
    });
    connect(m_source, &QObject::destroyed, this, [this]() {
        const QPointer<ConfigGpsInjectViewModel> guard(this);
        m_pendingUbloxAuthorization = 0;
        m_ownedUbloxOperation = 0;
        m_ubloxConnectAuthorized = false;
        m_active = false;
        m_connected = false;
        m_receiverActionsRequested = false;
        emit stateChanged();
        if (guard) {
            notifyProperties();
        }
    });

    if (m_ubloxService) {
        connect(m_ubloxService, &UbloxBaseStationService::stateChanged,
                this, &ConfigGpsInjectViewModel::ubloxStateChanged);
        connect(m_ubloxService,
                &UbloxBaseStationService::operationFinished,
                this, &ConfigGpsInjectViewModel::ubloxOperationFinished);
        connect(m_ubloxService, &QObject::destroyed, this, [this]() {
            const QPointer<ConfigGpsInjectViewModel> guard(this);
            m_ownedUbloxOperation = 0;
            m_ubloxService.clear();
            setStatus(tr("u-blox receiver configuration service is unavailable; RTCM injection remains available."));
            if (!guard) {
                return;
            }
            emit stateChanged();
            if (guard) {
                notifyProperties();
            }
        });
    }

    m_active = m_source->active();
    m_connected = m_source->connected();
    RefreshPorts();

    m_statisticsTimer = new QTimer(this);
    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    connect(m_statisticsTimer, &QTimer::timeout,
            this, &ConfigGpsInjectViewModel::UpdateStats);
    m_statisticsTimer->start();
}

void ConfigGpsInjectViewModel::loadSettings()
{
    m_selectedPort = m_settings->value(
        QStringLiteral("SerialInjectGPS_port"), NtripOption()).toString();
    m_selectedBaud = m_settings->value(
        QStringLiteral("SerialInjectGPS_baud"),
        QStringLiteral("115200")).toString();
    m_autoConfig = m_settings->value(
        QStringLiteral("SerialInjectGPS_autoconfig"), false).toBool();
    m_selectedReceiverType = m_settings->value(
        QStringLiteral("SerialInjectGPS_AutoConfigType"),
        QStringLiteral("UBlox M8P/F9P")).toString();
    m_m8p130Plus = m_settings->value(
        QStringLiteral("SerialInjectGPS_m8p_130p"), true).toBool();
    m_surveyInAcc = m_settings->value(
        QStringLiteral("SerialInjectGPS_SIAcc"),
        QStringLiteral("2")).toString();
    m_surveyInTime = m_settings->value(
        QStringLiteral("SerialInjectGPS_SITime"),
        QStringLiteral("60")).toString();

    // These are deliberately the non-secret NTRIP fields. Passwords remain
    // memory-only and are never read from or written to QSettings.
    m_host = m_settings->value(
        QStringLiteral("SerialInjectGPS_NTRIPHost")).toString();
    m_port = m_settings->value(
        QStringLiteral("SerialInjectGPS_NTRIPPort"), 2101).toInt();
    m_mount = m_settings->value(
        QStringLiteral("SerialInjectGPS_NTRIPMount")).toString();
    m_username = m_settings->value(
        QStringLiteral("SerialInjectGPS_NTRIPUsername")).toString();
    m_ntripV1 = m_settings->value(
        QStringLiteral("SerialInjectGPS_NTRIPv1"), false).toBool();
    m_sendGga = m_settings->value(
        QStringLiteral("SerialInjectGPS_SendGGA"), true).toBool();

    const int levelIndex = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioRTCMLevel"), 1).toInt();
    if (levelIndex >= 0 && levelIndex < m_septentrioRtcmLevels.size()) {
        m_selectedSeptentrioRtcmLevel =
            m_septentrioRtcmLevels.at(levelIndex);
    }
    m_septentrioRtcmInterval = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioRTCMInterval"),
        QStringLiteral("1.0")).toString();
    m_septentrioGps = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioGPS"), true).toBool();
    m_septentrioGlonass = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioGLONASS"), true).toBool();
    m_septentrioGalileo = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioGalileo"), true).toBool();
    m_septentrioBeidou = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioBeiDou"), true).toBool();
    m_septentrioFixedPosition = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedPosition"),
        false).toBool();
    m_septentrioLat = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedAtitude"),
        QStringLiteral("0")).toString();
    m_septentrioLng = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedLongitude"),
        QStringLiteral("0")).toString();
    m_septentrioAlt = m_settings->value(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedAltitude"),
        QStringLiteral("0")).toString();
}

void ConfigGpsInjectViewModel::loadBasePositions()
{
    m_basePositions.clear();
    const QString rawList = m_settings->value(
        QStringLiteral("base_pos_list")).toString();
    const QStringList entries = rawList.split(QLatin1Char(';'));
    for (const QString &entry : entries) {
        if (entry.isEmpty()) {
            continue;
        }
        const QStringList fields = entry.split(QLatin1Char(','));
        if (fields.size() < 3) {
            continue;
        }
        BasePosRow row;
        row.Lat = fields.at(0);
        row.Long = fields.at(1);
        row.Alt = fields.at(2);
        if (fields.size() > 3) {
            row.Name = fields.mid(3).join(QStringLiteral(","));
        }
        m_basePositions.append(row);
    }

    const QString active = m_settings->value(
        QStringLiteral("base_pos")).toString();
    const QStringList fields = active.split(QLatin1Char(','));
    if (fields.size() >= 3) {
        BasePosRow row;
        row.Lat = fields.at(0);
        row.Long = fields.at(1);
        row.Alt = fields.at(2);
        if (fields.size() > 3) {
            row.Name = fields.mid(3).join(QStringLiteral(","));
        }
        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;
        if (parsePosition(row, &latitude, &longitude, &altitude)) {
            m_activeBasePosition = row;
            m_hasActiveBasePosition = true;
        }
    }
}

void ConfigGpsInjectViewModel::saveConnectSettings()
{
    m_settings->setValue(QStringLiteral("SerialInjectGPS_port"),
                         m_selectedPort);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_baud"),
                         m_selectedBaud);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_autoconfig"),
                         m_autoConfig);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_AutoConfigType"),
                         m_selectedReceiverType);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_m8p_130p"),
                         m_m8p130Plus);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_SIAcc"),
                         m_surveyInAcc);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_SITime"),
                         m_surveyInTime);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_NTRIPHost"),
                         m_host);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_NTRIPPort"),
                         m_port);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_NTRIPMount"),
                         m_mount);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_NTRIPUsername"),
                         m_username);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_NTRIPv1"),
                         m_ntripV1);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_SendGGA"),
                         m_sendGga);
}

void ConfigGpsInjectViewModel::saveSeptentrioSettings()
{
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioRTCMLevel"),
        m_septentrioRtcmLevels.indexOf(m_selectedSeptentrioRtcmLevel));
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioRTCMInterval"),
        m_septentrioRtcmInterval);
    m_settings->setValue(QStringLiteral("SerialInjectGPS_SeptentrioGPS"),
                         m_septentrioGps);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioGLONASS"),
        m_septentrioGlonass);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioGalileo"),
        m_septentrioGalileo);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioBeiDou"),
        m_septentrioBeidou);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedPosition"),
        m_septentrioFixedPosition);
    // Keep Mission Planner's historical key spelling for compatibility.
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedAtitude"),
        m_septentrioLat);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedLongitude"),
        m_septentrioLng);
    m_settings->setValue(
        QStringLiteral("SerialInjectGPS_SeptentrioFixedAltitude"),
        m_septentrioAlt);
}

void ConfigGpsInjectViewModel::SetSelectedPort(const QString &value)
{
    if (m_selectedPort == value) {
        return;
    }
    m_selectedPort = value;
    saveConnectSettings();
    emit stateChanged();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSelectedBaud(const QString &value)
{
    if (m_selectedBaud == value) {
        return;
    }
    m_selectedBaud = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSelectedReceiverType(
    const QString &value)
{
    if (m_selectedReceiverType == value) {
        return;
    }
    m_selectedReceiverType = value;
    saveConnectSettings();
    emit stateChanged();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetHost(const QString &value)
{
    if (m_host == value) {
        return;
    }
    m_host = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetPort(int value)
{
    if (m_port == value) {
        return;
    }
    m_port = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetMount(const QString &value)
{
    if (m_mount == value) {
        return;
    }
    m_mount = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetUsername(const QString &value)
{
    if (m_username == value) {
        return;
    }
    m_username = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetPassword(const QString &value)
{
    if (m_password == value) {
        return;
    }
    m_password = value;
    // Password is intentionally not persisted.
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetNtripV1(bool value)
{
    if (m_ntripV1 == value) {
        return;
    }
    m_ntripV1 = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSendGga(bool value)
{
    if (m_sendGga == value) {
        return;
    }
    m_sendGga = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetAutoConfig(bool value)
{
    if (m_autoConfig == value) {
        return;
    }
    m_autoConfig = value;
    saveConnectSettings();
    emit stateChanged();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetM8p130Plus(bool value)
{
    if (m_m8p130Plus == value) {
        return;
    }
    m_m8p130Plus = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSurveyInAcc(const QString &value)
{
    if (m_surveyInAcc == value) {
        return;
    }
    m_surveyInAcc = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSurveyInTime(const QString &value)
{
    if (m_surveyInTime == value) {
        return;
    }
    m_surveyInTime = value;
    saveConnectSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSelectedSeptentrioRtcmLevel(
    const QString &value)
{
    if (m_selectedSeptentrioRtcmLevel == value) {
        return;
    }
    m_selectedSeptentrioRtcmLevel = value;
    saveSeptentrioSettings();
    notifyProperties();
    if (m_connected && IsSerial() && IsSeptentrio()) {
        ApplySeptentrioRtcm();
    }
}

void ConfigGpsInjectViewModel::SetSeptentrioGps(bool value)
{
    if (m_septentrioGps == value) {
        return;
    }
    m_septentrioGps = value;
    saveSeptentrioSettings();
    notifyProperties();
    if (m_connected && IsSerial() && IsSeptentrio()) {
        ApplySeptentrioRtcm();
    }
}

void ConfigGpsInjectViewModel::SetSeptentrioGlonass(bool value)
{
    if (m_septentrioGlonass == value) {
        return;
    }
    m_septentrioGlonass = value;
    saveSeptentrioSettings();
    notifyProperties();
    if (m_connected && IsSerial() && IsSeptentrio()) {
        ApplySeptentrioRtcm();
    }
}

void ConfigGpsInjectViewModel::SetSeptentrioGalileo(bool value)
{
    if (m_septentrioGalileo == value) {
        return;
    }
    m_septentrioGalileo = value;
    saveSeptentrioSettings();
    notifyProperties();
    if (m_connected && IsSerial() && IsSeptentrio()) {
        ApplySeptentrioRtcm();
    }
}

void ConfigGpsInjectViewModel::SetSeptentrioBeidou(bool value)
{
    if (m_septentrioBeidou == value) {
        return;
    }
    m_septentrioBeidou = value;
    saveSeptentrioSettings();
    notifyProperties();
    if (m_connected && IsSerial() && IsSeptentrio()) {
        ApplySeptentrioRtcm();
    }
}

void ConfigGpsInjectViewModel::SetSeptentrioRtcmInterval(
    const QString &value)
{
    if (m_septentrioRtcmInterval == value) {
        return;
    }
    m_septentrioRtcmInterval = value;
    saveSeptentrioSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSeptentrioFixedPosition(bool value)
{
    if (m_septentrioFixedPosition == value) {
        return;
    }
    m_septentrioFixedPosition = value;
    saveSeptentrioSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSeptentrioLat(const QString &value)
{
    if (m_septentrioLat == value) {
        return;
    }
    m_septentrioLat = value;
    saveSeptentrioSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSeptentrioLng(const QString &value)
{
    if (m_septentrioLng == value) {
        return;
    }
    m_septentrioLng = value;
    saveSeptentrioSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSeptentrioAlt(const QString &value)
{
    if (m_septentrioAlt == value) {
        return;
    }
    m_septentrioAlt = value;
    saveSeptentrioSettings();
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetVehiclePosition(
    double latitude, double longitude, double altitudeMsl, bool valid)
{
    m_vehiclePositionValid = valid
        && std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitudeMsl)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
    m_vehicleLatitude = latitude;
    m_vehicleLongitude = longitude;
    m_vehicleAltitudeMsl = altitudeMsl;
    if (m_source) {
        m_source->setGgaPosition(latitude, longitude, altitudeMsl,
                                 m_vehiclePositionValid);
    }
}

void ConfigGpsInjectViewModel::SetCurrentBasePosition(
    double latitude, double longitude, double altitude, bool valid)
{
    m_hasCurrentBasePosition = valid
        && std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
    if (m_hasCurrentBasePosition) {
        m_currentBasePosition.Lat = numberForSetting(latitude);
        m_currentBasePosition.Long = numberForSetting(longitude);
        m_currentBasePosition.Alt = numberForSetting(altitude);
        m_currentBasePosition.Name.clear();
    } else {
        m_currentBasePosition = BasePosRow();
    }
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetSurveyInStatus(
    const QString &status, bool valid)
{
    const QString previousStatus = m_surveyInStatus;
    const bool previousValid = m_surveyInValid;
    setSurveyInPresentation(status, valid);
    if (m_surveyInStatus == previousStatus
        && m_surveyInValid == previousValid) {
        return;
    }
    notifyProperties();
}

void ConfigGpsInjectViewModel::SetReceiverStatus(const QString &status)
{
    setStatus(status);
}

bool ConfigGpsInjectViewModel::RefreshPorts()
{
    if (!m_source) {
        setStatus(tr("Correction source is unavailable."));
        return false;
    }
    QStringList ports = m_source->availablePorts();
    ports.removeDuplicates();
    if (!ports.contains(NtripOption())) {
        ports.append(NtripOption());
    }
    const QString previous = m_selectedPort;
    m_ports = ports;
    // Keep a temporarily unplugged receiver visible and preserve the saved
    // selection. A launch without that USB device must not silently rewrite
    // the user's source to NTRIP.
    if (!previous.isEmpty() && !m_ports.contains(previous)) {
        m_ports.prepend(previous);
    }
    notifyProperties();
    return true;
}

GpsCorrectionSourceSettings ConfigGpsInjectViewModel::sourceSettings() const
{
    GpsCorrectionSourceSettings settings;
    settings.selectedPort = m_selectedPort;
    settings.baudRate = m_selectedBaud.toInt();
    settings.host = m_host;
    settings.casterPort = m_port;
    settings.mountPoint = m_mount;
    settings.username = m_username;
    settings.password = m_password;
    settings.ntripV1 = m_ntripV1;
    settings.sendGga = m_sendGga;
    return settings;
}

bool ConfigGpsInjectViewModel::ToggleConnect()
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    const QPointer<GpsCorrectionSource> source(m_source);
    if (!source) {
        setStatus(tr("Connect failed: correction source is unavailable."));
        return false;
    }
    if (m_pendingUbloxAuthorization != 0) {
        setStatus(tr("Respond to the u-blox configuration confirmation first."));
        return false;
    }
    if (source->active() || m_active) {
        const QPointer<UbloxBaseStationService> service(m_ubloxService);
        if (service && m_ownedUbloxOperation != 0
            && service->currentOperationId()
                == m_ownedUbloxOperation) {
            QString cancelError;
            (void) service->cancel(m_ownedUbloxOperation, &cancelError);
            if (!guard || !source || m_source != source) {
                return false;
            }
        }
        m_ownedUbloxOperation = 0;
        m_ubloxConnectAuthorized = false;
        source->stop();
        if (!guard || !source || m_source != source) {
            return false;
        }
        m_active = false;
        m_connected = false;
        m_receiverActionsRequested = false;
        setStatus(tr("Disconnected."));
        if (!guard) {
            return false;
        }
        emit stateChanged();
        if (guard) {
            notifyProperties();
        }
        return true;
    }

    saveConnectSettings();
    saveSeptentrioSettings();
    const GpsCorrectionSourceSettings settings = sourceSettings();
    const QString settingsError = settings.validationError();
    if (!settingsError.isEmpty()) {
        setStatus(tr("Connect failed: %1").arg(settingsError));
        return false;
    }

    const bool authorizeUblox = m_autoConfig && settings.isNtrip() == false
        && m_selectedReceiverType == QLatin1String("UBlox M8P/F9P");
    if (!authorizeUblox) {
        return beginSource(settings);
    }

    UbloxBaseStationService::FixedPosition fixed;
    bool useFixed = false;
    if (m_hasActiveBasePosition) {
        useFixed = parsePosition(m_activeBasePosition, &fixed.latitude,
                                 &fixed.longitude,
                                 &fixed.altitudeMeters);
        if (!useFixed || !fixed.isValid()) {
            setStatus(tr("Cannot configure u-blox receiver: the saved active base position is invalid."));
            return false;
        }
    }
    if (m_nextUbloxAuthorization == 0) {
        setStatus(tr("u-blox authorization identifiers are exhausted."));
        return false;
    }
    const quint64 authorizationId = m_nextUbloxAuthorization++;
    m_pendingUbloxAuthorization = authorizationId;
    m_pendingSourceSettings = settings;
    m_pendingM8p130Plus = m_m8p130Plus;
    m_pendingFixedPosition = useFixed;
    m_pendingFixed = fixed;
    const QString mode = useFixed
        ? tr("Fixed base: latitude %1, longitude %2, altitude %3 m.")
              .arg(QString::number(fixed.latitude, 'g', 15),
                   QString::number(fixed.longitude, 'g', 15),
                   QString::number(fixed.altitudeMeters, 'g', 15))
        : tr("Receiver setup only. Survey In will not start until you press Restart.");
    const QString confirmation =
        tr("Connect and automatically configure u-blox receiver\n\n"
           "Serial source: %1 at %2 baud.\n"
           "%3\n\n"
           "This authorization opens that serial source and sends the Mission Planner u-blox M8P/F9P setup sequence%4. "
           "It changes receiver baud, port protocols, navigation mode and RTCM message outputs; it may interrupt receiver output while configuration is in progress.\n\n"
           "The reference sequence uses timed serial writes and does not prove each setting with an acknowledgement. "
           "A Submitted result means all bytes entered the local serial queue, not that the receiver applied them. Continue?")
            .arg(settings.selectedPort,
                 QString::number(settings.baudRate), mode,
                 useFixed
                     ? tr(", then disables the previous base mode and applies the displayed fixed base")
                     : QString());
    setStatus(tr("Waiting for authorization before opening %1 and configuring the u-blox receiver.")
                  .arg(settings.selectedPort));
    if (!guard) {
        return false;
    }
    emit stateChanged();
    if (!guard) {
        return false;
    }
    notifyProperties();
    if (!guard) {
        return false;
    }
    emit ubloxAuthorizationRequested(authorizationId, confirmation);
    return !guard.isNull();
}

bool ConfigGpsInjectViewModel::ResolveUbloxAuthorization(
    quint64 authorizationId, bool accepted)
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    if (authorizationId == 0
        || authorizationId != m_pendingUbloxAuthorization) {
        return false;
    }
    const GpsCorrectionSourceSettings settings = m_pendingSourceSettings;
    m_pendingUbloxAuthorization = 0;
    if (!accepted) {
        m_ubloxConnectAuthorized = false;
        setStatus(tr("u-blox auto-configuration cancelled; the serial source was not opened."));
        if (!guard) {
            return false;
        }
        emit stateChanged();
        if (guard) {
            notifyProperties();
        }
        return true;
    }
    m_ubloxConnectAuthorized = true;
    const bool started = beginSource(settings);
    if (!guard) {
        return false;
    }
    if (!started) {
        m_ubloxConnectAuthorized = false;
        return false;
    }
    return true;
}

bool ConfigGpsInjectViewModel::beginSource(
    const GpsCorrectionSourceSettings &settings)
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    const QPointer<GpsCorrectionSource> source(m_source);
    if (!source) {
        setStatus(tr("Connect failed: correction source is unavailable."));
        return false;
    }
    resetStatistics();
    if (!guard || !source || m_source != source) {
        return false;
    }
    m_receiverActionsRequested = false;
    source->setGgaPosition(m_vehicleLatitude, m_vehicleLongitude,
                           m_vehicleAltitudeMsl, m_vehiclePositionValid);
    if (!guard || !source || m_source != source) {
        return false;
    }
    const QString previousStatus = m_status;
    const bool started = source->start(settings);
    if (!guard || !source || m_source != source) {
        return false;
    }
    sourceStateChanged(source->active(), source->connected());
    if (!guard || !source || m_source != source) {
        return false;
    }
    if (!started && m_status == previousStatus) {
        setStatus(tr("Connect failed."));
    }
    if (!started) {
        m_ubloxConnectAuthorized = false;
    }
    return started;
}

void ConfigGpsInjectViewModel::sourceStateChanged(
    bool active, bool connected)
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    const bool connectionBecameReady = connected && !m_connected;
    const bool changed = m_active != active || m_connected != connected;
    m_active = active;
    m_connected = connected;
    if (!m_active) {
        m_receiverActionsRequested = false;
        m_ubloxConnectAuthorized = false;
        m_ownedUbloxOperation = 0;
        m_hasCurrentBasePosition = false;
        m_currentBasePosition = {};
        setSurveyInPresentation(tr("Survey In: not started"), false);
    }
    if (connectionBecameReady) {
        setStatus(connectedStatus());
        if (!guard) {
            return;
        }
        requestAutoConfiguration();
        if (!guard) {
            return;
        }
    }
    if (changed) {
        emit stateChanged();
        if (guard) {
            notifyProperties();
        }
    }
}

void ConfigGpsInjectViewModel::sourceStatusChanged(const QString &status)
{
    if (m_ubloxService && m_ubloxService->busy()) {
        return;
    }
    if (m_connected && status == kConnectedStatus) {
        // setState() is emitted before this neutral source status. Preserve a
        // receiver driver's synchronous result (or our honest "requested"
        // status) after dispatching auto-configuration actions.
        if (m_receiverActionsRequested) {
            return;
        }
        setStatus(connectedStatus());
    } else {
        setStatus(status);
    }
}

QString ConfigGpsInjectViewModel::connectedStatus() const
{
    if (m_autoConfig && !IsSerial()) {
        return tr("Connected — receiving RTCM. Receiver auto-configuration requires a serial source.");
    }
    if (m_autoConfig
        && m_selectedReceiverType != QLatin1String("UBlox M8P/F9P")
        && m_selectedReceiverType != QLatin1String("Septentrio")) {
        return tr("Connected — receiving RTCM. Auto-config for %1 is not supported here.")
            .arg(m_selectedReceiverType);
    }
    if (m_autoConfig && IsSerial()
        && m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")) {
        return tr("Connected — receiving RTCM correction data. Receiver configuration requested.");
    }
    if (m_autoConfig && IsSerial()
        && m_selectedReceiverType == QLatin1String("Septentrio")) {
        return tr("Connected — receiving RTCM. Septentrio receiver auto-configuration is not implemented yet.");
    }
    return tr("Connected — receiving RTCM correction data.");
}

void ConfigGpsInjectViewModel::requestAutoConfiguration()
{
    if (m_receiverActionsRequested || !m_connected) {
        return;
    }
    if (m_ubloxConnectAuthorized) {
        m_receiverActionsRequested = true;
        m_ubloxConnectAuthorized = false;
        const QPointer<ConfigGpsInjectViewModel> guard(this);
        const QPointer<UbloxBaseStationService> service(m_ubloxService);
        if (!service || !service->available()) {
            setStatus(tr("Connected for RTCM injection, but the u-blox receiver configuration service is unavailable."));
            return;
        }
        QString error;
        quint64 operationId = 0;
        const bool started = m_pendingFixedPosition
            ? service->configureFixed(m_pendingFixed,
                                      m_pendingM8p130Plus,
                                      &operationId, &error)
            : service->configureReceiver(
                  m_pendingM8p130Plus, &operationId, &error);
        if (!guard || !service || m_ubloxService != service) {
            return;
        }
        if (!started || operationId == 0) {
            setStatus(tr("Connected for RTCM injection; u-blox auto-configuration did not start: %1")
                          .arg(error));
        } else {
            m_ownedUbloxOperation = operationId;
            if (m_pendingFixedPosition) {
                setSurveyInPresentation(
                    tr("Fixed base: submitting receiver configuration"),
                    false);
            }
            if (!service->busy()
                && service->lastReport().operationId == operationId) {
                ubloxOperationFinished(service->lastReport());
            } else {
                setStatus(service->status());
            }
        }
        return;
    }
    if (!m_autoConfig || !IsSerial()) {
        return;
    }
    m_receiverActionsRequested = true;
    if (m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")) {
        setStatus(tr("Connected for RTCM injection, but u-blox auto-configuration was not authorized."));
        return;
    }
    if (m_selectedReceiverType == QLatin1String("Septentrio")) {
        const QPointer<ConfigGpsInjectViewModel> guard(this);
        emit septentrioConfigureRequested();
        if (!guard) {
            return;
        }
        emit septentrioPositionRequested(
            m_septentrioFixedPosition,
            parseDouble(m_septentrioLat),
            parseDouble(m_septentrioLng),
            parseDouble(m_septentrioAlt));
        if (!guard) {
            return;
        }
        emit septentrioRtcmRequested(
            m_selectedSeptentrioRtcmLevel,
            parseDouble(m_septentrioRtcmInterval),
            m_septentrioGps, m_septentrioGlonass,
            m_septentrioGalileo, m_septentrioBeidou);
        if (!guard) {
            return;
        }
        setStatus(tr("Connected — receiving RTCM. Septentrio receiver auto-configuration is not implemented yet."));
    }
}

bool ConfigGpsInjectViewModel::validateSurveySettings(
    quint32 *durationSeconds, double *accuracyMeters, QString *error) const
{
    if (error) {
        error->clear();
    }
    bool durationOk = false;
    const qulonglong parsedDuration =
        m_surveyInTime.trimmed().toULongLong(&durationOk);
    if (!durationOk || parsedDuration == 0
        || parsedDuration
            > UbloxBaseStationService::MaximumSurveyDurationSeconds) {
        if (error) {
            *error = tr("Survey In time must be a whole number from 1 to %1 seconds.")
                         .arg(UbloxBaseStationService::MaximumSurveyDurationSeconds);
        }
        return false;
    }
    bool accuracyOk = false;
    const double parsedAccuracy =
        m_surveyInAcc.trimmed().toDouble(&accuracyOk);
    if (!accuracyOk || !std::isfinite(parsedAccuracy)
        || parsedAccuracy
            < UbloxBaseStationService::MinimumSurveyAccuracyMeters
        || parsedAccuracy
            > UbloxBaseStationService::MaximumSurveyAccuracyMeters) {
        if (error) {
            *error = tr("Survey In accuracy must be between %1 and %2 metres.")
                         .arg(UbloxBaseStationService::MinimumSurveyAccuracyMeters,
                              0, 'g', 15)
                         .arg(UbloxBaseStationService::MaximumSurveyAccuracyMeters,
                              0, 'g', 15);
        }
        return false;
    }
    if (durationSeconds) {
        *durationSeconds = static_cast<quint32>(parsedDuration);
    }
    if (accuracyMeters) {
        *accuracyMeters = parsedAccuracy;
    }
    return true;
}

void ConfigGpsInjectViewModel::refreshUbloxObservations()
{
    if (!m_ubloxService) {
        return;
    }
    const UbloxBaseStationProtocol::SurveyIn survey =
        m_ubloxService->surveyStatus();
    if (survey.valid && survey.hasPosition
        && std::isfinite(survey.latitude)
        && std::isfinite(survey.longitude)
        && std::isfinite(survey.altitudeMeters)
        && survey.latitude >= -90.0 && survey.latitude <= 90.0
        && survey.longitude >= -180.0 && survey.longitude <= 180.0) {
        m_hasCurrentBasePosition = true;
        m_currentBasePosition.Lat = numberForSetting(survey.latitude);
        m_currentBasePosition.Long = numberForSetting(survey.longitude);
        m_currentBasePosition.Alt = numberForSetting(
            survey.altitudeMeters);
        m_currentBasePosition.Name.clear();
    } else {
        const UbloxBaseStationProtocol::Position position =
            m_ubloxService->currentPosition();
        if (position.fixOk && position.fixType >= 3
            && std::isfinite(position.latitude)
            && std::isfinite(position.longitude)
            && std::isfinite(position.altitudeMeters)
            && position.latitude >= -90.0 && position.latitude <= 90.0
            && position.longitude >= -180.0
            && position.longitude <= 180.0) {
            m_hasCurrentBasePosition = true;
            m_currentBasePosition.Lat = numberForSetting(position.latitude);
            m_currentBasePosition.Long = numberForSetting(
                position.longitude);
            m_currentBasePosition.Alt = numberForSetting(
                position.altitudeMeters);
            m_currentBasePosition.Name.clear();
        }
    }

    QString baseStatus = m_surveyInBaseStatus;
    bool surveyValid = m_surveyInValid;
    if (survey.valid) {
        baseStatus = survey.hasPosition
            ? tr("Survey In: valid  Lat %1 Lng %2 Alt %3  Acc %4 m")
                  .arg(survey.latitude, 0, 'f', 7)
                  .arg(survey.longitude, 0, 'f', 7)
                  .arg(survey.altitudeMeters, 0, 'f', 2)
                  .arg(survey.accuracyMeters, 0, 'f', 2)
            : tr("Survey In: valid; position was not decoded.");
        surveyValid = true;
    } else if (survey.active || survey.durationSeconds != 0
               || survey.observations != 0) {
        baseStatus = tr("Survey In: %1  Dur %2 s  Obs %3  Acc %4 m")
            .arg(survey.active ? tr("in progress") : tr("complete"))
            .arg(survey.durationSeconds)
            .arg(survey.observations)
            .arg(survey.accuracyMeters, 0, 'f', 2);
        surveyValid = false;
    }
    setSurveyInPresentation(baseStatus, surveyValid);
}

void ConfigGpsInjectViewModel::setSurveyInPresentation(
    const QString &baseStatus, bool valid)
{
    m_surveyInBaseStatus = baseStatus;
    m_surveyInValid = valid;
    rebuildSurveyInPresentation();
}

void ConfigGpsInjectViewModel::rebuildSurveyInPresentation()
{
    m_surveyInStatus = m_surveyInBaseStatus;
    const QString acknowledgement = m_ubloxService
        ? m_ubloxService->acknowledgementStatus() : QString();
    if (!acknowledgement.isEmpty()) {
        m_surveyInStatus += QStringLiteral("  ") + acknowledgement;
    }
}

void ConfigGpsInjectViewModel::ubloxStateChanged()
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    const QPointer<UbloxBaseStationService> service(m_ubloxService);
    if (!service) {
        return;
    }
    refreshUbloxObservations();
    if (!guard || !service || m_ubloxService != service) {
        return;
    }
    if (service->busy()) {
        setStatus(service->status());
        if (!guard) {
            return;
        }
    }
    emit stateChanged();
    if (guard) {
        notifyProperties();
    }
}

void ConfigGpsInjectViewModel::ubloxOperationFinished(
    const UbloxBaseStationService::Report &report)
{
    if (m_ownedUbloxOperation == 0
        || report.operationId != m_ownedUbloxOperation) {
        return;
    }
    m_ownedUbloxOperation = 0;
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    QString mode;
    switch (report.mode) {
    case UbloxBaseStationService::Mode::Receiver:
        mode = tr("Receiver setup");
        break;
    case UbloxBaseStationService::Mode::SurveyIn:
        mode = tr("Survey In configuration");
        break;
    case UbloxBaseStationService::Mode::Fixed:
        mode = tr("Fixed base configuration");
        break;
    }
    switch (report.outcome) {
    case UbloxBaseStationService::Outcome::Submitted:
        setStatus(tr("%1 submitted: %2 This confirms local serial queueing, not that every receiver setting was applied.")
                      .arg(mode, report.description));
        break;
    case UbloxBaseStationService::Outcome::Cancelled:
        setStatus(tr("%1 cancelled: %2 Some earlier receiver writes may already have been submitted.")
                      .arg(mode, report.description));
        break;
    case UbloxBaseStationService::Outcome::Rejected:
        setStatus(tr("%1 failed before all commands were submitted: %2")
                      .arg(mode, report.description));
        break;
    case UbloxBaseStationService::Outcome::SourceLost:
        setStatus(tr("%1 stopped because the exact serial receiver session was lost: %2")
                      .arg(mode, report.description));
        break;
    }
    if (!guard) {
        return;
    }
    refreshUbloxObservations();
    emit stateChanged();
    if (guard) {
        notifyProperties();
    }
}

void ConfigGpsInjectViewModel::sourceInputBytes(qint64 bytes)
{
    if (bytes > 0) {
        m_inputBytesThisSecond += bytes;
    }
}

bool ConfigGpsInjectViewModel::validateFrame(
    const QByteArray &frame, quint16 messageId)
{
    if (frame.isEmpty()) {
        return false;
    }
    Rtcm3Parser parser;
    bool complete = false;
    for (char character : frame) {
        if (parser.addByte(static_cast<quint8>(character))) {
            if (complete) {
                return false;
            }
            complete = true;
        }
    }
    return complete && parser.validateCrc()
        && parser.messageId() == messageId
        && parser.currentFrame() == frame;
}

void ConfigGpsInjectViewModel::sourceRtcmFrame(
    const QByteArray &frame, quint16 messageId)
{
    if (!validateFrame(frame, messageId)) {
        return;
    }

    ++m_messageCounts[messageId];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    markFreshness(messageId, now);

    if (messageId == 1005 || messageId == 1006) {
        RtcmBasePosition position;
        if (Rtcm3Parser::extractBasePosition(frame, &position)) {
            m_hasCurrentBasePosition = true;
            m_currentBasePosition.Lat = numberForSetting(position.latitude);
            m_currentBasePosition.Long = numberForSetting(position.longitude);
            m_currentBasePosition.Alt = numberForSetting(position.altitude);
            m_currentBasePosition.Name.clear();
            m_rtcmBasePos = QStringLiteral("%1 %2 %3 - %4")
                .arg(QString::number(position.latitude, 'f', 7))
                .arg(QString::number(position.longitude, 'f', 7))
                .arg(QString::number(position.altitude, 'f', 2))
                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
        }
    }

    notifyProperties();
    emit rtcmDataReady(frame);
}

void ConfigGpsInjectViewModel::ReportInjectionResult(
    qint64 bytes, bool accepted, const QString &reason)
{
    if (accepted) {
        if (bytes > 0) {
            m_outputBytesThisSecond += bytes;
            m_injectedBytes += bytes;
        }
        if (m_connected
            && (m_status == kConnectedStatus
                || m_status.startsWith(
                    tr("RTCM received, but no vehicle accepted"))
                || m_status.startsWith(tr("RTCM injection rejected:")))) {
            setStatus(kQueuedStatus);
        }
        return;
    }

    if (reason.trimmed().isEmpty()) {
        setStatus(tr("RTCM received, but no vehicle accepted it for injection."));
    } else {
        setStatus(tr("RTCM injection rejected: %1").arg(reason));
    }
}

void ConfigGpsInjectViewModel::markFreshness(
    quint16 messageId, qint64 now)
{
    if (inRange(messageId, 1001, 1004)
        || inRange(messageId, 1071, 1077)) {
        m_gpsSeenAt = now;
    } else if (messageId == 1005 || messageId == 1006
               || messageId == 4072) {
        m_baseSeenAt = now;
    } else if (inRange(messageId, 1009, 1012)
               || inRange(messageId, 1081, 1087)) {
        m_glonassSeenAt = now;
    } else if (inRange(messageId, 1091, 1097)) {
        m_galileoSeenAt = now;
    } else if (inRange(messageId, 1121, 1127)) {
        m_beidouSeenAt = now;
    }
}

void ConfigGpsInjectViewModel::UpdateStats()
{
    m_inputRate = tr("%1 bps").arg(m_inputBytesThisSecond);
    m_outputRate = tr("%1 bps sent").arg(m_outputBytesThisSecond);
    m_injected = tr("%1 bytes").arg(m_injectedBytes);
    m_inputBytesThisSecond = 0;
    m_outputBytesThisSecond = 0;

    QList<quint16> ids = m_messageCounts.keys();
    std::sort(ids.begin(), ids.end());
    QStringList messages;
    for (quint16 id : ids) {
        messages.append(QStringLiteral("Rtcm%1=%2")
                            .arg(id)
                            .arg(m_messageCounts.value(id)));
    }
    m_messagesSeen = messages.join(QLatin1Char(' '));
    notifyProperties();
}

void ConfigGpsInjectViewModel::resetStatistics()
{
    m_inputBytesThisSecond = 0;
    m_outputBytesThisSecond = 0;
    m_injectedBytes = 0;
    m_messageCounts.clear();
    m_baseSeenAt = 0;
    m_gpsSeenAt = 0;
    m_glonassSeenAt = 0;
    m_beidouSeenAt = 0;
    m_galileoSeenAt = 0;
    m_injected = QStringLiteral("0 bytes");
    m_inputRate = QStringLiteral("0 bps");
    m_outputRate = QStringLiteral("0 bps");
    m_messagesSeen.clear();
    m_rtcmBasePos.clear();
    notifyProperties();
}

bool ConfigGpsInjectViewModel::isFresh(
    qint64 seenAt, int timeoutSeconds)
{
    return seenAt > 0
        && QDateTime::currentMSecsSinceEpoch() - seenAt
            < static_cast<qint64>(timeoutSeconds) * 1000;
}

bool ConfigGpsInjectViewModel::BaseFresh() const
{
    return isFresh(m_baseSeenAt, kBaseFreshnessSeconds);
}

bool ConfigGpsInjectViewModel::GpsFresh() const
{
    return isFresh(m_gpsSeenAt, kConstellationFreshnessSeconds);
}

bool ConfigGpsInjectViewModel::GlonassFresh() const
{
    return isFresh(m_glonassSeenAt, kConstellationFreshnessSeconds);
}

bool ConfigGpsInjectViewModel::BeidouFresh() const
{
    return isFresh(m_beidouSeenAt, kConstellationFreshnessSeconds);
}

bool ConfigGpsInjectViewModel::GalileoFresh() const
{
    return isFresh(m_galileoSeenAt, kConstellationFreshnessSeconds);
}

quint64 ConfigGpsInjectViewModel::RtcmMessageCount(quint16 messageId) const
{
    return m_messageCounts.value(messageId);
}

bool ConfigGpsInjectViewModel::RestartSurveyIn()
{
    if (!m_source || !m_connected || !IsSerial()
        || m_selectedReceiverType != QLatin1String("UBlox M8P/F9P")) {
        setStatus(tr("Connect to a UBlox M8P/F9P receiver on a serial port first."));
        return false;
    }
    if (!m_ubloxService || !m_ubloxService->available()) {
        setStatus(tr("Restart failed: the writable u-blox receiver session is unavailable."));
        return false;
    }
    if (ReceiverBusy()) {
        setStatus(tr("Restart failed: another receiver configuration is active."));
        return false;
    }
    quint32 durationSeconds = 0;
    double accuracyMeters = 0.0;
    QString error;
    if (!validateSurveySettings(&durationSeconds, &accuracyMeters, &error)) {
        setStatus(tr("Restart failed: %1").arg(error));
        return false;
    }

    const QPointer<ConfigGpsInjectViewModel> guard(this);
    const QPointer<UbloxBaseStationService> service(m_ubloxService);
    quint64 operationId = 0;
    const bool started = service->configureSurveyIn(
        durationSeconds, accuracyMeters, m_m8p130Plus,
        &operationId, &error);
    if (!guard || !service || m_ubloxService != service) {
        return false;
    }
    if (!started || operationId == 0) {
        setStatus(tr("Restart failed: %1").arg(error));
        return false;
    }
    m_ownedUbloxOperation = operationId;
    m_hasActiveBasePosition = false;
    m_activeBasePosition = {};
    m_settings->remove(QStringLiteral("base_pos"));
    setSurveyInPresentation(
        tr("Survey In: restarting — waiting for receiver NAV-SVIN status"),
        false);
    if (!service->busy()
        && service->lastReport().operationId == operationId) {
        ubloxOperationFinished(service->lastReport());
    } else {
        setStatus(service->status());
    }
    if (!guard) {
        return false;
    }
    emit stateChanged();
    if (guard) {
        notifyProperties();
    }
    return true;
}

bool ConfigGpsInjectViewModel::SaveCurrentPosition()
{
    if (!m_hasCurrentBasePosition) {
        setStatus(tr("No valid base position determined by GPS yet."));
        return false;
    }
    BasePosRow row = m_currentBasePosition;
    row.Name = tr("Base %1").arg(
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm")));
    m_basePositions.append(row);
    saveBasePositions();
    emit basePositionsChanged();
    notifyProperties();
    return true;
}

bool ConfigGpsInjectViewModel::UseBasePos(const BasePosRow &row)
{
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    if (!parsePosition(row, &latitude, &longitude, &altitude)) {
        setStatus(tr("Base position row has invalid numbers."));
        return false;
    }
    UbloxBaseStationService::FixedPosition position;
    position.latitude = latitude;
    position.longitude = longitude;
    position.altitudeMeters = altitude;
    if (!position.isValid()) {
        setStatus(tr("Base position is outside the receiver's supported range."));
        return false;
    }
    const bool writableUblox = m_connected && IsSerial()
        && m_selectedReceiverType == QLatin1String("UBlox M8P/F9P");
    if (writableUblox && ReceiverBusy()) {
        setStatus(tr("Fixed base position was not changed: another receiver configuration is active."));
        return false;
    }
    m_activeBasePosition = row;
    m_hasActiveBasePosition = true;
    saveActiveBasePosition();
    notifyProperties();
    if (!guard) {
        return false;
    }

    if (!writableUblox) {
        setStatus(tr("Saved fixed base position %1 for the next authorized u-blox connection; it was not sent to the current receiver.")
                      .arg(row.Name));
        return true;
    }
    if (!m_ubloxService || !m_ubloxService->available()) {
        setStatus(tr("Saved fixed base position %1, but the writable u-blox receiver session is unavailable.")
                      .arg(row.Name));
        return false;
    }

    const QPointer<UbloxBaseStationService> service(m_ubloxService);
    QString error;
    quint64 operationId = 0;
    const bool started = service->applyFixed(
        position, &operationId, &error);
    if (!guard || !service || m_ubloxService != service) {
        return false;
    }
    if (!started || operationId == 0) {
        setStatus(tr("Saved fixed base position %1, but receiver configuration did not start: %2")
                      .arg(row.Name, error));
        return false;
    }
    m_ownedUbloxOperation = operationId;
    setSurveyInPresentation(
        tr("Fixed base: submitting receiver configuration"), false);
    if (!service->busy()
        && service->lastReport().operationId == operationId) {
        ubloxOperationFinished(service->lastReport());
    } else {
        setStatus(service->status());
    }
    if (!guard) {
        return false;
    }
    emit stateChanged();
    if (guard) {
        notifyProperties();
    }
    return true;
}

bool ConfigGpsInjectViewModel::DeleteBasePos(const BasePosRow &row)
{
    const int index = m_basePositions.indexOf(row);
    if (index < 0) {
        return false;
    }
    m_basePositions.removeAt(index);
    saveBasePositions();
    emit basePositionsChanged();
    notifyProperties();
    return true;
}

bool ConfigGpsInjectViewModel::ApplySeptentrioRtcm()
{
    if (!m_source || !m_connected || !IsSerial()
        || m_selectedReceiverType != QLatin1String("Septentrio")) {
        setStatus(tr("Connect to a Septentrio receiver on a serial port first."));
        return false;
    }
    saveSeptentrioSettings();
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    emit septentrioRtcmRequested(
        m_selectedSeptentrioRtcmLevel,
        parseDouble(m_septentrioRtcmInterval),
        m_septentrioGps, m_septentrioGlonass,
        m_septentrioGalileo, m_septentrioBeidou);
    if (!guard) {
        return false;
    }
    setStatus(tr("Septentrio receiver configuration is not implemented yet; RTCM injection remains available."));
    return false;
}

bool ConfigGpsInjectViewModel::ApplySeptentrioPosition()
{
    if (!m_source || !m_connected || !IsSerial()
        || m_selectedReceiverType != QLatin1String("Septentrio")) {
        setStatus(tr("Connect to a Septentrio receiver on a serial port first."));
        return false;
    }
    saveSeptentrioSettings();
    const QPointer<ConfigGpsInjectViewModel> guard(this);
    emit septentrioPositionRequested(
        m_septentrioFixedPosition,
        parseDouble(m_septentrioLat),
        parseDouble(m_septentrioLng),
        parseDouble(m_septentrioAlt));
    if (!guard) {
        return false;
    }
    setStatus(tr("Septentrio receiver configuration is not implemented yet; RTCM injection remains available."));
    return false;
}

void ConfigGpsInjectViewModel::saveBasePositions()
{
    QStringList entries;
    for (const BasePosRow &row : m_basePositions) {
        entries.append(QStringLiteral("%1,%2,%3,%4")
                           .arg(sanitize(row.Lat))
                           .arg(sanitize(row.Long))
                           .arg(sanitize(row.Alt))
                           .arg(sanitize(row.Name)));
    }
    QString serialized = entries.join(QLatin1Char(';'));
    if (!serialized.isEmpty()) {
        serialized.append(QLatin1Char(';'));
    }
    m_settings->setValue(QStringLiteral("base_pos_list"), serialized);
}

void ConfigGpsInjectViewModel::saveActiveBasePosition()
{
    if (!m_hasActiveBasePosition) {
        return;
    }
    m_settings->setValue(
        QStringLiteral("base_pos"),
        QStringLiteral("%1,%2,%3,%4")
            .arg(sanitize(m_activeBasePosition.Lat))
            .arg(sanitize(m_activeBasePosition.Long))
            .arg(sanitize(m_activeBasePosition.Alt))
            .arg(sanitize(m_activeBasePosition.Name)));
}

bool ConfigGpsInjectViewModel::parsePosition(
    const BasePosRow &row, double *latitude, double *longitude,
    double *altitude)
{
    if (!latitude || !longitude || !altitude) {
        return false;
    }
    bool latitudeOk = false;
    bool longitudeOk = false;
    bool altitudeOk = false;
    const double parsedLatitude = row.Lat.toDouble(&latitudeOk);
    const double parsedLongitude = row.Long.toDouble(&longitudeOk);
    const double parsedAltitude = row.Alt.toDouble(&altitudeOk);
    if (!latitudeOk || !longitudeOk || !altitudeOk
        || !std::isfinite(parsedLatitude)
        || !std::isfinite(parsedLongitude)
        || !std::isfinite(parsedAltitude)
        || parsedLatitude < -90.0 || parsedLatitude > 90.0
        || parsedLongitude < -180.0 || parsedLongitude > 180.0) {
        return false;
    }
    *latitude = parsedLatitude;
    *longitude = parsedLongitude;
    *altitude = parsedAltitude;
    return true;
}

int ConfigGpsInjectViewModel::parseInt(const QString &value)
{
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : 0;
}

double ConfigGpsInjectViewModel::parseDouble(const QString &value)
{
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    return ok && std::isfinite(parsed) ? parsed : 0.0;
}

QString ConfigGpsInjectViewModel::sanitize(const QString &value)
{
    QString sanitized = value;
    sanitized.replace(QLatin1Char(','), QLatin1Char(' '));
    sanitized.replace(QLatin1Char(';'), QLatin1Char(' '));
    return sanitized;
}

void ConfigGpsInjectViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
    notifyProperties();
}

void ConfigGpsInjectViewModel::notifyProperties()
{
    emit propertiesChanged();
}
