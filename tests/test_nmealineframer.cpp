#include "comm/NmeaLineFramer.h"

#include <QtTest/QtTest>

class NmeaLineFramerTest final : public QObject
{
    Q_OBJECT

private slots:
    void framesFragmentedAndCoalescedRecords();
    void acceptsTheExactBound();
    void discardsOversizedRecordThroughNewline();
    void resetDropsPartialAndDiscardState();
};

void NmeaLineFramerTest::framesFragmentedAndCoalescedRecords()
{
    NmeaLineFramer framer;
    NmeaLineFramerResult result = framer.ingest(
        QByteArrayLiteral("$GPGGA,part"));
    QVERIFY(result.lines.isEmpty());
    QCOMPARE(framer.bufferedBytes(), 11);

    result = framer.ingest(
        QByteArrayLiteral("ial\r\n$GNGGA,second\n\nthird"));
    QCOMPARE(result.lines,
             QList<QByteArray>({QByteArrayLiteral("$GPGGA,partial"),
                                QByteArrayLiteral("$GNGGA,second"),
                                QByteArray()}));
    QCOMPARE(framer.bufferedBytes(), 5);

    result = framer.ingest(QByteArrayLiteral("\n"));
    QCOMPARE(result.lines,
             QList<QByteArray>({QByteArrayLiteral("third")}));
}

void NmeaLineFramerTest::acceptsTheExactBound()
{
    NmeaLineFramer framer(8);
    const NmeaLineFramerResult result = framer.ingest(
        QByteArrayLiteral("12345678\n"));
    QCOMPARE(result.oversizedLines, 0);
    QCOMPARE(result.lines,
             QList<QByteArray>({QByteArrayLiteral("12345678")}));
}

void NmeaLineFramerTest::discardsOversizedRecordThroughNewline()
{
    NmeaLineFramer framer(8);
    NmeaLineFramerResult result = framer.ingest(
        QByteArrayLiteral("123456789tail"));
    QCOMPARE(result.oversizedLines, 1);
    QVERIFY(result.lines.isEmpty());
    QVERIFY(framer.discardingOversizedLine());
    QCOMPARE(framer.bufferedBytes(), 0);

    result = framer.ingest(QByteArrayLiteral("$GPGGA,false\nvalid\n"));
    QCOMPARE(result.oversizedLines, 0);
    QCOMPARE(result.lines,
             QList<QByteArray>({QByteArrayLiteral("valid")}));
    QVERIFY(!framer.discardingOversizedLine());
}

void NmeaLineFramerTest::resetDropsPartialAndDiscardState()
{
    NmeaLineFramer framer(4);
    framer.ingest(QByteArrayLiteral("12345"));
    QVERIFY(framer.discardingOversizedLine());
    framer.reset();
    QVERIFY(!framer.discardingOversizedLine());
    QCOMPARE(framer.bufferedBytes(), 0);

    const NmeaLineFramerResult result = framer.ingest(
        QByteArrayLiteral("ok\n"));
    QCOMPARE(result.lines,
             QList<QByteArray>({QByteArrayLiteral("ok")}));
}

QTEST_GUILESS_MAIN(NmeaLineFramerTest)

#include "test_nmealineframer.moc"
