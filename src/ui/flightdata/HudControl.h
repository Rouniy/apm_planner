#ifndef HUDCONTROL_H
#define HUDCONTROL_H

#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QWidget>

class QContextMenuEvent;
class QPainter;
class QTimer;
class QMouseEvent;
class HudDisplaySettings;

class HudControl final : public QWidget
{
    Q_OBJECT

#define HUD_PROPERTY(Type, Name, Getter, Setter) \
    Q_PROPERTY(Type Name READ Getter WRITE Setter NOTIFY telemetryChanged)
    HUD_PROPERTY(double, Roll, roll, setRoll)
    HUD_PROPERTY(double, Pitch, pitch, setPitch)
    HUD_PROPERTY(double, Yaw, yaw, setYaw)
    HUD_PROPERTY(double, Alt, alt, setAlt)
    HUD_PROPERTY(double, AirSpeed, airSpeed, setAirSpeed)
    HUD_PROPERTY(double, GroundSpeed, groundSpeed, setGroundSpeed)
    HUD_PROPERTY(double, VerticalSpeed, verticalSpeed, setVerticalSpeed)
    HUD_PROPERTY(double, SatCount, satCount, setSatCount)
    HUD_PROPERTY(int, GpsFixType, gpsFixType, setGpsFixType)
    HUD_PROPERTY(bool, Armed, armed, setArmed)
    HUD_PROPERTY(bool, PrearmOk, prearmOk, setPrearmOk)
    HUD_PROPERTY(QString, Mode, mode, setMode)
    HUD_PROPERTY(double, BatteryVoltage, batteryVoltage, setBatteryVoltage)
    HUD_PROPERTY(int, BatteryRemaining, batteryRemaining, setBatteryRemaining)
    HUD_PROPERTY(bool, OverlayEnabled, overlayEnabled, setOverlayEnabled)
    HUD_PROPERTY(bool, ShowIcons, showIcons, setShowIcons)
    HUD_PROPERTY(bool, Russian, russian, setRussian)
    HUD_PROPERTY(int, BatteryCells, batteryCells, setBatteryCells)
    HUD_PROPERTY(bool, GroundBrown, groundBrown, setGroundBrown)
    HUD_PROPERTY(bool, SixteenByNine, sixteenByNine, setSixteenByNine)
    HUD_PROPERTY(bool, DisplayHeading, displayHeading, setDisplayHeading)
    HUD_PROPERTY(bool, DisplaySpeed, displaySpeed, setDisplaySpeed)
    HUD_PROPERTY(bool, DisplayAlt, displayAlt, setDisplayAlt)
    HUD_PROPERTY(bool, DisplayRollPitch, displayRollPitch, setDisplayRollPitch)
    HUD_PROPERTY(bool, DisplayGps, displayGps, setDisplayGps)
    HUD_PROPERTY(bool, DisplayBattery, displayBattery, setDisplayBattery)
    HUD_PROPERTY(bool, DisplayBattery2, displayBattery2, setDisplayBattery2)
    HUD_PROPERTY(bool, DisplayEkf, displayEkf, setDisplayEkf)
    HUD_PROPERTY(bool, DisplayVibe, displayVibe, setDisplayVibe)
    HUD_PROPERTY(bool, DisplayPrearm, displayPrearm, setDisplayPrearm)
    HUD_PROPERTY(bool, DisplayAoa, displayAoa, setDisplayAoa)
    HUD_PROPERTY(bool, DisplayXTrack, displayXTrack, setDisplayXTrack)
    HUD_PROPERTY(bool, DisplayConnection, displayConnection, setDisplayConnection)
    HUD_PROPERTY(double, NavBearing, navBearing, setNavBearing)
    HUD_PROPERTY(double, CurrentAmps, currentAmps, setCurrentAmps)
    HUD_PROPERTY(double, TargetAlt, targetAlt, setTargetAlt)
    HUD_PROPERTY(double, TargetSpeed, targetSpeed, setTargetSpeed)
    HUD_PROPERTY(QString, CustomItemsText, customItemsText, setCustomItemsText)
    HUD_PROPERTY(double, WindDir, windDir, setWindDir)
    HUD_PROPERTY(double, WindVel, windVel, setWindVel)
    HUD_PROPERTY(double, Aoa, aoa, setAoa)
    HUD_PROPERTY(double, Ssa, ssa, setSsa)
    HUD_PROPERTY(double, XTrackError, xTrackError, setXTrackError)
    HUD_PROPERTY(double, TurnRate, turnRate, setTurnRate)
    HUD_PROPERTY(double, WpDist, wpDist, setWpDist)
    HUD_PROPERTY(int, WpNo, wpNo, setWpNo)
    HUD_PROPERTY(double, BatteryVoltage2, batteryVoltage2, setBatteryVoltage2)
    HUD_PROPERTY(int, BatteryRemaining2, batteryRemaining2, setBatteryRemaining2)
    HUD_PROPERTY(double, CurrentAmps2, currentAmps2, setCurrentAmps2)
    HUD_PROPERTY(double, ThrottlePercent, throttlePercent, setThrottlePercent)
    HUD_PROPERTY(bool, Failsafe, failsafe, setFailsafe)
    HUD_PROPERTY(bool, SafetyActive, safetyActive, setSafetyActive)
    HUD_PROPERTY(double, LinkQuality, linkQuality, setLinkQuality)
#undef HUD_PROPERTY

public:
    explicit HudControl(QWidget *parent = nullptr,
                        HudDisplaySettings *displaySettings = nullptr);

