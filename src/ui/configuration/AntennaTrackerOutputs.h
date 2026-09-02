#ifndef ANTENNATRACKEROUTPUTS_H
#define ANTENNATRACKEROUTPUTS_H

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <utility>

/*
 * Pure protocol slice of Mission Planner 10's serial antenna tracker outputs
 * (Services/AntennaTrackerOutputs.cs): the Maestro compact byte protocol and
 * the ArduTracker / DegreeTracker text protocols. No serial port lives here;
 * the owner (the future AntennaTrackerSerialService) injects a writer and
 * keeps the QSerialPort. Everything is deterministic and platform independent.
 */

// One wire write. Maestro discards pending input before every command
// (MP10 Serial.DiscardInBuffer()); the text protocols write the line only.
struct AntennaTrackerCommand
{
    QByteArray payload;
    bool discardInputFirst = false;

    bool operator==(const AntennaTrackerCommand &other) const
    {
        return payload == other.payload && discardInputFirst == other.discardInputFirst;
    }
    bool operator!=(const AntennaTrackerCommand &other) const { return !(*this == other); }
};

class IAntennaTrackerOutput
{
    Q_DECLARE_TR_FUNCTIONS(IAntennaTrackerOutput)

public:
    // Returns false when the bytes could not be written; the sequence stops there.
    using Writer = std::function<bool(const AntennaTrackerCommand &command)>;

    virtual ~IAntennaTrackerOutput() = default;

    // Thread confinement is intentional: the future serial service owns the
    // output, writer and QSerialPort in one thread and queues all UI changes
    // into that thread. None of these methods may be called concurrently.

    virtual QString interfaceName() const = 0;

    // MP10 IAntennaTrackerOutput properties (all default to 0 / false like C#).
    double trimPan() const { return m_trimPan; }
    void setTrimPan(double value) { m_trimPan = value; }
    double trimTilt() const { return m_trimTilt; }
    void setTrimTilt(double value) { m_trimTilt = value; }
    int panStartRange() const { return m_panStartRange; }
    void setPanStartRange(int value) { m_panStartRange = value; }
    int tiltStartRange() const { return m_tiltStartRange; }
    void setTiltStartRange(int value) { m_tiltStartRange = value; }
    int panEndRange() const { return m_panEndRange; }
    void setPanEndRange(int value) { m_panEndRange = value; }
    int tiltEndRange() const { return m_tiltEndRange; }
    void setTiltEndRange(int value) { m_tiltEndRange = value; }
    int panPwmRange() const { return m_panPwmRange; }
    void setPanPwmRange(int value) { m_panPwmRange = value; }
    int tiltPwmRange() const { return m_tiltPwmRange; }
    void setTiltPwmRange(int value) { m_tiltPwmRange = value; }
    int panPwmCenter() const { return m_panPwmCenter; }
    void setPanPwmCenter(int value) { m_panPwmCenter = value; }
    int tiltPwmCenter() const { return m_tiltPwmCenter; }
    void setTiltPwmCenter(int value) { m_tiltPwmCenter = value; }
    int panSpeed() const { return m_panSpeed; }
    void setPanSpeed(int value) { m_panSpeed = value; }
    int tiltSpeed() const { return m_tiltSpeed; }
    void setTiltSpeed(int value) { m_tiltSpeed = value; }
    int panAccel() const { return m_panAccel; }
    void setPanAccel(int value) { m_panAccel = value; }
    int tiltAccel() const { return m_tiltAccel; }
    void setTiltAccel(int value) { m_tiltAccel = value; }
    bool panReverse() const { return m_panReverse; }
    void setPanReverse(bool value) { m_panReverse = value; }
    bool tiltReverse() const { return m_tiltReverse; }
    void setTiltReverse(bool value) { m_tiltReverse = value; }

    // --- pure payload API (no I/O) ---------------------------------------------------
    // Empty when the configuration is usable, otherwise MP10's
    // "Invalid pan range." / "Invalid tilt range.".
    virtual QString validateConfiguration() const { return QString(); }
    // MP10 Setup(): Maestro speed/acceleration commands; empty for the text protocols.
    virtual QList<AntennaTrackerCommand> setupCommands() const { return {}; }
    // MP10 PanAndTilt() payloads in wire order. Returns false and leaves *commands
    // untouched when the configuration is invalid or an angle is not finite.
    virtual bool panAndTiltCommands(double pan, double tilt,
                                    QList<AntennaTrackerCommand> *commands) const = 0;

    // --- MP10 lifecycle over the injected writer ----------------------------------------
    void setWriter(Writer writer) { m_writer = std::move(writer); }
    bool hasWriter() const { return static_cast<bool>(m_writer); }
    // MP10 Init(out error): validates before anything is written. The owner
    // opens the port and installs its writer before calling init(); port
    // ownership and setup rollback remain in the serial service.
    bool init(QString *error = nullptr);
    bool isInitialized() const { return m_initialized; }
    // MP10 Setup()/PanAndTilt(): false until init() succeeded, false when a write fails.
    bool setup();
    bool panAndTilt(double pan, double tilt);
    // MP10 Close(): later commands are refused until init() runs again.
    void close() { m_initialized = false; }

