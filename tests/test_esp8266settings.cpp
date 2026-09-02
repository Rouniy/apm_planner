#include <QtTest>

#include "ui/configuration/Esp8266Settings.h"

#include <QtEndian>

namespace {

using RawValues = Esp8266SettingsCodec::RawValues;

// Esp8266SettingsTests.AddPacked: ASCII bytes resized to 16, four 4-byte chunks.
void addPacked(RawValues &values, const QString &prefix, const QByteArray &text)
{
    QByteArray bytes = text.left(16);
    if (bytes.size() < 16) {
        bytes.append(QByteArray(16 - bytes.size(), '\0')); // Array.Resize pads with zeros
    }
    for (int index = 0; index < 4; ++index) {
        values.insert(prefix + QString::number(index + 1), bytes.mid(index * 4, 4));
    }
}

void addNumber(RawValues &values, const QString &name, quint32 value)
{
    values.insert(name, Esp8266SettingsCodec::RawFromUInt32(value));
}

void addIp(RawValues &values, const QString &name, quint8 a, quint8 b, quint8 c, quint8 d)
{
    QByteArray raw;
    raw.append(static_cast<char>(a));
    raw.append(static_cast<char>(b));
    raw.append(static_cast<char>(c));
    raw.append(static_cast<char>(d));
    values.insert(name, raw);
}

RawValues completeResponse()
{
    RawValues values;
    addPacked(values, QStringLiteral("WIFI_SSID"), QByteArrayLiteral("FieldNetwork"));
    addPacked(values, QStringLiteral("WIFI_PASSWORD"), QByteArrayLiteral("secret-password"));
    addNumber(values, QStringLiteral("UART_BAUDRATE"), 115200);
    addNumber(values, QStringLiteral("WIFI_CHANNEL"), 11);
    addNumber(values, QStringLiteral("DEBUG_ENABLED"), 0);
    addNumber(values, QStringLiteral("WIFI_MODE"), 1);
    addIp(values, QStringLiteral("WIFI_IPADDRESS"), 192, 168, 4, 1);
    addNumber(values, QStringLiteral("WIFI_UDP_HPORT"), 14550);
    addNumber(values, QStringLiteral("WIFI_UDP_CPORT"), 14555);
    addIp(values, QStringLiteral("WIFI_IPSTA"), 10, 42, 0, 20);
    addIp(values, QStringLiteral("WIFI_GATEWAYSTA"), 10, 42, 0, 1);
    addIp(values, QStringLiteral("WIFI_SUBNET_STA"), 255, 255, 255, 0);
    return values;
}

Esp8266SaveRequest validRequest()
{
    Esp8266SaveRequest request;
    request.ssid = QStringLiteral("FieldNetwork");
    request.password = QStringLiteral("secret-password");
    request.channel = QStringLiteral("11");
    request.baud = QStringLiteral("115200");
    request.ipSta = QStringLiteral("10.42.0.20");
    request.gatewaySta = QStringLiteral("10.42.0.1");
    request.subnetSta = QStringLiteral("255.255.255.0");
    request.staMode = true;
    return request;
}

quint32 le(const char *fourChars)
{
    return qFromLittleEndian<quint32>(fourChars);
}

} // namespace

class Esp8266SettingsTest final : public QObject
{
    Q_OBJECT

private slots:
    void optionsDefaultsAndOrders();
    void rawHelpersRoundTrip();
    void packsStringsLikeMissionPlanner();
    void parsesAndFormatsIPv4_data();
    void parsesAndFormatsIPv4();
    void completeResponseIsDecodedWithoutLocaleDependentValues();
    void partialResponseReportsEveryMissingValueInsteadOfThrowing();
    void packedStringsKeepInteriorBytesAndTrimTrailingNul();
    void validateSaveMatchesMissionPlanner();
    void writeListFollowsMissionPlannerOrderAndEncoding();
};

