#include <QtTest>

#include "ui/CotIdentityModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>

class CotIdentityModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesExactEditableColumns();
    void addsEditsRemovesAndLooksUpStableRows();
    void preservesSystemIdCellsAndUsesFirstExactMatch();
    void roundTripsOfficialSixCellJson();
    void handlesNullMalformedAndUnsupportedSettingsSafely();
    void rejectsOversizedOrMalformedRowsWithoutDataLoss();
};

void CotIdentityModelTest::exposesExactEditableColumns()
{
    CotIdentityModel model;
    QCOMPARE(model.columnCount(), 6);
    const QStringList headers = {
        QStringLiteral("System ID"),
        QStringLiteral("Event UID"),
        QStringLiteral("TAKV"),
        QStringLiteral("Contact callsign"),
        QStringLiteral("Contact endpoint"),
        QStringLiteral("VMF")
    };
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(model.headerData(column, Qt::Horizontal).toString(),
                 headers.at(column));
    }

    QVERIFY(model.appendIdentity({42, QStringLiteral("UAS-42"), true,
                                  QStringLiteral("Falcon 42"),
                                  QStringLiteral("10.0.0.42:4242:tcp"),
                                  QStringLiteral("VMF-42")}));
    for (int column = 0; column < model.columnCount(); ++column) {
        const Qt::ItemFlags flags = model.flags(model.index(0, column));
        QVERIFY(flags.testFlag(Qt::ItemIsEnabled));
        QVERIFY(flags.testFlag(Qt::ItemIsSelectable));
        QVERIFY(flags.testFlag(Qt::ItemIsEditable));
    }
    QVERIFY(model.flags(model.index(0, CotIdentityModel::TakvColumn))
                .testFlag(Qt::ItemIsUserCheckable));
    QCOMPARE(model.data(model.index(0, CotIdentityModel::TakvColumn),
                        Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
}

void CotIdentityModelTest::addsEditsRemovesAndLooksUpStableRows()
{
    CotIdentityModel model;
    QVERIFY(model.appendIdentity({42, QStringLiteral("UAS-42")}));
    QVERIFY(model.appendIdentity({7, QStringLiteral("UAS-7")}));
    QVERIFY(model.insertIdentity(1, {11, QStringLiteral("UAS-11")}));
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(0).systemId),
             QStringLiteral("42"));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(1).systemId),
             QStringLiteral("11"));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(2).systemId),
             QStringLiteral("7"));

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QVERIFY(model.setData(model.index(1, CotIdentityModel::EventUidColumn),
                          QStringLiteral("Aircraft-11")));
    QVERIFY(model.setData(model.index(1, CotIdentityModel::TakvColumn),
                          Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(changed.count(), 2);

    CotIdentityRecord found;
    QVERIFY(model.identityForSystemId(11, &found));
    QCOMPARE(found.eventUid, QStringLiteral("Aircraft-11"));
    QVERIFY(found.includeTakv);
    QCOMPARE(model.rowForSystemId(7), 2);
    QVERIFY(!model.identityForSystemId(12, &found));

    QVERIFY(model.removeRows(1, 1));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(0).systemId),
             QStringLiteral("42"));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(1).systemId),
             QStringLiteral("7"));
    QCOMPARE(model.rowForSystemId(11), -1);

    QVERIFY(model.insertRows(1, 2));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(1).systemId),
             QStringLiteral("0"));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(2).systemId),
             QStringLiteral("1"));
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(3).systemId),
             QStringLiteral("7"));
}

void CotIdentityModelTest::preservesSystemIdCellsAndUsesFirstExactMatch()
{
    CotIdentityModel model;
    QVERIFY(model.appendIdentity({QStringLiteral("042"), QStringLiteral("leading")}));
    QVERIFY(model.appendIdentity({999, QStringLiteral("out-of-range")}));
    QVERIFY(model.appendIdentity({QStringLiteral("42"), QStringLiteral("first")}));
    QVERIFY(model.appendIdentity({42, QStringLiteral("second")}));
    CotIdentityRecord nullCell;
    nullCell.systemId = QJsonValue(QJsonValue::Null);
    nullCell.eventUid = QStringLiteral("null");
    QVERIFY(model.appendIdentity(nullCell));

    CotIdentityRecord found;
    QVERIFY(model.identityForSystemId(42, &found));
    QCOMPARE(found.eventUid, QStringLiteral("first"));
    QCOMPARE(model.rowForSystemId(42), 2);
    QVERIFY(!model.identityForSystemId(41, &found));

    const QModelIndex system = model.index(0, CotIdentityModel::SystemIdColumn);
    QVERIFY(model.setData(system, QStringLiteral("custom")));
    QCOMPARE(model.data(system).toString(), QStringLiteral("custom"));
    QVERIFY(model.setData(system, QVariant()));
    QVERIFY(model.identityAt(0).systemId.isNull());

    const QString oversized(CotIdentityModel::MaxTextLength + 1,
                            QLatin1Char('x'));
    QVERIFY(!model.setData(
        model.index(0, CotIdentityModel::ContactCallsignColumn), oversized));
    QVERIFY(model.identityAt(0).contactCallsign.isNull());
}

