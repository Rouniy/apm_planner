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
    initialize();
}

ConfigGpsInjectViewModel::~ConfigGpsInjectViewModel()
{
    if (m_statisticsTimer) {
        m_statisticsTimer->stop();
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
    return m_active ? tr("Disconnect") : tr("Connect");
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
        m_active = false;
        m_connected = false;
        m_receiverActionsRequested = false;
        emit stateChanged();
        notifyProperties();
    });

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
    if (m_surveyInStatus == status && m_surveyInValid == valid) {
        return;
    }
    m_surveyInStatus = status;
    m_surveyInValid = valid;
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
    if (!m_source) {
        setStatus(tr("Connect failed: correction source is unavailable."));
        return false;
    }
    if (m_source->active() || m_active) {
        m_source->stop();
        m_active = false;
        m_connected = false;
        m_receiverActionsRequested = false;
        setStatus(tr("Disconnected."));
        emit stateChanged();
        notifyProperties();
        return true;
    }

    saveConnectSettings();
    saveSeptentrioSettings();
    resetStatistics();
    m_receiverActionsRequested = false;
    m_source->setGgaPosition(m_vehicleLatitude, m_vehicleLongitude,
                             m_vehicleAltitudeMsl,
                             m_vehiclePositionValid);
    const QString previousStatus = m_status;
    const bool started = m_source->start(sourceSettings());
    sourceStateChanged(m_source->active(), m_source->connected());
    if (!started && m_status == previousStatus) {
        setStatus(tr("Connect failed."));
    }
    return started;
}

void ConfigGpsInjectViewModel::sourceStateChanged(
    bool active, bool connected)
{
    const bool connectionBecameReady = connected && !m_connected;
    const bool changed = m_active != active || m_connected != connected;
    m_active = active;
    m_connected = connected;
    if (!m_active) {
        m_receiverActionsRequested = false;
    }
    if (connectionBecameReady) {
        setStatus(connectedStatus());
        requestAutoConfiguration();
    }
    if (changed) {
        emit stateChanged();
        notifyProperties();
    }
}

void ConfigGpsInjectViewModel::sourceStatusChanged(const QString &status)
{
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
        && (m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")
            || m_selectedReceiverType == QLatin1String("Septentrio"))) {
        return tr("Connected — receiving RTCM correction data. Receiver configuration requested.");
    }
    return tr("Connected — receiving RTCM correction data.");
}

void ConfigGpsInjectViewModel::requestAutoConfiguration()
{
    if (m_receiverActionsRequested || !m_connected
        || !m_autoConfig || !IsSerial()) {
        return;
    }
    m_receiverActionsRequested = true;
    if (m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")) {
        emit ubloxConfigureRequested(m_m8p130Plus);
        if (m_hasActiveBasePosition) {
            double latitude = 0.0;
            double longitude = 0.0;
            double altitude = 0.0;
            if (parsePosition(m_activeBasePosition, &latitude,
                              &longitude, &altitude)) {
                emit ubloxBasePositionRequested(
                    latitude, longitude, altitude,
                    parseInt(m_surveyInTime), parseDouble(m_surveyInAcc));
            }
        }
        return;
    }
    if (m_selectedReceiverType == QLatin1String("Septentrio")) {
        emit septentrioConfigureRequested();
        emit septentrioPositionRequested(
            m_septentrioFixedPosition,
            parseDouble(m_septentrioLat),
            parseDouble(m_septentrioLng),
            parseDouble(m_septentrioAlt));
        emit septentrioRtcmRequested(
            m_selectedSeptentrioRtcmLevel,
            parseDouble(m_septentrioRtcmInterval),
            m_septentrioGps, m_septentrioGlonass,
            m_septentrioGalileo, m_septentrioBeidou);
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
    m_surveyInStatus = tr("Survey In: restarting");
    m_surveyInValid = false;
    notifyProperties();
    m_hasActiveBasePosition = false;
    emit ubloxSurveyInRequested(parseInt(m_surveyInTime),
                                parseDouble(m_surveyInAcc),
                                m_m8p130Plus);
    setStatus(tr("Survey In: restart requested."));
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
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    if (!parsePosition(row, &latitude, &longitude, &altitude)) {
        setStatus(tr("Base position row has invalid numbers."));
        return false;
    }
    m_activeBasePosition = row;
    m_hasActiveBasePosition = true;
    saveActiveBasePosition();
    setStatus(tr("Using fixed base position: %1").arg(row.Name));
    notifyProperties();

    if (m_connected && IsSerial()
        && m_selectedReceiverType == QLatin1String("UBlox M8P/F9P")) {
        emit ubloxBasePositionRequested(
            latitude, longitude, altitude,
            parseInt(m_surveyInTime), parseDouble(m_surveyInAcc));
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
    emit septentrioRtcmRequested(
        m_selectedSeptentrioRtcmLevel,
        parseDouble(m_septentrioRtcmInterval),
        m_septentrioGps, m_septentrioGlonass,
        m_septentrioGalileo, m_septentrioBeidou);
    setStatus(tr("Septentrio RTCM settings requested."));
    return true;
}

bool ConfigGpsInjectViewModel::ApplySeptentrioPosition()
{
    if (!m_source || !m_connected || !IsSerial()
        || m_selectedReceiverType != QLatin1String("Septentrio")) {
        setStatus(tr("Connect to a Septentrio receiver on a serial port first."));
        return false;
    }
    saveSeptentrioSettings();
    emit septentrioPositionRequested(
        m_septentrioFixedPosition,
        parseDouble(m_septentrioLat),
        parseDouble(m_septentrioLng),
        parseDouble(m_septentrioAlt));
    setStatus(tr("Septentrio base position update requested."));
    return true;
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
