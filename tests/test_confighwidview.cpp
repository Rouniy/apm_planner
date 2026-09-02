#include <QtTest>

#include "ui/configuration/ConfigHWIDView.h"
#include "ui/configuration/ConfigHWIDViewModel.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {

ParameterRecord record(const QString &name, const QVariant &value, ParameterType type,
                       int component = 1)
{
    ParameterRecord result;
    result.key.componentId = static_cast<quint8>(component);
    result.key.name = name;
    result.value = value;
    result.type = type;
    return result;
}

quint32 deviceId(quint8 busType, quint8 bus, quint8 address, quint8 devtype)
{
    return static_cast<quint32>(busType & 0x7) | (static_cast<quint32>(bus & 0x1f) << 3) |
           (static_cast<quint32>(address) << 8) | (static_cast<quint32>(devtype) << 16);
}

QStringList column(const QAbstractItemModel *model, int column)
{
    QStringList result;
    for (int row = 0; row < model->rowCount(); ++row) {
        result << model->index(row, column).data(Qt::DisplayRole).toString();
    }
    return result;
}

QList<ParameterRecord> mixedSnapshot()
{
    return {
        record(QStringLiteral("INS_ACC_ID"), static_cast<int>(deviceId(2, 1, 0, 0x13)),
               ParameterType::Int32),
        record(QStringLiteral("COMPASS_DEV_ID"), 469530, ParameterType::Int32),
        record(QStringLiteral("GPS_TYPE"), 1, ParameterType::Int8),          // no _ID
        record(QStringLiteral("SYSID_THISMAV"), 1, ParameterType::Int16),    // no _ID
        record(QStringLiteral("RC_OPTIONS_IDX"), 5, ParameterType::Int32),   // _IDX excluded
        record(QStringLiteral("FRSKY_DEV_ID"), 7, ParameterType::Int32),     // FRSKY excluded
        record(QStringLiteral("MAV1_DEVID"), 14, ParameterType::Int32),
        record(QStringLiteral("BARO1_DEVID"), static_cast<int>(deviceId(1, 0, 0x76, 0x0B)),
               ParameterType::Int32),
        record(QStringLiteral("COMPASS_DEV_ID2"), -2147483641, ParameterType::Int32),
        record(QStringLiteral("COMPASS_DEV_ID"), static_cast<int>(deviceId(1, 2, 0x0E, 0x0A)),
               ParameterType::Int32, 2),
        record(QStringLiteral("BARO2_DEVID"), 0.0, ParameterType::Real32, 2),
    };
}

} // namespace

class ConfigHWIDViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void mavPortDeviceIdsUseTheFirmwarePortMapping_data();
    void mavPortDeviceIdsUseTheFirmwarePortMapping();
    void onlyNumberedMavDevidParametersUseThePortMapping_data();
    void onlyNumberedMavDevidParametersUseThePortMapping();
    void includesOnlyIdParametersLikeMissionPlanner();
    void decodesRowsLikeMissionPlanner();
    void rawIdWrapsSignedValues();
    void snapshotFiltersSortsAndSelectsComponent();
    void modelExposesSixReadOnlyColumns();
    void viewInventoryMatchesMissionPlanner();
    void viewSortsNumericColumnsNumerically();
    void refreshCarriesTheSelectedComponent();
    void emptySnapshotClearsTheTable();
};

