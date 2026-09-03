#ifndef ANTENNATRACKERUIVIEWMODEL_H
#define ANTENNATRACKERUIVIEWMODEL_H

#include "AntennaTrackerSerialService.h"
#include "AntennaTrackerTelemetrySource.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QSettings;
class QTimer;

/*
 * Mission Planner 10 AntennaTrackerUIViewModel (ViewModels/AntennaTrackerUIViewModel.cs),
 * shared by SETUP > "Antenna Tracker (Serial)" (ConfigAntennaTrackerView) and
 * "Antenna Tracker (Live)" (AntennaTrackerUIView). MP10 gives every page its own
 * instance; the Qt port keeps ONE instance for both pages so a single physical
 * tracker has one connection and both pages show the same state.
 *
 * Owns the field texts, MP10 status texts, the settings round trip, the 10 Hz
 * point-at-vehicle loop (cs.AZToMAV / cs.ELToMAV via AntennaTrackerGeometry) and
 * the SiK "Find Trim Pan" sweep, on top of a caller-supplied
 * AntennaTrackerSerialService and AntennaTrackerTelemetrySource. Everything runs
 * on the owning (GUI) thread; the service is event driven, so no worker is needed.
 *
 * Deliberate differences from MP10, all recorded in PORTING_DEVIATIONS.tsv:
 * - one shared instance instead of one per page;
 * - the trim sweep is cancellable and is stopped on disconnect/shutdown;
 * - "Connect" turns into "Disconnect" as soon as the asynchronous connect starts.
 */
class AntennaTrackerUIViewModel : public QObject
{
    Q_OBJECT

public:
    enum class Axis { Pan, Tilt };
    Q_ENUM(Axis)
    enum class Field { Range, PwmRange, Center, Speed, Accel };
    Q_ENUM(Field)

    using PortEnumerator = std::function<QStringList()>;

    struct TrimSearchTiming
    {
        int settleMs = 4000; // MP10 Thread.Sleep(4000) after moving to the phase start
        int stepMs = 2000;   // MP10 Thread.Sleep(2000) per probed position
    };

    // service/telemetry are re-parented to the view model when they have no parent.
    explicit AntennaTrackerUIViewModel(AntennaTrackerSerialService *service,
                                       AntennaTrackerTelemetrySource *telemetry,
                                       QSettings *settings = nullptr,
                                       PortEnumerator portEnumerator = {},
                                       QObject *parent = nullptr);
    ~AntennaTrackerUIViewModel() override;

    // --- MP10 texts ---
    static QString Title();                    // "Antenna Tracker"
    static QString ConnectText();              // "Connect"
    static QString DisconnectText();           // "Disconnect"
    static QString ServoWarningText();         // "Misusing this interface can cause servo damage, use with caution!!!"
    static QString FindTrimPanLiveText();      // "Find Trim Pan (SiK Radio)"
    static QString FindTrimPanSerialText();    // "Find Trim Pan (Sik Radio)"
    static QString ManualSlewText();           // "Manual Slew (override point-at-vehicle)"
    static QString HomeCenterText();           // "Home / Center"
    static QString ConnectFirstText();         // "Connect to the tracker first."
    static QString NoSikRadioText();           // "No valid SiK radio detected."
    static QString SearchingTrimText();        // "Searching for best pan trim..."
    static QString TrimSearchCompleteText();   // "Pan trim search complete."
    static QString TrimSearchCancelledText();  // Qt: "Pan trim search cancelled."
    static QString CenterFailedText(const QString &reason);   // "Center failed: %1"
    static QString InvalidNumberText(const QString &detail);  // "Invalid number entered: %1"
    static QString NotAnIntegerText(const QString &field);    // "%1 must be an integer."
    static QString BelowMinimumText(const QString &field);    // "%1 is below the safe minimum."
    static QString PlaceholderText();          // "--"
    static QString SettingsGroup();            // "AntennaTracker"
    static QString FieldName(Axis axis, Field field);    // "pan range", "tilt PWM center", ...
    static QString SettingsKey(Axis axis, Field field);  // "TXT_panrange", ...
    static QString FormatAngle(double degrees);          // C# "0.0"
    static QString FormatTrim(double degrees);           // C# "0"

    AntennaTrackerSerialService *service() const { return m_service; }
    AntennaTrackerTelemetrySource *telemetry() const { return m_telemetry; }

    QStringList interfaces() const;
    QStringList bauds() const;
    QStringList ports() const { return m_ports; }

    QString selectedInterface() const { return m_selectedInterface; }
    QString selectedPort() const { return m_selectedPort; }
    QString selectedBaud() const { return m_selectedBaud; }
    QString field(Axis axis, Field field) const;
    double trim(Axis axis) const;
    double trimMin(Axis axis) const;
    double trimMax(Axis axis) const;
    bool reverse(Axis axis) const;

    bool manualMode() const { return m_manualMode; }
    double manualAzimuth() const { return m_manualAzimuth; }
    double manualElevation() const { return m_manualElevation; }
    QString vehicleAzimuth() const { return m_vehicleAzimuth; }
    QString vehicleElevation() const { return m_vehicleElevation; }
    QString commandedAzimuth() const { return m_commandedAzimuth; }
    QString commandedElevation() const { return m_commandedElevation; }

