#include <QtTest/QtTest>

#include "comm/MicrodroneDownlinkEncoder.h"

#include <QLocale>
#include <limits>

class MicrodroneDownlinkEncoderTest : public QObject
{
    Q_OBJECT
private slots:
    void officialGoldenFrame();
    void widenedScientificGoldenFrame();
    void checksumAndRecordBoundaries();
    void utcInstantAndCounterCadence();
    void invariantShortestWidenedFormatting();
    void dotNetFormattingCorpus_data();
    void dotNetFormattingCorpus();
    void invalidInputsFailWithoutPartialFrame();
};

namespace {

MicrodroneTelemetry officialTelemetry()
{
    MicrodroneTelemetry value;
    value.latitude = 0;
    value.longitude = 0;
    value.altitude = 100;
    value.gpsHdop = 1.5;
    value.satelliteCount = 12;
    value.groundSpeed = 10;
    value.groundCourse = 90;
    value.verticalSpeed = -1.25;
    value.roll = 10;
    value.pitch = -5;
    value.yaw = 90;
    value.pressureTemperature = 24.5;
    value.magnetometerX = -139;
    value.magnetometerY = 12;
    value.magnetometerZ = 431;
    return value;
}

} // namespace

void MicrodroneDownlinkEncoderTest::widenedScientificGoldenFrame()
{
    MicrodroneTelemetry telemetry;
    telemetry.latitude = 37.7749;
    telemetry.longitude = -122.4194;
    telemetry.altitude = static_cast<double>(0.1f);
    telemetry.gpsHdop = static_cast<double>(1.23f);
    telemetry.satelliteCount = static_cast<double>(255.0f);
    telemetry.groundSpeed = static_cast<double>(0.1f);
    telemetry.groundCourse = static_cast<double>(359.99f);
    telemetry.verticalSpeed = -0.0;
    telemetry.roll = static_cast<double>(0.1f);
    telemetry.pitch = static_cast<double>(-0.2f);
    telemetry.yaw = static_cast<double>(359.99f);
    telemetry.pressureTemperature = -32768;
    telemetry.magnetometerX = 1.0e-5;
    telemetry.magnetometerY = 1.0e15;
    telemetry.magnetometerZ = -0.0;
    const QDateTime timestamp(QDate(2026, 9, 6), QTime(12, 34, 56),
                              Qt::OffsetFromUTC, 3 * 60 * 60);

    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        telemetry, timestamp, 123456789);
    QCOMPARE(frame, QByteArray(
        "#1,28,07,2,1,1,1,2,16000,0,2,166\r\n"
        "#4,12345678,34496,2435,25,233\r\n"
        "#5,-270277383.8467404,-425570436.6717493,388084207.1625089,1.2400000190734863,255,185\r\n"
        "#6,-1.7470336922429887E-05,0.09999999996405276,-0,2,138\r\n"
        "#7,0.0017453292780017621,-0.0034906585560035243,6.283010603812077,3\r\n"
        "#8,0.10000000149011612,0.10000000149011612,-32768,109\r\n"
        "#9,1E-05,1000000000000000,-0,141\r\n"));
}

void MicrodroneDownlinkEncoderTest::officialGoldenFrame()
{
    QString error;
    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        officialTelemetry(),
        QDateTime(QDate(1980, 1, 13), QTime(0, 0, 5), Qt::UTC), 29, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(frame, QByteArray(
        "#1,28,07,2,1,1,1,2,16000,0,2,166\r\n"
        "#4,2,5,1,25,205\r\n"
        "#5,637813701,0,0,1.51,12,67\r\n"
        "#6,10,6.123233995736766E-16,-1.25,2,239\r\n"
        "#7,0.17453292519943295,-0.08726646259971647,1.5707963267948966,70\r\n"
        "#8,100,100,24.5,9\r\n"
        "#9,-139,12,431,46\r\n"));
}

void MicrodroneDownlinkEncoderTest::checksumAndRecordBoundaries()
{
    QCOMPARE(MicrodroneDownlinkEncoder::Checksum(
        QByteArrayLiteral("#1,27,48,1,1,1,1,0,25343,8192,")), quint8(85));

    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        officialTelemetry(),
        QDateTime(QDate(1980, 1, 13), QTime(0, 0, 5), Qt::UTC), 0);
    QVERIFY(frame.endsWith("\r\n"));
    const QList<QByteArray> records = frame.split('\n');
    QCOMPARE(records.size(), 8);
    QVERIFY(records.last().isEmpty());
    for (int index = 0; index < 7; ++index) {
        QVERIFY(records.at(index).endsWith('\r'));
        const QByteArray line = records.at(index).left(records.at(index).size() - 1);
        const int comma = line.lastIndexOf(',');
        QVERIFY(comma > 0);
        const QByteArray payload = line.left(comma + 1);
        QCOMPARE(line.mid(comma + 1).toUInt(),
                 uint(MicrodroneDownlinkEncoder::Checksum(payload)));
    }
}

