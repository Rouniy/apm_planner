#include "AntennaTrackerOutputs.h"

#include <climits>
#include <cmath>
#include <cstdlib>

// --- IAntennaTrackerOutput -------------------------------------------------------------

bool IAntennaTrackerOutput::init(QString *error)
{
    m_initialized = false;
    const QString problem = validateConfiguration();
    if (!problem.isEmpty()) {
        if (error) {
            *error = problem;
        }
        return false;
    }
    if (!m_writer) {
        if (error) {
            *error = WriterUnavailableText();
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    m_initialized = true;
    return true;
}

bool IAntennaTrackerOutput::setup()
{
    return m_initialized && write(setupCommands());
}

bool IAntennaTrackerOutput::panAndTilt(double pan, double tilt)
{
    if (!m_initialized) {
        return false;
    }
    QList<AntennaTrackerCommand> commands;
    if (!panAndTiltCommands(pan, tilt, &commands)) {
        return false;
    }
    return write(commands);
}

bool IAntennaTrackerOutput::write(const QList<AntennaTrackerCommand> &commands) const
{
    if (commands.isEmpty()) {
        return true; // MP10 ArduTracker/DegreeTracker Setup() returns true without writing
    }
    const Writer writer = m_writer;
    if (!writer) {
        return false;
    }
    for (const AntennaTrackerCommand &command : commands) {
        bool accepted = false;
        try {
            accepted = writer(command);
        } catch (...) {
            return false;
        }
        // A writer may synchronously close the output while handling a frame.
        // Do not continue a multi-command Maestro sequence after that callback.
        if (!accepted || !m_initialized) {
            return false;
        }
    }
    return true;
}

double IAntennaTrackerOutput::Wrap180(double input)
{
    if (input > 180) {
        return input - 360;
    }
    if (input < -180) {
        return input + 360;
    }
    return input;
}

short IAntennaTrackerOutput::Constrain(double input, double min, double max)
{
    if (input < min) {
        return ToInt16(min);
    }
    if (input > max) {
        return ToInt16(max);
    }
    return ToInt16(input);
}

int IAntennaTrackerOutput::ToInt32(double value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= 2147483648.0) {
        return INT_MAX;
    }
    if (value <= -2147483649.0) {
        return INT_MIN;
    }
    return static_cast<int>(value); // truncation toward zero
}

short IAntennaTrackerOutput::ToInt16(double value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= 32768.0) {
        return SHRT_MAX;
    }
    if (value <= -32769.0) {
        return SHRT_MIN;
    }
    return static_cast<short>(value);
}

QString IAntennaTrackerOutput::FormatFourDigits(int value)
{
    const qint64 magnitude = std::llabs(static_cast<qint64>(value));
    const QString digits = QString::number(magnitude).rightJustified(4, QLatin1Char('0'));
    return value < 0 ? QStringLiteral("-") + digits : digits;
}

QByteArray IAntennaTrackerOutput::FormatPanTiltLine(int pan, int tilt)
{
    return QStringLiteral("!!!PAN:%1,TLT:%2\n")
        .arg(FormatFourDigits(pan), FormatFourDigits(tilt))
        .toLatin1();
}

QString IAntennaTrackerOutput::InvalidPanRangeText()
{
    return tr("Invalid pan range.");
}

QString IAntennaTrackerOutput::InvalidTiltRangeText()
{
    return tr("Invalid tilt range.");
}

QString IAntennaTrackerOutput::WriterUnavailableText()
{
    return tr("Antenna tracker output writer is unavailable.");
}

double IAntennaTrackerOutput::AngleRange(int start, int end)
{
    return static_cast<double>(std::llabs(static_cast<qint64>(start) - end));
}

bool IAntennaTrackerOutput::FiniteAngles(double pan, double tilt)
{
    return std::isfinite(pan) && std::isfinite(tilt);
}

namespace {

// MP10 only rejects equal endpoints. Reversed bounds make its Constrain helper
// treat the upper bound as the lower one and can drive an unexpected extreme,
// so the Qt port fails closed unless each angular interval is ascending.
QString validatePwmRanges(const IAntennaTrackerOutput &output)
{
    if (output.panStartRange() >= output.panEndRange()) {
        return IAntennaTrackerOutput::InvalidPanRangeText();
    }
    if (output.tiltStartRange() >= output.tiltEndRange()) {
        return IAntennaTrackerOutput::InvalidTiltRangeText();
    }
    return QString();
}

// C# `short target; target *= 4;` wraps modulo 2^16.
short timesFourWrapped(short target)
{
    const quint16 bits = static_cast<quint16>((static_cast<int>(target) * 4) & 0xFFFF);
    return bits >= 0x8000 ? static_cast<short>(static_cast<int>(bits) - 0x10000)
                          : static_cast<short>(bits);
}

} // namespace

// --- Maestro ---------------------------------------------------------------------------

QString MaestroAntennaTrackerOutput::interfaceName() const
{
    return AntennaTrackerOutputFactory::Maestro();
}

QString MaestroAntennaTrackerOutput::validateConfiguration() const
{
    return validatePwmRanges(*this);
}

AntennaTrackerCommand MaestroAntennaTrackerOutput::CompactCommand(quint8 command, quint8 address,
                                                                  int data)
{
    // Bits 0..6 and 7..13 of the two's complement value, as MP10's int masks.
    const quint32 bits = static_cast<quint32>(data);
    AntennaTrackerCommand result;
    result.payload.resize(4);
    result.payload[0] = static_cast<char>(command);
    result.payload[1] = static_cast<char>(address);
    result.payload[2] = static_cast<char>(bits & 0x7Fu);
    result.payload[3] = static_cast<char>((bits >> 7) & 0x7Fu);
    result.discardInputFirst = true;
    return result;
}

