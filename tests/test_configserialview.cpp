#include <QtTest>

#include "ui/configuration/ConfigSerialView.h"

#include <QBuffer>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QSignalSpy>
#include <QToolButton>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:SERIAL1_BAUD" humanName="Serial baud">
          <values>
            <value code="57">57600</value>
            <value code="115">115200</value>
          </values>
        </param>
        <param name="ArduCopter:SERIAL1_PROTOCOL" humanName="Protocol">
          <values>
            <value code="-1">None</value>
            <value code="1">MAVLink1</value>
            <value code="2">MAVLink2</value>
            <value code="5">GPS</value>
          </values>
        </param>
        <param name="ArduCopter:SERIAL1_OPTIONS" humanName="Options">
          <field name="Bitmask">0:InvertRX,3:Swap</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

SerialPortRow port(const ConfigSerialViewModel &model,
                   const QString &portName)
{
    for (const SerialPortRow &row : model.Ports()) {
        if (row.portName == portName) {
            return row;
        }
    }
    return {};
}

QWidget *portWidget(ConfigSerialView &view, const QString &portName)
{
    const QList<QWidget *> widgets = view.findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (widget->property("portName").toString() == portName) {
            return widget;
        }
    }
    return nullptr;
}
}

class ConfigSerialViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void discoversSparsePortsAndHydratesWithoutWrites();
    void protocolRulesWriteInOrderAndWarningPersists();
    void dependencyFailureSurvivesOtherAcknowledgements();
    void bitmaskPreservesUnknownBits();
    void pendingWritesHandleStaleAckMismatchAndFailure();
    void newestAckWinsOverLateSupersededAck();
    void mavlink2RuleDoesNotChangeBaud();
    void viewMatchesMissionPlannerContract();
};

void ConfigSerialViewTest::discoversSparsePortsAndHydratesWithoutWrites()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    QSignalSpy writes(&model, &ConfigSerialViewModel::writeRequested);
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL0_BAUD"), 115},
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL3_BAUD"), 115},
        {1, QStringLiteral("SERIAL3_PROTOCOL"), 2},
        {1, QStringLiteral("BRD_SER3_RTSCTS"), 1},
        {1, QStringLiteral("SERIAL10_BAUD"), 57},
        {1, QStringLiteral("SERIAL10_PROTOCOL"), -1},
        {1, QStringLiteral("BRD_SER10_RTSCTS"), 2},
        {1, QStringLiteral("SERIALX_BAUD"), 57},
        {154, QStringLiteral("SERIAL2_BAUD"), 57}
    });

    QCOMPARE(writes.count(), 0);
    const QList<SerialPortRow> ports = model.Ports();
    QCOMPARE(ports.size(), 3);
    QCOMPARE(ports.at(0).portName, QStringLiteral("SERIAL1"));
    QCOMPARE(ports.at(1).portName, QStringLiteral("SERIAL3"));
    QCOMPARE(ports.at(2).portName, QStringLiteral("SERIAL10"));
    QCOMPARE(ports.at(1).label,
             QStringLiteral("SERIAL PORT 3 (RTS/CTS)"));
    QCOMPARE(ports.at(2).label,
             QStringLiteral("SERIAL PORT 10 (RTS/CTS Auto)"));
    QCOMPARE(ports.at(2).baudOptions.size(), 2);
    QCOMPARE(ports.at(2).protocolOptions.first().value.toInt(), -1);

    model.parameterChanged(1, QStringLiteral("BRD_SER3_RTSCTS"), 2);
    QCOMPARE(port(model, QStringLiteral("SERIAL3")).label,
             QStringLiteral("SERIAL PORT 3 (RTS/CTS Auto)"));
}

