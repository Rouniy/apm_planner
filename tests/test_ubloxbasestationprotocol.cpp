#include <QtTest>
#include <QtEndian>
#include <QMap>
#include "comm/UbloxBaseStationProtocol.h"
#include <cmath>
#include <limits>

using Protocol = UbloxBaseStationProtocol;
namespace {
template<typename T> T field(const QByteArray &v, int offset)
{ return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(v.constData() + offset)); }
template<typename T> void set(QByteArray &v, int offset, T value)
{ qToLittleEndian<T>(value, reinterpret_cast<uchar *>(v.data() + offset)); }
// Independent framing fixture, not the production generator.
QByteArray wire(int cls, int id, QByteArray payload)
{
    QByteArray result = QByteArray::fromHex("b562");
    result.append(char(cls)); result.append(char(id));
    result.append(char(payload.size())); result.append(char(payload.size() >> 8)); result += payload;
    unsigned a = 0, b = 0;
    for (int i = 2; i < result.size(); ++i) { a = (a + quint8(result[i])) & 255; b = (b + a) & 255; }
    result.append(char(a)); result.append(char(b)); return result;
}
QVector<Protocol::Packet> packets(const QByteArray &bytes)
{
    Protocol parser; QVector<Protocol::Packet> result;
    for (char byte : bytes) { Protocol::Packet packet; if (parser.read(quint8(byte), &packet)) result.append(packet); }
    return result;
}
}

class TestUbloxBaseStationProtocol : public QObject
{
    Q_OBJECT
private slots:
    void framesAndBoundedParser();
    void configurationSequence_data();
    void configurationSequence();
    void surveyAndRestart();
    void fixedPrecisionAndLimits();
    void versionAndAcknowledgement();
    void surveyDecode();
    void positionDecode();
};

void TestUbloxBaseStationProtocol::framesAndBoundedParser()
{
    QCOMPARE(Protocol::frame(0x0a, 4), QByteArray::fromHex("b5620a0400000e34"));
    const QByteArray payload = QByteArray::fromHex("00b562ffd30001");
    QCOMPARE(Protocol::frame(0xff, 0xee, payload), wire(0xff, 0xee, payload));
    Protocol parser; Protocol::Packet out;
    QByteArray bad = wire(5, 1, QByteArray::fromHex("0671"));
    bad[bad.size() - 1] = char(quint8(bad.at(bad.size() - 1)) ^ 1);
    const QByteArray stream = QByteArray::fromHex("0012b5b5") + Protocol::frame(0x0a, 4)
        + bad + QByteArray::fromHex("b562010100ff") + wire(0xff, 0xee, payload);
    int count = 0;
    for (char byte : stream) {
        if (parser.read(quint8(byte), &out)) ++count;
        QVERIFY(parser.bufferedBytes() <= Protocol::MaximumPayload + 8);
    }
    QCOMPARE(count, 2); QCOMPARE(out.payload, payload);
    QCOMPARE(parser.statistics().frames, quint64(2));
    QCOMPARE(parser.statistics().badChecksums, quint64(1));
    QCOMPARE(parser.statistics().oversizedFrames, quint64(1));
    QCOMPARE(parser.bufferedBytes(), 0);
    const QByteArray maximum(Protocol::MaximumPayload, char(0xa5));
    QCOMPARE(packets(Protocol::frame(2, 3, maximum)).first().payload, maximum);
    QVERIFY(Protocol::frame(2, 3, QByteArray(Protocol::MaximumPayload + 1, 'x')).isEmpty());
    parser.reset(); QCOMPARE(parser.statistics().frames, quint64(0));
    QCOMPARE(parser.bufferedBytes(), 0);
}

void TestUbloxBaseStationProtocol::configurationSequence_data()
{ QTest::addColumn<bool>("modern"); QTest::newRow("M8P-old") << false; QTest::newRow("M8P130-F9P") << true; }