void Esp8266SettingsTest::optionsDefaultsAndOrders()
{
    QStringList channels;
    for (int channel = 1; channel <= 13; ++channel) {
        channels << QString::number(channel);
    }
    QCOMPARE(Esp8266SettingsCodec::ChannelOptions(), channels);
    QCOMPARE(Esp8266SettingsCodec::BaudOptions(),
             QStringList() << QStringLiteral("9600") << QStringLiteral("19200")
                           << QStringLiteral("38400") << QStringLiteral("57600")
                           << QStringLiteral("115200") << QStringLiteral("230400")
                           << QStringLiteral("460800") << QStringLiteral("921600"));
    QCOMPARE(Esp8266SettingsCodec::DefaultChannel(), QStringLiteral("11"));
    QCOMPARE(Esp8266SettingsCodec::DefaultBaud(), QStringLiteral("115200"));
    QCOMPARE(Esp8266SettingsCodec::DefaultIpSta(), QStringLiteral("192.168.4.1"));
    QCOMPARE(Esp8266SettingsCodec::DefaultGatewaySta(), QStringLiteral("192.168.4.1"));
    QCOMPARE(Esp8266SettingsCodec::DefaultSubnetSta(), QStringLiteral("255.255.255.0"));
    QVERIFY(Esp8266SettingsCodec::ChannelOptions().contains(Esp8266SettingsCodec::DefaultChannel()));
    QVERIFY(Esp8266SettingsCodec::BaudOptions().contains(Esp8266SettingsCodec::DefaultBaud()));
    QCOMPARE(Esp8266SettingsCodec::UdpBridgeComponentId, 240);
    QCOMPARE(Esp8266SettingsCodec::PackedStringLength, 16);

    const QStringList required = Esp8266SettingsCodec::RequiredParameterNames();
    QCOMPARE(required.size(), 18);
    QCOMPARE(required.first(), QStringLiteral("WIFI_SSID1"));
    QCOMPARE(required.at(8), QStringLiteral("UART_BAUDRATE"));
    QCOMPARE(required.last(), QStringLiteral("WIFI_SUBNET_STA"));
    QVERIFY(!required.contains(QStringLiteral("WIFI_SSIDSTA1"))); // never read by MP10

    const QStringList order = Esp8266SettingsCodec::WriteOrder();
    QCOMPARE(order.size(), 22);
    QCOMPARE(order, QStringList()
             << QStringLiteral("WIFI_CHANNEL") << QStringLiteral("UART_BAUDRATE")
             << QStringLiteral("WIFI_SSID1") << QStringLiteral("WIFI_SSID2")
             << QStringLiteral("WIFI_SSID3") << QStringLiteral("WIFI_SSID4")
             << QStringLiteral("WIFI_PASSWORD1") << QStringLiteral("WIFI_PASSWORD2")
             << QStringLiteral("WIFI_PASSWORD3") << QStringLiteral("WIFI_PASSWORD4")
             << QStringLiteral("WIFI_SSIDSTA1") << QStringLiteral("WIFI_SSIDSTA2")
             << QStringLiteral("WIFI_SSIDSTA3") << QStringLiteral("WIFI_SSIDSTA4")
             << QStringLiteral("WIFI_PWDSTA1") << QStringLiteral("WIFI_PWDSTA2")
             << QStringLiteral("WIFI_PWDSTA3") << QStringLiteral("WIFI_PWDSTA4")
             << QStringLiteral("WIFI_IPSTA") << QStringLiteral("WIFI_GATEWAYSTA")
             << QStringLiteral("WIFI_SUBNET_STA") << QStringLiteral("WIFI_MODE"));
}

void Esp8266SettingsTest::rawHelpersRoundTrip()
{
    QCOMPARE(Esp8266SettingsCodec::RawFromUInt32(0x0104A8C0u), QByteArray("\xC0\xA8\x04\x01", 4));
    bool ok = false;
    QCOMPARE(Esp8266SettingsCodec::UInt32FromRaw(QByteArray("\xC0\xA8\x04\x01", 4), &ok), 0x0104A8C0u);
    QVERIFY(ok);
    QCOMPARE(Esp8266SettingsCodec::UInt32FromRaw(QByteArray("\x01\x02\x03", 3), &ok), 0u);
    QVERIFY(!ok);
    QCOMPARE(Esp8266SettingsCodec::UInt32FromRaw(QByteArray(), &ok), 0u);
    QVERIFY(!ok);
    QCOMPARE(Esp8266SettingsCodec::UInt32FromRaw(Esp8266SettingsCodec::RawFromUInt32(4294967295u)),
             4294967295u);

    Esp8266ParameterWrite write;
    write.name = QStringLiteral("WIFI_IPSTA");
    write.value = 0x0104A8C0u;
    QCOMPARE(write.rawBytes(), QByteArray("\xC0\xA8\x04\x01", 4));
}

