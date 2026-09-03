#include "AntennaTrackerUIViewModel.h"

#include <QSerialPortInfo>
#include <QSettings>
#include <QTimer>

#include <cmath>
#include <utility>

namespace {

const char *const kInterfaceKey = "CMB_interface";
const char *const kPortKey = "CMB_serialport";
const char *const kBaudKey = "CMB_baudrate";
const char *const kPanTrimKey = "TRK_pantrim";
const char *const kTiltTrimKey = "TRK_tilttrim";
const char *const kPanReverseKey = "CHK_revpan";
const char *const kTiltReverseKey = "CHK_revtilt";

QStringList defaultPorts()
{
    QStringList result;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_WIN
        const QString port = info.portName();
#else
        const QString port = info.systemLocation().isEmpty()
            ? info.portName() : info.systemLocation();
#endif
        if (!port.isEmpty() && !result.contains(port)) {
            result.append(port);
        }
    }
    return result;
}

} // namespace

// --- texts -----------------------------------------------------------------------------

QString AntennaTrackerUIViewModel::Title() { return tr("Antenna Tracker"); }
QString AntennaTrackerUIViewModel::ConnectText() { return tr("Connect"); }
QString AntennaTrackerUIViewModel::DisconnectText() { return tr("Disconnect"); }
QString AntennaTrackerUIViewModel::ServoWarningText()
{
    return tr("Misusing this interface can cause servo damage, use with caution!!!");
}
QString AntennaTrackerUIViewModel::FindTrimPanLiveText() { return tr("Find Trim Pan (SiK Radio)"); }
QString AntennaTrackerUIViewModel::FindTrimPanSerialText() { return tr("Find Trim Pan (Sik Radio)"); }
QString AntennaTrackerUIViewModel::ManualSlewText()
{
    return tr("Manual Slew (override point-at-vehicle)");
}
QString AntennaTrackerUIViewModel::HomeCenterText() { return tr("Home / Center"); }
QString AntennaTrackerUIViewModel::ConnectFirstText() { return tr("Connect to the tracker first."); }
QString AntennaTrackerUIViewModel::NoSikRadioText() { return tr("No valid SiK radio detected."); }
QString AntennaTrackerUIViewModel::SearchingTrimText() { return tr("Searching for best pan trim..."); }
QString AntennaTrackerUIViewModel::TrimSearchCompleteText() { return tr("Pan trim search complete."); }
QString AntennaTrackerUIViewModel::TrimSearchCancelledText() { return tr("Pan trim search cancelled."); }
QString AntennaTrackerUIViewModel::CenterFailedText(const QString &reason)
{
    return tr("Center failed: %1").arg(reason);
}
QString AntennaTrackerUIViewModel::InvalidNumberText(const QString &detail)
{
    return tr("Invalid number entered: %1").arg(detail);
}
QString AntennaTrackerUIViewModel::NotAnIntegerText(const QString &field)
{
    return tr("%1 must be an integer.").arg(field);
}
QString AntennaTrackerUIViewModel::BelowMinimumText(const QString &field)
{
    return tr("%1 is below the safe minimum.").arg(field);
}
QString AntennaTrackerUIViewModel::PlaceholderText() { return QStringLiteral("--"); }
QString AntennaTrackerUIViewModel::SettingsGroup() { return QStringLiteral("AntennaTracker"); }

QString AntennaTrackerUIViewModel::FieldName(Axis axis, Field field)
{
    const QString prefix = axis == Axis::Pan ? tr("pan") : tr("tilt");
    switch (field) {
    case Field::Range: return tr("%1 range").arg(prefix);
    case Field::PwmRange: return tr("%1 PWM range").arg(prefix);
    case Field::Center: return tr("%1 PWM center").arg(prefix);
    case Field::Speed: return tr("%1 speed").arg(prefix);
    case Field::Accel: return tr("%1 acceleration").arg(prefix);
    }
    return QString();
}