void TestUbloxBaseStationProtocol::configurationSequence()
{
    QFETCH(bool, modern);
    const auto commands = Protocol::autoConfigure(115200, modern);
    QCOMPARE(commands.size(), 43);
    const int bauds[] = {115200,9600,38400,57600,115200,230400,460800};
    QMap<int,int> rates;
    for (int i = 0; i < commands.size(); ++i) {
        const auto &c = commands[i]; QVERIFY(!c.description.isEmpty());
        const auto parsed = packets(c.bytes); QCOMPARE(parsed.size(), 1);
        const auto &p = parsed.first();
        if (i < 7) {
            QCOMPARE(c.baudRate, bauds[i]); QCOMPARE(c.delayBeforeMs, 50); QCOMPARE(c.delayAfterMs, 100);
            QVERIFY(c.bytes.startsWith("UU")); QCOMPARE(p.messageClass, quint8(6)); QCOMPARE(p.messageId, quint8(0));
            QCOMPARE(p.payload, QByteArray::fromHex("01000000d0080000000807002300230000000000"));
        } else QCOMPARE(c.baudRate, 0);
        if (p.messageClass == 6 && p.messageId == 1) {
            QCOMPARE(p.payload.size(), 8); QCOMPARE(p.payload.at(3), p.payload.at(5));
            rates[(quint8(p.payload.at(0)) << 8) | quint8(p.payload.at(1))] = quint8(p.payload.at(3));
        }
    }
    QCOMPARE(packets(commands[7].bytes).first().payload, QByteArray::fromHex("0300000000000000000000002300230000000000"));
    QCOMPARE(packets(commands[8].bytes).first().payload, QByteArray::fromHex("e80301000100"));
    QCOMPARE(packets(commands[9].bytes).first().payload.size(), 36);
    QCOMPARE(packets(commands[9].bytes).first().payload.at(2), char(2));
    QCOMPARE(rates.size(), 32);
    for (int i=0; i<16; ++i) {
        if (i==11 || i==12 || i==14) QVERIFY(!rates.contains(0xf000+i));
        else { QVERIFY(rates.contains(0xf000+i)); QCOMPARE(rates.value(0xf000+i), 0); }
    }
    for (int id : {0x4a,0x54,0x5e,0x7c}) QCOMPARE(rates.value(0xf500+id), modern ? 1 : 0);
    for (int id : {0x4d,0x57,0x61,0x7f}) QCOMPARE(rates.value(0xf500+id), modern ? 0 : 1);
    QCOMPARE(rates.value(0xf505), 5); QCOMPARE(rates.value(0xf5e6), 5);
    QCOMPARE(rates.value(0xf5fe), 0); QCOMPARE(rates.value(0x013b), 1);
    QCOMPARE(rates.value(0x0107), 1); QCOMPARE(rates.value(0x0213), 2);
    QCOMPARE(rates.value(0x0211), 2); QCOMPARE(rates.value(0x0a09), 2);
    QCOMPARE(commands.last().delayAfterMs, 110);
    QVERIFY(Protocol::autoConfigure(0).isEmpty());
}