    QString connectText() const { return m_connectText; }
    QString status() const { return m_status; }
    bool controlsEnabled() const { return m_controlsEnabled; }
    bool speedAccelEnabled() const { return m_speedAccelEnabled; }
    // MP10 IsRunning: the service is Connecting or Connected.
    bool isRunning() const;
    bool isSearchingTrim() const { return m_search.active; }

    int loopIntervalMs() const { return m_loopIntervalMs; }
    void setLoopIntervalMs(int milliseconds);
    int loopTicks() const { return m_loopTicks; }
    TrimSearchTiming trimSearchTiming() const { return m_searchTiming; }
    void setTrimSearchTiming(const TrimSearchTiming &timing) { m_searchTiming = timing; }

    // MP10 Connect() parsing in MP10 order; *error carries the MP10 status text.
    AntennaTrackerSerialSettings buildSettings(QString *error) const;

public slots:
    void setSelectedInterface(const QString &value);
    void setSelectedPort(const QString &value);
    void setSelectedBaud(const QString &value);
    void setField(Axis axis, Field field, const QString &value);
    void setTrim(Axis axis, double value);
    void setReverse(Axis axis, bool value);
    void setManualMode(bool value);
    void setManualAzimuth(double value);
    void setManualElevation(double value);

    // MP10 RefreshPorts(): keep the selection when still present, else the first port.
    void refreshPorts();
    // MP10 Connect command: disconnects when running, otherwise validates and connects.
    void connectOrDisconnect();
    // MP10 HomeCenter: manual angles to 0 and one PanAndTilt(0, 0).
    void homeCenter();
    // MP10 FindTrimPan; a second call while sweeping cancels (Qt addition).
    void findTrimPan();
    void cancelTrimSearch();
    // MP10 IActivationAware / IDeactivationAware.
    void activate();
    void deactivate();
    // Save, stop the sweep and disconnect; for the owner's destructor/close path.
    void shutdown();
    void loadSettings();
    void saveSettings();

signals:
    void selectedInterfaceChanged(const QString &value);
    void selectedPortChanged(const QString &value);
    void selectedBaudChanged(const QString &value);
    void portsChanged(const QStringList &ports);
    void fieldChanged(AntennaTrackerUIViewModel::Axis axis,
                      AntennaTrackerUIViewModel::Field field, const QString &value);
    void trimChanged(AntennaTrackerUIViewModel::Axis axis, double value);
    void trimRangeChanged(AntennaTrackerUIViewModel::Axis axis, double minimum, double maximum);
    void reverseChanged(AntennaTrackerUIViewModel::Axis axis, bool value);
    void manualModeChanged(bool value);
    void manualAzimuthChanged(double value);
    void manualElevationChanged(double value);
    // vehicleAzimuth/vehicleElevation/commandedAzimuth/commandedElevation changed.
    void telemetryChanged();
    void connectTextChanged(const QString &value);
    void statusChanged(const QString &value);
    void controlsEnabledChanged(bool value);
    void speedAccelEnabledChanged(bool value);
    void runningChanged(bool running);
    void searchingTrimChanged(bool searching);

private:
    struct AxisState
    {
        QString fields[5];
        double trim = 0.0;
        double trimMin = -180.0;
        double trimMax = 180.0;
        bool reverse = false;
    };
    struct TrimSearch
    {
        bool active = false;
        int phase = 0;
        float pan = 0.0f;
        float panRange = 360.0f;
        float start = 0.0f;
        float end = 0.0f;
        float scale = 0.0f;
        float n = 0.0f;
        float lastSnr = 0.0f;
        float best = 0.0f;
        bool settling = false;
    };

    AxisState &axisState(Axis axis);
    const AxisState &axisState(Axis axis) const;
    void onServiceStateChanged(AntennaTrackerSerialService::State state, quint64 generation);
    void onServiceStatusChanged(const QString &status);
    void tick();
    void updateEnabledStates();
    void updateTiltTrimRange();
    void setStatus(const QString &status);
    void setTelemetryTexts(const QString &vehicleAz, const QString &vehicleEl,
                           const QString &commandedAz, const QString &commandedEl);
    void beginTrimPhase(int phase, float centre);
    void trimTimerElapsed();
    void finishTrimSearch(bool cancelled, bool announce);
    static bool ParseInt(const QString &text, int *value);
    static int ParseIntOr(const QString &text, int fallback);

    QPointer<AntennaTrackerSerialService> m_service;
    QPointer<AntennaTrackerTelemetrySource> m_telemetry;
    QSettings *m_settings = nullptr;
    PortEnumerator m_portEnumerator;
    QTimer *m_loopTimer = nullptr;
    QTimer *m_trimTimer = nullptr;
    int m_loopIntervalMs = 100;
    int m_loopTicks = 0;
    TrimSearchTiming m_searchTiming;
    TrimSearch m_search;

    QStringList m_ports;
    QString m_selectedInterface;
    QString m_selectedPort;
    QString m_selectedBaud;
    AxisState m_pan;
    AxisState m_tilt;
    bool m_manualMode = false;
    double m_manualAzimuth = 0.0;
    double m_manualElevation = 0.0;
    QString m_vehicleAzimuth;
    QString m_vehicleElevation;
    QString m_commandedAzimuth;
    QString m_commandedElevation;
    QString m_connectText;
    QString m_status;
    bool m_controlsEnabled = true;
    bool m_speedAccelEnabled = true;
    bool m_shutdown = false;
};

#endif // ANTENNATRACKERUIVIEWMODEL_H