void MicrodroneDownlinkEncoderTest::utcInstantAndCounterCadence()
{
    const QDateTime offsetTime(QDate(1980, 1, 13), QTime(3, 0, 5),
                               Qt::OffsetFromUTC, 3 * 60 * 60);
    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        officialTelemetry(), offsetTime, 109);
    const QList<QByteArray> records = frame.split('\n');
    QVERIFY(records.at(1).startsWith("#4,10,5,1,25,"));
}

void MicrodroneDownlinkEncoderTest::invariantShortestWidenedFormatting()
{
    const QLocale previous = QLocale();
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));

    MicrodroneTelemetry telemetry;
    telemetry.altitude = static_cast<double>(0.1f);
    telemetry.magnetometerX = 1.0e-5;
    telemetry.magnetometerY = 1.0e15;
    telemetry.magnetometerZ = -0.0;
    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        telemetry, QDateTime(QDate(2026, 8, 22), QTime(9, 34, 56), Qt::UTC), 1);

    QLocale::setDefault(previous);
    QVERIFY(frame.contains("#8,0.10000000149011612,0.10000000149011612,0,"));
    QVERIFY(frame.contains("#9,1E-05,1000000000000000,-0,"));
    QVERIFY(!frame.contains("0,10000000149011612"));
}

void MicrodroneDownlinkEncoderTest::dotNetFormattingCorpus_data()
{
    QTest::addColumn<double>("value");
    QTest::addColumn<QByteArray>("expected");

    QTest::newRow("float-0.1-widened") << static_cast<double>(0.1f)
        << QByteArray("0.10000000149011612");
    QTest::newRow("1e-5") << 1.0e-5 << QByteArray("1E-05");
    QTest::newRow("1e-4") << 1.0e-4 << QByteArray("0.0001");
    QTest::newRow("1e14") << 1.0e14 << QByteArray("100000000000000");
    QTest::newRow("1e15") << 1.0e15 << QByteArray("1000000000000000");
    QTest::newRow("1e16") << 1.0e16 << QByteArray("10000000000000000");
    QTest::newRow("1e17") << 1.0e17 << QByteArray("1E+17");
    QTest::newRow("mantissa-e15") << 1.234567890123456e15
        << QByteArray("1234567890123456");
    QTest::newRow("minimum-subnormal")
        << std::numeric_limits<double>::denorm_min() << QByteArray("5E-324");
    QTest::newRow("negative-zero") << -0.0 << QByteArray("-0");
    QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN()
        << QByteArray("NaN");
    QTest::newRow("positive-infinity")
        << std::numeric_limits<double>::infinity() << QByteArray("Infinity");
    QTest::newRow("negative-infinity")
        << -std::numeric_limits<double>::infinity() << QByteArray("-Infinity");
}

void MicrodroneDownlinkEncoderTest::dotNetFormattingCorpus()
{
    QFETCH(double, value);
    QFETCH(QByteArray, expected);
    MicrodroneTelemetry telemetry;
    telemetry.magnetometerX = value;
    const QByteArray frame = MicrodroneDownlinkEncoder::EncodeFrame(
        telemetry, QDateTime(QDate(2026, 9, 6), QTime(12, 0), Qt::UTC), 0);
    const QList<QByteArray> records = frame.split('\n');
    QCOMPARE(records.size(), 8);
    const QList<QByteArray> fields = records.at(6).split(',');
    QCOMPARE(fields.size(), 5);
    QCOMPARE(fields.at(1), expected);
}

void MicrodroneDownlinkEncoderTest::invalidInputsFailWithoutPartialFrame()
{
    QString error = QStringLiteral("old");
    QVERIFY(MicrodroneDownlinkEncoder::EncodeFrame(
        {}, QDateTime(QDate(1980, 1, 6), QTime(), Qt::UTC), -1, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("counter"), Qt::CaseInsensitive));

    error = QStringLiteral("old");
    QVERIFY(MicrodroneDownlinkEncoder::EncodeFrame(
        {}, QDateTime(QDate(1980, 1, 5), QTime(23, 59, 59), Qt::UTC),
        0, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("predates"), Qt::CaseInsensitive));

    error = QStringLiteral("old");
    QVERIFY(MicrodroneDownlinkEncoder::EncodeFrame(
        {}, QDateTime(), 0, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("invalid"), Qt::CaseInsensitive));
}

QTEST_GUILESS_MAIN(MicrodroneDownlinkEncoderTest)
#include "test_microdronedownlinkencoder.moc"