QString AntennaTrackerUIViewModel::SettingsKey(Axis axis, Field field)
{
    const bool pan = axis == Axis::Pan;
    switch (field) {
    case Field::Range: return pan ? QStringLiteral("TXT_panrange") : QStringLiteral("TXT_tiltrange");
    case Field::PwmRange:
        return pan ? QStringLiteral("TXT_pwmrangepan") : QStringLiteral("TXT_pwmrangetilt");
    case Field::Center: return pan ? QStringLiteral("TXT_centerpan") : QStringLiteral("TXT_centertilt");
    case Field::Speed: return pan ? QStringLiteral("TXT_panspeed") : QStringLiteral("TXT_tiltspeed");
    case Field::Accel: return pan ? QStringLiteral("TXT_panaccel") : QStringLiteral("TXT_tiltaccel");
    }
    return QString();
}

QString AntennaTrackerUIViewModel::FormatAngle(double degrees)
{
    return QString::number(std::isfinite(degrees) ? degrees : 0.0, 'f', 1);
}

QString AntennaTrackerUIViewModel::FormatTrim(double degrees)
{
    return QString::number(std::isfinite(degrees) ? qRound(degrees) : 0);
}

// --- lifecycle -------------------------------------------------------------------------

AntennaTrackerUIViewModel::AntennaTrackerUIViewModel(AntennaTrackerSerialService *service,
                                                     AntennaTrackerTelemetrySource *telemetry,
                                                     QSettings *settings,
                                                     PortEnumerator portEnumerator,
                                                     QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_telemetry(telemetry)
    , m_settings(settings ? settings : new QSettings(this))
    , m_portEnumerator(portEnumerator ? std::move(portEnumerator) : PortEnumerator(defaultPorts))
    , m_loopTimer(new QTimer(this))
    , m_trimTimer(new QTimer(this))
{
    qRegisterMetaType<AntennaTrackerUIViewModel::Axis>("AntennaTrackerUIViewModel::Axis");
    qRegisterMetaType<AntennaTrackerUIViewModel::Field>("AntennaTrackerUIViewModel::Field");
    if (m_service && !m_service->parent()) {
        m_service->setParent(this);
    }
    if (m_telemetry && !m_telemetry->parent()) {
        m_telemetry->setParent(this);
    }

    // MP10 defaults (AntennaTrackerUIViewModel.cs field initialisers).
    m_selectedInterface = AntennaTrackerOutputFactory::Maestro();
    m_selectedBaud = QString::number(AntennaTrackerSerialSettings::DefaultBaud());
    const AntennaTrackerSerialSettings defaults;
    m_pan.fields[int(Field::Range)] = QString::number(defaults.panRange);
    m_pan.fields[int(Field::PwmRange)] = QString::number(defaults.panPwmRange);
    m_pan.fields[int(Field::Center)] = QString::number(defaults.panPwmCenter);
    m_pan.fields[int(Field::Speed)] = QString::number(defaults.panSpeed);
    m_pan.fields[int(Field::Accel)] = QString::number(defaults.panAccel);
    m_tilt.fields[int(Field::Range)] = QString::number(defaults.tiltRange);
    m_tilt.fields[int(Field::PwmRange)] = QString::number(defaults.tiltPwmRange);
    m_tilt.fields[int(Field::Center)] = QString::number(defaults.tiltPwmCenter);
    m_tilt.fields[int(Field::Speed)] = QString::number(defaults.tiltSpeed);
    m_tilt.fields[int(Field::Accel)] = QString::number(defaults.tiltAccel);
    m_tilt.trimMin = -45.0;
    m_tilt.trimMax = 45.0;
    m_vehicleAzimuth = m_vehicleElevation = m_commandedAzimuth = m_commandedElevation =
        PlaceholderText();
    m_connectText = ConnectText();

    m_loopTimer->setInterval(m_loopIntervalMs);
    connect(m_loopTimer, &QTimer::timeout, this, &AntennaTrackerUIViewModel::tick);
    m_trimTimer->setSingleShot(true);
    connect(m_trimTimer, &QTimer::timeout, this, &AntennaTrackerUIViewModel::trimTimerElapsed);

    if (m_service) {
        connect(m_service, &AntennaTrackerSerialService::stateChanged, this,
                &AntennaTrackerUIViewModel::onServiceStateChanged);
        connect(m_service, &AntennaTrackerSerialService::statusChanged, this,
                &AntennaTrackerUIViewModel::onServiceStatusChanged);
        m_status = m_service->status();
    }

    loadSettings();
    if (!interfaces().contains(m_selectedInterface)) {
        m_selectedInterface = AntennaTrackerOutputFactory::Maestro();
    }
    refreshPorts();
    updateTiltTrimRange();
    updateEnabledStates();
}

AntennaTrackerUIViewModel::~AntennaTrackerUIViewModel()
{
    // MP10 Dispose(): save, stop the loop, release the tracker.
    shutdown();
}

QStringList AntennaTrackerUIViewModel::interfaces() const
{
    return AntennaTrackerSerialSettings::Interfaces();
}

QStringList AntennaTrackerUIViewModel::bauds() const
{
    return AntennaTrackerSerialSettings::Bauds();
}

AntennaTrackerUIViewModel::AxisState &AntennaTrackerUIViewModel::axisState(Axis axis)
{
    return axis == Axis::Pan ? m_pan : m_tilt;
}

const AntennaTrackerUIViewModel::AxisState &AntennaTrackerUIViewModel::axisState(Axis axis) const
{
    return axis == Axis::Pan ? m_pan : m_tilt;
}

QString AntennaTrackerUIViewModel::field(Axis axis, Field field) const
{
    return axisState(axis).fields[int(field)];
}

double AntennaTrackerUIViewModel::trim(Axis axis) const { return axisState(axis).trim; }
double AntennaTrackerUIViewModel::trimMin(Axis axis) const { return axisState(axis).trimMin; }
double AntennaTrackerUIViewModel::trimMax(Axis axis) const { return axisState(axis).trimMax; }
bool AntennaTrackerUIViewModel::reverse(Axis axis) const { return axisState(axis).reverse; }

bool AntennaTrackerUIViewModel::isRunning() const
{
    return m_service && m_service->isRunning();
}

void AntennaTrackerUIViewModel::setLoopIntervalMs(int milliseconds)
{
    m_loopIntervalMs = qMax(1, milliseconds);
    m_loopTimer->setInterval(m_loopIntervalMs);
}

// --- fields ----------------------------------------------------------------------------

void AntennaTrackerUIViewModel::setSelectedInterface(const QString &value)
{
    if (m_selectedInterface == value) {
        return;
    }
    m_selectedInterface = value;
    emit selectedInterfaceChanged(value);
    updateEnabledStates(); // MP10 OnSelectedInterfaceChanged -> UpdateSpeedAccelEnabled
}

void AntennaTrackerUIViewModel::setSelectedPort(const QString &value)
{
    if (m_selectedPort == value) {
        return;
    }
    m_selectedPort = value;
    emit selectedPortChanged(value);
}

void AntennaTrackerUIViewModel::setSelectedBaud(const QString &value)
{
    if (m_selectedBaud == value) {
        return;
    }
    m_selectedBaud = value;
    emit selectedBaudChanged(value);
}

void AntennaTrackerUIViewModel::setField(Axis axis, Field field, const QString &value)
{
    QString &slot = axisState(axis).fields[int(field)];
    if (slot == value) {
        return;
    }
    slot = value;
    emit fieldChanged(axis, field, value);
    if (axis == Axis::Tilt && field == Field::Range) {
        updateTiltTrimRange(); // MP10 OnTiltRangeChanged
    }
    // MP10 pushes speed/acceleration into the driver live, but they only take
    // effect in Setup() and the inputs are locked while connected, so the
    // service needs no live setter for them.
}

void AntennaTrackerUIViewModel::setTrim(Axis axis, double value)
{
    if (!std::isfinite(value)) {
        return;
    }
    AxisState &state = axisState(axis);
    if (state.trim == value) {
        return;
    }
    state.trim = value;
    emit trimChanged(axis, value);
    if (m_service) {
        m_service->setTrim(m_pan.trim, m_tilt.trim); // live, like MP10 OnPanTrimChanged
    }
}

void AntennaTrackerUIViewModel::setReverse(Axis axis, bool value)
{
    AxisState &state = axisState(axis);
    if (state.reverse == value) {
        return;
    }
    state.reverse = value;
    emit reverseChanged(axis, value);
    if (m_service) {
        m_service->setReverse(m_pan.reverse, m_tilt.reverse);
    }
}

void AntennaTrackerUIViewModel::setManualMode(bool value)
{
    if (m_manualMode == value) {
        return;
    }
    m_manualMode = value;
    emit manualModeChanged(value);
}

void AntennaTrackerUIViewModel::setManualAzimuth(double value)
{
    if (!std::isfinite(value) || m_manualAzimuth == value) {
        return;
    }
    m_manualAzimuth = value;
    emit manualAzimuthChanged(value);
}

void AntennaTrackerUIViewModel::setManualElevation(double value)
{
    if (!std::isfinite(value) || m_manualElevation == value) {
        return;
    }
    m_manualElevation = value;
    emit manualElevationChanged(value);
}

void AntennaTrackerUIViewModel::updateTiltTrimRange()
{
    // MP10 UpdateTiltTrimRange: range / 2 * -1 .. range / 2 with C# integer division.
    const int range = ParseIntOr(m_tilt.fields[int(Field::Range)], 90);
    const double minimum = range / 2 * -1;
    const double maximum = range / 2;
    if (m_tilt.trimMin == minimum && m_tilt.trimMax == maximum) {
        return;
    }
    m_tilt.trimMin = minimum;
    m_tilt.trimMax = maximum;
    emit trimRangeChanged(Axis::Tilt, minimum, maximum);
}

void AntennaTrackerUIViewModel::updateEnabledStates()
{
    const bool running = isRunning();
    const bool controls = !running;
    const bool speedAccel = controls && m_selectedInterface == AntennaTrackerOutputFactory::Maestro();
    const QString connectText = running ? DisconnectText() : ConnectText();
    if (m_controlsEnabled != controls) {
        m_controlsEnabled = controls;
        emit controlsEnabledChanged(controls);
    }
    if (m_speedAccelEnabled != speedAccel) {
        m_speedAccelEnabled = speedAccel;
        emit speedAccelEnabledChanged(speedAccel);
    }
    if (m_connectText != connectText) {
        m_connectText = connectText;
        emit connectTextChanged(connectText);
    }
}

void AntennaTrackerUIViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
}