    // --- shared MP10 arithmetic ---------------------------------------------------------
    // Single wrap: > 180 subtracts 360, < -180 adds 360 (MP10 Wrap180).
    static double Wrap180(double input);
    // MP10 Constrain(): (short) casts of the bound or the input.
    static short Constrain(double input, double min, double max);
    // .NET (int)/(short) double casts: truncate toward zero, saturate, NaN -> 0.
    static int ToInt32(double value);
    static short ToInt16(double value);
    // C# "{0:0000}": at least four digits, '-' in front of a negative value.
    static QString FormatFourDigits(int value);
    // "!!!PAN:0000,TLT:0000\n" as ASCII.
    static QByteArray FormatPanTiltLine(int pan, int tilt);

    static QString InvalidPanRangeText();
    static QString InvalidTiltRangeText();
    static QString WriterUnavailableText();

protected:
    // Writes every command in order through the writer; stops at the first refusal.
    bool write(const QList<AntennaTrackerCommand> &commands) const;
    int panReverseSign() const { return m_panReverse ? -1 : 1; }
    int tiltReverseSign() const { return m_tiltReverse ? -1 : 1; }
    // MP10 Math.Abs(start - end) as a double.
    static double AngleRange(int start, int end);
    static bool FiniteAngles(double pan, double tilt);

private:
    Writer m_writer;
    bool m_initialized = false;
    double m_trimPan = 0.0;
    double m_trimTilt = 0.0;
    int m_panStartRange = 0;
    int m_tiltStartRange = 0;
    int m_panEndRange = 0;
    int m_tiltEndRange = 0;
    int m_panPwmRange = 0;
    int m_tiltPwmRange = 0;
    int m_panPwmCenter = 0;
    int m_tiltPwmCenter = 0;
    int m_panSpeed = 0;
    int m_tiltSpeed = 0;
    int m_panAccel = 0;
    int m_tiltAccel = 0;
    bool m_panReverse = false;
    bool m_tiltReverse = false;
};

// Pololu Maestro compact protocol: channel 0 = pan, channel 1 = tilt.
class MaestroAntennaTrackerOutput final : public IAntennaTrackerOutput
{
public:
    static constexpr quint8 SetTarget = 0x84;
    static constexpr quint8 SetSpeed = 0x87;
    static constexpr quint8 SetAcceleration = 0x89;
    static constexpr quint8 PanAddress = 0;
    static constexpr quint8 TiltAddress = 1;

    QString interfaceName() const override;
    QString validateConfiguration() const override;
    QList<AntennaTrackerCommand> setupCommands() const override;
    bool panAndTiltCommands(double pan, double tilt,
                            QList<AntennaTrackerCommand> *commands) const override;

    // MP10 Pan()/Tilt() targets: clamped pulse width times four (quarter microseconds).
    short panTarget(double angle) const;
    short tiltTarget(double angle) const;

    // {command, address, data & 0x7F, (data >> 7) & 0x7F} with the input discarded first.
    static AntennaTrackerCommand CompactCommand(quint8 command, quint8 address, int data);
};

// ArduTracker text protocol: calculated PWM as "!!!PAN:1750,TLT:1777\n".
class ArduAntennaTrackerOutput final : public IAntennaTrackerOutput
{
public:
    QString interfaceName() const override;
    QString validateConfiguration() const override;
    bool panAndTiltCommands(double pan, double tilt,
                            QList<AntennaTrackerCommand> *commands) const override;

    // MP10 Pan()/Tilt() pulse widths.
    int panPwm(double angle) const;
    int tiltPwm(double angle) const;
};

// DegreeTracker text protocol: tenths of a degree truncated toward zero as
// "!!!PAN:0123,TLT:-0056\n". Ranges, PWM, trim and reverse do not apply.
class DegreeAntennaTrackerOutput final : public IAntennaTrackerOutput
{
public:
    QString interfaceName() const override;
    bool panAndTiltCommands(double pan, double tilt,
                            QList<AntennaTrackerCommand> *commands) const override;
};

class AntennaTrackerOutputFactory
{
    Q_DECLARE_TR_FUNCTIONS(AntennaTrackerOutputFactory)

public:
    static QString Maestro() { return QStringLiteral("Maestro"); }
    static QString ArduTracker() { return QStringLiteral("ArduTracker"); }
    static QString DegreeTracker() { return QStringLiteral("DegreeTracker"); }
    // MP10 InterfaceNames order: Maestro, ArduTracker, DegreeTracker.
    static QStringList InterfaceNames();
    // nullptr for an unknown name (MP10 throws ArgumentOutOfRangeException).
    static std::unique_ptr<IAntennaTrackerOutput> Create(const QString &interfaceName);
    // MP10 exception text for an unknown name.
    static QString UnknownInterfaceText();
};

#endif // ANTENNATRACKEROUTPUTS_H