void ConfigSerialViewTest::protocolRulesWriteInOrderAndWarningPersists()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL1_OPTIONS"), 9},
        {1, QStringLiteral("SERIAL2_BAUD"), 57},
        {1, QStringLiteral("SERIAL2_PROTOCOL"), 1},
        {1, QStringLiteral("SERIAL3_BAUD"), 57},
        {1, QStringLiteral("SERIAL3_PROTOCOL"), 2},
        {1, QStringLiteral("SERIAL4_BAUD"), 57},
        {1, QStringLiteral("SERIAL4_PROTOCOL"), 1}
    });
    QSignalSpy writes(&model, &ConfigSerialViewModel::writeRequested);

    QVERIFY(model.selectProtocol(QStringLiteral("SERIAL1"), 1));
    QCOMPARE(writes.count(), 3);
    QCOMPARE(writes.at(0).at(1).toString(),
             QStringLiteral("SERIAL1_PROTOCOL"));
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("SERIAL1_BAUD"));
    QCOMPARE(writes.at(1).at(2).toInt(), 115);
    QCOMPARE(writes.at(2).at(1).toString(),
             QStringLiteral("SERIAL1_OPTIONS"));
    QCOMPARE(writes.at(2).at(2).toLongLong(), 0);
    QVERIFY(model.Warning().startsWith(QStringLiteral(
        "SERIAL1 : If connecting a Mavlink sensor")));
    QVERIFY(model.Warning().contains(QStringLiteral(
        "Maximum number of Mavlink ports are 5")));

    model.parameterChanged(1, QStringLiteral("SERIAL1_OPTIONS"), 0);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("…"));
    model.parameterChanged(1, QStringLiteral("SERIAL1_PROTOCOL"), 1);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("…"));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 115);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("✓"));
    QVERIFY(model.Warning().startsWith(QStringLiteral(
        "SERIAL1 : If connecting a Mavlink sensor")));

    QVERIFY(model.selectProtocol(QStringLiteral("SERIAL1"), 5));
    QVERIFY(model.Warning().isEmpty());
}

void ConfigSerialViewTest::dependencyFailureSurvivesOtherAcknowledgements()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL1_OPTIONS"), 9}
    });

    QVERIFY(model.selectProtocol(QStringLiteral("SERIAL1"), 1));
    model.parameterWriteFailed(1, QStringLiteral("SERIAL1_BAUD"),
                               QStringLiteral("write failed"));
    model.parameterChanged(1, QStringLiteral("SERIAL1_OPTIONS"), 0);
    model.parameterChanged(1, QStringLiteral("SERIAL1_PROTOCOL"), 1);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("write failed"));
}

void ConfigSerialViewTest::bitmaskPreservesUnknownBits()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    const qulonglong unknownBit = qulonglong(1) << 27;
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL1_OPTIONS"),
            QVariant::fromValue(unknownBit)}
    });
    QSignalSpy writes(&model, &ConfigSerialViewModel::writeRequested);

    QVERIFY(model.setOptionBit(QStringLiteral("SERIAL1"), 3, true));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(2).toULongLong(),
             unknownBit | (qulonglong(1) << 3));
    const SerialPortRow row = port(model, QStringLiteral("SERIAL1"));
    QCOMPARE(row.optionsValue, unknownBit | (qulonglong(1) << 3));
    QCOMPARE(row.optionsText, QStringLiteral("3: Swap"));
}

void ConfigSerialViewTest::pendingWritesHandleStaleAckMismatchAndFailure()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5}
    });

    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 115));
    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 57));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 115);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).selectedBaud.toInt(), 57);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("…"));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 57);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("✓"));

    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 115));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 57);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("write mismatch"));
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).selectedBaud.toInt(), 57);

    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 115));
    model.parameterWriteFailed(1, QStringLiteral("SERIAL1_BAUD"),
                               QStringLiteral("not connected"));
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("not connected"));
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).selectedBaud.toInt(), 57);
}

void ConfigSerialViewTest::newestAckWinsOverLateSupersededAck()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5}
    });

    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 115));
    QVERIFY(model.selectBaud(QStringLiteral("SERIAL1"), 57));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 57);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("✓"));
    model.parameterChanged(1, QStringLiteral("SERIAL1_BAUD"), 115);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).selectedBaud.toInt(), 57);
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).status,
             QStringLiteral("✓"));
}