void AntennaTrackerUIViewModel::setTelemetryTexts(const QString &vehicleAz,
                                                  const QString &vehicleEl,
                                                  const QString &commandedAz,
                                                  const QString &commandedEl)
{
    if (m_vehicleAzimuth == vehicleAz && m_vehicleElevation == vehicleEl
        && m_commandedAzimuth == commandedAz && m_commandedElevation == commandedEl) {
        return;
    }
    m_vehicleAzimuth = vehicleAz;
    m_vehicleElevation = vehicleEl;
    m_commandedAzimuth = commandedAz;
    m_commandedElevation = commandedEl;
    emit telemetryChanged();
}

// --- ports -----------------------------------------------------------------------------

void AntennaTrackerUIViewModel::refreshPorts()
{
    QStringList ports = m_portEnumerator ? m_portEnumerator() : QStringList();
    ports.removeDuplicates();
    if (m_ports != ports) {
        m_ports = ports;
        emit portsChanged(ports);
    }
    if (!m_ports.contains(m_selectedPort)) {
        setSelectedPort(m_ports.isEmpty() ? QString() : m_ports.first());
    }
}

// --- connect ---------------------------------------------------------------------------

bool AntennaTrackerUIViewModel::ParseInt(const QString &text, int *value)
{
    bool ok = false;
    const int parsed = text.trimmed().toInt(&ok, 10);
    if (ok && value) {
        *value = parsed;
    }
    return ok;
}