QList<AntennaTrackerCommand> MaestroAntennaTrackerOutput::setupCommands() const
{
    return {CompactCommand(SetSpeed, PanAddress, panSpeed()),
            CompactCommand(SetSpeed, TiltAddress, tiltSpeed()),
            CompactCommand(SetAcceleration, PanAddress, panAccel()),
            CompactCommand(SetAcceleration, TiltAddress, tiltAccel())};
}

short MaestroAntennaTrackerOutput::panTarget(double angle) const
{
    const double angleRange = AngleRange(panStartRange(), panEndRange());
    const double pulseWidth = panPwmRange() / angleRange * Wrap180(angle - trimPan()) *
                                  panReverseSign() +
                              panPwmCenter();
    const short target = Constrain(pulseWidth, panPwmCenter() - panPwmRange() / 2.0,
                                   panPwmCenter() + panPwmRange() / 2.0);
    return timesFourWrapped(target);
}

short MaestroAntennaTrackerOutput::tiltTarget(double angle) const
{
    const double angleRange = AngleRange(tiltStartRange(), tiltEndRange());
    const double pulseWidth = tiltPwmRange() / angleRange * (angle - trimTilt()) *
                                  tiltReverseSign() +
                              tiltPwmCenter();
    const short target = Constrain(pulseWidth, tiltPwmCenter() - tiltPwmRange() / 2.0,
                                   tiltPwmCenter() + tiltPwmRange() / 2.0);
    return timesFourWrapped(target);
}

bool MaestroAntennaTrackerOutput::panAndTiltCommands(double pan, double tilt,
                                                     QList<AntennaTrackerCommand> *commands) const
{
    if (!FiniteAngles(pan, tilt) || !validateConfiguration().isEmpty()) {
        return false;
    }
    double panAngle = pan;
    double tiltAngle = tilt;
    // MP10: 180 + 180 degree servos flip over the top instead of panning around.
    // The flipped pan target is trimmed here and again inside Pan(), as in MP10.
    if (std::llabs(static_cast<qint64>(tiltStartRange()) - tiltEndRange()) > 120) {
        const double target = Wrap180(pan - trimPan());
        if (std::fabs(target) > 90) {
            tiltAngle = 180 - tilt;
            panAngle = target;
        }
    }
    QList<AntennaTrackerCommand> result;
    result.append(CompactCommand(SetTarget, TiltAddress, tiltTarget(tiltAngle)));
    result.append(CompactCommand(SetTarget, PanAddress, panTarget(panAngle)));
    if (commands) {
        *commands = result;
    }
    return true;
}

// --- ArduTracker -----------------------------------------------------------------------

QString ArduAntennaTrackerOutput::interfaceName() const
{
    return AntennaTrackerOutputFactory::ArduTracker();
}

QString ArduAntennaTrackerOutput::validateConfiguration() const
{
    return validatePwmRanges(*this);
}

int ArduAntennaTrackerOutput::panPwm(double angle) const
{
    const double range = AngleRange(panStartRange(), panEndRange());
    const short pointAt = Constrain(Wrap180(angle - trimPan()), panStartRange(), panEndRange());
    // MP10: PanPWMRange / 2 is integer division.
    return ToInt32(pointAt / range * 2.0 * (panPwmRange() / 2) * panReverseSign() +
                   panPwmCenter());
}

int ArduAntennaTrackerOutput::tiltPwm(double angle) const
{
    const double range = AngleRange(tiltStartRange(), tiltEndRange());
    const short pointAt = Constrain(angle - trimTilt(), tiltStartRange(), tiltEndRange());
    return ToInt32(pointAt / range * 2.0 * (tiltPwmRange() / 2) * tiltReverseSign() +
                   tiltPwmCenter());
}

bool ArduAntennaTrackerOutput::panAndTiltCommands(double pan, double tilt,
                                                  QList<AntennaTrackerCommand> *commands) const
{
    if (!FiniteAngles(pan, tilt) || !validateConfiguration().isEmpty()) {
        return false;
    }
    AntennaTrackerCommand line;
    line.payload = FormatPanTiltLine(panPwm(pan), tiltPwm(tilt));
    if (commands) {
        *commands = {line};
    }
    return true;
}

// --- DegreeTracker ---------------------------------------------------------------------

QString DegreeAntennaTrackerOutput::interfaceName() const
{
    return AntennaTrackerOutputFactory::DegreeTracker();
}

bool DegreeAntennaTrackerOutput::panAndTiltCommands(double pan, double tilt,
                                                    QList<AntennaTrackerCommand> *commands) const
{
    if (!FiniteAngles(pan, tilt)) {
        return false;
    }
    AntennaTrackerCommand line;
    line.payload = FormatPanTiltLine(ToInt32(pan * 10), ToInt32(tilt * 10));
    if (commands) {
        *commands = {line};
    }
    return true;
}

// --- factory ---------------------------------------------------------------------------

QStringList AntennaTrackerOutputFactory::InterfaceNames()
{
    return {Maestro(), ArduTracker(), DegreeTracker()};
}

std::unique_ptr<IAntennaTrackerOutput> AntennaTrackerOutputFactory::Create(
    const QString &interfaceName)
{
    if (interfaceName == Maestro()) {
        return std::make_unique<MaestroAntennaTrackerOutput>();
    }
    if (interfaceName == ArduTracker()) {
        return std::make_unique<ArduAntennaTrackerOutput>();
    }
    if (interfaceName == DegreeTracker()) {
        return std::make_unique<DegreeAntennaTrackerOutput>();
    }
    return nullptr;
}

QString AntennaTrackerOutputFactory::UnknownInterfaceText()
{
    return tr("Unknown antenna tracker interface.");
}
