#include "comm/NmeaGgaParser.h"

#include <QtTest/QtTest>

class NmeaGgaParserTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesReferenceFixes();
    void validatesChecksumKindAndFix();
    void validatesCoordinatesAltitudeAndGeoid();
    void checksumStopsAtAsterisk();
};

namespace
{
QString withChecksum(const QString &body)
{
    return body + QLatin1Char('*') + NmeaGgaParser::checksum(body);
}
}

void NmeaGgaParserTest::parsesReferenceFixes()
{
    NmeaGgaParseResult result = NmeaGgaParser::parse(QStringLiteral(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QVERIFY(qAbs(result.fix.latitude - 48.1173) < 0.0000001);
    QVERIFY(qAbs(result.fix.longitude - 11.5166666667) < 0.0000001);
    QCOMPARE(result.fix.altitudeM, 545.4);
    QCOMPARE(result.fix.satellites, 8);
    QCOMPARE(result.fix.hdop, 0.9);
    QCOMPARE(result.fix.fixQuality, 1);
    QVERIFY(result.fix.hasGeoidSeparation);
    QCOMPARE(result.fix.geoidSeparationM, 46.9);
    QVERIFY(qAbs(result.fix.geodeticAltitudeM() - 592.3) < 0.0000001);

    result = NmeaGgaParser::parse(withChecksum(QStringLiteral(
        "$GNGGA,092750.000,5321.6802,S,00630.3372,W,2,10,0.8,61.7,M,55.2,M,,")));
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QVERIFY(qAbs(result.fix.latitude + 53.3613366667) < 0.0000001);
    QVERIFY(qAbs(result.fix.longitude + 6.50562) < 0.0000001);
    QCOMPARE(result.fix.fixQuality, 2);
}

void NmeaGgaParserTest::validatesChecksumKindAndFix()
{
    NmeaGgaParseResult result = NmeaGgaParser::parse(QString());
    QCOMPARE(result.errorCode, NmeaGgaParseError::EmptySentence);

    result = NmeaGgaParser::parse(QStringLiteral("$GPGGA,1,2"));
    QCOMPARE(result.errorCode, NmeaGgaParseError::MissingChecksum);

    result = NmeaGgaParser::parse(QString::fromUtf8(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,\xc2\xb0,M,,*00"));
    QCOMPARE(result.errorCode, NmeaGgaParseError::InvalidEncoding);

    result = NmeaGgaParser::parse(withChecksum(QStringLiteral(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,0,0,010100,,,")));
    QCOMPARE(result.errorCode, NmeaGgaParseError::NotGga);

    result = NmeaGgaParser::parse(QStringLiteral(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00"));
    QCOMPARE(result.errorCode, NmeaGgaParseError::ChecksumMismatch);

    result = NmeaGgaParser::parse(withChecksum(QStringLiteral(
        "$GPGGA,123519,4807.038,N,01131.000,E,0,00,99.9,545.4,M,46.9,M,,")));
    QCOMPARE(result.errorCode, NmeaGgaParseError::NoPositionFix);
}

void NmeaGgaParserTest::validatesCoordinatesAltitudeAndGeoid()
{
    const QStringList invalidBodies{
        QStringLiteral("$GPGGA,1,1260.0,N,01131.0,E,1,1,1,10,M,1,M,,"),
        QStringLiteral("$GPGGA,1,9000.1,N,01131.0,E,1,1,1,10,M,1,M,,"),
        QStringLiteral("$GPGGA,1,1200.0,Q,01131.0,E,1,1,1,10,M,1,M,,"),
        QStringLiteral("$GPGGA,1,1200.0,N,18100.0,E,1,1,1,10,M,1,M,,")
    };
    for (const QString &body : invalidBodies) {
        const NmeaGgaParseResult result = NmeaGgaParser::parse(
            withChecksum(body));
        QCOMPARE(result.errorCode, NmeaGgaParseError::InvalidCoordinate);
    }

    NmeaGgaParseResult result = NmeaGgaParser::parse(withChecksum(
        QStringLiteral("$GPGGA,1,1200.0,N,01131.0,E,1,1,1,nan,M,1,M,,")));
    QCOMPARE(result.errorCode, NmeaGgaParseError::InvalidAltitude);

    result = NmeaGgaParser::parse(withChecksum(
        QStringLiteral("$GPGGA,1,1200.0,N,01131.0,E,1,1,1,10,M,nan,M,,")));
    QCOMPARE(result.errorCode,
             NmeaGgaParseError::InvalidGeoidSeparation);

    result = NmeaGgaParser::parse(withChecksum(
        QStringLiteral("$GPGGA,1,0000.0,N,00000.1,E,1,x,bad,10,M,,M,,")));
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.fix.latitude, 0.0);
    QVERIFY(result.fix.longitude > 0.0);
    QCOMPARE(result.fix.satellites, 0);
    QCOMPARE(result.fix.hdop, 0.0);
    QVERIFY(!result.fix.hasGeoidSeparation);
    QCOMPARE(result.fix.geodeticAltitudeM(), -1000.0);
}

void NmeaGgaParserTest::checksumStopsAtAsterisk()
{
    QCOMPARE(NmeaGgaParser::checksum(QStringLiteral(
                 "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,")),
             QStringLiteral("47"));
    QCOMPARE(NmeaGgaParser::checksum(QStringLiteral("$ABC*00ignored")),
             NmeaGgaParser::checksum(QStringLiteral("$ABC")));
}

QTEST_GUILESS_MAIN(NmeaGgaParserTest)

#include "test_nmeaggaparser.moc"