void Esp8266SettingsTest::packsStringsLikeMissionPlanner()
{
    // StringToByteArray(text, start, 4) chunks of "FieldNetwork".
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("FieldNetwork"), 0), QByteArrayLiteral("Fiel"));
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("FieldNetwork"), 4), QByteArrayLiteral("dNet"));
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("FieldNetwork"), 8), QByteArrayLiteral("work"));
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("FieldNetwork"), 12), QByteArray(4, '\0'));
    QCOMPARE(Esp8266SettingsCodec::PackStringUInt32(QStringLiteral("FieldNetwork"), 0), le("Fiel"));
    QCOMPARE(Esp8266SettingsCodec::PackStringUInt32(QStringLiteral("FieldNetwork"), 12), 0u);

    // Short text is NUL padded, an empty text is all NUL.
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("ab"), 0), QByteArray("ab\0\0", 4));
    QCOMPARE(Esp8266SettingsCodec::PackString(QString(), 8), QByteArray(4, '\0'));

    // Exactly 16 characters fill every chunk; longer text is silently cut.
    const QString sixteen = QStringLiteral("0123456789ABCDEF");
    QCOMPARE(Esp8266SettingsCodec::PackString(sixteen, 12), QByteArrayLiteral("CDEF"));
    QCOMPARE(Esp8266SettingsCodec::PackString(sixteen + QStringLiteral("XYZ"), 12), QByteArrayLiteral("CDEF"));

    // Encoding.ASCII maps non-ASCII characters to '?'.
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("Nét"), 0), QByteArray("N?t\0", 4));

    // A negative start is treated as the first chunk.
    QCOMPARE(Esp8266SettingsCodec::PackString(QStringLiteral("abcd"), -4), QByteArrayLiteral("abcd"));
}

void Esp8266SettingsTest::parsesAndFormatsIPv4_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QByteArray>("raw");

    QTest::newRow("access point") << QStringLiteral("192.168.4.1") << true << QByteArray("\xC0\xA8\x04\x01", 4);
    QTest::newRow("station") << QStringLiteral("10.42.0.20") << true << QByteArray("\x0A\x2A\x00\x14", 4);
    QTest::newRow("mask") << QStringLiteral("255.255.255.0") << true << QByteArray("\xFF\xFF\xFF\x00", 4);
    QTest::newRow("zero") << QStringLiteral("0.0.0.0") << true << QByteArray(4, '\0');
    QTest::newRow("trimmed") << QStringLiteral("  1.2.3.4 ") << true << QByteArray("\x01\x02\x03\x04", 4);
    QTest::newRow("leading zeros") << QStringLiteral("010.001.000.007") << true << QByteArray("\x0A\x01\x00\x07", 4);
    QTest::newRow("ipv6") << QStringLiteral("::1") << false << QByteArray();
    QTest::newRow("octet too large") << QStringLiteral("300.1.1.1") << false << QByteArray();
    QTest::newRow("three parts") << QStringLiteral("1.2.3") << false << QByteArray();
    QTest::newRow("five parts") << QStringLiteral("1.2.3.4.5") << false << QByteArray();
    QTest::newRow("empty octet") << QStringLiteral("1..3.4") << false << QByteArray();
    QTest::newRow("sign") << QStringLiteral("-1.2.3.4") << false << QByteArray();
    QTest::newRow("letters") << QStringLiteral("a.b.c.d") << false << QByteArray();
    QTest::newRow("empty") << QString() << false << QByteArray();
}