int AntennaTrackerUIViewModel::ParseIntOr(const QString &text, int fallback)
{
    int value = 0;
    return ParseInt(text, &value) ? value : fallback;
}

AntennaTrackerSerialSettings AntennaTrackerUIViewModel::buildSettings(QString *error) const
{
    AntennaTrackerSerialSettings settings;
    if (error) {
        error->clear();
    }
    settings.interfaceName = m_selectedInterface;
    settings.portName = m_selectedPort.trimmed();

    // MP10 :270 ParseRequiredInt(SelectedBaud, "baud rate", 1) inside "Error connecting: ".
    int baud = 0;
    if (!ParseInt(m_selectedBaud, &baud)) {
        if (error) {
            *error = AntennaTrackerSerialService::ErrorConnectingText(NotAnIntegerText(tr("baud rate")));
        }
        return settings;
    }
    if (baud < 1) {
        if (error) {
            *error = AntennaTrackerSerialService::ErrorConnectingText(BelowMinimumText(tr("baud rate")));
        }
        return settings;
    }
    settings.baudRate = baud;

    // MP10 :288-310 order, each with its own minimum, inside "Invalid number entered: ".
    struct Item
    {
        Axis axis;
        Field field;
        int minimum;
        int *target;
    };
    const Item items[] = {
        {Axis::Pan, Field::Range, 1, &settings.panRange},
        {Axis::Tilt, Field::Range, 1, &settings.tiltRange},
        {Axis::Pan, Field::PwmRange, 1, &settings.panPwmRange},
        {Axis::Tilt, Field::PwmRange, 1, &settings.tiltPwmRange},
        {Axis::Pan, Field::Center, 1, &settings.panPwmCenter},
        {Axis::Tilt, Field::Center, 1, &settings.tiltPwmCenter},
        {Axis::Pan, Field::Speed, 0, &settings.panSpeed},
        {Axis::Pan, Field::Accel, 0, &settings.panAccel},
        {Axis::Tilt, Field::Speed, 0, &settings.tiltSpeed},
        {Axis::Tilt, Field::Accel, 0, &settings.tiltAccel},
    };
    for (const Item &item : items) {
        int value = 0;
        if (!ParseInt(field(item.axis, item.field), &value)) {
            if (error) {
                *error = InvalidNumberText(NotAnIntegerText(FieldName(item.axis, item.field)));
            }
            return settings;
        }
        if (value < item.minimum) {
            if (error) {
                *error = InvalidNumberText(BelowMinimumText(FieldName(item.axis, item.field)));
            }
            return settings;
        }
        *item.target = value;
    }
    settings.panTrim = m_pan.trim;
    settings.tiltTrim = m_tilt.trim;
    settings.panReverse = m_pan.reverse;
    settings.tiltReverse = m_tilt.reverse;
    return settings;
}

