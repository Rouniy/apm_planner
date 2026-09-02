#include "Esp8266Settings.h"

#include <QtEndian>

namespace {

const char *const kRequiredNames[] = {
    "WIFI_SSID1",      "WIFI_SSID2",      "WIFI_SSID3",      "WIFI_SSID4",
    "WIFI_PASSWORD1",  "WIFI_PASSWORD2",  "WIFI_PASSWORD3",  "WIFI_PASSWORD4",
    "UART_BAUDRATE",   "WIFI_CHANNEL",    "DEBUG_ENABLED",   "WIFI_MODE",
    "WIFI_IPADDRESS",  "WIFI_UDP_HPORT",  "WIFI_UDP_CPORT",  "WIFI_IPSTA",
    "WIFI_GATEWAYSTA", "WIFI_SUBNET_STA",
};

const char *const kWriteOrder[] = {
    "WIFI_CHANNEL",    "UART_BAUDRATE",
    "WIFI_SSID1",      "WIFI_SSID2",      "WIFI_SSID3",      "WIFI_SSID4",
    "WIFI_PASSWORD1",  "WIFI_PASSWORD2",  "WIFI_PASSWORD3",  "WIFI_PASSWORD4",
    "WIFI_SSIDSTA1",   "WIFI_SSIDSTA2",   "WIFI_SSIDSTA3",   "WIFI_SSIDSTA4",
    "WIFI_PWDSTA1",    "WIFI_PWDSTA2",    "WIFI_PWDSTA3",    "WIFI_PWDSTA4",
    "WIFI_IPSTA",      "WIFI_GATEWAYSTA", "WIFI_SUBNET_STA", "WIFI_MODE",
};

QStringList namesOf(const char *const *names, int count)
{
    QStringList result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result << QString::fromLatin1(names[index]);
    }
    return result;
}

// Encoding.ASCII.GetBytes: characters above 0x7F become '?'.
QByteArray asciiBytes(const QString &text)
{
    QByteArray bytes;
    bytes.reserve(text.size());
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        bytes.append(code > 0x7F ? '?' : static_cast<char>(code));
    }
    return bytes;
}

bool parseInteger(const QString &text, int *value)
{
    bool ok = false;
    const int parsed = text.trimmed().toInt(&ok, 10); // NumberStyles.Integer
    if (ok && value) {
        *value = parsed;
    }
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------

QByteArray Esp8266ParameterWrite::rawBytes() const
{
    return Esp8266SettingsCodec::RawFromUInt32(value);
}

QStringList Esp8266SettingsCodec::ChannelOptions()
{
    QStringList options;
    for (int channel = 1; channel <= 13; ++channel) {
        options << QString::number(channel);
    }
    return options;
}

QStringList Esp8266SettingsCodec::BaudOptions()
{
    return {QStringLiteral("9600"),   QStringLiteral("19200"),  QStringLiteral("38400"),
            QStringLiteral("57600"),  QStringLiteral("115200"), QStringLiteral("230400"),
            QStringLiteral("460800"), QStringLiteral("921600")};
}

QString Esp8266SettingsCodec::DefaultChannel()
{
    return QStringLiteral("11");
}

QString Esp8266SettingsCodec::DefaultBaud()
{
    return QStringLiteral("115200");
}

QString Esp8266SettingsCodec::DefaultIpSta()
{
    return QStringLiteral("192.168.4.1");
}

QString Esp8266SettingsCodec::DefaultGatewaySta()
{
    return QStringLiteral("192.168.4.1");
}

QString Esp8266SettingsCodec::DefaultSubnetSta()
{
    return QStringLiteral("255.255.255.0");
}

QStringList Esp8266SettingsCodec::RequiredParameterNames()
{
    return namesOf(kRequiredNames, static_cast<int>(sizeof(kRequiredNames) / sizeof(kRequiredNames[0])));
}

QStringList Esp8266SettingsCodec::WriteOrder()
{
    return namesOf(kWriteOrder, static_cast<int>(sizeof(kWriteOrder) / sizeof(kWriteOrder[0])));
}

// --- raw helpers ------------------------------------------------------------

QByteArray Esp8266SettingsCodec::RawFromUInt32(quint32 value)
{
    QByteArray raw(4, '\0');
    qToLittleEndian<quint32>(value, raw.data());
    return raw;
}

quint32 Esp8266SettingsCodec::UInt32FromRaw(const QByteArray &raw, bool *ok)
{
    if (raw.size() != 4) {
        if (ok) {
            *ok = false;
        }
        return 0;
    }
    if (ok) {
        *ok = true;
    }
    return qFromLittleEndian<quint32>(raw.constData());
}

QByteArray Esp8266SettingsCodec::PackString(const QString &text, int start)
{
    // StringToByteArray: ASCII bytes, Array.Resize to start + 4, copy 4 bytes.
    QByteArray bytes = asciiBytes(text);
    const int required = qMax(0, start) + 4;
    if (bytes.size() < required) {
        bytes.append(QByteArray(required - bytes.size(), '\0'));
    }
    return bytes.mid(qMax(0, start), 4);
}

quint32 Esp8266SettingsCodec::PackStringUInt32(const QString &text, int start)
{
    return UInt32FromRaw(PackString(text, start));
}

bool Esp8266SettingsCodec::ParseIPv4(const QString &text, QByteArray *raw, quint32 *value)
{
    const QStringList octets = text.trimmed().split(QLatin1Char('.'));
    if (octets.size() != 4) {
        return false;
    }
    QByteArray bytes(4, '\0');
    for (int index = 0; index < 4; ++index) {
        const QString &octet = octets.at(index);
        if (octet.isEmpty() || octet.size() > 3) {
            return false;
        }
        for (const QChar ch : octet) {
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
                return false;
            }
        }
        const int number = octet.toInt();
        if (number < 0 || number > 255) {
            return false;
        }
        bytes[index] = static_cast<char>(number);
    }
    if (raw) {
        *raw = bytes;
    }
    if (value) {
        *value = UInt32FromRaw(bytes); // BitConverter.ToUInt32(GetAddressBytes())
    }
    return true;
}