void CotIdentityModelTest::roundTripsOfficialSixCellJson()
{
    CotIdentityRecord first;
    first.systemId = 42;
    first.eventUid = QStringLiteral("UAS-42");
    first.includeTakv = true;
    first.contactCallsign = QStringLiteral("Falcon 42");
    first.contactEndpoint = QStringLiteral("10.0.0.42:4242:tcp");
    first.vmf = QStringLiteral("VMF-42");
    CotIdentityRecord second;
    second.systemId = 7;
    second.eventUid = QStringLiteral("UAS-7");
    second.contactCallsign = QString();

    CotIdentityModel model;
    model.setIdentities({first, second});
    const QByteArray json = model.toJson();
    const QJsonArray root = QJsonDocument::fromJson(json).array();
    QCOMPARE(root.size(), 2);
    QCOMPARE(root.at(0).toArray().size(), 6);
    QCOMPARE(root.at(0).toArray().at(0).toInt(), 42);
    QVERIFY(root.at(1).toArray().at(3).isNull());

    CotIdentityModel restored;
    QString error;
    QVERIFY(restored.loadSetting(QString::fromUtf8(json), &error));
    QVERIFY(error.isEmpty());
    const QVector<CotIdentityRecord> expected{first, second};
    QVERIFY(restored.identities() == expected);

    QJsonArray unusual;
    const auto row = [](const QJsonValue &systemId, const QString &uid) {
        QJsonArray value;
        value.append(systemId);
        value.append(uid);
        value.append(false);
        value.append(QJsonValue(QJsonValue::Null));
        value.append(QJsonValue(QJsonValue::Null));
        value.append(QJsonValue(QJsonValue::Null));
        return value;
    };
    unusual.append(row(QStringLiteral("custom"), QStringLiteral("custom-uid")));
    unusual.append(row(999, QStringLiteral("out-of-range")));
    unusual.append(row(QStringLiteral("042"), QStringLiteral("leading-zero")));
    unusual.append(row(QStringLiteral("42"), QStringLiteral("first-exact")));
    unusual.append(row(42, QStringLiteral("second-exact")));
    unusual.append(row(QJsonValue(QJsonValue::Null), QStringLiteral("null-id")));
    const QByteArray unusualJson =
        QJsonDocument(unusual).toJson(QJsonDocument::Compact);
    QVERIFY(restored.loadJson(unusualJson, &error));
    QCOMPARE(restored.toJson(), unusualJson);
    QCOMPARE(restored.rowCount(), 6);
    CotIdentityRecord exact;
    QVERIFY(restored.identityForSystemId(42, &exact));
    QCOMPARE(exact.eventUid, QStringLiteral("first-exact"));
}

void CotIdentityModelTest::handlesNullMalformedAndUnsupportedSettingsSafely()
{
    CotIdentityModel model;
    QVERIFY(model.appendIdentity({42, QStringLiteral("UAS-42")}));

    QString error;
    QVERIFY(!model.loadJson(QByteArrayLiteral("not json"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(CotIdentityModel::systemIdText(model.identityAt(0).systemId),
             QStringLiteral("42"));

    QVERIFY(!model.loadJson(QByteArrayLiteral("{}"), &error));
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(!model.loadSetting(QVariant(12.5), &error));
    QCOMPARE(model.rowCount(), 1);

    QVERIFY(model.loadSetting(QVariant(), &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.appendIdentity({7, QStringLiteral("UAS-7")}));
    QVERIFY(model.loadJson(QByteArray(), &error));
    QCOMPARE(model.rowCount(), 0);

    const QByteArray oversized(CotIdentityModel::MaxJsonBytes + 1, 'x');
    QVERIFY(!model.loadJson(oversized, &error));
    QVERIFY(!error.isEmpty());
}

void CotIdentityModelTest::rejectsOversizedOrMalformedRowsWithoutDataLoss()
{
    const QString longText(CotIdentityModel::MaxTextLength + 20,
                           QLatin1Char('u'));
    QJsonArray root;
    QJsonArray first;
    first.append(42);
    first.append(QStringLiteral("first"));
    first.append(true);
    first.append(longText);
    first.append(QJsonValue(QJsonValue::Null));
    first.append(QStringLiteral("vmf"));
    root.append(first);

    CotIdentityModel model;
    QVERIFY(model.appendIdentity({7, QStringLiteral("preserved")}));
    QString error;
    QVERIFY(!model.loadJson(QJsonDocument(root).toJson(QJsonDocument::Compact),
                            &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.identityAt(0).eventUid, QStringLiteral("preserved"));

    QJsonArray shortRow;
    shortRow.append(42);
    shortRow.append(QStringLiteral("uid"));
    QVERIFY(!model.loadJson(
        QJsonDocument(QJsonArray{shortRow}).toJson(QJsonDocument::Compact),
        &error));
    QCOMPARE(model.rowCount(), 1);
}

QTEST_GUILESS_MAIN(CotIdentityModelTest)
#include "test_cotidentitymodel.moc"