void ConfigHWIDViewTest::mavPortDeviceIdsUseTheFirmwarePortMapping_data()
{
    QTest::addColumn<quint32>("value");
    QTest::addColumn<QString>("expected");

    QTest::newRow("0") << quint32(0) << QStringLiteral("Unknown");
    QTest::newRow("6") << quint32(6) << QStringLiteral("USB0");
    QTest::newRow("14") << quint32(14) << QStringLiteral("SERIAL1");
    QTest::newRow("22") << quint32(22) << QStringLiteral("SERIAL2");
    QTest::newRow("30") << quint32(30) << QStringLiteral("SERIAL3");
    QTest::newRow("38") << quint32(38) << QStringLiteral("SERIAL4");
    QTest::newRow("46") << quint32(46) << QStringLiteral("SERIAL5");
    QTest::newRow("54") << quint32(54) << QStringLiteral("SERIAL6");
    QTest::newRow("62") << quint32(62) << QStringLiteral("SERIAL7");
    QTest::newRow("70") << quint32(70) << QStringLiteral("SERIAL8");
    QTest::newRow("78") << quint32(78) << QStringLiteral("SERIAL9");
    QTest::newRow("174") << quint32(174) << QStringLiteral("NET_P1");
    QTest::newRow("182") << quint32(182) << QStringLiteral("NET_P2");
    QTest::newRow("190") << quint32(190) << QStringLiteral("NET_P3");
    QTest::newRow("198") << quint32(198) << QStringLiteral("NET_P4");
    QTest::newRow("334") << quint32(334) << QStringLiteral("CAN_D1_UC_S1");
    QTest::newRow("414") << quint32(414) << QStringLiteral("CAN_D2_UC_S1");
    QTest::newRow("494") << quint32(494) << QStringLiteral("SCR_SDEV1");
    QTest::newRow("502") << quint32(502) << QStringLiteral("SCR_SDEV2");
    QTest::newRow("999") << quint32(999) << QStringLiteral("Unknown (999)");
    QTest::newRow("max") << quint32(4294967295u) << QStringLiteral("Unknown (4294967295)");
}

void ConfigHWIDViewTest::mavPortDeviceIdsUseTheFirmwarePortMapping()
{
    QFETCH(quint32, value);
    QFETCH(QString, expected);
    QCOMPARE(ConfigHWIDViewModel::DecodeMavPortDeviceId(value), expected);
}

void ConfigHWIDViewTest::onlyNumberedMavDevidParametersUseThePortMapping_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("expected");

    QTest::newRow("MAV1_DEVID") << QStringLiteral("MAV1_DEVID") << true;
    QTest::newRow("mav12_devid") << QStringLiteral("mav12_devid") << true;
    QTest::newRow("Mav3_DevId") << QStringLiteral("Mav3_DevId") << true;
    QTest::newRow("MAV_DEVID") << QStringLiteral("MAV_DEVID") << false;
    QTest::newRow("MAVX_DEVID") << QStringLiteral("MAVX_DEVID") << false;
    QTest::newRow("MAV1X_DEVID") << QStringLiteral("MAV1X_DEVID") << false;
    QTest::newRow("MAV1_DEVID2") << QStringLiteral("MAV1_DEVID2") << false;
    QTest::newRow("XMAV1_DEVID") << QStringLiteral("XMAV1_DEVID") << false;
    QTest::newRow("COMPASS_DEV_ID") << QStringLiteral("COMPASS_DEV_ID") << false;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("unicode digit") << QStringLiteral("MAV٣_DEVID") << false;
}

void ConfigHWIDViewTest::onlyNumberedMavDevidParametersUseThePortMapping()
{
    QFETCH(QString, name);
    QFETCH(bool, expected);
    QCOMPARE(ConfigHWIDViewModel::IsMavPortDeviceId(name), expected);
}

void ConfigHWIDViewTest::includesOnlyIdParametersLikeMissionPlanner()
{
    QVERIFY(ConfigHWIDViewModel::IncludesParameter(QStringLiteral("COMPASS_DEV_ID")));
    QVERIFY(ConfigHWIDViewModel::IncludesParameter(QStringLiteral("BARO1_DEVID")));
    QVERIFY(ConfigHWIDViewModel::IncludesParameter(QStringLiteral("INS_ACC_ID")));
    QVERIFY(ConfigHWIDViewModel::IncludesParameter(QStringLiteral("MAV1_DEVID")));
    QVERIFY(!ConfigHWIDViewModel::IncludesParameter(QStringLiteral("RC_OPTIONS_IDX")));
    QVERIFY(!ConfigHWIDViewModel::IncludesParameter(QStringLiteral("FRSKY_DEV_ID")));
    QVERIFY(!ConfigHWIDViewModel::IncludesParameter(QStringLiteral("SYSID_THISMAV")));
    QVERIFY(!ConfigHWIDViewModel::IncludesParameter(QStringLiteral("GPS_TYPE")));
    QVERIFY(!ConfigHWIDViewModel::IncludesParameter(QStringLiteral("compass_dev_id"))); // ordinal
}

