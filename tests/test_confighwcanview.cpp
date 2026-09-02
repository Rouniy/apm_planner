#include <QtTest>

#include "ui/configuration/ConfigHWCANView.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:BRD_CAN_ENABLE"
               humanName="Enable use of CAN buses"
               documentation="Enabling this option enables use of CAN buses.">
          <values>
            <value code="0">Disabled</value>
            <value code="1">Enabled first channel</value>
            <value code="2">Enabled both channels</value>
          </values>
          <field name="RebootRequired">True</field>
        </param>
        <param name="ArduCopter:CAN_P1_DRIVER"
               humanName="CAN port 1 driver">
          <values>
            <value code="0">Disabled</value>
            <value code="1">First driver</value>
            <value code="2">Second driver</value>
          </values>
          <field name="RebootRequired">True</field>
        </param>
        <param name="ArduCopter:CAN_P1_BITRATE"
               humanName="CAN port 1 bitrate">
          <field name="Range">10000 1000000</field>
          <field name="Increment">1</field>
          <field name="Units">bit/s</field>
        </param>
        <param name="ArduCopter:CAN_P1_FDBITRATE"
               humanName="CAN port 1 FD bitrate">
          <values>
            <value code="1">1M</value>
            <value code="2">2M</value>
            <value code="4">4M</value>
            <value code="5">5M</value>
            <value code="8">8M</value>
          </values>
        </param>
        <param name="ArduCopter:CAN_D1_PROTOCOL"
               humanName="CAN driver 1 protocol">
          <values>
            <value code="0">Disabled</value>
            <value code="1">DroneCAN</value>
            <value code="10">Scripting</value>
          </values>
          <field name="RebootRequired">True</field>
        </param>
        <param name="ArduCopter:CAN_D1_PROTOCOL2"
               humanName="CAN driver 1 secondary protocol">
          <values>
            <value code="0">Disabled</value>
            <value code="10">Scripting</value>
          </values>
          <field name="RebootRequired">True</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot(int component = 1,
                                             int value = 1)
{
    return {{component, QStringLiteral("BRD_CAN_ENABLE"), value}};
}

QList<ConfigFriendlyParameterValue> modernSnapshot(int component = 1)
{
    return {
        {component, QStringLiteral("CAN_P1_DRIVER"), 1},
        {component, QStringLiteral("CAN_P1_BITRATE"), 1000000},
        {component, QStringLiteral("CAN_P1_FDBITRATE"), 4},
        // Sparse instances deliberately exercise metadata fallback to P1/D1.
        {component, QStringLiteral("CAN_P2_DRIVER"), 2},
        {component, QStringLiteral("CAN_D1_PROTOCOL"), 1},
        {component, QStringLiteral("CAN_D1_PROTOCOL2"), 0},
        {component, QStringLiteral("CAN_D2_PROTOCOL"), 10}
    };
}

ParamField fieldNamed(const ConfigHWCANViewModel &model,
                      const QString &name)
{
    for (const ParamField &field : model.Fields()) {
        if (field.name == name) {
            return field;
        }
    }
    return {};
}
} // namespace

class ConfigHWCANViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void hydratesExactMissionPlannerOptionsAndMissingState();
    void parameterWriteRequiresExactEchoAndRollsBackOnFailure();
    void adaptsToModernSparseCanPortsAndDrivers();
    void commandsUseExactMavlinkContractAndAckLifecycle();
    void factoryResetRequiresFreshExplicitArm();
    void widgetMatchesMissionPlannerSurfaceAndBindings();
};

