#include <QtTest>

#include "QGCParamID.h"

using OpalRT::QGCParamID;

class QGCParamIDTest final : public QObject
{
    Q_OBJECT

private slots:
    void shortIdsAreZeroPadded();
    void fullAndLongIdsUseExactlyTheWireField();
    void copiesOwnTheirWireStorage();
};

void QGCParamIDTest::shortIdsAreZeroPadded()
{
    const QGCParamID id(QStringLiteral("ARMING_CHECK"));
    const auto& wire = id.wireData();

    QCOMPARE(wire.size(), QGCParamID::WireSize);
    QCOMPARE(QByteArray(wire.data(), static_cast<int>(wire.size())),
             QByteArray("ARMING_CHECK\0\0\0\0", 16));
}

void QGCParamIDTest::fullAndLongIdsUseExactlyTheWireField()
{
    const QGCParamID full(QStringLiteral("1234567890ABCDEF"));
    QCOMPARE(QByteArray(full.wireData().data(), 16), QByteArray("1234567890ABCDEF", 16));

    const QGCParamID longId(QStringLiteral("1234567890ABCDEFGH"));
    QCOMPARE(QByteArray(longId.wireData().data(), 16), QByteArray("1234567890ABCDEF", 16));
    QCOMPARE(longId.getParamString(), QStringLiteral("1234567890ABCDEFGH"));
}

void QGCParamIDTest::copiesOwnTheirWireStorage()
{
    QGCParamID copy;
    {
        const QGCParamID original(QStringLiteral("SERIAL1_BAUD"));
        copy = original;
        QVERIFY(copy.wireData().data() != original.wireData().data());
    }

    QCOMPARE(QByteArray(copy.wireData().data(), 16), QByteArray("SERIAL1_BAUD\0\0\0\0", 16));
}

QTEST_APPLESS_MAIN(QGCParamIDTest)

#include "test_qgcparamid.moc"
