#include "comm/FollowMeGpsInput.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class FakeFollowMeGpsSource final : public FollowMeGpsByteSource
{
    Q_OBJECT

public:
    explicit FakeFollowMeGpsSource(QObject *parent = nullptr)
        : FollowMeGpsByteSource(parent)
    {
    }

    bool open(const QString &portName, int baud, QString *error) override
    {
        openedPort = portName;
        openedBaud = baud;
        if (!openSucceeds) {
            if (error) {
                *error = QStringLiteral("synthetic open failure");
            }
            return false;
        }
        openState = true;
        return true;
    }

    void close() override
    {
        wasClosed = true;
        openState = false;
    }

    bool isOpen() const override { return openState; }

    void deliver(const QByteArray &bytes) { emit bytesReceived(bytes); }
    void fail(const QString &description) { emit resourceError(description); }

    QString openedPort;
    int openedBaud = 0;
    bool openSucceeds = true;
    bool openState = false;
    bool wasClosed = false;
};

class FollowMeGpsInputTest final : public QObject
{
    Q_OBJECT

private slots:
    void portsAndBaudsMatchReference();
    void fragmentedAndCoalescedLinesProduceFixes();
    void noFixAndMalformedSentencesAreDistinct();
    void oversizedLineIsDiscardedThroughNewline();
    void openValidationAndResourceFailureStopInput();
};

namespace
{
QString withChecksum(const QString &body)
{
    return body + QLatin1Char('*') + NmeaGgaParser::checksum(body);
}

const QByteArray kFix = QByteArrayLiteral(
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
}

void FollowMeGpsInputTest::portsAndBaudsMatchReference()
{
    FollowMeGpsInput input(
        {}, []() {
            return QStringList{QStringLiteral("ttyUSB1"),
                               QStringLiteral("ttyUSB0"),
                               QStringLiteral("ttyUSB1"), QString()};
        });
    QCOMPARE(input.availablePorts(),
             QStringList({QStringLiteral("ttyUSB0"),
                          QStringLiteral("ttyUSB1")}));
    QCOMPARE(FollowMeGpsInput::supportedBaudRates(),
             QList<int>({4800, 9600, 14400, 19200,
                         28800, 38400, 57600, 115200}));
}

void FollowMeGpsInputTest::fragmentedAndCoalescedLinesProduceFixes()
{
    FollowMeGpsInput input;
    QSignalSpy fixes(&input, &FollowMeGpsInput::fixReceived);

    input.ingestBytesForTesting(kFix.left(19));
    QCOMPARE(fixes.count(), 0);
    input.ingestBytesForTesting(kFix.mid(19) + kFix);
    QCOMPARE(fixes.count(), 2);

    const NmeaGgaFix fix = qvariant_cast<NmeaGgaFix>(
        fixes.at(0).at(0));
    QVERIFY(qAbs(fix.latitude - 48.1173) < 0.0000001);
    QVERIFY(qAbs(fix.longitude - 11.5166666667) < 0.0000001);
}

void FollowMeGpsInputTest::noFixAndMalformedSentencesAreDistinct()
{
    FollowMeGpsInput input;
    QSignalSpy noFix(&input, &FollowMeGpsInput::noPositionFix);
    QSignalSpy rejected(&input, &FollowMeGpsInput::sentenceRejected);

    const QString noFixBody = QStringLiteral(
        "$GNGGA,123519,4807.038,N,01131.000,E,0,00,99.9,545.4,M,46.9,M,,");
    input.ingestBytesForTesting(
        (withChecksum(noFixBody) + QStringLiteral("\n")).toLatin1());
    QCOMPARE(noFix.count(), 1);
    QCOMPARE(rejected.count(), 0);

    input.ingestBytesForTesting(QByteArrayLiteral("$GPGGA,bad*00\n"));
    QCOMPARE(noFix.count(), 1);
    QCOMPARE(rejected.count(), 1);
    QVERIFY(qvariant_cast<NmeaGgaParseError>(rejected.at(0).at(0))
            != NmeaGgaParseError::NoPositionFix);
}

void FollowMeGpsInputTest::oversizedLineIsDiscardedThroughNewline()
{
    FollowMeGpsInput input;
    QSignalSpy fixes(&input, &FollowMeGpsInput::fixReceived);
    QSignalSpy rejected(&input, &FollowMeGpsInput::lineRejected);

    QByteArray oversized(FollowMeGpsInput::MaximumLineCharacters + 25,
                         'x');
    oversized.append(kFix); // Its newline ends the oversized record.
    input.ingestBytesForTesting(oversized);
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(fixes.count(), 0);

    input.ingestBytesForTesting(kFix);
    QCOMPARE(fixes.count(), 1);
}

void FollowMeGpsInputTest::openValidationAndResourceFailureStopInput()
{
    FakeFollowMeGpsSource *source = nullptr;
    FollowMeGpsInput input(
        [&source](QObject *parent) {
            source = new FakeFollowMeGpsSource(parent);
            return source;
        }, []() { return QStringList(); });

    QString error;
    QVERIFY(!input.open(QString(), 4800, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!input.open(QStringLiteral("ttyGPS"), 12345, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(input.open(QStringLiteral(" ttyGPS "), 4800, &error));
    QVERIFY(input.isOpen());
    QCOMPARE(input.portName(), QStringLiteral("ttyGPS"));
    QCOMPARE(input.baudRate(), 4800);
    QCOMPARE(source->openedPort, QStringLiteral("ttyGPS"));
    QCOMPARE(source->openedBaud, 4800);

    QSignalSpy stopped(&input, &FollowMeGpsInput::inputStopped);
    source->fail(QStringLiteral("GPS unplugged"));
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(stopped.at(0).at(0).toString(),
             QStringLiteral("GPS unplugged"));
    QVERIFY(!input.isOpen());
    QVERIFY(source->wasClosed);
}

QTEST_GUILESS_MAIN(FollowMeGpsInputTest)

#include "test_followmegpsinput.moc"