void ConfigHWCANViewTest::
    hydratesExactMissionPlannerOptionsAndMissingState()
{
    ConfigHWCANViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot(7, 1), 7);

    QCOMPARE(model.Title(), QStringLiteral("CAN"));
    QCOMPARE(model.ComponentId(), 7);
    QVERIFY(model.HasCanEnableParameter());
    QCOMPARE(model.SelectedCanEnable().toInt(), 1);
    QCOMPARE(model.CanEnableOptions().size(), 3);
    QCOMPARE(model.CanEnableOptions().at(0).value.toInt(), 0);
    QCOMPARE(model.CanEnableOptions().at(0).text,
             QStringLiteral("Disabled"));
    QCOMPARE(model.CanEnableOptions().at(1).text,
             QStringLiteral("Enabled first channel"));
    QCOMPARE(model.CanEnableOptions().at(2).text,
             QStringLiteral("Enabled both channels"));
    QCOMPARE(ConfigHWCANViewModel::WriteTimeoutMs(), 5000);
    QCOMPARE(ConfigHWCANViewModel::CommandTimeoutMs(), 5000);

    model.setParameterSnapshot({}, 3);
    QCOMPARE(model.ComponentId(), 3);
    QVERIFY(!model.HasCanEnableParameter());
    QVERIFY(!model.SelectedCanEnable().isValid());
    QCOMPARE(model.Status(), QStringLiteral(
        "No CAN configuration parameters are available."));
}

void ConfigHWCANViewTest::
    parameterWriteRequiresExactEchoAndRollsBackOnFailure()
{
    ConfigHWCANViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot(4, 1), 4);
    QSignalSpy writes(&model, &ConfigHWCANViewModel::writeRequested);

    QVERIFY(!model.SetSelectedCanEnable(2));
    QCOMPARE(model.Status(), QStringLiteral("offline"));
    model.setConnected(true);
    QVERIFY(model.SetSelectedCanEnable(2));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 4);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("BRD_CAN_ENABLE"));
    QCOMPARE(writes.first().at(2).toInt(), 2);
    QVERIFY(model.Busy());
    QVERIFY(!model.CanEditCanEnable());

    model.parameterChanged(3, QStringLiteral("BRD_CAN_ENABLE"), 2);
    QVERIFY(model.Busy());
    model.parameterChanged(4, QStringLiteral("BRD_CAN_ENABLE"), 2);
    QVERIFY(!model.Busy());
    QCOMPARE(model.SelectedCanEnable().toInt(), 2);
    QCOMPARE(model.Status(), QStringLiteral(
        "BRD_CAN_ENABLE = 2. Restart the vehicle to apply it."));

    QVERIFY(model.SetSelectedCanEnable(0));
    model.parameterWriteFailed(4, QStringLiteral("BRD_CAN_ENABLE"),
                               QStringLiteral("link lost"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.SelectedCanEnable().toInt(), 2);
    QCOMPARE(model.Status(),
             QStringLiteral("Set BRD_CAN_ENABLE failed: link lost"));

    QVERIFY(model.SetSelectedCanEnable(1));
    model.setArmed(true);
    QVERIFY(!model.Busy());
    QCOMPARE(model.SelectedCanEnable().toInt(), 2);
    model.parameterChanged(4, QStringLiteral("BRD_CAN_ENABLE"), 1);
    QCOMPARE(model.SelectedCanEnable().toInt(), 1);
    model.setArmed(false);
    model.setConnected(false);
    QVERIFY(!model.Busy());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.SelectedCanEnable().isValid());
    QCOMPARE(model.Status(), QStringLiteral("offline"));

    model.parameterChanged(4, QStringLiteral("BRD_CAN_ENABLE"), 1);
    QVERIFY(!model.HasCanEnableParameter());
}