void Esp8266SettingsTest::parsesAndFormatsIPv4()
{
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QFETCH(QByteArray, raw);

    QByteArray parsedRaw;
    quint32 parsedValue = 0;
    QCOMPARE(Esp8266SettingsCodec::ParseIPv4(text, &parsedRaw, &parsedValue), valid);
    if (valid) {
        QCOMPARE(parsedRaw, raw);
        QCOMPARE(parsedValue, Esp8266SettingsCodec::UInt32FromRaw(raw)); // BitConverter.ToUInt32 order
        QCOMPARE(Esp8266SettingsCodec::FormatIPv4(raw), text.trimmed().isEmpty()
                     ? QString()
                     : QStringLiteral("%1.%2.%3.%4")
                           .arg(static_cast<quint8>(raw.at(0)))
                           .arg(static_cast<quint8>(raw.at(1)))
                           .arg(static_cast<quint8>(raw.at(2)))
                           .arg(static_cast<quint8>(raw.at(3))));
    }
    QVERIFY(Esp8266SettingsCodec::ParseIPv4(text) == valid); // optional outputs
}

void Esp8266SettingsTest::completeResponseIsDecodedWithoutLocaleDependentValues()
{
    Esp8266Settings settings;
    QStringList missing;
    QVERIFY(Esp8266SettingsCodec::TryReadSettings(completeResponse(), &settings, &missing));
    QVERIFY(missing.isEmpty());
    QCOMPARE(settings.ssid, QStringLiteral("FieldNetwork"));
    QCOMPARE(settings.password, QStringLiteral("secret-password"));
    QCOMPARE(settings.baud, QStringLiteral("115200"));
    QCOMPARE(settings.channel, QStringLiteral("11"));
    QCOMPARE(settings.wifiMode, QStringLiteral("1"));
    QVERIFY(settings.staMode());
    QCOMPARE(settings.ipSta, QStringLiteral("10.42.0.20"));
    QCOMPARE(settings.gatewaySta, QStringLiteral("10.42.0.1"));
    QCOMPARE(settings.subnetSta, QStringLiteral("255.255.255.0"));
    QVERIFY(settings.details.contains(QStringLiteral("WIFI_UDP_CPORT 14555")));
    QCOMPARE(settings.details, QStringLiteral("DEBUG_ENABLED 0,\n"
                                              "WIFI_MODE 1,\n"
                                              "WIFI_IPADDRESS 192.168.4.1,\n"
                                              "WIFI_UDP_HPORT 14550,\n"
                                              "WIFI_UDP_CPORT 14555,\n"
                                              "WIFI_IPSTA 10.42.0.20,\n"
                                              "WIFI_GATEWAYSTA 10.42.0.1,\n"
                                              "WIFI_SUBNET_STA 255.255.255.0\n"));

    // Access-point mode and large integers.
    RawValues values = completeResponse();
    addNumber(values, QStringLiteral("WIFI_MODE"), 0);
    addNumber(values, QStringLiteral("UART_BAUDRATE"), 4294967295u);
    Esp8266Settings accessPoint;
    QVERIFY(Esp8266SettingsCodec::TryReadSettings(values, &accessPoint));
    QVERIFY(!accessPoint.staMode());
    QCOMPARE(accessPoint.baud, QStringLiteral("4294967295"));
    QVERIFY(accessPoint != settings);
    QVERIFY(Esp8266SettingsCodec::TryReadSettings(completeResponse(), nullptr, nullptr));
}

void Esp8266SettingsTest::partialResponseReportsEveryMissingValueInsteadOfThrowing()
{
    RawValues values;
    values.insert(QStringLiteral("WIFI_SSID1"), QByteArrayLiteral("test"));

    Esp8266Settings settings;
    settings.ssid = QStringLiteral("untouched");
    QStringList missing;
    QVERIFY(!Esp8266SettingsCodec::TryReadSettings(values, &settings, &missing));
    QCOMPARE(settings.ssid, QStringLiteral("untouched")); // left as it was
    QVERIFY(missing.contains(QStringLiteral("WIFI_SSID2")));
    QVERIFY(missing.contains(QStringLiteral("WIFI_PASSWORD1")));
    QVERIFY(missing.contains(QStringLiteral("UART_BAUDRATE")));
    QVERIFY(missing.contains(QStringLiteral("WIFI_SUBNET_STA")));
    QVERIFY(!missing.contains(QStringLiteral("WIFI_SSID1")));
    QCOMPARE(missing.size(), Esp8266SettingsCodec::RequiredParameterNames().size() - 1);
    QStringList expected = Esp8266SettingsCodec::RequiredParameterNames();
    expected.removeAll(QStringLiteral("WIFI_SSID1"));
    QCOMPARE(missing, expected); // read order preserved

    // An empty response lists everything, and a missing pointer is fine.
    QStringList all;
    QVERIFY(!Esp8266SettingsCodec::TryReadSettings(RawValues(), nullptr, &all));
    QCOMPARE(all, Esp8266SettingsCodec::RequiredParameterNames());
    QVERIFY(!Esp8266SettingsCodec::TryReadSettings(RawValues(), nullptr, nullptr));
}