void AntennaTrackerUIViewModel::connectOrDisconnect()
{
    saveSettings(); // MP10 Connect() starts with SaveSettings()
    if (!m_service) {
        return;
    }
    if (m_service->isRunning()) {
        m_service->disconnectFromTracker();
        return;
    }
    if (m_selectedPort.trimmed().isEmpty()) {
        setStatus(AntennaTrackerSerialService::NoPortText());
        return;
    }
    QString error;
    const AntennaTrackerSerialSettings settings = buildSettings(&error);
    if (!error.isEmpty()) {
        setStatus(error);
        return;
    }
    m_service->connectToTracker(settings);
}

void AntennaTrackerUIViewModel::onServiceStateChanged(AntennaTrackerSerialService::State state,
                                                      quint64 generation)
{
    Q_UNUSED(generation);
    using State = AntennaTrackerSerialService::State;
    updateEnabledStates();
    switch (state) {
    case State::Connecting:
        break;
    case State::Connected: {
        // MP10 :335-336 echoes the driver's centre values back into the inputs.
        const AntennaTrackerSerialSettings applied = m_service->settings();
        setField(Axis::Pan, Field::Center, QString::number(applied.panPwmCenter));
        setField(Axis::Tilt, Field::Center, QString::number(applied.tiltPwmCenter));
        m_loopTicks = 0;
        m_loopTimer->start(); // MP10 StartLoop()
        emit runningChanged(true);
        break;
    }
    case State::Disconnected:
    case State::Failed:
        m_loopTimer->stop();
        if (m_search.active) {
            finishTrimSearch(true, false); // the service already reports the reason
        }
        emit runningChanged(false);
        break;
    }
}

void AntennaTrackerUIViewModel::onServiceStatusChanged(const QString &status)
{
    setStatus(status);
}

// --- loop ------------------------------------------------------------------------------