void ConfigHWCANViewTest::adaptsToModernSparseCanPortsAndDrivers()
{
    ConfigHWCANViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(modernSnapshot(9), 9);
    QCOMPARE(model.ComponentId(), 9);
    QVERIFY(!model.HasCanEnableParameter());
    QVERIFY(model.HasModernFields());
    QCOMPARE(model.PhysicalPortIndexes(), QList<int>({1, 2}));
    QCOMPARE(model.DriverIndexes(), QList<int>({1, 2}));
    QCOMPARE(model.Fields().size(), 7);

    const ParamField portDriver = fieldNamed(
        model, QStringLiteral("CAN_P1_DRIVER"));
    QVERIFY(portDriver.editorKind == ParamField::EditorKind::Combo);
    QCOMPARE(portDriver.options.size(), 3);
    QCOMPARE(portDriver.status, QStringLiteral("restart required"));
    const ParamField sparsePort = fieldNamed(
        model, QStringLiteral("CAN_P2_DRIVER"));
    QCOMPARE(sparsePort.options.size(), 3);
    const ParamField bitrate = fieldNamed(
        model, QStringLiteral("CAN_P1_BITRATE"));
    QVERIFY(bitrate.editorKind == ParamField::EditorKind::Numeric);
    QCOMPARE(bitrate.minimum, 10000.0);
    QCOMPARE(bitrate.maximum, 1000000.0);
    QCOMPARE(bitrate.units, QStringLiteral("bit/s"));
    const ParamField sparseProtocol = fieldNamed(
        model, QStringLiteral("CAN_D2_PROTOCOL"));
    QCOMPARE(sparseProtocol.options.size(), 3);
    QCOMPARE(sparseProtocol.status, QStringLiteral("restart required"));

    QSignalSpy writes(&model, &ConfigHWCANViewModel::writeRequested);
    model.setConnected(true);
    QVERIFY(model.setFieldValue(QStringLiteral("CAN_P1_DRIVER"), 2));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 9);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("CAN_P1_DRIVER"));
    QCOMPARE(writes.first().at(2).toLongLong(), qlonglong(2));
    model.parameterChanged(8, QStringLiteral("CAN_P1_DRIVER"), 2);
    QVERIFY(model.Busy());
    model.parameterChanged(9, QStringLiteral("CAN_P1_DRIVER"), 2);
    QVERIFY(!model.Busy());
    QVERIFY(model.Status().contains(QStringLiteral("Restart")));

    QVERIFY(model.setFieldValue(QStringLiteral("CAN_P1_BITRATE"), 500000));
    model.parameterChanged(9, QStringLiteral("CAN_P1_BITRATE"), 500001);
    QVERIFY(!model.Busy());
    QCOMPARE(fieldNamed(model, QStringLiteral("CAN_P1_BITRATE"))
                 .value.toInt(), 500001);
    QVERIFY(model.Status().contains(QStringLiteral("different value")));
}

void ConfigHWCANViewTest::
    commandsUseExactMavlinkContractAndAckLifecycle()
{
    ConfigHWCANViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot(6), 6);
    QSignalSpy commands(&model, &ConfigHWCANViewModel::commandRequested);

    QVERIFY(!model.StartEnumeration());
    QCOMPARE(model.Status(),
             QStringLiteral("Not connected — open a link first."));
    model.setConnected(true);

    QVERIFY(model.StartEnumeration());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.last().at(0).toInt(), 6);
    QCOMPARE(commands.last().at(1).toInt(),
             ConfigHWCANViewModel::PreflightUavcanCommand());
    QCOMPARE(commands.last().at(2).toFloat(), 1.0f);
    QCOMPARE(commands.last().at(3).toFloat(), 0.0f);
    QVERIFY(model.Busy());
    QVERIFY(!model.StopEnumeration());

    model.setArmed(true);
    QVERIFY(model.Busy());
    QVERIFY(!model.CanIssueCommands());

    model.commandAckReceived(6,
        ConfigHWCANViewModel::PreflightUavcanCommand(), 5);
    QVERIFY(model.Busy());
    QVERIFY(model.Status().contains(QStringLiteral("in progress")));
    model.commandAckReceived(6,
        ConfigHWCANViewModel::PreflightUavcanCommand(), 0);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("Start Enumeration accepted."));
    model.setArmed(false);

    QVERIFY(model.StopEnumeration());
    QCOMPARE(commands.last().at(1).toInt(),
             ConfigHWCANViewModel::PreflightUavcanCommand());
    QCOMPARE(commands.last().at(2).toFloat(), 0.0f);
    model.commandSendFailed(6,
        ConfigHWCANViewModel::PreflightUavcanCommand(),
        QStringLiteral("no connected link"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral(
        "Stop Enumeration failed: no connected link"));

    QVERIFY(model.StartEnumeration());
    model.setConnected(false);
    QCOMPARE(model.Status(), QStringLiteral("offline"));
    model.commandAckReceived(6,
        ConfigHWCANViewModel::PreflightUavcanCommand(), 0);
    QCOMPARE(model.Status(), QStringLiteral("offline"));
    model.setParameterSnapshot(snapshot(6), 6);
    model.setConnected(true);

    QVERIFY(model.SaveConfig());
    QCOMPARE(commands.last().at(1).toInt(),
             ConfigHWCANViewModel::PreflightStorageCommand());
    QCOMPARE(commands.last().at(2).toFloat(), 1.0f);
    QCOMPARE(commands.last().at(3).toFloat(), 0.0f);
    model.commandAckReceived(6,
        ConfigHWCANViewModel::PreflightStorageCommand(), 2);
    QVERIFY(!model.Busy());
    QVERIFY(model.Status().contains(QStringLiteral("MAV_RESULT 2")));

    QVERIFY(model.StartEnumeration());
    model.commandConfirmationTimedOut();
    QVERIFY(!model.Busy());
    QVERIFY(model.CommandResultUncertain());
    QVERIFY(model.Status().contains(QStringLiteral("result unknown")));
    const QString timedOutStatus = model.Status();
    model.commandAckReceived(6,
        ConfigHWCANViewModel::PreflightUavcanCommand(), 0);
    QCOMPARE(model.Status(), timedOutStatus);
    QVERIFY(!model.StopEnumeration());
    QVERIFY(model.Status().contains(QStringLiteral("Reconnect")));
}

