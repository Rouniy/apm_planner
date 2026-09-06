#include "JoystickConfiguration.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <quazip.h>
#include <quazipfile.h>
#include <quazipfileinfo.h>
#include <quazipnewinfo.h>
#include <algorithm>
#include <cmath>

namespace {
using C = JoystickConfiguration;
constexpr int MaximumArchive = 8 * 1024 * 1024;
constexpr int MaximumMember = 1024 * 1024;
bool fail(QString *error, const QString &s) { if (error) *error = s; return false; }
QStringList standardAxes()
{
    return QStringLiteral("None Pass ARx ARy ARz AX AY AZ FRx FRy FRz FX FY FZ Rx Ry Rz VRx VRy VRz VX VY VZ X Y Z Slider1 Slider2 Hatud1 Hatlr2 Custom1 Custom2 UINT16_MAX").split(' ');
}
QStringList nativeAliases()
{ return QStringLiteral("X Y Z Rx Ry Rz Slider1 AZ AY AX Slider2").split(' '); }
QString portableAxis(const QString &axis)
{
    if (standardAxes().contains(axis)) return axis;
    if (axis.startsWith("Axis")) {
        bool ok = false; const int index = axis.mid(4).toInt(&ok);
        const auto aliases = nativeAliases();
        if (ok && index >= 0 && index < aliases.size()) return aliases[index];
    }
    return {};
}
bool validAxis(const QString &axis)
{
    if (standardAxes().contains(axis)) return true;
    static const QRegularExpression extra(QStringLiteral("^(?:Axis(\\d+)|Hat(\\d+)([XY]))$"));
    const auto m = extra.match(axis);
    if (!m.hasMatch()) return false;
    bool ok = false;
    const int index = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toInt(&ok);
    return ok && index >= 0 && index < C::MaximumControls;
}
QByteArray json(const C::Profile &profile)
{
    QJsonObject root{{"version", 1}, {"deviceId", profile.deviceId}, {"deviceName", profile.deviceName},
                     {"firmware", profile.firmware}, {"elevons", profile.elevons}, {"manualControl", profile.manualControl}};
    QJsonArray channels, buttons;
    for (const auto &c : profile.channels)
        channels.append(QJsonObject{{"channel", c.channel}, {"axis", c.axis}, {"reverse", c.reverse}, {"expo", c.expo}});
    for (const auto &b : profile.buttons)
        buttons.append(QJsonObject{{"buttonno", b.buttonno}, {"function", b.function}, {"mode", b.mode},
            {"p1", b.p1}, {"p2", b.p2}, {"p3", b.p3}, {"p4", b.p4}, {"state", b.state}});
    root.insert("channels", channels); root.insert("buttons", buttons);
    QJsonObject devices;
    for (auto d = profile.calibration.cbegin(); d != profile.calibration.cend(); ++d) {
        QJsonObject axes;
        for (auto a = d->cbegin(); a != d->cend(); ++a)
            axes.insert(QString::number(a.key()), QJsonArray{a->minimum, a->maximum});
        devices.insert(d.key(), axes);
    }
    root.insert("calibration", devices);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
bool parseJson(const QByteArray &bytes, C::Profile *profile, QString *error)
{
    if (bytes.size() > MaximumMember) return fail(error, "Joystick profile exceeds 1 MiB.");
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return fail(error, "Invalid joystick profile JSON.");
    const auto root = doc.object();
    if (root.value("version").toInt() != 1) return fail(error, "Unsupported joystick profile version.");
    C::Profile p = C::defaults();
    p.deviceId = root.value("deviceId").toString(); p.deviceName = root.value("deviceName").toString();
    p.firmware = root.value("firmware").toString();
    p.elevons = root.value("elevons").toBool(); p.manualControl = root.value("manualControl").toBool();
    const auto channels = root.value("channels").toArray(), buttons = root.value("buttons").toArray();
    if (channels.size() > C::ChannelCount || buttons.size() > C::MaximumControls)
        return fail(error, "Joystick profile row limit exceeded.");
    for (int i = 0; i < channels.size(); ++i) {
        const auto o = channels[i].toObject();
        p.channels[i] = {i + 1, o.value("axis").toString("None"), o.value("reverse").toBool(), o.value("expo").toInt()};
    }
    for (int i = 0; i < buttons.size(); ++i) {
        const auto o = buttons[i].toObject(); auto &b = p.buttons[i];
        b.buttonno = o.value("buttonno").toInt(-1); b.function = o.value("function").toString("ChangeMode");
        b.mode = o.value("mode").toString(); b.p1 = o.value("p1").toDouble(); b.p2 = o.value("p2").toDouble();
        b.p3 = o.value("p3").toDouble(); b.p4 = o.value("p4").toDouble(); b.state = o.value("state").toBool();
    }
    const auto calibration = root.value("calibration").toObject();
    if (calibration.size() > 32) return fail(error, "At most 32 device calibration profiles are supported.");
    for (auto d = calibration.begin(); d != calibration.end(); ++d) {
        const auto axes = d.value().toObject();
        for (auto a = axes.begin(); a != axes.end(); ++a) {
            bool ok = false; const int index = a.key().toInt(&ok); const auto r = a.value().toArray();
            if (!ok || index < 0 || index >= C::MaximumControls || r.size() != 2)
                return fail(error, "Invalid joystick calibration axis.");
            C::Range range{r[0].toInt(-99999), r[1].toInt(99999)};
            if (!C::validRange(range)) return fail(error, "Invalid joystick calibration range.");
            p.calibration[d.key()].insert(index, range);
        }
    }
    if (!C::validate(p, error)) return false;
    *profile = p; return true;
}
QByteArray xmlChannels(const C::Profile &p)
{
    QByteArray out; QXmlStreamWriter w(&out); w.setAutoFormatting(true); w.writeStartDocument(); w.writeStartElement("ArrayOfJoyChannel");
    // MP arrays are indexed by channel, with an unused element zero and slots
    // through 19. Export that exact shape so MP can index RC1..16 directly.
    for (int i = 0; i < 20; ++i) {
        C::Channel c; c.channel = i;
        if (i >= 1 && i <= C::ChannelCount) c = p.channels[i - 1];
        w.writeStartElement("JoyChannel"); w.writeTextElement("channel", QString::number(c.channel));
        w.writeTextElement("axis", portableAxis(c.axis)); w.writeTextElement("reverse", c.reverse ? "true" : "false");
        w.writeTextElement("expo", QString::number(c.expo)); w.writeEndElement();
    }
    w.writeEndElement(); w.writeEndDocument(); return out;
}
QByteArray xmlButtons(const C::Profile &p)
{
    QByteArray out; QXmlStreamWriter w(&out); w.setAutoFormatting(true); w.writeStartDocument(); w.writeStartElement("ArrayOfJoyButton");
    for (const auto &b : p.buttons) {
        w.writeStartElement("JoyButton"); w.writeTextElement("buttonno", QString::number(b.buttonno));
        w.writeTextElement("function", b.function);
        if (!b.mode.isEmpty()) w.writeTextElement("mode", b.mode);
        w.writeTextElement("p1", QString::number(b.p1, 'g', 17)); w.writeTextElement("p2", QString::number(b.p2, 'g', 17));
        w.writeTextElement("p3", QString::number(b.p3, 'g', 17)); w.writeTextElement("p4", QString::number(b.p4, 'g', 17));
        w.writeTextElement("state", b.state ? "true" : "false"); w.writeEndElement();
    }
    w.writeEndElement(); w.writeEndDocument(); return out;
}
bool parseXml(const QByteArray &bytes, bool channels, C::Profile *p, QString *error)
{
    QXmlStreamReader r(bytes); int rows = 0;
    const QString root = channels ? "ArrayOfJoyChannel" : "ArrayOfJoyButton";
    const QString row = channels ? "JoyChannel" : "JoyButton";
    bool rootSeen = false;
    while (!r.atEnd()) {
        const auto token = r.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference)
            return fail(error, "DTD/entities are not permitted in joystick XML.");
        if (!r.isStartElement()) continue;
        if (!rootSeen) { if (r.name() != root) return fail(error, "Unexpected joystick XML root."); rootSeen = true; continue; }
        if (r.name() != row) return fail(error, "Unexpected joystick XML element.");
        if (++rows > (channels ? 20 : C::MaximumControls)) return fail(error, "Too many joystick XML rows.");
        QMap<QString, QString> fields;
        while (r.readNextStartElement()) {
            const QString key = r.name().toString();
            if (fields.contains(key)) return fail(error, "Duplicate joystick XML field.");
            fields.insert(key, r.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement));
        }
        bool ok = true;
        const auto integer = [&](const QString &name, int fallback) {
            if (!fields.contains(name)) return fallback;
            bool converted = false; const int v = fields.value(name).toInt(&converted); ok = ok && converted; return v;
        };
        const auto boolean = [&](const QString &name) {
            const QString value = fields.value(name, "false");
            if (value != "false" && value != "true" && value != "0" && value != "1") ok = false;
            return value == "true" || value == "1";
        };
        if (channels) {
            // JoystickBase indexes the XML array itself, not JoyChannel.channel.
            // Unconfigured array slots legitimately contain channel=0 repeatedly.
            const int declared = integer("channel", rows - 1);
            if (declared < 0 || declared >= 20) return fail(error, "Invalid RC channel index.");
            const int index = rows - 1;
            C::Channel c{index, fields.value("axis", "None"), boolean("reverse"), integer("expo", 0)};
            if (!validAxis(c.axis) || c.expo < -100 || c.expo > 100) ok = false;
            if (index >= 1 && index <= C::ChannelCount) p->channels[index - 1] = c;
            else if (c.axis != "None") return fail(error, "This setup supports RC1..16; the archive maps an additional RC channel.");
        } else {
            auto &b = p->buttons[rows - 1]; b.buttonno = integer("buttonno", -1);
            b.function = fields.value("function", "ChangeMode"); b.mode = fields.value("mode"); b.state = boolean("state");
            double *values[] = {&b.p1, &b.p2, &b.p3, &b.p4};
            for (int i = 0; i < 4; ++i) {
                const QString key = "p" + QString::number(i + 1);
                if (fields.contains(key)) { bool converted = false; *values[i] = fields.value(key).toDouble(&converted); ok = ok && converted; }
            }
        }
        if (!ok) return fail(error, "Invalid joystick XML field value.");
    }
    if (r.hasError() || !rootSeen) return fail(error, "Malformed joystick XML: " + r.errorString());
    return true;
}
}