void AntennaTrackerUIViewModel::tick()
{
    if (!m_service || m_service->state() != AntennaTrackerSerialService::State::Connected) {
        return;
    }
    ++m_loopTicks;
    double vehicleAzimuth = 0.0;
    double vehicleElevation = 0.0;
    if (m_telemetry) {
        const AntennaTrackerVehicleFix fix = m_telemetry->vehicleFix();
        bool trackerValid = false;
        const AntennaTrackerPosition tracker = m_telemetry->trackerLocation(&trackerValid);
        if (fix.valid && trackerValid) {
            const AntennaTrackerPosition vehicle(fix.latitude, fix.longitude, fix.altitudeAmsl);
            vehicleAzimuth = AntennaTrackerGeometry::AZToMAV(tracker, vehicle);
            vehicleElevation = AntennaTrackerGeometry::ELToMAV(tracker, vehicle);
        }
    }
    const double az = m_manualMode ? m_manualAzimuth : vehicleAzimuth;
    const double el = m_manualMode ? m_manualElevation : vehicleElevation;
    m_service->setTarget(az, el); // MP10 _tracker?.PanAndTilt(az, el); failures are reported by the service
    setTelemetryTexts(FormatAngle(vehicleAzimuth), FormatAngle(vehicleElevation),
                      FormatAngle(az), FormatAngle(el));
}

void AntennaTrackerUIViewModel::homeCenter()
{
    setManualAzimuth(0.0);
    setManualElevation(0.0);
    if (m_service && m_service->isRunning()) {
        if (!m_service->centerTracker()) {
            setStatus(CenterFailedText(tr("the tracker output refused the command")));
        }
    }
}

// --- SiK trim sweep --------------------------------------------------------------------

void AntennaTrackerUIViewModel::findTrimPan()
{
    if (m_search.active) {
        cancelTrimSearch();
        return;
    }
    if (!isRunning()) {
        setStatus(ConnectFirstText());
        return;
    }
    const double snr = m_telemetry ? m_telemetry->localSnrDb() : 0.0;
    if (snr == 0.0) {
        setStatus(NoSikRadioText());
        return;
    }
    setStatus(SearchingTrimText());
    m_search = TrimSearch();
    m_search.active = true;
    m_search.pan = static_cast<float>(m_pan.trim);
    m_search.panRange = static_cast<float>(ParseIntOr(m_pan.fields[int(Field::Range)], 360));
    emit searchingTrimChanged(true);
    beginTrimPhase(0, m_search.pan);
}

void AntennaTrackerUIViewModel::beginTrimPhase(int phase, float centre)
{
    // MP10 FindTrimPan: CheckPos(pan - range/4, pan + range/4 - 1, 30),
    // CheckPos(-30 + ans, 30 + ans, 5), CheckPos(-5 + ans, 5 + ans, 1).
    m_search.phase = phase;
    switch (phase) {
    case 0:
        m_search.start = centre - m_search.panRange / 4;
        m_search.end = centre + m_search.panRange / 4 - 1;
        m_search.scale = 30.0f;
        break;
    case 1:
        m_search.start = -30.0f + centre;
        m_search.end = 30.0f + centre;
        m_search.scale = 5.0f;
        break;
    default:
        m_search.start = -5.0f + centre;
        m_search.end = 5.0f + centre;
        m_search.scale = 1.0f;
        break;
    }
    m_search.n = m_search.start;
    m_search.lastSnr = 0.0f;
    m_search.best = 0.0f;
    m_search.settling = true;
    setTrim(Axis::Pan, m_search.start); // MP10 SetPan(start); Thread.Sleep(4000)
    m_trimTimer->start(qMax(0, m_searchTiming.settleMs));
}

void AntennaTrackerUIViewModel::trimTimerElapsed()
{
    if (!m_search.active) {
        return;
    }
    if (m_search.settling) {
        m_search.settling = false;
    } else {
        // MP10: after Thread.Sleep(2000) at position n, read localsnrdb.
        const float snr = static_cast<float>(m_telemetry ? m_telemetry->localSnrDb() : 0.0);
        if (snr > m_search.lastSnr) {
            m_search.best = m_search.n;
            m_search.lastSnr = snr;
        }
        m_search.n += m_search.scale;
    }
    if (m_search.n < m_search.end) {
        setTrim(Axis::Pan, m_search.n); // MP10 SetPan(n); Thread.Sleep(2000)
        m_trimTimer->start(qMax(0, m_searchTiming.stepMs));
        return;
    }
    const float answer = m_search.best;
    if (m_search.phase < 2) {
        beginTrimPhase(m_search.phase + 1, answer);
        return;
    }
    setTrim(Axis::Pan, answer); // MP10 SetPan(ans)
    finishTrimSearch(false, true);
}

