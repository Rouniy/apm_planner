#include <QtTest>

#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"

class ParameterCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void duplicatePacketsDoNotFakeCompletion();
    void sameNameFromTwoComponentsDoesNotCollide();
    void newlyDiscoveredComponentReopensCompleteness();
    void targetChangeInvalidatesSnapshots();
    void cancellationKeepsPartialSnapshot();
    void codecPreservesBytewiseTypes();
    void integerAndFloatEqualityAreTypeAware();
};

void ParameterCoreTest::duplicatePacketsDoNotFakeCompletion()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();

    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QVERIFY(store.ingest(1, 2, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QCOMPARE(store.progress().received, 1);
    QCOMPARE(store.progress().reported, 2);
    QCOMPARE(store.state(), ParameterLoadState::Loading);

    QVERIFY(store.ingest(1, 2, 1, QStringLiteral("B"), 2, ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.state(), ParameterLoadState::Complete);
    QVERIFY(store.specializedPagesReady());
}

void ParameterCoreTest::sameNameFromTwoComponentsDoesNotCollide()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("VERSION"), 10, ParameterType::Int32));
    QVERIFY(store.ingest(154, 1, 0, QStringLiteral("VERSION"), 20, ParameterType::Int32));

    const ParameterSnapshot snapshot = store.snapshot();
    QCOMPARE(snapshot.records().size(), 2);
    QCOMPARE(snapshot.value(1, QStringLiteral("VERSION")).value.toInt(), 10);
    QCOMPARE(snapshot.value(154, QStringLiteral("VERSION")).value.toInt(), 20);
}

void ParameterCoreTest::newlyDiscoveredComponentReopensCompleteness()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    QCOMPARE(store.state(), ParameterLoadState::Complete);

    QVERIFY(store.ingest(154, 2, 0, QStringLiteral("B"), 2, ParameterType::Int32));
    QCOMPARE(store.progress().received, 2);
    QCOMPARE(store.progress().reported, 3);
    QCOMPARE(store.state(), ParameterLoadState::Loading);
    QVERIFY(!store.specializedPagesReady());

    QVERIFY(store.ingest(154, 2, 1, QStringLiteral("C"), 3, ParameterType::Int32));
    QCOMPARE(store.state(), ParameterLoadState::Complete);
}

void ParameterCoreTest::targetChangeInvalidatesSnapshots()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 1, 0, QStringLiteral("OLD"), 10, ParameterType::Int32));
    const ParameterSnapshot oldSnapshot = store.snapshot();

    store.selectTarget(7, 2, 1);
    const ParameterSnapshot current = store.snapshot();
    QVERIFY(oldSnapshot.contains(1, QStringLiteral("OLD")));
    QVERIFY(!current.contains(1, QStringLiteral("OLD")));
    QVERIFY(current.target().revision > oldSnapshot.target().revision);
}

void ParameterCoreTest::cancellationKeepsPartialSnapshot()
{
    ParameterStore store;
    store.selectTarget(7, 1, 1);
    store.beginLoad();
    QVERIFY(store.ingest(1, 3, 0, QStringLiteral("A"), 1, ParameterType::Int32));
    store.cancelLoad();

    QCOMPARE(store.state(), ParameterLoadState::Cancelled);
    QCOMPARE(store.progress().received, 1);
    QVERIFY(store.snapshot().contains(1, QStringLiteral("A")));
    QVERIFY(!store.specializedPagesReady());
}

void ParameterCoreTest::codecPreservesBytewiseTypes()
{
    const QList<QPair<ParameterType, QVariant>> values = {
        {ParameterType::UInt8, QVariant::fromValue<quint32>(255)},
        {ParameterType::Int8, QVariant::fromValue<qint32>(-128)},
        {ParameterType::UInt16, QVariant::fromValue<quint32>(65535)},
        {ParameterType::Int16, QVariant::fromValue<qint32>(-32768)},
        {ParameterType::UInt32, QVariant::fromValue<quint32>(4000000000U)},
        {ParameterType::Int32, QVariant::fromValue<qint32>(-2147483647)}
    };

    for (const auto &entry : values) {
        bool encoded = false;
        const float wire = ParameterCodec::encodeClassic(
            entry.second, entry.first, ParameterEncoding::Bytewise, &encoded);
        QVERIFY(encoded);
        bool decoded = false;
        const QVariant roundTrip = ParameterCodec::decodeClassic(
            wire, entry.first, ParameterEncoding::Bytewise, &decoded);
        QVERIFY(decoded);
        QVERIFY(ParameterCodec::valuesEqual(roundTrip, entry.second, entry.first));
    }
}

void ParameterCoreTest::integerAndFloatEqualityAreTypeAware()
{
    QVERIFY(!ParameterCodec::valuesEqual(1, 2, ParameterType::Int32));
    QVERIFY(ParameterCodec::valuesEqual(1.0, 1.0 + 1.0e-8, ParameterType::Real32));
    QVERIFY(!ParameterCodec::valuesEqual(1.0, 1.01, ParameterType::Real32));
}

QTEST_APPLESS_MAIN(ParameterCoreTest)

#include "test_parametercore.moc"