void ConfigHWIDViewTest::decodesRowsLikeMissionPlanner()
{
    // COMPASS_DEV_ID 2 | 3<<3 | 42<<8 | 7<<16 (the MP10 developer-tool fixture).
    HwIdRow row = ConfigHWIDViewModel::Decode(QStringLiteral("COMPASS_DEV_ID"), 469530u);
    QCOMPARE(row.paramName, QStringLiteral("COMPASS_DEV_ID"));
    QCOMPARE(row.devId, 469530);
    QCOMPARE(row.busType, QStringLiteral("SPI"));
    QCOMPARE(row.bus, 3);
    QCOMPARE(row.address, 42);
    QCOMPARE(row.devType, QStringLiteral("HMC5883"));

    // "COMP" (not only "COMPASS") selects the compass table.
    row = ConfigHWIDViewModel::Decode(QStringLiteral("COMP_PRIO1_ID"), deviceId(1, 0, 0x0E, 0x0D));
    QCOMPARE(row.busType, QStringLiteral("I2C"));
    QCOMPARE(row.address, 0x0E);
    QCOMPARE(row.devType, QStringLiteral("QMC5883L"));

    row = ConfigHWIDViewModel::Decode(QStringLiteral("BARO1_DEVID"), deviceId(1, 0, 0x76, 0x0B));
    QCOMPARE(row.busType, QStringLiteral("I2C"));
    QCOMPARE(row.bus, 0);
    QCOMPARE(row.address, 0x76);
    QCOMPARE(row.devType, QStringLiteral("BARO_MS5611"));

    row = ConfigHWIDViewModel::Decode(QStringLiteral("ASP1_DEVID"), deviceId(1, 1, 0x28, 0x02));
    QCOMPARE(row.devType, QStringLiteral("AIRSPEED_MS4525"));

    // MP10 quirk kept: ARSPD_* does not contain "ASP" and falls to the IMU table.
    row = ConfigHWIDViewModel::Decode(QStringLiteral("ARSPD_DEVID"), deviceId(1, 1, 0x28, 0x02));
    QCOMPARE(row.devType, QStringLiteral("2"));

    row = ConfigHWIDViewModel::Decode(QStringLiteral("INS_ACC_ID"), deviceId(2, 1, 0, 0x13));
    QCOMPARE(row.busType, QStringLiteral("SPI"));
    QCOMPARE(row.bus, 1);
    QCOMPARE(row.address, 0);
    QCOMPARE(row.devType, QStringLiteral("ACC_MPU6000"));

    // UAVCAN devices report SENSOR_ID#<devtype> regardless of the family.
    row = ConfigHWIDViewModel::Decode(QStringLiteral("COMPASS_DEV_ID3"), deviceId(3, 0, 125, 7));
    QCOMPARE(row.busType, QStringLiteral("UAVCAN"));
    QCOMPARE(row.bus, 0);
    QCOMPARE(row.address, 125);
    QCOMPARE(row.devType, QStringLiteral("SENSOR_ID#7"));
    row = ConfigHWIDViewModel::Decode(QStringLiteral("BARO3_DEVID"), deviceId(3, 1, 30, 0x0D));
    QCOMPARE(row.devType, QStringLiteral("SENSOR_ID#13"));

    // Numbered MAVn_DEVID rows use the port mapping, bus type MAVLink, zeros.
    row = ConfigHWIDViewModel::Decode(QStringLiteral("MAV1_DEVID"), 14u);
    QCOMPARE(row.devId, 14);
    QCOMPARE(row.busType, QStringLiteral("MAVLink"));
    QCOMPARE(row.bus, 0);
    QCOMPARE(row.address, 0);
    QCOMPARE(row.devType, QStringLiteral("SERIAL1"));
    row = ConfigHWIDViewModel::Decode(QStringLiteral("mav2_devid"), 999u);
    QCOMPARE(row.busType, QStringLiteral("MAVLink"));
    QCOMPARE(row.devType, QStringLiteral("Unknown (999)"));

    // High-bit ids show the signed INT32 value; unknown enums print numbers.
    row = ConfigHWIDViewModel::Decode(QStringLiteral("COMPASS_DEV_ID2"), 0x80000007u);
    QCOMPARE(row.devId, -2147483641);
    QCOMPARE(row.busType, QStringLiteral("7"));
    QCOMPARE(row.bus, 0);
    QCOMPARE(row.address, 0);
    QCOMPARE(row.devType, QStringLiteral("0"));
}

