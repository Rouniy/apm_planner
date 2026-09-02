#include <QtTest>

#include "ui/configuration/ConfigGPSOrderView.h"

#include <QApplication>
#include <QBuffer>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:GPS_CAN_NODEID1"
               humanName="GPS Node ID 1"
               documentation="First detected DroneCAN GPS node.">
          <field name="ReadOnly">True</field>
        </param>
        <param name="ArduCopter:GPS_CAN_NODEID2"
               humanName="GPS Node ID 2"
               documentation="Second detected DroneCAN GPS node.">
          <field name="ReadOnly">True</field>
        </param>
        <param name="ArduCopter:GPS1_CAN_OVRIDE"
               humanName="First DroneCAN GPS NODE ID"
               documentation="Pinned first GPS node.">
          <field name="Range">0 127</field>
          <field name="Increment">1</field>
        </param>
        <param name="ArduCopter:GPS2_CAN_OVRIDE"
               humanName="Second DroneCAN GPS NODE ID"
               documentation="Pinned second GPS node.">
          <field name="Range">0 127</field>
          <field name="Increment">1</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> parameterFixture(int component = 1)
{
    return {
        {component, QStringLiteral("GPS_CAN_NODEID1"), 42},
        {component, QStringLiteral("GPS_CAN_NODEID2"), 84},
        {component, QStringLiteral("GPS1_CAN_OVRIDE"), 0},
        {component, QStringLiteral("GPS2_CAN_OVRIDE"), 11}
    };
}

ParamField fieldNamed(const ConfigGPSOrderViewModel &model,
                      const QString &name)
{
    for (const ParamField &field : model.Fields()) {
        if (field.name == name) {
            return field;
        }
    }
    return {};
}

GpsCanRow rowNamed(const ConfigGPSOrderViewModel &model,
                   const QString &name)
{
    for (const GpsCanRow &row : model.Rows()) {
        if (row.Name == name) {
            return row;
        }
    }
    return {};
}

void setParameter(QList<ConfigFriendlyParameterValue> *parameters,
                  const QString &name, int value)
{
    for (ConfigFriendlyParameterValue &parameter : *parameters) {
        if (parameter.name == name) {
            parameter.value = value;
            return;
        }
    }
}
} // namespace

class ConfigGPSOrderViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactFieldsMetadataAndHydration();
    void rowsMatchMissionPlannerOrderingAndFiltering();
    void availabilityAndConnectionStatusesAreExact();
    void overrideRequiresExactEchoThenFullRefresh();
    void normalWritesRejectReadOnlyAndIgnoreStaleEchoes();
    void widgetMatchesMissionPlannerContractAndRendersOpaque();
};

void ConfigGPSOrderViewTest::exactFieldsMetadataAndHydration()
{
    ConfigGPSOrderViewModel model;
    QSignalSpy writes(&model,
                     &ConfigGPSOrderViewModel::writeRequested);
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(parameterFixture(7), 7);

    QCOMPARE(writes.count(), 0);
    QCOMPARE(ConfigGPSOrderViewModel::WriteTimeoutMs(), 5000);
    QCOMPARE(ConfigGPSOrderViewModel::FieldNames(), QStringList({
        QStringLiteral("GPS_CAN_NODEID1"),
        QStringLiteral("GPS_CAN_NODEID2"),
        QStringLiteral("GPS1_CAN_OVRIDE"),
        QStringLiteral("GPS2_CAN_OVRIDE")
    }));
    QCOMPARE(model.ComponentId(), 7);
    QCOMPARE(model.Fields().size(), 4);

    const ParamField detected = fieldNamed(
        model, QStringLiteral("GPS_CAN_NODEID1"));
    QCOMPARE(detected.label, QStringLiteral("GPS Node ID 1"));
    QCOMPARE(detected.description,
             QStringLiteral("First detected DroneCAN GPS node."));
    QCOMPARE(detected.value.toInt(), 42);
    QVERIFY(detected.readOnly);
    QVERIFY(detected.editorKind == ParamField::EditorKind::Numeric);

    const ParamField override = fieldNamed(
        model, QStringLiteral("GPS1_CAN_OVRIDE"));
    QCOMPARE(override.label,
             QStringLiteral("First DroneCAN GPS NODE ID"));
    QVERIFY(!override.readOnly);
    QVERIFY(override.hasRange);
    QVERIFY(override.enforceRange);
    QCOMPARE(override.minimum, 0.0);
    QCOMPARE(override.maximum, 127.0);
    QCOMPARE(override.increment, 1.0);

    QList<ConfigFriendlyParameterValue> missing = parameterFixture();
    missing.removeLast();
    model.setParameterSnapshot(missing);
    const ParamField missingField = fieldNamed(
        model, QStringLiteral("GPS2_CAN_OVRIDE"));
    QCOMPARE(missingField.status, QStringLiteral("n/a"));
    QVERIFY(missingField.readOnly);
}