void Esp8266SettingsTest::packedStringsKeepInteriorBytesAndTrimTrailingNul()
{
    RawValues values;
    values.insert(QStringLiteral("WIFI_SSID1"), QByteArray("ab\0c", 4));
    values.insert(QStringLiteral("WIFI_SSID2"), QByteArray("d\0\0\0", 4));
    values.insert(QStringLiteral("WIFI_SSID3"), QByteArray(4, '\0'));
    values.insert(QStringLiteral("WIFI_SSID4"), QByteArray(4, '\0'));
    QStringList missing;
    QCOMPARE(Esp8266SettingsCodec::ReadPackedString(values, QStringLiteral("WIFI_SSID"), &missing),
             QString::fromLatin1("ab\0cd", 5)); // TrimEnd('\0') only
    QVERIFY(missing.isEmpty());

    // High bytes become '?', a full 16-byte name survives.
    values.insert(QStringLiteral("WIFI_SSID1"), QByteArray("N\xC3\xA9t", 4));
    values.insert(QStringLiteral("WIFI_SSID2"), QByteArrayLiteral("work"));
    values.insert(QStringLiteral("WIFI_SSID3"), QByteArrayLiteral("1234"));
    values.insert(QStringLiteral("WIFI_SSID4"), QByteArrayLiteral("5678"));
    QCOMPARE(Esp8266SettingsCodec::ReadPackedString(values, QStringLiteral("WIFI_SSID")),
             QStringLiteral("N??twork12345678"));

    // Missing chunks are reported and the rest is still concatenated.
    values.remove(QStringLiteral("WIFI_SSID3"));
    QCOMPARE(Esp8266SettingsCodec::ReadPackedString(values, QStringLiteral("WIFI_SSID"), &missing),
             QStringLiteral("N??twork5678"));
    QCOMPARE(missing, QStringList() << QStringLiteral("WIFI_SSID3"));

    // Numbers and addresses report their own missing names.
    QStringList absent;
    QCOMPARE(Esp8266SettingsCodec::ReadNumber(values, QStringLiteral("WIFI_CHANNEL"), &absent), QString());
    QCOMPARE(Esp8266SettingsCodec::ReadIp(values, QStringLiteral("WIFI_IPSTA"), &absent), QString());
    QCOMPARE(absent, QStringList() << QStringLiteral("WIFI_CHANNEL") << QStringLiteral("WIFI_IPSTA"));
    addNumber(values, QStringLiteral("WIFI_CHANNEL"), 6);
    addIp(values, QStringLiteral("WIFI_IPSTA"), 192, 168, 4, 1);
    QCOMPARE(Esp8266SettingsCodec::ReadNumber(values, QStringLiteral("WIFI_CHANNEL")), QStringLiteral("6"));
    QCOMPARE(Esp8266SettingsCodec::ReadIp(values, QStringLiteral("WIFI_IPSTA")), QStringLiteral("192.168.4.1"));
}