void ConfigHWCANViewTest::factoryResetRequiresFreshExplicitArm()
{
    ConfigHWCANViewModel model;
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    QSignalSpy commands(&model, &ConfigHWCANViewModel::commandRequested);

    model.setArmed(true);
    model.SetFactoryResetArmed(true);
    QVERIFY(model.VehicleArmed());
    QVERIFY(!model.FactoryResetArmed());
    QVERIFY(!model.StartEnumeration());
    QVERIFY(!model.SetSelectedCanEnable(2));
    QCOMPARE(commands.count(), 0);
    QVERIFY(model.Status().contains(QStringLiteral("Disarm")));

    model.setArmed(false);
    QVERIFY(!model.FactoryReset());
    QCOMPARE(commands.count(), 0);
    QCOMPARE(model.Status(), QStringLiteral("Arm Factory Reset first."));
    model.SetFactoryResetArmed(true);
    QVERIFY(model.CanFactoryReset());
    QVERIFY(model.FactoryReset());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.first().at(1).toInt(),
             ConfigHWCANViewModel::PreflightStorageCommand());
    QCOMPARE(commands.first().at(2).toFloat(), 2.0f);
    QCOMPARE(commands.first().at(3).toFloat(), 0.0f);
    QVERIFY(!model.FactoryResetArmed());
    QVERIFY(!model.CanFactoryReset());

    model.commandAckReceived(1,
        ConfigHWCANViewModel::PreflightStorageCommand(), 0);
    QVERIFY(!model.Busy());
    QVERIFY(!model.FactoryReset());
    QCOMPARE(commands.count(), 1);
}