void ConfigHWIDViewTest::rawIdWrapsSignedValues()
{
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(469530)), 469530u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(-2147483641)), 0x80000007u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(static_cast<qint64>(-1))), 4294967295u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(4294967295u)), 4294967295u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(14.0)), 14u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(-5.9f)), 4294967291u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant(QStringLiteral("nope"))), 0u);
    QCOMPARE(ConfigHWIDViewModel::RawId(QVariant()), 0u);
}

void ConfigHWIDViewTest::snapshotFiltersSortsAndSelectsComponent()
{
    ConfigHWIDViewModel model;
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &ConfigHWIDViewModel::devicesChanged);

    // Component 1 (preferred and present): filtered, sorted by name.
    model.setParameterSnapshot(mixedSnapshot(), 1);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toInt(), 5);
    QCOMPARE(model.selectedComponent(), 1);
    QCOMPARE(column(&model, ConfigHWIDViewModel::ParamNameColumn), QStringList()
             << QStringLiteral("BARO1_DEVID") << QStringLiteral("COMPASS_DEV_ID")
             << QStringLiteral("COMPASS_DEV_ID2") << QStringLiteral("INS_ACC_ID")
             << QStringLiteral("MAV1_DEVID"));
    QCOMPARE(column(&model, ConfigHWIDViewModel::DevTypeColumn), QStringList()
             << QStringLiteral("BARO_MS5611") << QStringLiteral("HMC5883")
             << QStringLiteral("0") << QStringLiteral("ACC_MPU6000")
             << QStringLiteral("SERIAL1"));
    QCOMPARE(column(&model, ConfigHWIDViewModel::DevIDColumn), QStringList()
             << QString::number(static_cast<qint32>(deviceId(1, 0, 0x76, 0x0B)))
             << QStringLiteral("469530") << QStringLiteral("-2147483641")
             << QString::number(static_cast<qint32>(deviceId(2, 1, 0, 0x13)))
             << QStringLiteral("14"));
    const QVector<HwIdRow> devices = model.Devices();
    QCOMPARE(devices.size(), 5);
    QCOMPARE(devices.at(1), ConfigHWIDViewModel::Decode(QStringLiteral("COMPASS_DEV_ID"), 469530u));

    // Component 2 (preferred and present): only its rows.
    model.setParameterSnapshot(mixedSnapshot(), 2);
    QCOMPARE(model.selectedComponent(), 2);
    QCOMPARE(column(&model, ConfigHWIDViewModel::ParamNameColumn), QStringList()
             << QStringLiteral("BARO2_DEVID") << QStringLiteral("COMPASS_DEV_ID"));
    // BARO2_DEVID is 0.0 (Real32): bus type UNKNOWN, unknown baro type "0".
    QCOMPARE(column(&model, ConfigHWIDViewModel::DevTypeColumn), QStringList()
             << QStringLiteral("0") << QStringLiteral("IST8310"));
    QCOMPARE(column(&model, ConfigHWIDViewModel::BusTypeColumn), QStringList()
             << QStringLiteral("UNKNOWN") << QStringLiteral("I2C"));
    QCOMPARE(column(&model, ConfigHWIDViewModel::AddressColumn), QStringList()
             << QStringLiteral("0") << QStringLiteral("14"));

    // Absent preferred component: the lowest present component is shown.
    model.setParameterSnapshot(mixedSnapshot(), 9);
    QCOMPARE(model.selectedComponent(), 1);
    QCOMPARE(model.rowCount(), 5);
}