    double roll() const { return m_roll; }
    double pitch() const { return m_pitch; }
    double yaw() const { return m_yaw; }
    double alt() const { return m_alt; }
    double airSpeed() const { return m_airSpeed; }
    double groundSpeed() const { return m_groundSpeed; }
    double verticalSpeed() const { return m_verticalSpeed; }
    double satCount() const { return m_satCount; }
    int gpsFixType() const { return m_gpsFixType; }
    bool armed() const { return m_armed; }
    bool prearmOk() const { return m_prearmOk; }
    QString mode() const { return m_mode; }
    double batteryVoltage() const { return m_batteryVoltage; }
    int batteryRemaining() const { return m_batteryRemaining; }
    bool overlayEnabled() const { return m_overlayEnabled; }
    bool showIcons() const { return m_showIcons; }
    bool russian() const { return m_russian; }
    int batteryCells() const { return m_batteryCells; }
    bool groundBrown() const { return m_groundBrown; }
    bool sixteenByNine() const { return m_sixteenByNine; }
    bool displayHeading() const { return m_displayHeading; }
    bool displaySpeed() const { return m_displaySpeed; }
    bool displayAlt() const { return m_displayAlt; }
    bool displayRollPitch() const { return m_displayRollPitch; }
    bool displayGps() const { return m_displayGps; }
    bool displayBattery() const { return m_displayBattery; }
    bool displayBattery2() const { return m_displayBattery2; }
    bool displayEkf() const { return m_displayEkf; }
    bool displayVibe() const { return m_displayVibe; }
    bool displayPrearm() const { return m_displayPrearm; }
    bool displayAoa() const { return m_displayAoa; }
    bool displayXTrack() const { return m_displayXTrack; }
    bool displayConnection() const { return m_displayConnection; }
    double navBearing() const { return m_navBearing; }
    double currentAmps() const { return m_currentAmps; }
    double targetAlt() const { return m_targetAlt; }
    double targetSpeed() const { return m_targetSpeed; }
    QString customItemsText() const { return m_customItemsText; }
    double windDir() const { return m_windDir; }
    double windVel() const { return m_windVel; }
    double aoa() const { return m_aoa; }
    double ssa() const { return m_ssa; }
    double xTrackError() const { return m_xTrackError; }
    double turnRate() const { return m_turnRate; }
    double wpDist() const { return m_wpDist; }
    int wpNo() const { return m_wpNo; }
    double batteryVoltage2() const { return m_batteryVoltage2; }
    int batteryRemaining2() const { return m_batteryRemaining2; }
    double currentAmps2() const { return m_currentAmps2; }
    double throttlePercent() const { return m_throttlePercent; }
    bool failsafe() const { return m_failsafe; }
    bool safetyActive() const { return m_safetyActive; }
    double linkQuality() const { return m_linkQuality; }

