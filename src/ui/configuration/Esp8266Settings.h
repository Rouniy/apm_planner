#ifndef ESP8266SETTINGS_H
#define ESP8266SETTINGS_H

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

/*
 * Pure codec for the Mission Planner 10 SETUP > ESP8266 Setup page
 * (ViewModels/GCSViews/ConfigurationView/ConfigHWESP8266ViewModel.cs:
 * TryReadSettings, ReadPackedString, ReadNumber, ReadIp, StringToByteArray,
 * SetU32 and the Save write order).
 *
 * The MAVESP8266 UDP bridge (MAV_COMP_ID_UDP_BRIDGE = 240) reports every
 * setting as a bytewise UINT32 parameter: the four wire bytes of PARAM_VALUE
 * carry either four ASCII characters of a packed string, a little-endian
 * integer, or the four octets of an IPv4 address in network order. Raw
 * values are therefore handled as 4-byte QByteArrays in wire order.
 * No transport, UI or Qt Network dependency lives here.
 */

struct Esp8266Settings
{
    QString ssid;
    QString password;
    QString baud;        // decimal text, e.g. "115200"
    QString channel;     // decimal text, e.g. "11"
    QString wifiMode;    // "0" = access point, anything else = station
    QString ipSta;       // dotted IPv4
    QString gatewaySta;
    QString subnetSta;
    QString details;     // read-only dump shown under the buttons

    bool staMode() const { return wifiMode != QLatin1String("0"); }

    bool operator==(const Esp8266Settings &other) const
    {
        return ssid == other.ssid && password == other.password && baud == other.baud &&
               channel == other.channel && wifiMode == other.wifiMode && ipSta == other.ipSta &&
               gatewaySta == other.gatewaySta && subnetSta == other.subnetSta &&
               details == other.details;
    }
    bool operator!=(const Esp8266Settings &other) const { return !(*this == other); }
};

// One PARAM_SET for the UDP bridge: UINT32 sent bytewise.
struct Esp8266ParameterWrite
{
    QString name;
    quint32 value = 0; // host-order value whose little-endian bytes go on the wire

    QByteArray rawBytes() const; // the 4 wire bytes

    bool operator==(const Esp8266ParameterWrite &other) const
    {
        return name == other.name && value == other.value;
    }
    bool operator!=(const Esp8266ParameterWrite &other) const { return !(*this == other); }
};

// Editable inputs of the page in the shape MP10 validates and writes.
struct Esp8266SaveRequest
{
    QString ssid;
    QString password;
    QString channel;
    QString baud;
    QString ipSta;
    QString gatewaySta;
    QString subnetSta;
    bool staMode = false;
};

class Esp8266SettingsCodec final
{
    Q_DECLARE_TR_FUNCTIONS(Esp8266SettingsCodec)

public:
    // Raw parameter values keyed by name; each value is the 4 wire bytes.
    using RawValues = QHash<QString, QByteArray>;

    static constexpr int UdpBridgeComponentId = 240; // MAV_COMP_ID_UDP_BRIDGE
    static constexpr int PackedStringChunks = 4;      // WIFI_SSID1..4
    static constexpr int PackedStringLength = 16;     // 4 chunks x 4 bytes

    static QStringList ChannelOptions(); // "1".."13"
    static QStringList BaudOptions();    // 9600 .. 921600
    static QString DefaultChannel();     // "11"
    static QString DefaultBaud();        // "115200"
    static QString DefaultIpSta();       // "192.168.4.1"
    static QString DefaultGatewaySta();  // "192.168.4.1"
    static QString DefaultSubnetSta();   // "255.255.255.0"

    // The 18 parameters TryReadSettings needs, in MP10 read order.
    static QStringList RequiredParameterNames();
    // The 22 PARAM_SET names of Save, in MP10 write order.
    static QStringList WriteOrder();

    // --- raw helpers ---
    static QByteArray RawFromUInt32(quint32 value);               // little-endian
    static quint32 UInt32FromRaw(const QByteArray &raw, bool *ok = nullptr);
    // MP10 StringToByteArray(text, start, 4): ASCII bytes resized to
    // start + 4, then the 4 bytes at start (NUL padded, non-ASCII -> '?').
    static QByteArray PackString(const QString &text, int start);
    static quint32 PackStringUInt32(const QString &text, int start);
    // Strict dotted IPv4 ("a.b.c.d", each 0..255); raw bytes are the octets
    // in order, which is the little-endian uint32 MP10 writes.
    static bool ParseIPv4(const QString &text, QByteArray *raw = nullptr,
                          quint32 *value = nullptr);
    static QString FormatIPv4(const QByteArray &raw);

    // --- MP10 decode ---
    // Fills `settings` when every required parameter is present; otherwise
    // lists every missing name (in read order) and leaves `settings` untouched.
    static bool TryReadSettings(const RawValues &values, Esp8266Settings *settings,
                                QStringList *missing = nullptr);
    static QString ReadPackedString(const RawValues &values, const QString &prefix,
                                    QStringList *missing = nullptr);
    static QString ReadNumber(const RawValues &values, const QString &name,
                              QStringList *missing = nullptr);
    static QString ReadIp(const RawValues &values, const QString &name,
                          QStringList *missing = nullptr);
    static QString FormatDetails(const QString &debugEnabled, const QString &wifiMode,
                                 const QString &wifiIpAddress, const QString &wifiUdpHport,
                                 const QString &wifiUdpCport, const QString &ipSta,
                                 const QString &gatewaySta, const QString &subnetSta);

    // --- MP10 save ---
    // "Invalid channel, baud rate, or IPv4 station settings." when the
    // channel/baud are not integers or an address is not IPv4.
    static bool ValidateSave(const Esp8266SaveRequest &request, QString *error = nullptr);
    // The 22 writes of MP10 Save() in order (validated input expected).
    static QList<Esp8266ParameterWrite> BuildWriteList(const Esp8266SaveRequest &request);
    static QString SaveErrorText();
};

#endif