JoystickConfiguration::Profile JoystickConfiguration::defaults()
{
    Profile p; p.channels.resize(ChannelCount); p.buttons.resize(MaximumControls);
    for (int i = 0; i < ChannelCount; ++i) p.channels[i].channel = i + 1;
    return p;
}
void JoystickConfiguration::normalize(Profile *p)
{
    if (!p) return;
    p->channels.resize(ChannelCount); p->buttons.resize(MaximumControls);
    for (int i = 0; i < ChannelCount; ++i) { p->channels[i].channel = i + 1; p->channels[i].expo = qBound(-100, p->channels[i].expo, 100); }
    for (auto &b : p->buttons) b.buttonno = qBound(-1, b.buttonno, MaximumControls - 1);
}
QStringList JoystickConfiguration::buttonFunctions()
{
    return QStringLiteral("ChangeMode Do_Set_Relay Do_Repeat_Relay Do_Set_Servo Do_Repeat_Servo Arm Disarm Digicam_Control TakeOff Mount_Mode Toggle_Pan_Stab Gimbal_pnt_track Mount_Control_0 Button_axis0 Button_axis1").split(' ');
}
bool JoystickConfiguration::validate(const Profile &p, QString *error)
{
    if (error) error->clear();
    if (p.channels.size() != ChannelCount || p.buttons.size() != MaximumControls) return fail(error, "Expected 16 RC channels and 128 button slots.");
    if (p.deviceId.size() > 512 || p.deviceName.size() > 512 || p.firmware.size() > 64
            || (!p.firmware.isEmpty() && !QRegularExpression("^[A-Za-z0-9_-]+$").match(p.firmware).hasMatch()))
        return fail(error, "Invalid device or firmware identity.");
    for (int i = 0; i < p.channels.size(); ++i) {
        const auto &c = p.channels[i];
        if (c.channel != i + 1 || !validAxis(c.axis) || c.expo < -100 || c.expo > 100)
            return fail(error, "Invalid RC channel axis/index/expo.");
    }
    for (const auto &b : p.buttons) {
        if (b.buttonno < -1 || b.buttonno >= MaximumControls || !buttonFunctions().contains(b.function) || b.mode.size() > 128
                || !std::isfinite(b.p1) || !std::isfinite(b.p2) || !std::isfinite(b.p3) || !std::isfinite(b.p4)
                || std::abs(b.p1) > 3.402823466e38 || std::abs(b.p2) > 3.402823466e38
                || std::abs(b.p3) > 3.402823466e38 || std::abs(b.p4) > 3.402823466e38)
            return fail(error, "Invalid button index/function/mode or non-finite parameter.");
    }
    if (p.calibration.size() > 32) return fail(error, "At most 32 device calibrations are supported.");
    for (auto d = p.calibration.cbegin(); d != p.calibration.cend(); ++d) {
        if (d.key().size() > 512 || d->size() > MaximumControls) return fail(error, "Invalid calibration device.");
        for (auto a = d->cbegin(); a != d->cend(); ++a)
            if (a.key() < 0 || a.key() >= MaximumControls || !validRange(a.value())) return fail(error, "Invalid calibration range.");
    }
    return true;
}
QStringList JoystickConfiguration::axisNames(int axes, int hats)
{
    QStringList result = standardAxes();
    for (int i = 0; i < qBound(0, axes, MaximumControls); ++i) result += "Axis" + QString::number(i);
    for (int i = 0; i < qBound(0, hats, MaximumControls); ++i) { result += "Hat" + QString::number(i) + "X"; result += "Hat" + QString::number(i) + "Y"; }
    return result;
}
bool JoystickConfiguration::validRange(const Range &r)
{ return r.minimum >= -32768 && r.maximum <= 32767 && qint64(r.maximum) - r.minimum >= 4096; }
quint16 JoystickConfiguration::normalizeAxis(qint16 raw, const Range &r)
{
    if (!validRange(r)) return quint16(int(raw) + 32768);
    if (raw <= r.minimum) return 0;
    if (raw >= r.maximum) return 65535;
    const qint64 span = r.maximum - r.minimum;
    return quint16(((qint64(raw) - r.minimum) * 65535 + span / 2) / span);
}
bool JoystickConfiguration::axisValue(const QString &name, const QVector<quint16> &axes, const QVector<quint8> &hats, quint16 *value)
{
    if (!value) return false;
    int index = -1;
    const QStringList aliases = nativeAliases();
    index = aliases.indexOf(name);
    if (name.startsWith("Axis")) { bool ok = false; index = name.mid(4).toInt(&ok); if (!ok) index = -1; }
    if (index >= 0) { if (index >= axes.size()) return false; *value = axes[index]; return true; }
    int hat = -1; bool horizontal = false;
    if (name == "Hatud1") hat = 0;
    else if (name == "Hatlr2") { hat = 0; horizontal = true; }
    else {
        const auto m = QRegularExpression("^Hat(\\d+)([XY])$").match(name);
        if (m.hasMatch()) { hat = m.captured(1).toInt(); horizontal = m.captured(2) == "X"; }
    }
    if (hat < 0 || hat >= hats.size()) return false;
    const int bits = hats[hat];
    const int direction = horizontal ? ((bits & 2) ? 1 : 0) - ((bits & 8) ? 1 : 0)
                                     : ((bits & 1) ? 1 : 0) - ((bits & 4) ? 1 : 0);
    *value = direction < 0 ? 0 : direction > 0 ? 65535 : 32768; return true;
}
double JoystickConfiguration::expo(double input, double percent, double minimum, double maximum, double trim)
{
    const double linear = input >= 0 ? trim + input / 500 * (maximum - trim) : minimum + (input + 500) / 500 * (trim - minimum);
    const double factor = input > 250 ? 500 - input : input < -250 ? -500 - input : input;
    return linear - factor * (percent / 100.0);
}
int JoystickConfiguration::channelValue(quint16 normalized, const Channel &c, bool manual, int minimum, int maximum, int trim)
{
    if (manual) { minimum = -1000; maximum = 1000; trim = 0; }
    if (minimum >= maximum || trim < minimum || trim > maximum) { minimum = manual ? -1000 : 1000; maximum = manual ? 1000 : 2000; trim = manual ? 0 : 1500; }
    if (c.channel == 3) trim = (minimum + maximum) / 2;
    int input = int(double(normalized) * 1000 / 65535 - 500);
    if (c.reverse) input = -input;
    return qBound(minimum, int(expo(input, qBound(-100, c.expo, 100), minimum, maximum, trim)), maximum);
}
bool JoystickConfiguration::load(QSettings *settings, Profile *out, QString *error)
{
    if (!settings || !out) return fail(error, "Missing joystick settings/profile destination.");
    const QByteArray bytes = settings->value("JoystickConfiguration/profileV1").toByteArray();
    if (bytes.isEmpty()) { *out = defaults(); if (error) error->clear(); return true; }
    return parseJson(bytes, out, error);
}
bool JoystickConfiguration::save(QSettings *settings, const Profile &p, QString *error)
{
    if (!settings || !validate(p, error)) return settings ? false : fail(error, "Missing settings store.");
    settings->setValue("JoystickConfiguration/profileV1", json(p)); settings->sync();
    return settings->status() == QSettings::NoError || fail(error, "Could not save joystick settings.");
}
bool JoystickConfiguration::importConfig(const QString &path, Profile *out, QString *error)
{
    if (!out) return fail(error, "Missing profile destination.");
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > MaximumArchive) return fail(error, "Joystick archive must be a regular file no larger than 8 MiB.");
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QByteArray bytes = file.read(MaximumArchive + 1);
    if (bytes.size() > MaximumArchive || file.error() != QFile::NoError) return fail(error, "Could not read bounded joystick archive.");
    QBuffer buffer(&bytes); QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdUnzip)) return fail(error, "Invalid joycfg ZIP archive.");
    QMap<QString, QByteArray> members; QMap<QString, QString> originalNames; int count = 0, total = 0;
    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        if (++count > 64) return fail(error, "Joystick archive exceeds 64 members.");
        const QString name = zip.getCurrentFileName();
        if (name.contains('/') || name.contains('\\') || name == "." || name == "..") return fail(error, "Joystick archive paths must be plain filenames.");
        if (members.contains(name.toCaseFolded())) return fail(error, "Duplicate joystick archive filename.");
        QuaZipFile member(&zip); if (!member.open(QIODevice::ReadOnly)) return fail(error, "Could not open joystick archive member.");
        QByteArray content = member.read(MaximumMember + 1); member.close();
        if (content.size() > MaximumMember || member.getZipError() != 0 || (total += content.size()) > MaximumArchive)
            return fail(error, "Joystick archive member exceeds bounds or failed CRC validation.");
        members.insert(name.toCaseFolded(), content);
        originalNames.insert(name.toCaseFolded(), name);
    }
    if (zip.getZipError() != 0) return fail(error, "Could not read joystick ZIP directory.");
    Profile p = defaults(); p.firmware = out->firmware;
    if (members.contains("apm-joystick-profile.json") && !parseJson(members.value("apm-joystick-profile.json"), &p, error)) return false;
    QString axis = "joystickaxis" + p.firmware.toCaseFolded() + ".xml";
    if (!members.contains(axis)) axis = "joystickaxis.xml";
    if (!members.contains(axis)) {
        QStringList choices;
        for (auto m = members.cbegin(); m != members.cend(); ++m)
            if (m.key().startsWith("joystickaxis") && m.key().endsWith(".xml")) choices += m.key();
        if (choices.size() != 1) return fail(error, "Archive has no unambiguous generic/current-firmware joystick profile.");
        axis = choices.first();
    }
    const QString suffix = axis.mid(QStringLiteral("joystickaxis").size());
    const QString buttons = "joystickbuttons" + suffix;
    if (!members.contains(buttons)) return fail(error, "Matching joystick button XML is missing.");
    const QString selectedName = originalNames.value(axis);
    p.firmware = selectedName.mid(QStringLiteral("joystickaxis").size());
    p.firmware.chop(4); // Preserve the original case required by MP's filenames.
    if (!parseXml(members.value(axis), true, &p, error) || !parseXml(members.value(buttons), false, &p, error) || !validate(p, error)) return false;
    *out = p; return true;
}
bool JoystickConfiguration::exportConfig(const QString &path, const Profile &p, QString *error)
{
    if (!validate(p, error)) return false;
    QStringList unsupported;
    for (const auto &c : p.channels)
        if (portableAxis(c.axis).isEmpty()) unsupported += QStringLiteral("RC%1=%2").arg(c.channel).arg(c.axis);
    if (!unsupported.isEmpty()) return fail(error, QStringLiteral("These SDL mappings cannot be represented by Mission Planner joycfg: %1. Save retains them locally; export requires standard mappings.").arg(unsupported.join(", ")));
    const QFileInfo target(path);
    if (target.isSymLink() || (target.exists() && !target.isFile())) return fail(error, "Refusing a non-regular/symlink joystick output.");
    QByteArray bytes; QBuffer buffer(&bytes); QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdCreate)) return fail(error, "Could not create joystick ZIP.");
    const QMap<QString, QByteArray> members{{"joystickaxis" + p.firmware + ".xml", xmlChannels(p)},
        {"joystickbuttons" + p.firmware + ".xml", xmlButtons(p)}, {"apm-joystick-profile.json", json(p)}};
    for (auto m = members.cbegin(); m != members.cend(); ++m) {
        QuaZipFile member(&zip);
        if (!member.open(QIODevice::WriteOnly, QuaZipNewInfo(m.key())) || member.write(m.value()) != m.value().size())
            return fail(error, "Could not write joystick ZIP member.");
        member.close(); if (member.getZipError() != 0) return fail(error, "Could not finish joystick ZIP member.");
    }
    zip.close(); if (zip.getZipError() != 0) return fail(error, "Could not finish joystick ZIP.");
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return fail(error, output.errorString());
    return true;
}