void ConfigGPSOrderViewTest::
    rowsMatchMissionPlannerOrderingAndFiltering()
{
    ConfigGPSOrderViewModel model;
    model.setParameterSnapshot(parameterFixture());

    const QList<GpsCanRow> rows = model.Rows();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0).Order, 2);
    QCOMPARE(rows.at(0).Name, QStringLiteral("GPS Override 2"));
    QCOMPARE(rows.at(0).NodeID, 11);
    QCOMPARE(rows.at(1).Order, 98);
    QCOMPARE(rows.at(1).Name, QStringLiteral("GPS Detect 1"));
    QCOMPARE(rows.at(1).NodeID, 42);
    QCOMPARE(rows.at(2).Order, 99);
    QCOMPARE(rows.at(2).Name, QStringLiteral("GPS Detect 2"));
    QCOMPARE(rows.at(2).NodeID, 84);

    QList<ConfigFriendlyParameterValue> assigned = parameterFixture();
    setParameter(&assigned, QStringLiteral("GPS1_CAN_OVRIDE"), 84);
    model.setParameterSnapshot(assigned);
    QCOMPARE(model.Rows().size(), 3);
    QCOMPARE(model.Rows().at(0).Name, QStringLiteral("GPS Override 1"));
    QCOMPARE(model.Rows().at(0).NodeID, 84);
    QCOMPARE(model.Rows().at(1).Name, QStringLiteral("GPS Override 2"));
    QCOMPARE(model.Rows().at(2).Name, QStringLiteral("GPS Detect 1"));

    QList<ConfigFriendlyParameterValue> duplicate = parameterFixture();
    setParameter(&duplicate, QStringLiteral("GPS_CAN_NODEID2"), 42);
    setParameter(&duplicate, QStringLiteral("GPS2_CAN_OVRIDE"), 0);
    model.setParameterSnapshot(duplicate);
    QCOMPARE(model.Rows().size(), 2);
    QCOMPARE(model.Rows().at(0).Order, 98);
    QCOMPARE(model.Rows().at(0).NodeID, 42);
    QCOMPARE(model.Rows().at(1).Order, 99);
    QCOMPARE(model.Rows().at(1).NodeID, 42);
}

void ConfigGPSOrderViewTest::
    availabilityAndConnectionStatusesAreExact()
{
    ConfigGPSOrderViewModel model;
    QList<ConfigFriendlyParameterValue> missing = parameterFixture();
    for (auto iterator = missing.begin(); iterator != missing.end();) {
        if (iterator->name == QLatin1String("GPS1_CAN_OVRIDE")) {
            iterator = missing.erase(iterator);
        } else {
            ++iterator;
        }
    }
    model.setParameterSnapshot(missing);
    QCOMPARE(model.Status(), QStringLiteral(
        "GPS1_CAN_OVRIDE not available — connect a "
        "DroneCAN-capable autopilot."));

    QList<ConfigFriendlyParameterValue> empty = parameterFixture();
    for (ConfigFriendlyParameterValue &parameter : empty) {
        parameter.value = 0;
    }
    model.setParameterSnapshot(empty);
    QCOMPARE(model.Status(), QStringLiteral("No CAN GPS nodes detected."));

    model.setConnected(true);
    model.setConnected(false);
    QCOMPARE(model.Status(), QStringLiteral("offline"));
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.CanEdit());
}