    void setVideoBackground(const QImage &image);
    QImage videoBackground() const { return m_videoBackground; }
    QRect contentViewport() const;
    QSize sizeHint() const override;

public slots:
    void setRoll(double value);
    void setPitch(double value);
    void setYaw(double value);
    void setAlt(double value);
    void setAirSpeed(double value);
    void setGroundSpeed(double value);
    void setVerticalSpeed(double value);
    void setSatCount(double value);
    void setGpsFixType(int value);
    void setArmed(bool value);
    void setPrearmOk(bool value);
    void setMode(const QString &value);
    void setBatteryVoltage(double value);
    void setBatteryRemaining(int value);
    void setOverlayEnabled(bool value);
    void setShowIcons(bool value);
    void setRussian(bool value);
    void setBatteryCells(int value);
    void setGroundBrown(bool value);
    void setSixteenByNine(bool value);
    void setDisplayHeading(bool value);
    void setDisplaySpeed(bool value);
    void setDisplayAlt(bool value);
    void setDisplayRollPitch(bool value);
    void setDisplayGps(bool value);
    void setDisplayBattery(bool value);
    void setDisplayBattery2(bool value);
    void setDisplayEkf(bool value);
    void setDisplayVibe(bool value);
    void setDisplayPrearm(bool value);
    void setDisplayAoa(bool value);
    void setDisplayXTrack(bool value);
    void setDisplayConnection(bool value);
    void setNavBearing(double value);
    void setCurrentAmps(double value);
    void setTargetAlt(double value);
    void setTargetSpeed(double value);
    void setCustomItemsText(const QString &value);
    void setWindDir(double value);
    void setWindVel(double value);
    void setAoa(double value);
    void setSsa(double value);
    void setXTrackError(double value);
    void setTurnRate(double value);
    void setWpDist(double value);
    void setWpNo(int value);
    void setBatteryVoltage2(double value);
    void setBatteryRemaining2(int value);
    void setCurrentAmps2(double value);
    void setThrottlePercent(double value);
    void setFailsafe(bool value);
    void setSafetyActive(bool value);
    void setLinkQuality(double value);
    void snapToValues();

signals:
    void telemetryChanged();
    void indicatorClicked(const QString &indicator);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private slots:
    void stepEase();
    void applyOverlayEnabled(bool enabled);

private:
    void changed();
    void drawHeadingTape(QPainter &painter, const QRectF &rect, double fontSize) const;
    void drawRollArc(QPainter &painter, const QRectF &rect, double headHeight) const;
    void drawScrollTape(QPainter &painter, const QRectF &rect, double value,
                        double target, bool labelsOnLeft, double fontSize) const;
    void drawVsi(QPainter &painter, const QRectF &altRect) const;
    void drawXTrack(QPainter &painter, const QRectF &rect, double headHeight) const;
    void drawAoaSsa(QPainter &painter, const QRectF &rect) const;
    void drawHaloText(QPainter &painter, const QPointF &at, const QString &text,
                      const QColor &color, double fontSize,
                      Qt::Alignment alignment = Qt::AlignLeft) const;
    QString gpsFixText() const;
    void loadDisplaySettings();
    void saveDisplaySetting(const QString &name, bool value);

    double m_roll = 0.0;
    double m_pitch = 0.0;
    double m_yaw = 0.0;
    double m_alt = 0.0;
    double m_airSpeed = 0.0;
    double m_groundSpeed = 0.0;
    double m_verticalSpeed = 0.0;
    double m_satCount = 0.0;
    int m_gpsFixType = 0;
    bool m_armed = false;
    bool m_prearmOk = false;
    QString m_mode = QStringLiteral("UNKNOWN");
    double m_batteryVoltage = 0.0;
    int m_batteryRemaining = 0;
    bool m_overlayEnabled = true;
    bool m_showIcons = true;
    bool m_russian = false;
    int m_batteryCells = 0;
    bool m_groundBrown = false;
    bool m_sixteenByNine = false;
    bool m_displayHeading = true;
    bool m_displaySpeed = true;
    bool m_displayAlt = true;
    bool m_displayRollPitch = true;
    bool m_displayGps = true;
    bool m_displayBattery = true;
    bool m_displayBattery2 = true;
    bool m_displayEkf = true;
    bool m_displayVibe = true;
    bool m_displayPrearm = true;
    bool m_displayAoa = false;
    bool m_displayXTrack = true;
    bool m_displayConnection = true;
    double m_navBearing = 0.0;
    double m_currentAmps = 0.0;
    double m_targetAlt = 0.0;
    double m_targetSpeed = 0.0;
    QString m_customItemsText;
    double m_windDir = 0.0;
    double m_windVel = 0.0;
    double m_aoa = 0.0;
    double m_ssa = 0.0;
    double m_xTrackError = 0.0;
    double m_turnRate = 0.0;
    double m_wpDist = 0.0;
    int m_wpNo = 0;
    double m_batteryVoltage2 = 0.0;
    int m_batteryRemaining2 = 0;
    double m_currentAmps2 = 0.0;
    double m_throttlePercent = 0.0;
    bool m_failsafe = false;
    bool m_safetyActive = false;
    double m_linkQuality = 0.0;
    QImage m_videoBackground;

    double m_easedRoll = 0.0;
    double m_easedPitch = 0.0;
    double m_easedYaw = 0.0;
    double m_easedAlt = 0.0;
    double m_easedAirSpeed = 0.0;
    double m_easedGroundSpeed = 0.0;
    double m_easedVerticalSpeed = 0.0;
    bool m_easeInitialized = false;
    QTimer *m_easeTimer = nullptr;
    QPointer<HudDisplaySettings> m_displaySettings;
    QRectF m_ekfRect;
    QRectF m_vibeRect;
    QRectF m_prearmRect;
};

#endif