void TestUbloxBaseStationProtocol::surveyAndRestart()
{
    QString error;
    auto p = packets(Protocol::surveyIn(120, 1.25, &error)).first().payload;
    QVERIFY(error.isEmpty()); QCOMPARE(p.size(), 40);
    QCOMPARE(field<quint16>(p, 2), quint16(1)); QCOMPARE(field<quint32>(p, 24), quint32(120));
    QCOMPARE(field<quint32>(p, 28), quint32(12500)); QCOMPARE(p.mid(4,20), QByteArray(20,'\0'));
    p = packets(Protocol::surveyIn(0,0)).first().payload;
    QCOMPARE(field<quint32>(p,24), quint32(60)); QCOMPARE(field<quint32>(p,28), quint32(20000));
    const auto disabled = Protocol::disableBase(); QCOMPARE(disabled.size(), 3);
    QCOMPARE(packets(disabled[0].bytes).first().payload, QByteArray(40,'\0'));
    QCOMPARE(disabled[0].delayBeforeMs, 200); QCOMPARE(disabled[1].delayAfterMs, 1000);
    QCOMPARE(disabled[2].delayAfterMs, 3000);
    QCOMPARE(packets(disabled[1].bytes).first().payload, QByteArray::fromHex("00000000ffff00000000000001"));
    QCOMPARE(packets(disabled[2].bytes).first().payload, QByteArray::fromHex("14ff0200"));
    const auto restarted = Protocol::restartSurvey(38400,120,1.25,true,&error);
    QCOMPARE(restarted.size(), 47); QCOMPARE(restarted.first().bytes, disabled.first().bytes);
    QCOMPARE(restarted.last().bytes, Protocol::surveyIn(120,1.25)); QCOMPARE(restarted.last().delayBeforeMs,200);
    QVERIFY(Protocol::surveyIn(1,-1,&error).isEmpty()); QVERIFY(!error.isEmpty());
    QVERIFY(Protocol::surveyIn(1,std::numeric_limits<double>::infinity()).isEmpty());
    QVERIFY(Protocol::surveyIn(1,1e9).isEmpty());
    QVERIFY(Protocol::restartSurvey(0,60,2).isEmpty());
}

void TestUbloxBaseStationProtocol::fixedPrecisionAndLimits()
{
    const double lat=-35.123456789, lon=149.876543219, alt=-12.3456;
    const auto p = packets(Protocol::fixedLla(lat,lon,alt)).first().payload;
    QCOMPARE(field<quint16>(p,2), quint16(258));
    const double decodedLat = field<qint32>(p,4)*1e-7 + qint8(p.at(16))*1e-9;
    const double decodedLon = field<qint32>(p,8)*1e-7 + qint8(p.at(17))*1e-9;
    const double decodedAlt = field<qint32>(p,12)*.01 + qint8(p.at(18))*.0001;
    QVERIFY(std::abs(decodedLat-lat) < 1.1e-9); QVERIFY(std::abs(decodedLon-lon) < 1.1e-9);
    QVERIFY(std::abs(decodedAlt-alt) < .00011); QVERIFY(qint8(p.at(16)) < 0);
    QCOMPARE(field<quint32>(p,20), quint32(1)); // Exact reference's metres *1000.
    QCOMPARE(field<quint32>(p,24), quint32(60)); QCOMPARE(field<quint32>(p,28), quint32(2000));
    QVERIFY(!Protocol::fixedLla(0,0,0).isEmpty()); QVERIFY(!Protocol::fixedLla(90,180,-1).isEmpty());
    for (double invalid : {91.0, std::numeric_limits<double>::quiet_NaN()})
        QVERIFY(Protocol::fixedLla(invalid,0,0).isEmpty());
    QVERIFY(Protocol::fixedLla(0,181,0).isEmpty()); QVERIFY(Protocol::fixedLla(0,0,1e10).isEmpty());
    QVERIFY(Protocol::fixedLla(0,0,0,0).isEmpty());
}

void TestUbloxBaseStationProtocol::versionAndAcknowledgement()
{
    Protocol::Packet p; p.messageClass=10; p.messageId=4; p.payload=QByteArray(100,'\0');
    p.payload.replace(0,8,"HPG 1.32"); p.payload.replace(30,8,"00190000");
    p.payload.replace(40,12,"PROTVER=27.3"); p.payload.replace(70,9,"MOD=F9P  ");
    Protocol::Version version; QString error;
    QVERIFY(Protocol::decodeVersion(p,&version,&error)); QCOMPARE(version.software,QStringLiteral("HPG 1.32"));
    QCOMPARE(version.hardware,QStringLiteral("00190000")); QCOMPARE(version.extensions.size(),2);
    QCOMPARE(version.extensions.last(),QStringLiteral("MOD=F9P"));
    p.payload.chop(1); QVERIFY(!Protocol::decodeVersion(p,&version,&error));
    QVERIFY(!error.isEmpty()); QCOMPARE(version.extensions.size(),2);
    p.messageClass=5; p.messageId=1; p.payload=QByteArray::fromHex("0671");
    Protocol::Acknowledgement ack; QVERIFY(Protocol::decodeAcknowledgement(p,&ack));
    QVERIFY(ack.accepted); QCOMPARE(ack.messageClass,quint8(6)); QCOMPARE(ack.messageId,quint8(0x71));
    p.messageId=0; QVERIFY(Protocol::decodeAcknowledgement(p,&ack)); QVERIFY(!ack.accepted);
    p.payload.append('x'); QVERIFY(!Protocol::decodeAcknowledgement(p,&ack));
}