void ConfigSerialViewTest::mavlink2RuleDoesNotChangeBaud()
{
    ConfigSerialViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL1_OPTIONS"), 9}
    });
    QSignalSpy writes(&model, &ConfigSerialViewModel::writeRequested);

    QVERIFY(model.selectProtocol(QStringLiteral("SERIAL1"), 2));
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(0).at(0).toInt(), 1);
    QCOMPARE(writes.at(0).at(1).toString(),
             QStringLiteral("SERIAL1_PROTOCOL"));
    QCOMPARE(writes.at(0).at(2).toInt(), 2);
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("SERIAL1_OPTIONS"));
    QCOMPARE(port(model, QStringLiteral("SERIAL1")).selectedBaud.toInt(), 57);

    ConfigSerialViewModel withoutOptions;
    withoutOptions.setCatalog(catalogFixture());
    withoutOptions.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5}
    });
    QSignalSpy writesWithoutOptions(
        &withoutOptions, &ConfigSerialViewModel::writeRequested);
    QVERIFY(withoutOptions.selectProtocol(QStringLiteral("SERIAL1"), 1));
    QCOMPARE(writesWithoutOptions.count(), 2);
    QCOMPARE(writesWithoutOptions.at(0).at(1).toString(),
             QStringLiteral("SERIAL1_PROTOCOL"));
    QCOMPARE(writesWithoutOptions.at(1).at(1).toString(),
             QStringLiteral("SERIAL1_BAUD"));
}

void ConfigSerialViewTest::viewMatchesMissionPlannerContract()
{
    ConfigSerialView view(catalogFixture());
    QSignalSpy writes(&view, &ConfigSerialView::writeRequested);
    view.setParameterSnapshot({
        {1, QStringLiteral("SERIAL1_BAUD"), 57},
        {1, QStringLiteral("SERIAL1_PROTOCOL"), 5},
        {1, QStringLiteral("SERIAL1_OPTIONS"), 1}
    });
    QCOMPARE(writes.count(), 0);
    QCOMPARE(view.objectName(), QStringLiteral("ConfigSerialView"));

    QStringList headerText;
    QWidget *header = view.findChild<QWidget *>(
        QStringLiteral("serialPortsHeader"));
    QVERIFY(header);
    for (int column = 0; column < 4; ++column) {
        headerText.append(header->findChild<QLabel *>(
            QStringLiteral("serialPortsHeader%1").arg(column))->text());
    }
    QCOMPARE(headerText, QStringList({QStringLiteral("Port Name"),
                                      QStringLiteral("Speed"),
                                      QStringLiteral("Protcol"),
                                      QStringLiteral("Options")}));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("serialPortsNote"))
                 ->text(),
             QStringLiteral("Note: Changes to the serial port settings will "
                            "not take effect until the board is rebooted."));

    QWidget *row = portWidget(view, QStringLiteral("SERIAL1"));
    QVERIFY(row);
    auto *baud = row->findChild<QComboBox *>(QStringLiteral("baudCombo"));
    auto *protocol = row->findChild<QComboBox *>(
        QStringLiteral("protocolCombo"));
    auto *options = row->findChild<QToolButton *>(
        QStringLiteral("optionsButton"));
    QVERIFY(baud);
    QVERIFY(protocol);
    QVERIFY(options);
    QVERIFY(baud->minimumWidth() >= 160);
    QVERIFY(protocol->minimumWidth() >= 160);
    QVERIFY(protocol->isEnabled());
    QCOMPARE(options->text(), QStringLiteral("Set Bitmask"));
    QCOMPARE(options->menu()->maximumHeight(), 320);
    QCOMPARE(options->menu()->actions().first()->text(),
             QStringLiteral("0: InvertRX"));

    ConfigSerialView missingEditors(catalogFixture());
    missingEditors.setParameterSnapshot({
        {1, QStringLiteral("SERIAL2_BAUD"), 57}
    });
    QWidget *limitedRow = portWidget(
        missingEditors, QStringLiteral("SERIAL2"));
    QVERIFY(limitedRow);
    auto *limitedProtocol = limitedRow->findChild<QComboBox *>(
        QStringLiteral("protocolCombo"));
    auto *limitedOptions = limitedRow->findChild<QToolButton *>(
        QStringLiteral("optionsButton"));
    QVERIFY(limitedProtocol->isVisibleTo(&missingEditors));
    QVERIFY(!limitedProtocol->isEnabled());
    QVERIFY(!limitedOptions->isVisibleTo(&missingEditors));
}

QTEST_MAIN(ConfigSerialViewTest)
#include "test_configserialview.moc"