void AntennaTrackerUIViewModel::cancelTrimSearch()
{
    if (!m_search.active) {
        return;
    }
    finishTrimSearch(true, true);
}

void AntennaTrackerUIViewModel::finishTrimSearch(bool cancelled, bool announce)
{
    m_trimTimer->stop();
    m_search.active = false;
    emit searchingTrimChanged(false);
    if (announce) {
        setStatus(cancelled ? TrimSearchCancelledText() : TrimSearchCompleteText());
    }
}

// --- activation / settings -------------------------------------------------------------

void AntennaTrackerUIViewModel::activate()
{
    refreshPorts();
    updateEnabledStates(); // MP10 Activate(): ConnectText follows IsRunning
}

void AntennaTrackerUIViewModel::deactivate()
{
    saveSettings();
}

void AntennaTrackerUIViewModel::shutdown()
{
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;
    saveSettings();
    if (m_search.active) {
        finishTrimSearch(true, false);
    }
    m_loopTimer->stop();
    if (m_service && m_service->isRunning()) {
        m_service->disconnectFromTracker();
    }
}

void AntennaTrackerUIViewModel::loadSettings()
{
    if (!m_settings) {
        return;
    }
    m_settings->beginGroup(SettingsGroup());
    const auto text = [this](const char *key, const QString &fallback) {
        const QVariant value = m_settings->value(QLatin1String(key));
        return value.isValid() && !value.toString().isNull() ? value.toString() : fallback;
    };
    m_selectedInterface = text(kInterfaceKey, m_selectedInterface);
    m_selectedPort = text(kPortKey, m_selectedPort);
    m_selectedBaud = text(kBaudKey, m_selectedBaud);
    for (Axis axis : {Axis::Pan, Axis::Tilt}) {
        for (Field field : {Field::Range, Field::PwmRange, Field::Center, Field::Speed, Field::Accel}) {
            QString &slot = axisState(axis).fields[int(field)];
            slot = text(SettingsKey(axis, field).toLatin1().constData(), slot);
        }
    }
    // MP10 stores the trims through GetInt32.
    m_pan.trim = m_settings->value(QLatin1String(kPanTrimKey), 0).toInt();
    m_tilt.trim = m_settings->value(QLatin1String(kTiltTrimKey), 0).toInt();
    m_pan.reverse = m_settings->value(QLatin1String(kPanReverseKey), false).toBool();
    m_tilt.reverse = m_settings->value(QLatin1String(kTiltReverseKey), false).toBool();
    m_settings->endGroup();
    if (m_service) {
        m_service->setTrim(m_pan.trim, m_tilt.trim);
        m_service->setReverse(m_pan.reverse, m_tilt.reverse);
    }
}

void AntennaTrackerUIViewModel::saveSettings()
{
    if (!m_settings) {
        return;
    }
    m_settings->beginGroup(SettingsGroup());
    m_settings->setValue(QLatin1String(kInterfaceKey), m_selectedInterface);
    m_settings->setValue(QLatin1String(kPortKey), m_selectedPort);
    m_settings->setValue(QLatin1String(kBaudKey), m_selectedBaud);
    for (Axis axis : {Axis::Pan, Axis::Tilt}) {
        for (Field kind : {Field::Range, Field::PwmRange, Field::Center, Field::Speed, Field::Accel}) {
            m_settings->setValue(SettingsKey(axis, kind), field(axis, kind));
        }
    }
    // MP10 :508-509: ((int)PanTrim) truncates toward zero.
    m_settings->setValue(QLatin1String(kPanTrimKey), static_cast<int>(m_pan.trim));
    m_settings->setValue(QLatin1String(kTiltTrimKey), static_cast<int>(m_tilt.trim));
    m_settings->setValue(QLatin1String(kPanReverseKey), m_pan.reverse);
    m_settings->setValue(QLatin1String(kTiltReverseKey), m_tilt.reverse);
    m_settings->endGroup();
    m_settings->sync();
}