void ConfigHWIDViewTest::modelExposesSixReadOnlyColumns()
{
    ConfigHWIDViewModel model;
    QCOMPARE(model.columnCount(), 6);
    const QStringList headers{QStringLiteral("ParamName"), QStringLiteral("DevID"),
                              QStringLiteral("BusType"), QStringLiteral("Bus"),
                              QStringLiteral("Address"), QStringLiteral("DevType")};
    for (int section = 0; section < headers.size(); ++section) {
        QCOMPARE(model.headerData(section, Qt::Horizontal).toString(), headers.at(section));
    }
    QVERIFY(!model.headerData(0, Qt::Vertical).isValid());
    QVERIFY(!model.headerData(6, Qt::Horizontal).isValid());
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.data(model.index(0, 0)).isValid());

    model.setParameterSnapshot({record(QStringLiteral("COMPASS_DEV_ID"), 469530, ParameterType::Int32)}, 1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    const QModelIndex devId = model.index(0, ConfigHWIDViewModel::DevIDColumn);
    QCOMPARE(devId.data(Qt::DisplayRole).toString(), QStringLiteral("469530"));
    QCOMPARE(devId.data(ConfigHWIDViewModel::SortRole).type(), QVariant::Int);
    QCOMPARE(devId.data(ConfigHWIDViewModel::SortRole).toInt(), 469530);
    QCOMPARE(model.index(0, ConfigHWIDViewModel::BusColumn).data(ConfigHWIDViewModel::SortRole).toInt(), 3);
    QCOMPARE(model.index(0, ConfigHWIDViewModel::AddressColumn).data(ConfigHWIDViewModel::SortRole).toInt(), 42);
    QCOMPARE(model.index(0, ConfigHWIDViewModel::BusTypeColumn).data(ConfigHWIDViewModel::SortRole).toString(),
             QStringLiteral("SPI"));
    QCOMPARE(model.index(0, ConfigHWIDViewModel::DevTypeColumn).data().toString(), QStringLiteral("HMC5883"));
    QVERIFY(!model.index(0, 0).data(Qt::EditRole).isValid());
    QCOMPARE(model.flags(model.index(0, 0)), Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    QVERIFY(!(model.flags(model.index(0, 1)) & Qt::ItemIsEditable));
    QCOMPARE(model.flags(QModelIndex()), Qt::ItemFlags(Qt::NoItemFlags));
    QVERIFY(!model.data(model.index(0, 6)).isValid());
    QVERIFY(!model.data(model.index(1, 0)).isValid());
}

void ConfigHWIDViewTest::viewInventoryMatchesMissionPlanner()
{
    ConfigHWIDView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigHWIDView"));
    QVERIFY(view.viewModel());
    QVERIFY(view.viewModel()->parent() == &view);
    auto *layout = qobject_cast<QVBoxLayout *>(view.layout());
    QVERIFY(layout);
    QCOMPARE(layout->contentsMargins(), QMargins(16, 16, 16, 16));

    auto *title = view.findChild<QLabel *>(QStringLiteral("hwIdTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("HW ID"));
    auto *refresh = view.findChild<QPushButton *>(QStringLiteral("hwIdRefreshButton"));
    QVERIFY(refresh);
    QCOMPARE(refresh->text(), QStringLiteral("Refresh"));
    QCOMPARE(view.findChildren<QPushButton *>().size(), 1);

    QTableView *table = view.deviceTable();
    QVERIFY(table);
    QCOMPARE(table->objectName(), QStringLiteral("hwIdTable"));
    QCOMPARE(table->model(), view.sortProxy());
    QCOMPARE(view.sortProxy()->sourceModel(), view.viewModel());
    QCOMPARE(view.sortProxy()->sortRole(), static_cast<int>(ConfigHWIDViewModel::SortRole));
    QCOMPARE(table->editTriggers(),
             QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers)); // IsReadOnly
    QVERIFY(table->isSortingEnabled());                                  // CanUserSortColumns
    QVERIFY(table->showGrid());                                          // GridLinesVisibility=All
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(0), QHeaderView::Interactive);
    QVERIFY(table->horizontalHeader()->stretchLastSection());            // DevType Width="*"
    QVERIFY(!table->verticalHeader()->isVisible() || table->verticalHeader()->isHidden());
    const int widths[] = {200, 120, 100, 70, 90};
    for (int section = 0; section < 5; ++section) {
        QCOMPARE(table->columnWidth(section), widths[section]);
    }
    QCOMPARE(table->model()->columnCount(), 6);
    QCOMPARE(table->model()->headerData(5, Qt::Horizontal).toString(), QStringLiteral("DevType"));

    // Injected model variant.
    ConfigHWIDViewModel model;
    ConfigHWIDView injected(&model);
    QCOMPARE(injected.viewModel(), &model);
    QVERIFY(model.parent() != &injected);
}

void ConfigHWIDViewTest::viewSortsNumericColumnsNumerically()
{
    ConfigHWIDView view;
    view.setParameterSnapshot({
        record(QStringLiteral("MAV1_DEVID"), 14, ParameterType::Int32),
        record(QStringLiteral("COMPASS_DEV_ID"), -5, ParameterType::Int32),
        record(QStringLiteral("BARO1_DEVID"), 100, ParameterType::Int32),
        record(QStringLiteral("INS_ACC_ID"), 3, ParameterType::Int32),
    }, 1);
    QAbstractItemModel *shown = view.deviceTable()->model();
    QCOMPARE(shown->rowCount(), 4);
    QCOMPARE(column(shown, ConfigHWIDViewModel::ParamNameColumn), QStringList()
             << QStringLiteral("BARO1_DEVID") << QStringLiteral("COMPASS_DEV_ID")
             << QStringLiteral("INS_ACC_ID") << QStringLiteral("MAV1_DEVID"));

    view.deviceTable()->sortByColumn(ConfigHWIDViewModel::DevIDColumn, Qt::AscendingOrder);
    QCOMPARE(column(shown, ConfigHWIDViewModel::DevIDColumn), QStringList()
             << QStringLiteral("-5") << QStringLiteral("3") << QStringLiteral("14")
             << QStringLiteral("100"));
    view.deviceTable()->sortByColumn(ConfigHWIDViewModel::DevIDColumn, Qt::DescendingOrder);
    QCOMPARE(column(shown, ConfigHWIDViewModel::DevIDColumn), QStringList()
             << QStringLiteral("100") << QStringLiteral("14") << QStringLiteral("3")
             << QStringLiteral("-5"));
    view.deviceTable()->sortByColumn(ConfigHWIDViewModel::ParamNameColumn, Qt::AscendingOrder);
    QCOMPARE(column(shown, ConfigHWIDViewModel::ParamNameColumn).first(),
             QStringLiteral("BARO1_DEVID"));
}

void ConfigHWIDViewTest::refreshCarriesTheSelectedComponent()
{
    ConfigHWIDView view;
    QSignalSpy refreshRequested(&view, &ConfigHWIDView::refreshRequested);
    auto *refresh = view.findChild<QPushButton *>(QStringLiteral("hwIdRefreshButton"));
    QVERIFY(refresh);

    refresh->click();
    QCOMPARE(refreshRequested.count(), 1);
    QCOMPARE(refreshRequested.at(0).at(0).toInt(), 1); // nothing loaded yet

    view.setParameterSnapshot(mixedSnapshot(), 2);
    refresh->click();
    QCOMPARE(refreshRequested.count(), 2);
    QCOMPARE(refreshRequested.at(1).at(0).toInt(), 2);

    view.setParameterSnapshot(mixedSnapshot(), 9); // absent: lowest component
    QCOMPARE(view.viewModel()->selectedComponent(), 1);
    refresh->click();
    QCOMPARE(refreshRequested.at(2).at(0).toInt(), 1);
}

void ConfigHWIDViewTest::emptySnapshotClearsTheTable()
{
    ConfigHWIDView view;
    view.setParameterSnapshot(mixedSnapshot(), 1);
    QCOMPARE(view.deviceTable()->model()->rowCount(), 5);

    QSignalSpy reset(view.viewModel(), &QAbstractItemModel::modelReset);
    view.setParameterSnapshot({}, 1);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(view.deviceTable()->model()->rowCount(), 0);
    QCOMPARE(view.viewModel()->Devices().size(), 0);
    QCOMPARE(view.viewModel()->selectedComponent(), 1);

    // A snapshot without any id parameter is also empty but keeps the
    // component that was actually present.
    view.setParameterSnapshot({record(QStringLiteral("GPS_TYPE"), 1, ParameterType::Int8, 3)}, 1);
    QCOMPARE(view.deviceTable()->model()->rowCount(), 0);
    QCOMPARE(view.viewModel()->selectedComponent(), 3);
}

QTEST_MAIN(ConfigHWIDViewTest)
#include "test_confighwidview.moc"