void ConfigGPSOrderViewTest::overrideRequiresExactEchoThenFullRefresh()
{
    ConfigGPSOrderViewModel model;
    model.setParameterSnapshot(parameterFixture());
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigGPSOrderViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                        &ConfigGPSOrderViewModel::refreshRequested);

    const GpsCanRow detected = rowNamed(
        model, QStringLiteral("GPS Detect 1"));
    QCOMPARE(detected.NodeID, 42);
    QVERIFY(model.Override1(detected));
    QVERIFY(model.Busy());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("GPS1_CAN_OVRIDE"));
    QCOMPARE(writes.first().at(2).toInt(), 42);

    model.parameterChanged(2, QStringLiteral("GPS1_CAN_OVRIDE"), 42);
    QVERIFY(model.Busy());
    QCOMPARE(refreshes.count(), 0);
    model.parameterChanged(
        1, QStringLiteral("GPS1_CAN_OVRIDE"), 42.0000005);
    QVERIFY(!model.Busy());
    QCOMPARE(refreshes.count(), 0);
    QCOMPARE(model.Status(), QStringLiteral("write mismatch"));

    QVERIFY(model.Override1(detected));
    QVERIFY(model.Busy());
    QCOMPARE(writes.count(), 2);
    model.parameterChanged(1, QStringLiteral("GPS1_CAN_OVRIDE"), 42.0);
    QVERIFY(model.Busy());
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(refreshes.first().at(0).toInt(), 1);
    QCOMPARE(model.Status(), QStringLiteral("GPS1_CAN_OVRIDE = 42"));

    QList<ConfigFriendlyParameterValue> refreshed = parameterFixture();
    setParameter(&refreshed, QStringLiteral("GPS1_CAN_OVRIDE"), 42);
    model.setParameterSnapshot(refreshed);
    QVERIFY(!model.Busy());
    QCOMPARE(rowNamed(model, QStringLiteral("GPS Override 1")).NodeID, 42);

    const GpsCanRow second = rowNamed(
        model, QStringLiteral("GPS Detect 2"));
    QVERIFY(model.Override2(second));
    model.parameterChanged(1, QStringLiteral("GPS2_CAN_OVRIDE"), 83);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("write mismatch"));
    QCOMPARE(fieldNamed(model, QStringLiteral("GPS2_CAN_OVRIDE"))
                 .value.toInt(), 83);

    QVERIFY(model.Override2(second));
    model.parameterWriteFailed(
        1, QStringLiteral("GPS2_CAN_OVRIDE"), QString());
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("write failed"));
    QCOMPARE(fieldNamed(model, QStringLiteral("GPS2_CAN_OVRIDE"))
                 .value.toInt(), 83);
}

void ConfigGPSOrderViewTest::
    normalWritesRejectReadOnlyAndIgnoreStaleEchoes()
{
    ConfigGPSOrderViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(parameterFixture());
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigGPSOrderViewModel::writeRequested);

    QVERIFY(!model.setFieldValue(
        QStringLiteral("GPS_CAN_NODEID1"), 12));
    QVERIFY(!model.setFieldValue(
        QStringLiteral("GPS1_CAN_OVRIDE"), -1));
    QVERIFY(!model.setFieldValue(
        QStringLiteral("GPS1_CAN_OVRIDE"), 128));
    QVERIFY(!model.setFieldValue(
        QStringLiteral("GPS1_CAN_OVRIDE"), 12.5));
    QCOMPARE(writes.count(), 0);

    QVERIFY(model.setFieldValue(
        QStringLiteral("GPS1_CAN_OVRIDE"), 17));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("GPS1_CAN_OVRIDE"));
    QCOMPARE(writes.first().at(2).toInt(), 17);

    QList<ConfigFriendlyParameterValue> replacement = parameterFixture();
    setParameter(&replacement, QStringLiteral("GPS1_CAN_OVRIDE"), 17);
    model.setParameterSnapshot(replacement);
    QVERIFY(!model.Busy());
    QCOMPARE(fieldNamed(model, QStringLiteral("GPS1_CAN_OVRIDE"))
                 .value.toInt(), 17);
    model.parameterChanged(1, QStringLiteral("GPS1_CAN_OVRIDE"), 17);
    QCOMPARE(writes.count(), 1);

    QVERIFY(model.setFieldValue(
        QStringLiteral("GPS1_CAN_OVRIDE"), 18));
    model.parameterChanged(2, QStringLiteral("GPS1_CAN_OVRIDE"), 18);
    QVERIFY(model.Busy());
    model.setConnected(false);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("offline"));
    model.parameterChanged(1, QStringLiteral("GPS1_CAN_OVRIDE"), 18);
    QCOMPARE(writes.count(), 2);
}