void ConfigHWCANViewTest::
    widgetMatchesMissionPlannerSurfaceAndBindings()
{
    ConfigHWCANView view(catalogFixture());
    view.setParameterSnapshot(snapshot(1, 1));
    view.setConnected(true);
    view.resize(view.sizeHint());
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigHWCANView"));
    auto *title = view.findChild<QLabel *>(QStringLiteral("hwCanTitle"));
    auto *combo = view.findChild<QComboBox *>(
        QStringLiteral("hwCanEnableCombo"));
    auto *start = view.findChild<QPushButton *>(
        QStringLiteral("hwCanStartEnumerationButton"));
    auto *stop = view.findChild<QPushButton *>(
        QStringLiteral("hwCanStopEnumerationButton"));
    auto *save = view.findChild<QPushButton *>(
        QStringLiteral("hwCanSaveConfigButton"));
    auto *arm = view.findChild<QCheckBox *>(
        QStringLiteral("hwCanFactoryResetCheckBox"));
    auto *reset = view.findChild<QPushButton *>(
        QStringLiteral("hwCanFactoryResetButton"));
    auto *status = view.findChild<QLabel *>(QStringLiteral("hwCanStatus"));
    auto *restartNote = view.findChild<QLabel *>(
        QStringLiteral("hwCanRestartNote"));
    QVERIFY(title);
    QVERIFY(combo);
    QVERIFY(start);
    QVERIFY(stop);
    QVERIFY(save);
    QVERIFY(arm);
    QVERIFY(reset);
    QVERIFY(status);
    QVERIFY(restartNote);
    QVERIFY(!restartNote->isHidden());
    QCOMPARE(title->text(), QStringLiteral("CAN"));
    QCOMPARE(combo->count(), 3);
    QCOMPARE(combo->currentData().toInt(), 1);
    QCOMPARE(start->text(), QStringLiteral("Start Enumeration"));
    QCOMPARE(stop->text(), QStringLiteral("Stop Enumeration"));
    QCOMPARE(save->text(), QStringLiteral("Save All Config"));
    QCOMPARE(arm->text(), QStringLiteral("Factory Reset"));
    QCOMPARE(reset->text(), QStringLiteral("Reset config"));
    QVERIFY(!reset->isEnabled());

    QSignalSpy writes(&view, &ConfigHWCANView::writeRequested);
    combo->setCurrentIndex(2);
    QCOMPARE(writes.count(), 1);
    QVERIFY(!combo->isEnabled());
    view.parameterChanged(1, QStringLiteral("BRD_CAN_ENABLE"), 2);
    QVERIFY(combo->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("Restart")));

    QSignalSpy commands(&view, &ConfigHWCANView::commandRequested);
    arm->click();
    QVERIFY(reset->isEnabled());
    reset->click();
    QCOMPARE(commands.count(), 1);
    QVERIFY(!arm->isChecked());
    QVERIFY(!reset->isEnabled());

    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    view.render(&image);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QCOMPARE(qAlpha(image.pixel(x, y)), 255);
        }
    }
    QVERIFY(image.pixelColor(1, 1).lightness() < 100);

    ConfigHWCANView modern(catalogFixture());
    modern.setParameterSnapshot(modernSnapshot());
    modern.setConnected(true);
    modern.resize(modern.sizeHint());
    modern.show();
    QApplication::processEvents();
    auto *legacyRow = modern.findChild<QWidget *>(
        QStringLiteral("hwCanEnableRow"));
    auto *ports = modern.findChild<QGroupBox *>(
        QStringLiteral("hwCanPortsGroup"));
    auto *drivers = modern.findChild<QGroupBox *>(
        QStringLiteral("hwCanDriversGroup"));
    auto *driverEditor = modern.findChild<QComboBox *>(
        QStringLiteral("hwCanFieldEditor_CAN_P1_DRIVER"));
    auto *bitrateEditor = modern.findChild<QDoubleSpinBox *>(
        QStringLiteral("hwCanFieldEditor_CAN_P1_BITRATE"));
    QVERIFY(legacyRow);
    QVERIFY(legacyRow->isHidden());
    auto *modernRestartNote = modern.findChild<QLabel *>(
        QStringLiteral("hwCanRestartNote"));
    QVERIFY(modernRestartNote);
    QVERIFY(modernRestartNote->isHidden());
    QVERIFY(ports);
    QVERIFY(!ports->isHidden());
    QVERIFY(drivers);
    QVERIFY(!drivers->isHidden());
    QVERIFY(driverEditor);
    QVERIFY(bitrateEditor);
    QCOMPARE(driverEditor->count(), 3);
    QCOMPARE(driverEditor->currentData().toInt(), 1);
    QCOMPARE(bitrateEditor->value(), 1000000.0);
    QCOMPARE(bitrateEditor->suffix(), QString());

    QSignalSpy modernWrites(&modern, &ConfigHWCANView::writeRequested);
    driverEditor->setCurrentIndex(2);
    QCOMPARE(modernWrites.count(), 1);
    QCOMPARE(modernWrites.first().at(1).toString(),
             QStringLiteral("CAN_P1_DRIVER"));
    modern.parameterChanged(1, QStringLiteral("CAN_P1_DRIVER"), 2);
    QVERIFY(driverEditor->isEnabled());
}

QTEST_MAIN(ConfigHWCANViewTest)
#include "test_confighwcanview.moc"