QString Esp8266SettingsCodec::FormatIPv4(const QByteArray &raw)
{
    if (raw.size() != 4) {
        return QString();
    }
    return QStringLiteral("%1.%2.%3.%4")
        .arg(static_cast<quint8>(raw.at(0)))
        .arg(static_cast<quint8>(raw.at(1)))
        .arg(static_cast<quint8>(raw.at(2)))
        .arg(static_cast<quint8>(raw.at(3)));
}

// --- decode -------------------------------------------------------------------

QString Esp8266SettingsCodec::ReadPackedString(const RawValues &values, const QString &prefix,
                                               QStringList *missing)
{
    QByteArray bytes;
    for (int index = 1; index <= PackedStringChunks; ++index) {
        const QString name = prefix + QString::number(index);
        const auto found = values.constFind(name);
        if (found == values.constEnd()) {
            if (missing) {
                missing->append(name);
            }
        } else {
            bytes.append(found.value().left(4));
        }
    }
    // Encoding.ASCII.GetString(...).TrimEnd('\0'): only trailing NULs go.
    int end = bytes.size();
    while (end > 0 && bytes.at(end - 1) == '\0') {
        --end;
    }
    bytes.truncate(end);
    QString text;
    text.reserve(bytes.size());
    for (const char byte : bytes) {
        const unsigned char code = static_cast<unsigned char>(byte);
        text.append(code > 0x7F ? QLatin1Char('?') : QLatin1Char(static_cast<char>(code)));
    }
    return text;
}

QString Esp8266SettingsCodec::ReadNumber(const RawValues &values, const QString &name,
                                         QStringList *missing)
{
    const auto found = values.constFind(name);
    if (found == values.constEnd()) {
        if (missing) {
            missing->append(name);
        }
        return QString();
    }
    // UINT32 values printed with the invariant "0.######" pattern: integers.
    return QString::number(UInt32FromRaw(found.value()));
}

QString Esp8266SettingsCodec::ReadIp(const RawValues &values, const QString &name,
                                     QStringList *missing)
{
    const auto found = values.constFind(name);
    if (found == values.constEnd()) {
        if (missing) {
            missing->append(name);
        }
        return QString();
    }
    QByteArray raw = found.value().left(4);
    if (raw.size() < 4) {
        raw.append(QByteArray(4 - raw.size(), '\0'));
    }
    return FormatIPv4(raw);
}

QString Esp8266SettingsCodec::FormatDetails(
    const QString &debugEnabled, const QString &wifiMode, const QString &wifiIpAddress,
    const QString &wifiUdpHport, const QString &wifiUdpCport, const QString &ipSta,
    const QString &gatewaySta, const QString &subnetSta)
{
    return QStringLiteral("DEBUG_ENABLED %1,\n"
                          "WIFI_MODE %2,\n"
                          "WIFI_IPADDRESS %3,\n"
                          "WIFI_UDP_HPORT %4,\n"
                          "WIFI_UDP_CPORT %5,\n"
                          "WIFI_IPSTA %6,\n"
                          "WIFI_GATEWAYSTA %7,\n"
                          "WIFI_SUBNET_STA %8\n")
        .arg(debugEnabled, wifiMode, wifiIpAddress, wifiUdpHport, wifiUdpCport, ipSta,
             gatewaySta, subnetSta);
}