void TestUbloxBaseStationProtocol::surveyDecode()
{
    Protocol::Packet p; p.messageClass=1; p.messageId=0x3b; p.payload=QByteArray(40,'\0');
    set<quint32>(p.payload,4,1234); set<quint32>(p.payload,8,120);
    set<qint32>(p.payload,12,637813700); p.payload[24]=char(25);
    set<quint32>(p.payload,28,12500); set<quint32>(p.payload,32,119); p.payload[36]=1;
    Protocol::SurveyIn svin; QVERIFY(Protocol::decodeSurveyIn(p,&svin));
    QVERIFY(svin.valid); QVERIFY(!svin.active); QVERIFY(svin.hasPosition);
    QCOMPARE(svin.durationSeconds,quint32(120)); QCOMPARE(svin.observations,quint32(119));
    QCOMPARE(svin.accuracyMeters,1.25); QCOMPARE(svin.latitude,0.0); QCOMPARE(svin.longitude,0.0);
    QVERIFY(std::abs(svin.altitudeMeters-.0025) < 1e-7);
    p.payload[24]=100; QVERIFY(!Protocol::decodeSurveyIn(p,&svin));
    p.payload=QByteArray(40,'\0'); p.payload[37]=1;
    QVERIFY(Protocol::decodeSurveyIn(p,&svin)); QVERIFY(svin.active); QVERIFY(!svin.hasPosition);
    p.payload[0]=1; QVERIFY(!Protocol::decodeSurveyIn(p,&svin));
}

void TestUbloxBaseStationProtocol::positionDecode()
{
    Protocol::Packet p; p.messageClass=1; p.messageId=7; p.payload=QByteArray(92,'\0');
    set<quint32>(p.payload,0,1000); set<quint16>(p.payload,4,2026);
    p.payload[6]=9; p.payload[7]=6; p.payload[8]=12; p.payload[9]=34; p.payload[10]=56; p.payload[11]=3;
    set<qint32>(p.payload,16,-250000000); p.payload[20]=3; p.payload[21]=char(0x83); p.payload[23]=22;
    set<qint32>(p.payload,24,1491234567); set<qint32>(p.payload,28,-351234567);
    set<qint32>(p.payload,32,-12345); set<qint32>(p.payload,36,-15000);
    set<quint32>(p.payload,40,20); set<quint32>(p.payload,44,30);
    Protocol::Position pos; QVERIFY(Protocol::decodePosition(p,&pos));
    QVERIFY(pos.fixOk); QVERIFY(pos.differential); QCOMPARE(pos.carrierSolution,quint8(2));
    QCOMPARE(pos.satellites,quint8(22)); QVERIFY(std::abs(pos.latitude+35.1234567)<1e-10);
    QCOMPARE(pos.altitudeMeters,-12.345); QCOMPARE(pos.horizontalAccuracyMeters,.02);
    QCOMPARE(pos.utc,QDateTime(QDate(2026,9,6),QTime(12,34,55,750),Qt::UTC));
    p.payload[11]=0; QVERIFY(Protocol::decodePosition(p,&pos)); QVERIFY(!pos.utc.isValid());
    set<qint32>(p.payload,28,910000000); QVERIFY(!Protocol::decodePosition(p,&pos));
    p.payload.chop(1); QVERIFY(!Protocol::decodePosition(p,&pos));
}

QTEST_GUILESS_MAIN(TestUbloxBaseStationProtocol)
#include "test_ubloxbasestationprotocol.moc"