void ConfigGPSOrderViewTest::
    widgetMatchesMissionPlannerContractAndRendersOpaque()
{
    ConfigGPSOrderView view(catalogFixture());
    view.resize(800, 560);
    view.setParameterSnapshot(parameterFixture());
    view.setConnected(true);

    QCOMPARE(view.objectName(), QStringLiteral("ConfigGPSOrderView"));
    QCOMPARE(view.sizeHint(), QSize(800, 560));
    QCOMPARE(ConfigGPSOrderView::ArmedRefreshWarningTitle(),
             QStringLiteral("Refresh Params"));
    QCOMPARE(ConfigGPSOrderView::ArmedRefreshWarningText(),
             QStringLiteral(
                 "Update Params\nDON'T DO THIS IF YOU ARE IN THE AIR\n"));
    QVERIFY(view.testAttribute(Qt::WA_OpaquePaintEvent));
    QVERIFY(view.autoFillBackground());

    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("gpsOrderTitle"));
    QLabel *intro = view.findChild<QLabel *>(
        QStringLiteral("gpsOrderIntro"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("gpsOrderRefreshButton"));
    QTableWidget *table = view.findChild<QTableWidget *>(
        QStringLiteral("gpsOrderTable"));
    QVERIFY(title);
    QVERIFY(intro);
    QVERIFY(refresh);
    QVERIFY(table);
    QCOMPARE(title->text(), QStringLiteral("UAVCAN GPS Order"));
    QCOMPARE(intro->text(), QStringLiteral(
        "Detected DroneCAN GPS nodes. Use Override to pin a node to "
        "GPS1 or GPS2."));
    QCOMPARE(refresh->text(), QStringLiteral("Refresh Params"));
    QCOMPARE(table->columnCount(), 5);
    QCOMPARE(table->rowCount(), 3);
    const QStringList headings = {
        QStringLiteral("Order"), QStringLiteral("NodeID"),
        QStringLiteral("Name"), QStringLiteral("GPS1"),
        QStringLiteral("GPS2")
    };
    for (int column = 0; column < headings.size(); ++column) {
        QCOMPARE(table->horizontalHeaderItem(column)->text(),
                 headings.at(column));
    }

    for (const QString &name : ConfigGPSOrderViewModel::FieldNames()) {
        QWidget *row = view.findChild<QWidget *>(
            QStringLiteral("gpsOrderFieldRow_%1").arg(name));
        QDoubleSpinBox *editor = view.findChild<QDoubleSpinBox *>(
            QStringLiteral("gpsOrderFieldEditor_%1").arg(name));
        QVERIFY(row);
        QVERIFY(editor);
        QCOMPARE(row->property("parameterName").toString(), name);
        QCOMPARE(editor->isEnabled(), name.contains(
                     QStringLiteral("OVRIDE")));
    }

    QSignalSpy writes(&view, &ConfigGPSOrderView::writeRequested);
    QSignalSpy refreshes(&view, &ConfigGPSOrderView::refreshRequested);
    QPushButton *override1 = view.findChild<QPushButton *>(
        QStringLiteral("gpsOrderOverride1_42"));
    QVERIFY(override1);
    QVERIFY(override1->isEnabled());
    override1->click();
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("GPS1_CAN_OVRIDE"));
    view.parameterChanged(1, QStringLiteral("GPS1_CAN_OVRIDE"), 42);
    QCOMPARE(refreshes.count(), 1);
    view.refreshCanceled();

    view.setArmed(true);
    QTimer::singleShot(0, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                message->done(QMessageBox::No);
            }
        }
    });
    refresh->click();
    QCOMPARE(refreshes.count(), 1);
    view.setArmed(false);
    refresh->click();
    QCOMPARE(refreshes.count(), 2);
    view.refreshCanceled();

    QImage rendered(view.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    view.render(&rendered);
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            QCOMPARE(qAlpha(rendered.pixel(x, y)), 255);
        }
    }

    view.setConnected(false);
    QVERIFY(!refresh->isEnabled());
    QLabel *status = view.findChild<QLabel *>(
        QStringLiteral("gpsOrderStatus"));
    QVERIFY(status);
    QCOMPARE(status->text(), QStringLiteral("offline"));
}

QTEST_MAIN(ConfigGPSOrderViewTest)
#include "test_configgpsorderview.moc"