bool Esp8266SettingsCodec::TryReadSettings(const RawValues &values, Esp8266Settings *settings,
                                           QStringList *missing)
{
    QStringList absent;
    const QString ssid = ReadPackedString(values, QStringLiteral("WIFI_SSID"), &absent);
    const QString password = ReadPackedString(values, QStringLiteral("WIFI_PASSWORD"), &absent);
    const QString baud = ReadNumber(values, QStringLiteral("UART_BAUDRATE"), &absent);
    const QString channel = ReadNumber(values, QStringLiteral("WIFI_CHANNEL"), &absent);
    const QString debugEnabled = ReadNumber(values, QStringLiteral("DEBUG_ENABLED"), &absent);
    const QString wifiMode = ReadNumber(values, QStringLiteral("WIFI_MODE"), &absent);
    const QString wifiIpAddress = ReadIp(values, QStringLiteral("WIFI_IPADDRESS"), &absent);
    const QString wifiUdpHport = ReadNumber(values, QStringLiteral("WIFI_UDP_HPORT"), &absent);
    const QString wifiUdpCport = ReadNumber(values, QStringLiteral("WIFI_UDP_CPORT"), &absent);
    const QString ipSta = ReadIp(values, QStringLiteral("WIFI_IPSTA"), &absent);
    const QString gatewaySta = ReadIp(values, QStringLiteral("WIFI_GATEWAYSTA"), &absent);
    const QString subnetSta = ReadIp(values, QStringLiteral("WIFI_SUBNET_STA"), &absent);
    if (missing) {
        *missing = absent;
    }
    if (!absent.isEmpty()) {
        return false;
    }

    Esp8266Settings result;
    result.ssid = ssid;
    result.password = password;
    result.baud = baud;
    result.channel = channel;
    result.wifiMode = wifiMode;
    result.ipSta = ipSta;
    result.gatewaySta = gatewaySta;
    result.subnetSta = subnetSta;
    result.details = FormatDetails(debugEnabled, wifiMode, wifiIpAddress, wifiUdpHport,
                                   wifiUdpCport, ipSta, gatewaySta, subnetSta);
    if (settings) {
        *settings = result;
    }
    return true;
}

// --- save -----------------------------------------------------------------------

QString Esp8266SettingsCodec::SaveErrorText()
{
    return tr("Invalid channel, baud rate, or IPv4 station settings.");
}

bool Esp8266SettingsCodec::ValidateSave(const Esp8266SaveRequest &request, QString *error)
{
    const bool valid = parseInteger(request.channel, nullptr) &&
                       parseInteger(request.baud, nullptr) && ParseIPv4(request.ipSta) &&
                       ParseIPv4(request.gatewaySta) && ParseIPv4(request.subnetSta);
    if (!valid && error) {
        *error = SaveErrorText();
    }
    if (valid && error) {
        error->clear();
    }
    return valid;
}

QList<Esp8266ParameterWrite> Esp8266SettingsCodec::BuildWriteList(
    const Esp8266SaveRequest &request)
{
    int channel = 0;
    int baud = 0;
    quint32 ipSta = 0;
    quint32 gatewaySta = 0;
    quint32 subnetSta = 0;
    parseInteger(request.channel, &channel);
    parseInteger(request.baud, &baud);
    ParseIPv4(request.ipSta, nullptr, &ipSta);
    ParseIPv4(request.gatewaySta, nullptr, &gatewaySta);
    ParseIPv4(request.subnetSta, nullptr, &subnetSta);

    QList<Esp8266ParameterWrite> writes;
    writes.reserve(22);
    auto add = [&writes](const char *name, quint32 value) {
        Esp8266ParameterWrite write;
        write.name = QString::fromLatin1(name);
        write.value = value;
        writes.append(write);
    };
    add("WIFI_CHANNEL", static_cast<quint32>(channel));
    add("UART_BAUDRATE", static_cast<quint32>(baud));
    add("WIFI_SSID1", PackStringUInt32(request.ssid, 0));
    add("WIFI_SSID2", PackStringUInt32(request.ssid, 4));
    add("WIFI_SSID3", PackStringUInt32(request.ssid, 8));
    add("WIFI_SSID4", PackStringUInt32(request.ssid, 12));
    add("WIFI_PASSWORD1", PackStringUInt32(request.password, 0));
    add("WIFI_PASSWORD2", PackStringUInt32(request.password, 4));
    add("WIFI_PASSWORD3", PackStringUInt32(request.password, 8));
    add("WIFI_PASSWORD4", PackStringUInt32(request.password, 12));
    add("WIFI_SSIDSTA1", PackStringUInt32(request.ssid, 0));
    add("WIFI_SSIDSTA2", PackStringUInt32(request.ssid, 4));
    add("WIFI_SSIDSTA3", PackStringUInt32(request.ssid, 8));
    add("WIFI_SSIDSTA4", PackStringUInt32(request.ssid, 12));
    add("WIFI_PWDSTA1", PackStringUInt32(request.password, 0));
    add("WIFI_PWDSTA2", PackStringUInt32(request.password, 4));
    add("WIFI_PWDSTA3", PackStringUInt32(request.password, 8));
    add("WIFI_PWDSTA4", PackStringUInt32(request.password, 12));
    add("WIFI_IPSTA", ipSta);
    add("WIFI_GATEWAYSTA", gatewaySta);
    add("WIFI_SUBNET_STA", subnetSta);
    add("WIFI_MODE", request.staMode ? 1U : 0U);
    return writes;
}