void Esp8266SettingsTest::validateSaveMatchesMissionPlanner()
{
    QString error;
    QVERIFY(Esp8266SettingsCodec::ValidateSave(validRequest(), &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(Esp8266SettingsCodec::SaveErrorText(),
             QStringLiteral("Invalid channel, baud rate, or IPv4 station settings."));

    Esp8266SaveRequest request = validRequest();
    request.channel = QStringLiteral(" 11 ");
    request.baud = QStringLiteral("+57600"); // NumberStyles.Integer accepts a sign
    QVERIFY(Esp8266SettingsCodec::ValidateSave(request, &error));

    request = validRequest();
    request.channel = QStringLiteral("x");
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request, &error));
    QCOMPARE(error, Esp8266SettingsCodec::SaveErrorText());

    request = validRequest();
    request.baud = QString();
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request, &error));

    request = validRequest();
    request.baud = QStringLiteral("115200.5");
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request));

    request = validRequest();
    request.ipSta = QStringLiteral("::1");
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request));

    request = validRequest();
    request.gatewaySta = QStringLiteral("300.1.1.1");
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request));

    request = validRequest();
    request.subnetSta = QStringLiteral("255.255.255");
    QVERIFY(!Esp8266SettingsCodec::ValidateSave(request));

    // SSID/password contents are never validated (MP10 truncates silently).
    request = validRequest();
    request.ssid = QString();
    request.password = QStringLiteral("this password is much longer than sixteen characters");
    QVERIFY(Esp8266SettingsCodec::ValidateSave(request));
}

void Esp8266SettingsTest::writeListFollowsMissionPlannerOrderAndEncoding()
{
    const QList<Esp8266ParameterWrite> writes = Esp8266SettingsCodec::BuildWriteList(validRequest());
    QCOMPARE(writes.size(), 22);
    QStringList names;
    for (const Esp8266ParameterWrite &write : writes) {
        names << write.name;
    }
    QCOMPARE(names, Esp8266SettingsCodec::WriteOrder());

    QCOMPARE(writes.at(0).value, 11u);       // WIFI_CHANNEL
    QCOMPARE(writes.at(1).value, 115200u);   // UART_BAUDRATE
    QCOMPARE(writes.at(2).value, le("Fiel")); // WIFI_SSID1
    QCOMPARE(writes.at(3).value, le("dNet"));
    QCOMPARE(writes.at(4).value, le("work"));
    QCOMPARE(writes.at(5).value, 0u);
    QCOMPARE(writes.at(2).rawBytes(), QByteArrayLiteral("Fiel"));
    QCOMPARE(writes.at(6).value, le("secr")); // WIFI_PASSWORD1
    QCOMPARE(writes.at(7).value, le("et-p"));
    QCOMPARE(writes.at(8).value, le("assw"));
    QCOMPARE(writes.at(9).value, le("ord\0"));
    // Station SSID/password repeat the access-point values.
    for (int chunk = 0; chunk < 4; ++chunk) {
        QCOMPARE(writes.at(10 + chunk).value, writes.at(2 + chunk).value);
        QCOMPARE(writes.at(14 + chunk).value, writes.at(6 + chunk).value);
    }
    QCOMPARE(writes.at(18).value, 0x14002A0Au);  // WIFI_IPSTA 10.42.0.20 (bytes 0A 2A 00 14)
    QCOMPARE(writes.at(18).rawBytes(), QByteArray("\x0A\x2A\x00\x14", 4));
    QCOMPARE(writes.at(19).value, 0x01002A0Au);  // WIFI_GATEWAYSTA 10.42.0.1
    QCOMPARE(writes.at(20).value, 0x00FFFFFFu);  // WIFI_SUBNET_STA 255.255.255.0
    QCOMPARE(writes.at(21).value, 1u);           // WIFI_MODE (station)

    Esp8266SaveRequest accessPoint = validRequest();
    accessPoint.staMode = false;
    accessPoint.channel = QStringLiteral("1");
    accessPoint.baud = QStringLiteral("921600");
    accessPoint.ssid = QStringLiteral("0123456789ABCDEFXYZ"); // truncated to 16
    const QList<Esp8266ParameterWrite> apWrites = Esp8266SettingsCodec::BuildWriteList(accessPoint);
    QCOMPARE(apWrites.at(0).value, 1u);
    QCOMPARE(apWrites.at(1).value, 921600u);
    QCOMPARE(apWrites.at(5).rawBytes(), QByteArrayLiteral("CDEF"));
    QCOMPARE(apWrites.at(21).value, 0u);
    QVERIFY(apWrites.at(2) != writes.at(2));
    QCOMPARE(apWrites.at(10).value, apWrites.at(2).value); // station copy of the SSID chunk
    QCOMPARE(apWrites.at(10).name, QStringLiteral("WIFI_SSIDSTA1"));
}

QTEST_APPLESS_MAIN(Esp8266SettingsTest)
#include "test_esp8266settings.moc"
