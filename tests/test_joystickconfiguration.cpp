#include "input/JoystickConfiguration.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <quazip.h>
#include <quazipfile.h>
#include <quazipnewinfo.h>
#include <limits>

namespace {
bool archive(const QString &path, const QMap<QString, QByteArray> &members)
{
    QuaZip zip(path); if (!zip.open(QuaZip::mdCreate)) return false;
    for (auto m = members.cbegin(); m != members.cend(); ++m) {
        QuaZipFile file(&zip);
        if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(m.key())) || file.write(m.value()) != m.value().size()) return false;
        file.close(); if (file.getZipError()) return false;
    }
    zip.close(); return zip.getZipError() == 0;
}
QByteArray channelsXml()
{
    return QByteArrayLiteral("<?xml version=\"1.0\"?><ArrayOfJoyChannel xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<JoyChannel><channel>0</channel><axis>None</axis><reverse>false</reverse><expo>0</expo></JoyChannel>"
        "<JoyChannel><channel>1</channel><axis>X</axis><reverse>true</reverse><expo>30</expo></JoyChannel>"
        "<JoyChannel><channel>0</channel><axis>None</axis><reverse>false</reverse><expo>0</expo></JoyChannel>"
        "<JoyChannel><channel>3</channel><axis>Z</axis><reverse>false</reverse><expo>-20</expo></JoyChannel>"
        "</ArrayOfJoyChannel>");
}
QByteArray buttonsXml()
{
    return QByteArrayLiteral("<ArrayOfJoyButton><JoyButton><buttonno>5</buttonno><function>Do_Set_Servo</function>"
        "<mode>LOITER</mode><p1>9</p1><p2>1750</p2><p3>1.5</p3><p4>-2</p4><state>true</state></JoyButton></ArrayOfJoyButton>");
}
}
class TestJoystickConfiguration : public QObject
{
    Q_OBJECT
private slots:
    void defaultsAndNormalization()
    {
        auto p = JoystickConfiguration::defaults(); QString error;
        QCOMPARE(p.channels.size(), 16); QCOMPARE(p.buttons.size(), 128);
        QVERIFY(JoystickConfiguration::validate(p, &error));
        for (int i = 0; i < 16; ++i) { QCOMPARE(p.channels[i].channel, i + 1); QCOMPARE(p.channels[i].axis, QString("None")); }
        for (const auto &b : p.buttons) QCOMPARE(b.buttonno, -1);
        p.channels.resize(3); p.channels[0].expo = 900; p.buttons[0].buttonno = 1000;
        JoystickConfiguration::normalize(&p); QCOMPARE(p.channels.size(), 16); QCOMPARE(p.channels[0].expo, 100); QCOMPARE(p.buttons[0].buttonno, 127);
        QVERIFY(JoystickConfiguration::validate(p));
        p.channels[0].axis = "Unrecognized"; QVERIFY(!JoystickConfiguration::validate(p, &error)); QVERIFY(!error.isEmpty());
    }
    void normalizationAndExpo()
    {
        using C = JoystickConfiguration;
        QCOMPARE(C::normalizeAxis(-32768, {}), quint16(0)); QCOMPARE(C::normalizeAxis(32767, {}), quint16(65535));
        QCOMPARE(C::normalizeAxis(0, {}), quint16(32768));
        QCOMPARE(C::normalizeAxis(0, {-12000, 12000}), quint16(32768));
        QCOMPARE(C::normalizeAxis(-16000, {-12000, 12000}), quint16(0));
        QCOMPARE(C::normalizeAxis(16000, {-12000, 12000}), quint16(65535));
        QCOMPARE(C::normalizeAxis(50, {0, 100}), quint16(32818));
        QVERIFY(!C::validRange({-32769, 32767})); QVERIFY(!C::validRange({0, 4095})); QVERIFY(C::validRange({0, 4096}));
        QCOMPARE(C::expo(250, 100, 1000, 2000, 1500), 1500.0);
        QCOMPARE(C::expo(-250, 100, 1000, 2000, 1500), 1500.0);
        QCOMPARE(C::expo(250, -100, 1000, 2000, 1500), 2000.0);
        QCOMPARE(C::expo(500, 100, 1000, 2000, 1500), 2000.0);
        C::Channel c; QCOMPARE(C::channelValue(0, c), 1000); QCOMPARE(C::channelValue(65535, c), 2000);
        QCOMPARE(C::channelValue(32768, c), 1500); c.reverse = true; QCOMPARE(C::channelValue(0, c), 2000);
        QCOMPARE(C::channelValue(65535, c, true), -1000);
        QCOMPARE(C::channelValue(0, c, true), 1000);
    }
    void arbitraryAxesAndHats()
    {
        QVector<quint16> axes(128, 1234); axes[93] = 65000; quint16 value = 0;
        QVERIFY(JoystickConfiguration::axisValue("Axis93", axes, {}, &value)); QCOMPARE(value, quint16(65000));
        QVERIFY(JoystickConfiguration::axisValue("X", axes, {}, &value)); QCOMPARE(value, quint16(1234));
        QVERIFY(!JoystickConfiguration::axisValue("Axis128", axes, {}, &value));
        QVERIFY(!JoystickConfiguration::axisValue("None", axes, {}, &value));
        QVERIFY(!JoystickConfiguration::axisValue("Pass", axes, {}, &value));
        QVERIFY(!JoystickConfiguration::axisValue("Custom1", axes, {}, &value));
        QVERIFY(JoystickConfiguration::axisValue("Hat1X", {}, {0, 2}, &value)); QCOMPARE(value, quint16(65535));
        QVERIFY(JoystickConfiguration::axisValue("Hat0Y", {}, {4}, &value)); QCOMPARE(value, quint16(0));
        QVERIFY(JoystickConfiguration::axisValue("Hatud1", {}, {0}, &value)); QCOMPARE(value, quint16(32768));
        QVERIFY(JoystickConfiguration::axisNames(128, 4).contains("Axis127"));
        QVERIFY(JoystickConfiguration::axisNames(128, 4).contains("Hat3Y"));
        QCOMPARE(JoystickConfiguration::buttonFunctions().size(), 15);
    }
    void saveAndRoundtrip()
    {
        using C = JoystickConfiguration; QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings settings(dir.filePath("local.ini"), QSettings::IniFormat);
        auto p = C::defaults(); p.deviceId = "guid:serial"; p.deviceName = "Flight Stick"; p.firmware = "ArduCopter2";
        p.elevons = true; p.manualControl = true; p.channels[0] = {1, "X", true, 35}; p.channels[15] = {16, "Rz", false, -50};
        p.buttons[0] = {7, "Do_Repeat_Servo", "LOITER", 9, 1850, 2, 100, true};
        p.calibration[p.deviceId][0] = {-16000, 15000}; QString error;
        QVERIFY2(C::save(&settings, p, &error), qPrintable(error));
        C::Profile loaded; QVERIFY(C::load(&settings, &loaded, &error));
        QCOMPARE(loaded.deviceName, p.deviceName); QCOMPARE(loaded.channels[0].expo, 35); QVERIFY(loaded.elevons); QVERIFY(loaded.manualControl);
        QCOMPARE(loaded.buttons[0].buttonno, 7); QCOMPARE(loaded.buttons[0].p2, 1850.0);
        QCOMPARE(loaded.calibration[p.deviceId][0].minimum, -16000);
        const QString output = dir.filePath("stick.joycfg"); QVERIFY2(C::exportConfig(output, p, &error), qPrintable(error));
        C::Profile imported = C::defaults(); QVERIFY2(C::importConfig(output, &imported, &error), qPrintable(error));
        QCOMPARE(imported.channels[0].axis, p.channels[0].axis); QCOMPARE(imported.channels[15].expo, -50);
        QCOMPARE(imported.buttons[0].function, p.buttons[0].function); QCOMPARE(imported.buttons[0].p4, 100.0);
        QCOMPARE(imported.deviceId, p.deviceId); QVERIFY(imported.manualControl); QVERIFY(imported.elevons);
        QCOMPARE(imported.calibration[p.deviceId][0].maximum, 15000);
        p.channels[0].axis = "Axis93"; QVERIFY(C::save(&settings, p));
        QVERIFY(!C::exportConfig(dir.filePath("unsupported.joycfg"), p, &error)); QVERIFY(error.contains("RC1=Axis93"));
        QVERIFY(!QFileInfo::exists(dir.filePath("unsupported.joycfg")));
    }
    void actualMpXmlAndAtomicFailure()
    {
        QTemporaryDir dir; const QString path = dir.filePath("mp.joycfg");
        QVERIFY(archive(path, {{"joystickaxis.xml", channelsXml()}, {"joystickbuttons.xml", buttonsXml()}}));
        auto p = JoystickConfiguration::defaults(); QString error;
        QVERIFY2(JoystickConfiguration::importConfig(path, &p, &error), qPrintable(error));
        QCOMPARE(p.channels[0].axis, QString("X")); QVERIFY(p.channels[0].reverse); QCOMPARE(p.channels[0].expo, 30);
        QCOMPARE(p.channels[1].axis, QString("None")); QCOMPARE(p.channels[2].axis, QString("Z"));
        QCOMPARE(p.buttons[0].buttonno, 5); QCOMPARE(p.buttons[0].p3, 1.5); QVERIFY(p.buttons[0].state);
        QByteArray malicious = channelsXml(); malicious.replace("<expo>30</expo>", "<expo>9999</expo>");
        QVERIFY(archive(path, {{"joystickaxis.xml", malicious}, {"joystickbuttons.xml", buttonsXml()}}));
        QVERIFY(!JoystickConfiguration::importConfig(path, &p, &error)); QCOMPARE(p.channels[0].expo, 30);
        QVERIFY(archive(path, {{"../joystickaxis.xml", channelsXml()}, {"joystickbuttons.xml", buttonsXml()}}));
        QVERIFY(!JoystickConfiguration::importConfig(path, &p, &error)); QVERIFY(error.contains("filenames"));
        QVERIFY(archive(path, {{"joystickaxis.xml", "<!DOCTYPE x [<!ENTITY x 'bad'>]>" + channelsXml()}, {"joystickbuttons.xml", buttonsXml()}}));
        QVERIFY(!JoystickConfiguration::importConfig(path, &p, &error));
        p.buttons[0].p1 = std::numeric_limits<double>::quiet_NaN(); QVERIFY(!JoystickConfiguration::validate(p));
    }
    void firmwareArchiveSelection()
    {
        QTemporaryDir dir; const QString path = dir.filePath("firmware.joycfg");
        QMap<QString, QByteArray> members{{"joystickaxisArduCopter2.xml", channelsXml()},
            {"joystickbuttonsArduCopter2.xml", buttonsXml()}};
        QVERIFY(archive(path, members));
        auto p = JoystickConfiguration::defaults(); QString error;
        QVERIFY2(JoystickConfiguration::importConfig(path, &p, &error), qPrintable(error));
        QCOMPARE(p.firmware, QString("ArduCopter2"));
        members.insert("joystickaxisArduPlane.xml", channelsXml());
        members.insert("joystickbuttonsArduPlane.xml", buttonsXml());
        QVERIFY(archive(path, members));
        p = JoystickConfiguration::defaults();
        QVERIFY(!JoystickConfiguration::importConfig(path, &p, &error)); QVERIFY(error.contains("unambiguous"));
        p.firmware = "ArduPlane";
        QVERIFY2(JoystickConfiguration::importConfig(path, &p, &error), qPrintable(error));
        QCOMPARE(p.firmware, QString("ArduPlane"));
    }
};
QTEST_GUILESS_MAIN(TestJoystickConfiguration)
#include "test_joystickconfiguration.moc"
